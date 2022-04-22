// SPDX-License-Identifier: GPL-3.0-or-later

#include "dbengineapi.h"
#include <mongoc.h>

/* Default global database instance */
struct mongoengine_instance mongodb_ctx;

char *mongoengine_uri = "";
char *mongoengine_database = "netdata";
int mongoengine_timeout = 1000;
int mongoengine_expiration = 86400;

static inline struct mongoengine_instance *get_mongoeng_ctx_from_host(RRDHOST *host)
{
    return (struct mongoengine_instance *)host->rrdeng_ctx;
}

bool mongoeng_create_host_collection(struct mongoengine_instance *ctx, RRDHOST *host)
{
    bson_error_t error;
    char *index_name;
    bson_t keys;
    bson_t *create_indexes;
    bool r;
    struct mongoeng_host_data *mongoeng_data;

    bson_t *opts = bson_new();
    bson_t *timeseries = bson_new();

    mongoeng_data = callocz(1, sizeof(struct mongoeng_host_data));

    BSON_APPEND_UTF8(timeseries, "timeField", "t");
    BSON_APPEND_UTF8(timeseries, "metaField", "m");
    BSON_APPEND_UTF8(timeseries, "granularity", "seconds");
    BSON_APPEND_DOCUMENT(opts, "timeseries", timeseries);
    if(mongoengine_expiration)
        BSON_APPEND_INT32(opts, "expireAfterSeconds", mongoengine_expiration);

    uv_mutex_lock(&ctx->client_lock);
    mongoeng_data->collection = mongoc_database_create_collection(ctx->database, &host->machine_guid[0], opts, &error);
    uv_mutex_unlock(&ctx->client_lock);

    bson_destroy(opts);
    bson_destroy(timeseries);

    if (!mongoeng_data->collection) {
        if(error.code == MONGOENG_TIMESERIES_ALREADY_EXISTS)
            mongoeng_data->collection = mongoc_database_get_collection(ctx->database, &host->machine_guid[0]);
        else {
            error("MongoDB mongoeng_create_host_collection couldn't create collection");
            return false;
        }
    }

    mongoeng_data->op = mongoc_collection_create_bulk_operation_with_opts (mongoeng_data->collection, NULL);

    bson_init (&keys);
    BSON_APPEND_INT32 (&keys, "m.s", 1);
    index_name = mongoc_collection_keys_to_index_string (&keys);
    create_indexes = BCON_NEW ("createIndexes", BCON_UTF8 (&host->machine_guid[0]), "indexes", "[", "{",
                                                                                  "key", BCON_DOCUMENT(&keys),
                                                                                  "name", BCON_UTF8(index_name),
                                                                               "}", "]");

    uv_mutex_lock(&ctx->client_lock);
    r = mongoc_database_write_command_with_opts(ctx->database, create_indexes, NULL, NULL, &error);
    uv_mutex_unlock(&ctx->client_lock);

    uv_mutex_init(&mongoeng_data->bulk_write_lock);

    host->mongoeng_data = mongoeng_data;

    bson_free (index_name);
    bson_destroy (create_indexes);

    if(!r) {
        error("MongoDB mongoeng_create_host_collection ERROR %s", error.message);
    }

    return r;
}

void mongoeng_metric_init(RRDDIM *rd)
{
    struct mongoengine_instance *ctx;
    struct mongoeng_dim_data *data;
    struct mongoeng_cmd cmd_latest;
    struct completion compl_latest;
    struct mongoeng_cmd cmd_oldest;
    struct completion compl_oldest;

    ctx = get_mongoeng_ctx_from_host(rd->rrdset->rrdhost);
    if (unlikely(!ctx)) {
        error("MongoDB mongoeng_metric_init failed to fetch context");
        return;
    }

    // TODO should be in host creation, wouldn't need the lock
    uv_mutex_lock(&ctx->collection_create_lock);
    if(!rd->rrdset->rrdhost->mongoeng_data)
        if(!mongoeng_create_host_collection(ctx, rd->rrdset->rrdhost))
            return;
    uv_mutex_unlock(&ctx->collection_create_lock);

    data = callocz(1, sizeof(struct mongoeng_dim_data));
    data->latest_time = 0;
    data->oldest_time = 0;
    data->region_count = 0;
    data->regions = NULL;
    uv_mutex_init(&data->regions_lock);
    rd->state->mongoeng_data = data;

    completion_init(&compl_latest);
    cmd_latest.opcode = MONGOENGINE_QUERY_TIME_LATEST;
    cmd_latest.rd = rd;
    cmd_latest.completion = &compl_latest;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd_latest);
    completion_wait_for(&compl_latest); 
    completion_destroy(&compl_latest);

    // can skip getting oldest time if latest time is 0
    if(!data->latest_time)
        return;

    completion_init(&compl_oldest);
    cmd_oldest.opcode = MONGOENGINE_QUERY_TIME_OLDEST;
    cmd_oldest.rd = rd;
    cmd_oldest.completion = &compl_oldest;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd_oldest);
    completion_wait_for(&compl_oldest);
    completion_destroy(&compl_oldest);
}

