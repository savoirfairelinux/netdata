// SPDX-License-Identifier: GPL-3.0-or-later

#include "dbengineapi.h"
#include <mongoc.h>

uint32_t TIMESERIES_ALREADY_EXISTS_ERROR = 48;

/* Default global database instance */
struct mongoengine_instance mongodb_ctx;

char *mongoengine_uri = "";
char *mongoengine_database = "netdata";
int mongoengine_timeout = 1000;
int mongoengine_expiration = 3600;

static inline struct mongoengine_instance *get_mongoeng_ctx_from_host(RRDHOST *host)
{
    return (struct mongoengine_instance *)host->rrdeng_ctx;
}

bool mongoeng_create_host_collection(struct mongoengine_instance *ctx, RRDHOST *host)
{
    bson_error_t error;
    bson_t *opts = bson_new();
    bson_t *timeseries = bson_new();

    BSON_APPEND_UTF8(timeseries, "timeField", "t");
    BSON_APPEND_UTF8(timeseries, "metaField", "m");
    BSON_APPEND_UTF8(timeseries, "granularity", "seconds");
    BSON_APPEND_DOCUMENT(opts, "timeseries", timeseries);
    if(mongoengine_expiration)
        BSON_APPEND_INT32(opts, "expireAfterSeconds", mongoengine_expiration);

    uv_mutex_lock(&ctx->client_lock);
    host->collection = mongoc_database_create_collection(ctx->database, &host->machine_guid[0], opts, &error);
    uv_mutex_unlock(&ctx->client_lock);

    bson_destroy(opts);
    bson_destroy(timeseries);

    if (!host->collection) {
        if(error.code == TIMESERIES_ALREADY_EXISTS_ERROR)
            host->collection = mongoc_database_get_collection(ctx->database, &host->machine_guid[0]);
        else {
            error("MongoDB mongoeng_create_host_collection couldn't create collection");
            return false;
        }
    }

    host->op = mongoc_collection_create_bulk_operation_with_opts (host->collection, NULL);

    char *index_name;
    bson_t keys;
    bson_t *create_indexes;
    bson_init (&keys);
    BSON_APPEND_INT32 (&keys, "m.s", 1);
    BSON_APPEND_INT32 (&keys, "m.d", 1);
    index_name = mongoc_collection_keys_to_index_string (&keys);
    create_indexes = BCON_NEW ("createIndexes", BCON_UTF8 (&host->machine_guid[0]), "indexes", "[", "{",
                                                                                  "key", BCON_DOCUMENT(&keys),
                                                                                  "name", BCON_UTF8(index_name),
                                                                               "}", "]");

    uv_mutex_lock(&ctx->client_lock);
    bool r = mongoc_database_write_command_with_opts(ctx->database, create_indexes, NULL, NULL, &error);
    uv_mutex_unlock(&ctx->client_lock);

    bson_free (index_name);
    bson_destroy (create_indexes);

    if(!r) {
        error("MongoDB mongoeng_create_host_collection ERROR %s", error.message);
    }

    return r;
}

RRDDIM* mongoeng_metric_init(RRDDIM *rd)
{
    //info("MongoDB mongoeng_metric_init %s", id);

    struct mongoeng_collect_handle *handle;
    struct mongoengine_instance *ctx;

    ctx = get_mongoeng_ctx_from_host(rd->rrdset->rrdhost);
    if (unlikely(!ctx)) {
        error("MongoDB mongoeng_metric_init failed to fetch context");
        return NULL;
    }

    // move collection creation into host creation ? or single collection move to mongo init
    if(!rd->rrdset->rrdhost->collection)
        if(!mongoeng_create_host_collection(ctx, rd->rrdset->rrdhost))
            return NULL;

    handle = callocz(1, sizeof(struct mongoeng_collect_handle));
    rd->state->handle = (STORAGE_COLLECT_HANDLE *) handle;
    handle->ctx = ctx;

    struct mongoeng_cmd cmd_latest;
    struct completion compl_latest;
    completion_init(&compl_latest);
    cmd_latest.opcode = MONGOENGINE_QUERY_TIME_LATEST;
    cmd_latest.rd = rd;
    cmd_latest.completion = &compl_latest;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd_latest);

    struct mongoeng_cmd cmd_oldest;
    struct completion compl_oldest;
    completion_init(&compl_oldest);
    cmd_oldest.opcode = MONGOENGINE_QUERY_TIME_OLDEST;
    cmd_oldest.rd = rd;
    cmd_oldest.completion = &compl_oldest;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd_oldest);

    completion_wait_for(&compl_latest); 
    completion_wait_for(&compl_oldest);

    completion_destroy(&compl_latest);
    completion_destroy(&compl_oldest);

    freez(handle);
    rd->state->handle = NULL;
    return rd;
}