/*
 * Gets a handle for storing metrics to the database.
 * The handle must be released with mongoeng_store_metric_final().
 */
void mongoeng_store_metric_init(RRDDIM *rd)
{
    struct mongoeng_collect_handle *collect_handle;
    collect_handle = callocz(1, sizeof(struct mongoeng_collect_handle));
    collect_handle->host_data = rd->rrdset->rrdhost->mongoeng_data;
    rd->state->handle = (STORAGE_COLLECT_HANDLE *) collect_handle;
}

void mongoeng_store_metric_next(RRDDIM *rd, usec_t point_in_time, storage_number number)
{
    struct mongoeng_collect_handle *handle = (struct mongoeng_collect_handle *) rd->state->handle;

    bson_t *doc = bson_new ();
    bson_t *meta = bson_new ();

    BSON_APPEND_INT32 (meta, "v", number);
    BSON_APPEND_UTF8 (meta, "s", rd->rrdset->id);
    BSON_APPEND_UTF8 (meta, "d", rd->id);
    BSON_APPEND_INT32 (meta, "r", rd->update_every);

    BSON_APPEND_DATE_TIME (doc, "t", point_in_time/USEC_PER_MS);
    BSON_APPEND_DOCUMENT(doc, "m", meta);

    uv_mutex_lock(&handle->host_data->bulk_write_lock);

    if (unlikely(!handle->host_data->op)) {
        error("MongoDB mongoeng_store_metric_next ERROR no op");
        uv_mutex_unlock(&handle->host_data->bulk_write_lock);
        bson_destroy (doc);
        bson_destroy (meta);
        return;
    }

    mongoc_bulk_operation_insert (handle->host_data->op, doc);
    rd->state->mongoeng_data->latest_time = point_in_time;

    uv_mutex_unlock(&handle->host_data->bulk_write_lock);

    bson_destroy (doc);
    bson_destroy (meta);
}

/*
 * Releases the database reference from the handle for storing metrics.
 * Returns 1 if it's safe to delete the dimension.
 */
int mongoeng_store_metric_finalize(RRDDIM *rd)
{
    struct mongoeng_query_handle *collect_handle;
    collect_handle = (struct mongoeng_query_handle*) rd->state->handle;
    freez(collect_handle);
    rd->state->handle = NULL;
    return 1;
}

/**
 * Calculates the regions of different data collection intervals in a netdata chart in the time range
 * [start_time,end_time]. This call takes the netdata chart read lock.
 * @param st the netdata chart whose data collection interval boundaries are calculated.
 * @param start_time inclusive starting time in usec
 * @param end_time inclusive ending time in usec
 * @param region_info_arrayp It allocates (*region_info_arrayp) and populates it with information of regions of a
 *         reference dimension that that have different data collection intervals and overlap with the time range
 *         [start_time,end_time]. The caller must free (*region_info_arrayp) with freez(). If region_info_arrayp is set
 *         to NULL nothing was allocated.
 * @param max_intervalp is dereferenced and set to be the largest data collection interval of all regions.
 * @return number of regions with different data collection intervals.
 */
unsigned mongoeng_variable_step_boundaries(RRDSET *st, time_t start_time, time_t end_time, 
                                           struct rrdr_region_info **region_info_arrayp, unsigned *max_intervalp, struct context_param *context_param_list)
{
    RRDDIM *rd_iter, *rd;
    usec_t min_time, curr_time;
    unsigned max_interval, i;
    struct rrdr_region_info *region_info_array;
    struct mongoeng_dim_data *mongoeng_data;
    
    *max_intervalp = max_interval = 0;
    region_info_array = NULL;
    *region_info_arrayp = NULL;

    RRDDIM *temp_rd = context_param_list ? context_param_list->rd : NULL;
    for(rd_iter = temp_rd?temp_rd:st->dimensions, rd = NULL, min_time = (usec_t)-1 ; rd_iter ; rd_iter = rd_iter->next) {
        /*
         * Choose oldest dimension as reference. This is not equivalent to the union of all dimensions
         * but it is a best effort approximation with a bias towards older metrics in a chart. It
         * matches netdata behaviour in the sense that dimensions are generally aligned in a chart
         * and older dimensions contain more information about the time range. It does not work well
         * for metrics that have recently stopped being collected.
         */
        mongoeng_data = rd_iter->state->mongoeng_data;
        if(mongoeng_data->region_count && mongoeng_data->regions[0].value_count) {
            curr_time = mongoeng_data->regions[0].values[0].t;
            if (0 != curr_time && curr_time < min_time) {
                rd = rd_iter;
                min_time = curr_time;
            }
        }
    }
    if (NULL == rd) {
        return 1;
    }

    mongoeng_data = rd->state->mongoeng_data;
    region_info_array = mallocz(sizeof(*region_info_array) * mongoeng_data->region_count);
    for(i=0; i < mongoeng_data->region_count; i++) {
        region_info_array[i].points = mongoeng_data->regions[i].value_count;
        region_info_array[i].update_every = mongoeng_data->regions[i].interval;
        region_info_array[i].start_time = mongoeng_data->regions[i].values[0].t;
        if(max_interval < mongoeng_data->regions[i].interval)
            max_interval = mongoeng_data->regions[i].interval;
    }

    *region_info_arrayp = region_info_array;
    *max_intervalp = max_interval;

    return mongoeng_data->region_count;
}

/*
 * Gets a handle for loading metrics from the database.
 * The handle must be released with mongoeng_load_metric_final().
 */
void mongoeng_load_metric_init(RRDDIM *rd, struct rrddim_query_handle *rrdimm_handle, time_t start_time, time_t end_time)
{
    struct mongoeng_query_handle *handle;

    rrdimm_handle->rd = rd;
    rrdimm_handle->start_time = start_time;
    rrdimm_handle->end_time = end_time;

    handle = callocz(1, sizeof(struct mongoeng_query_handle));

    handle->data = rd->state->mongoeng_data;
    handle->region_index = 0;
    handle->value_index = 0;
    uv_mutex_lock(&handle->data->regions_lock);

    rrdimm_handle->handle = (STORAGE_QUERY_HANDLE *) handle;
}

/* Returns the metric and sets its timestamp into current_time */
storage_number mongoeng_load_metric_next(struct rrddim_query_handle *rrdimm_handle, time_t *current_time)
{
    struct mongoeng_query_handle *handle;
    struct mongoeng_dim_data *data;
    struct mongoeng_value *current_value;
    struct mongoeng_region *region = NULL;

    handle = (struct mongoeng_query_handle *)rrdimm_handle->handle;
    data = handle->data;

    while(data->region_count > handle->region_index) {
        region = &data->regions[handle->region_index];
        if(region->value_count > handle->value_index) {
            current_value = &region->values[handle->value_index];
            *current_time = current_value->t / USEC_PER_SEC;
            ++handle->value_index;
            return current_value->v;
        }
        else {
            ++handle->region_index;
            handle->value_index = 0;
        }
    }

    return SN_EMPTY_SLOT;
}

int mongoeng_load_metric_is_finished(struct rrddim_query_handle *rrdimm_handle)
{
    struct mongoeng_query_handle *handle;
    handle = (struct mongoeng_query_handle *)rrdimm_handle->handle;

    return handle->region_index >= handle->data->region_count && 
           handle->value_index >= handle->data->regions[handle->region_index].value_count;
}

/*
 * Releases the database reference from the handle for loading metrics.
 */
void mongoeng_load_metric_finalize(struct rrddim_query_handle *rrdimm_handle)
{
    struct mongoeng_query_handle *handle;
    struct mongoeng_dim_data *data;
    unsigned i;
    
    handle = (struct mongoeng_query_handle *)rrdimm_handle->handle;
    data = handle->data;

    freez(handle);
    rrdimm_handle->handle = NULL;

    for(i = 0; i < data->region_count; i++) {
        freez(data->regions[i].values);
    }
    freez(data->regions);
    data->region_count = 0;
    data->regions = NULL;

    uv_mutex_unlock(&data->regions_lock);
}