void replace_char(char* str, char find, char replace){
    while ((str = strchr(str, find)))
        *str++ = replace;
}

/*
 * Gets a handle for storing metrics to the database.
 * The handle must be released with mongoeng_store_metric_final().
 */
void mongoeng_store_metric_init(RRDDIM *rd)
{
    //info("MongoDB mongoeng_store_metric_init %s %s", rd->rrdset->id, rd->id);
    struct mongoeng_collect_handle *handle;
    struct mongoengine_instance *ctx;

    ctx = get_mongoeng_ctx_from_host(rd->rrdset->rrdhost);

    handle = callocz(1, sizeof(struct mongoeng_collect_handle));
    rd->state->handle = (STORAGE_COLLECT_HANDLE *) handle;
    handle->ctx = ctx;

    // handle->descr = NULL;
    // handle->prev_descr = NULL;
    // handle->unaligned_page = 0;
}

void mongoeng_store_metric_next(RRDDIM *rd, usec_t point_in_time, storage_number number)
{
    //info("MongoDB mongoeng_store_metric_next %s %llu", rd->id, point_in_time);
    struct mongoeng_collect_handle *handle;
    struct mongoengine_instance *ctx;
    handle = (struct mongoeng_collect_handle *)rd->state->handle;
    ctx = handle->ctx;

    bson_t *doc = bson_new ();
    bson_t *meta = bson_new ();
    BSON_APPEND_INT32 (meta, "v", number);
    BSON_APPEND_UTF8 (meta, "s", rd->rrdset->id);
    BSON_APPEND_UTF8 (meta, "d", rd->id);
    BSON_APPEND_DATE_TIME (doc, "t", point_in_time/USEC_PER_MS);
    BSON_APPEND_DOCUMENT(doc, "m", meta);

    uv_mutex_lock(&ctx->bulk_write_lock);

    if (unlikely(!rd->rrdset->rrdhost->op)) {
        error("MongoDB mongoeng_store_metric_next ERROR no op");
        uv_mutex_unlock(&ctx->bulk_write_lock);
        bson_destroy (doc);
        bson_destroy (meta);
        return;
    }

    mongoc_bulk_operation_insert (rd->rrdset->rrdhost->op, doc);
    rd->state->latest_time = point_in_time;

    uv_mutex_unlock(&ctx->bulk_write_lock);

    bson_destroy (doc);
    bson_destroy (meta);
}

/*
 * Releases the database reference from the handle for storing metrics.
 * Returns 1 if it's safe to delete the dimension.
 */
int mongoeng_store_metric_finalize(RRDDIM *rd)
{
    //info("MongoDB mongoeng_store_metric_finalize %s", rd->id);
    struct mongoeng_collect_handle *handle;

    handle = (struct mongoeng_collect_handle *)rd->state->handle;
    freez(handle);
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
    //info("MongoDB mongoeng_variable_step_boundaries %s %ld %ld", st->id, start_time, end_time);
    UNUSED(st);
    UNUSED(start_time);
    UNUSED(end_time);
    UNUSED(region_info_arrayp);
    UNUSED(max_intervalp);
    UNUSED(context_param_list);
    return 1;
}

/*
 * Gets a handle for loading metrics from the database.
 * The handle must be released with mongoeng_load_metric_final().
 */
void mongoeng_load_metric_init(RRDDIM *rd, struct rrddim_query_handle *rrdimm_handle, time_t start_time, time_t end_time)
{
    //info("MongoDB mongoeng_load_metric_init %s %ld %ld", rd->id, start_time, end_time);
    struct mongoeng_query_handle *handle;
    struct mongoengine_instance *ctx;

    ctx = get_mongoeng_ctx_from_host(rd->rrdset->rrdhost);
    rrdimm_handle->rd = rd;
    rrdimm_handle->start_time = start_time;
    rrdimm_handle->end_time = end_time;

    handle = callocz(1, sizeof(struct mongoeng_query_handle));
    handle->next_page_time = start_time;
    handle->now = start_time;
    handle->position = 0;
    handle->ctx = ctx;
    handle->count = 0;
    rrdimm_handle->handle = (STORAGE_QUERY_HANDLE *) handle;

    struct mongoeng_cmd cmd;
    struct completion compl;
    completion_init(&compl);
    cmd.opcode = MONGOENGINE_QUERY_METRIC;
    cmd.handle = rrdimm_handle;
    cmd.completion = &compl;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd);
    completion_wait_for(&compl);
    completion_destroy(&compl);
}