time_t mongoeng_metric_latest_time(RRDDIM *rd)
{
    return rd->state->mongoeng_data->latest_time / USEC_PER_SEC;
}

time_t mongoeng_metric_oldest_time(RRDDIM *rd)
{
    return rd->state->mongoeng_data->oldest_time / USEC_PER_SEC;
}

/*
 * Returns 0 on success, negative on error
 */
STORAGE_ENGINE_INSTANCE* mongoeng_init(STORAGE_ENGINE* eng, RRDHOST *host)
{
    struct mongoengine_instance *ctx;
    mongoc_uri_t *uri;
    bson_error_t error;
    int32_t socket_timeout;
    bson_t *cmd, reply;
    bool r;

    ctx = &mongodb_ctx;
    ctx = callocz(1, sizeof(*ctx));
    ctx->parent.engine = eng;

    if (NULL == host)
        strncpyz(ctx->parent.machine_guid, registry_get_this_machine_guid(), GUID_LEN);
    else
        strncpyz(ctx->parent.machine_guid, host->machine_guid, GUID_LEN);

    ctx->quiesce = NO_QUIESCE;
    ctx->host = host;

    memset(&ctx->worker_config, 0, sizeof(ctx->worker_config));
    ctx->worker_config.ctx = ctx;

    mongoc_init();
    uri = mongoc_uri_new_with_error(mongoengine_uri, &error);
    if(unlikely(!uri)) {
        error("MongoDB mongoeng_init failed to parse URI: %s. Error message: %s", mongoengine_uri, error.message);
        goto error_after_ctx;
    }

    socket_timeout = mongoc_uri_get_option_as_int32(uri, MONGOC_URI_SOCKETTIMEOUTMS, mongoengine_timeout);
    if(!mongoc_uri_set_option_as_int32(uri, MONGOC_URI_SOCKETTIMEOUTMS, socket_timeout)) {
        error("MongoDB mongoeng_init failed to set %s to the value %d", MONGOC_URI_SOCKETTIMEOUTMS, socket_timeout);
        mongoc_uri_destroy(uri);
        goto error_after_ctx;
    };

    ctx->mongo_client = mongoc_client_new_from_uri(uri);
    mongoc_uri_destroy(uri);
    if(unlikely(!ctx->mongo_client)) {
        error("MongoDB mongoeng_init failed to create a new client");
        goto error_after_ctx;
    }

    if(!mongoc_client_set_appname(ctx->mongo_client, "netdata")) {
        error("MongoDB mongoeng_init failed to set client appname");
    }

    ctx->database = mongoc_client_get_database (ctx->mongo_client, mongoengine_database);
    if(unlikely(!ctx->database)) {
        error("MongoDB mongoeng_init failed to create database");
        goto error_after_ctx;
    }

    uv_mutex_init(&ctx->client_lock);
    uv_mutex_init(&ctx->collection_create_lock);

    cmd = BCON_NEW("ping", BCON_INT32(1));    
    uv_mutex_lock(&ctx->client_lock);
    r = mongoc_client_read_command_with_opts(ctx->mongo_client, mongoengine_database, cmd, NULL, NULL, &reply, &error);
    uv_mutex_unlock(&ctx->client_lock);
    bson_destroy(cmd);
    bson_destroy(&reply);
    if(!r) {
        error("MongoDB mongoeng_init failed to find server %s", error.message);
        goto error_after_mutex;
    }

    // prevent duplicate data in the mongo db
    // TODO per streaming host mongo db uri checks
/*     if(default_rrdpush_enabled) {
        error("MongoDB mongoeng_init cannot use mongoeng with streaming enabled");
        goto error_after_mutex;
    } */

    completion_init(&ctx->mongoeng_completion);
    fatal_assert(0 == uv_thread_create(&ctx->worker_config.thread, mongoeng_worker, &ctx->worker_config));
    /* wait for worker thread to initialize */
    completion_wait_for(&ctx->mongoeng_completion);
    completion_destroy(&ctx->mongoeng_completion);
    uv_thread_set_name_np(ctx->worker_config.thread, "MONGOENGINE");
    if (ctx->worker_config.error) {
        error("MongoDB mongoeng_init failed to find start worker");
        goto error_after_mutex;
    }

    return (STORAGE_ENGINE_INSTANCE*)ctx;
error_after_mutex:
    uv_mutex_destroy(&ctx->client_lock);
    uv_mutex_destroy(&ctx->collection_create_lock);
error_after_ctx:
    freez(ctx);
    return NULL;
}

/*
 * Returns 0 on success, 1 on error
 */
int mongoeng_exit(struct mongoengine_instance *ctx)
{
    struct mongoeng_cmd cmd;

    if (NULL == ctx) {
        return 1;
    }

    cmd.opcode = MONGOENGINE_SHUTDOWN;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd);

    fatal_assert(0 == uv_thread_join(&ctx->worker_config.thread));

    mongoc_database_destroy(ctx->database);
    mongoc_client_destroy(ctx->mongo_client);

    uv_mutex_destroy(&ctx->client_lock);
    uv_mutex_destroy(&ctx->collection_create_lock);

    if (ctx != &mongodb_ctx) {
        freez(ctx);
    }

    return 0;
}

void mongoeng_prepare_exit(struct mongoengine_instance *ctx)
{
    struct mongoeng_cmd cmd;

    if (NULL == ctx) {
        return;
    }

    completion_init(&ctx->mongoeng_completion);
    cmd.opcode = MONGOENGINE_QUIESCE;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd);

    /* wait for dbengine to quiesce */
    completion_wait_for(&ctx->mongoeng_completion);
    completion_destroy(&ctx->mongoeng_completion);
}

/* Calls the chart read-lock */
RRDR* mongoeng_query(
    RRDSET *st
    , long points_requested
    , long long after_requested
    , long long before_requested
    , RRDR_GROUPING group_method
    , long resampling_time_requested
    , RRDR_OPTIONS options
    , const char *dimensions
    , int update_every
    , time_t first_entry_t
    , time_t last_entry_t
    , int absolute_period_requested
    , struct context_param *context_param_list)
{
    struct rrdr_region_info *region_info_array = NULL;
    unsigned regions, max_interval = 0;
    unsigned i;
    RRDR *ret;
    RRDDIM *rd;
    struct mongoeng_dim_data *data;
    struct mongoengine_instance *ctx;
    struct mongoeng_cmd cmd;
    struct completion compl;
    struct set_query set_query;

    ctx = get_mongoeng_ctx_from_host(st->rrdhost);
    if (unlikely(!ctx)) {
        error("MongoDB mongoeng_query failed to fetch context");
        return NULL;
    }

    set_query.st = st;
    set_query.start = after_requested;
    set_query.end = before_requested;

    completion_init(&compl);

    cmd.opcode = MONGOENGINE_QUERY_SET;
    cmd.completion = &compl;
    cmd.set_query = &set_query;

    rrdset_rdlock(st);

    mongoeng_enq_cmd(&ctx->worker_config, &cmd);
    completion_wait_for(&compl);
    completion_destroy(&compl);

    regions = mongoeng_variable_step_boundaries(st, after_requested, before_requested,
                                                &region_info_array, &max_interval, context_param_list);
    if (1 == regions) {
        if (region_info_array) {
            if (update_every != region_info_array[0].update_every) {
                update_every = region_info_array[0].update_every;
                /* recalculate query alignment */
                absolute_period_requested =
                        rrdr_convert_before_after_to_absolute(&after_requested, &before_requested, update_every,
                                                                first_entry_t, last_entry_t, options);
            }
            freez(region_info_array);
        }
        ret = rrd2rrdr_fixedstep(st, points_requested, after_requested, before_requested, group_method,
                                    resampling_time_requested, options, dimensions, update_every,
                                    first_entry_t, last_entry_t, absolute_period_requested, context_param_list);
    } else {
        if (update_every != (uint16_t)max_interval) {
            update_every = (uint16_t) max_interval;
            /* recalculate query alignment */
            absolute_period_requested = rrdr_convert_before_after_to_absolute(&after_requested, &before_requested,
                                                                                update_every, first_entry_t,
                                                                                last_entry_t, options);
        }
        ret = rrd2rrdr_variablestep(st, points_requested, after_requested, before_requested, group_method,
                                        resampling_time_requested, options, dimensions, update_every,
                                        first_entry_t, last_entry_t, absolute_period_requested, region_info_array, context_param_list);
    }

    rrddim_foreach_read(rd, st) {
        data = rd->state->mongoeng_data;
        if(data->regions) {
            for(i = 0; i < data->region_count; i++) {                
                freez(data->regions[i].values);
            }
        }
        freez(data->regions);
        data->region_count = 0;
        data->regions = NULL;
    }

    rrdset_unlock(st);
    return ret;
}