/* Returns the metric and sets its timestamp into current_time */
storage_number mongoeng_load_metric_next(struct rrddim_query_handle *rrdimm_handle, time_t *current_time)
{
    //info("MongoDB mongoeng_load_metric_next %s %ld %ld %ld", rrdimm_handle->rd->id, rrdimm_handle->start_time, rrdimm_handle->end_time, *current_time);
    struct mongoeng_query_handle *handle;
    storage_number ret = SN_EMPTY_SLOT;

    handle = (struct mongoeng_query_handle *)rrdimm_handle->handle;

    if(handle->count) {
        handle->count--;
        struct mongoeng_query_data *current = handle->data;
        ret = current->value;
        *current_time = current->time / USEC_PER_SEC;
        handle->data = current->next;
        freez(current);
    }

    return ret;
}

int mongoeng_load_metric_is_finished(struct rrddim_query_handle *rrdimm_handle)
{
    //info("MongoDB mongoeng_load_metric_is_finished %s %ld %ld", rrdimm_handle->rd->id, rrdimm_handle->start_time, rrdimm_handle->end_time);
    struct mongoeng_query_handle *handle;
    handle = (struct mongoeng_query_handle *)rrdimm_handle->handle;
    return !handle->count;
}

/*
 * Releases the database reference from the handle for loading metrics.
 */
void mongoeng_load_metric_finalize(struct rrddim_query_handle *rrdimm_handle)
{
    //info("MongoDB mongoeng_load_metric_finalize %s %ld %ld", rrdimm_handle->rd->id, rrdimm_handle->start_time, rrdimm_handle->end_time);

    struct mongoeng_query_handle *handle;

    handle = (struct mongoeng_query_handle *)rrdimm_handle->handle;
    freez(handle);
    rrdimm_handle->handle = NULL;
}

time_t mongoeng_metric_latest_time(RRDDIM *rd)
{
    //info("MongoDB mongoeng_metric_latest_time %s %llu", rd->id, rd->state->latest_time);
    time_t time = rd->state->latest_time / USEC_PER_SEC;
    return time;
}

time_t mongoeng_metric_oldest_time(RRDDIM *rd)
{
    //info("MongoDB mongoeng_metric_oldest_time %s %llu", rd->id, rd->state->oldest_time);
    time_t time = rd->state->oldest_time / USEC_PER_SEC;
    return time;
}

int mongoeng_metric_latest_time_by_uuid(uuid_t *dim_uuid, time_t *first_entry_t, time_t *last_entry_t)
{
    //info("MongoDB mongoeng_metric_latest_time_by_uuid %.*s %ld %ld", 16, dim_uuid[0], *first_entry_t, *last_entry_t);
    UNUSED(dim_uuid);
    UNUSED(first_entry_t);
    UNUSED(last_entry_t);
    struct mongoengine_instance *ctx;

    ctx = get_mongoeng_ctx_from_host(localhost);
    if (unlikely(!ctx)) {
        error("MongoDB mongoeng_metric_latest_time_by_uuid failed to fetch context");
        return 1;
    }

    return 1;
}

/*
 * Returns 0 on success, negative on error
 */
STORAGE_ENGINE_INSTANCE* mongoeng_init(RRDHOST *host)
//, char *dbfiles_path, unsigned page_cache_mb, unsigned disk_space_mb)
{
    //info("MongoDB mongoeng_init");
    struct mongoengine_instance *ctx;
    //int error = 0;

    ctx = &mongodb_ctx;
    memset(ctx, 0, sizeof(*ctx));

    if (NULL == host)
        strncpyz(ctx->parent.machine_guid, registry_get_this_machine_guid(), GUID_LEN);
    else
        strncpyz(ctx->parent.machine_guid, host->machine_guid, GUID_LEN);

    ctx->quiesce = NO_QUIESCE;
    ctx->parent.metalog_ctx = NULL; /* only set this after the metadata log has finished initializing */
    ctx->host = host;

    memset(&ctx->worker_config, 0, sizeof(ctx->worker_config));
    ctx->worker_config.ctx = ctx;
    //init_commit_log(ctx);
    //error = init_rrd_files(ctx);
    //if (error) {
    //    goto error_after_init_rrd_files;
    //}

    mongoc_uri_t *uri;
    bson_error_t error;

    mongoc_init();
    uri = mongoc_uri_new_with_error(mongoengine_uri, &error);
    if(unlikely(!uri)) {
        error("MongoDB mongoeng_init failed to parse URI: %s. Error message: %s", mongoengine_uri, error.message);
        return NULL;
    }

    int32_t socket_timeout = mongoc_uri_get_option_as_int32(uri, MONGOC_URI_SOCKETTIMEOUTMS, mongoengine_timeout);
    if(!mongoc_uri_set_option_as_int32(uri, MONGOC_URI_SOCKETTIMEOUTMS, socket_timeout)) {
        error("MongoDB mongoeng_init failed to set %s to the value %d", MONGOC_URI_SOCKETTIMEOUTMS, socket_timeout);
        mongoc_uri_destroy(uri);
        return NULL;
    };

    uv_mutex_init(&ctx->client_lock);
    uv_mutex_init(&ctx->bulk_write_lock);

    ctx->mongo_client = mongoc_client_new_from_uri(uri);
    mongoc_uri_destroy(uri);
    if(unlikely(!ctx->mongo_client)) {
        error("MongoDB mongoeng_init failed to create a new client");
        return NULL;
    }

    if(!mongoc_client_set_appname(ctx->mongo_client, "netdata")) {
        error("MongoDB mongoeng_init failed to set client appname");
    }

    ctx->database = mongoc_client_get_database (ctx->mongo_client, mongoengine_database);
    if(unlikely(!ctx->database)) {
        error("MongoDB mongoeng_init failed to create database");
        return NULL;
    }

    bson_t *cmd, reply;
    cmd = BCON_NEW("ping", BCON_INT32(1));    
    uv_mutex_lock(&ctx->client_lock);
    bool r = mongoc_client_read_command_with_opts(ctx->mongo_client, mongoengine_database, cmd, NULL, NULL, &reply, &error);
    uv_mutex_unlock(&ctx->client_lock);
    bson_destroy(cmd);
    bson_destroy(&reply);
    if(!r) {
        error("MongoDB mongoeng_init failed to find server %s", error.message);
        return NULL;
    }

    completion_init(&ctx->mongoeng_completion);
    fatal_assert(0 == uv_thread_create(&ctx->worker_config.thread, mongoeng_worker, &ctx->worker_config));
    /* wait for worker thread to initialize */
    completion_wait_for(&ctx->mongoeng_completion);
    completion_destroy(&ctx->mongoeng_completion);
    uv_thread_set_name_np(ctx->worker_config.thread, "MONGOENGINE");
    if (ctx->worker_config.error) {
        //goto error_after_mongoeng_worker;
    }
    /*error = metalog_init(NULL, ctx);
    if (error) {
        error("Failed to initialize metadata log file event loop.");
        goto error_after_mongoeng_worker;
    }*/

    return (STORAGE_ENGINE_INSTANCE*)ctx;

// error_after_mongoeng_worker:
//     finalize_rrd_files(ctx);
// error_after_init_rrd_files:
//     free_page_cache(ctx);
//     if (ctx != &mongodb_ctx) {
//         freez(ctx);
//         *ctxp = NULL;
//     }
//     rrd_stat_atomic_add(&mongoeng_reserved_file_descriptors, -mongoeng_FD_BUDGET_PER_INSTANCE);
//    return UV_EIO;
}

/*
 * Returns 0 on success, 1 on error
 */
int mongoeng_exit(struct mongoengine_instance *ctx)
{
    //info("MongoDB mongoeng_exit");
    struct mongoeng_cmd cmd;

    if (NULL == ctx) {
        return 1;
    }

    /* TODO: add page to page cache */
    cmd.opcode = MONGOENGINE_SHUTDOWN;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd);

    fatal_assert(0 == uv_thread_join(&ctx->worker_config.thread));

    //mongoc_bulk_operation_destroy(ctx->op);
    //mongoc_collection_destroy(ctx->collection);
    mongoc_database_destroy(ctx->database);
    mongoc_client_destroy(ctx->mongo_client);

    uv_mutex_destroy(&ctx->client_lock);
    uv_mutex_destroy(&ctx->bulk_write_lock);
    //metalog_exit(ctx->parent.metalog_ctx);

    if (ctx != &mongodb_ctx) {
        freez(ctx);
    }
    //rrd_stat_atomic_add(&mongoeng_reserved_file_descriptors, -mongoeng_FD_BUDGET_PER_INSTANCE);
    return 0;
}

void mongoeng_prepare_exit(struct mongoengine_instance *ctx)
{
    //info("MongoDB mongoeng_prepare_exit");
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

    //metalog_prepare_exit(ctx->parent.metalog_ctx);
}

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
        struct rrdr_region_info *region_info_array;
        unsigned regions, max_interval;

        /* This call takes the chart read-lock */
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
            return rrd2rrdr_fixedstep(st, points_requested, after_requested, before_requested, group_method,
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
            return rrd2rrdr_variablestep(st, points_requested, after_requested, before_requested, group_method,
                                         resampling_time_requested, options, dimensions, update_every,
                                         first_entry_t, last_entry_t, absolute_period_requested, region_info_array, context_param_list);
        }
}


