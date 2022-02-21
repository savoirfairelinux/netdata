// SPDX-License-Identifier: GPL-3.0-or-later
#include "dbengine.h"

#include <mongoc.h>

/* Default global database instance */
struct mongoengine_instance mongodb_ctx;
extern int global_backend_update_every;


static inline struct mongoengine_instance *get_mongoeng_ctx_from_host(RRDHOST *host)
{
    return host->mongoeng_ctx;
}

void mongoeng_metric_init(RRDDIM *rd)
{
    info("MongoDB mongoeng_metric_init %s %s", rd->id, rd->name);
    //struct mongoengine_instance *ctx;
    //ctx = get_mongoeng_ctx_from_host(rd->rrdset->rrdhost);

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
    //info("MongoDB mongoeng_store_metric_init %s %s / %s %s", rd->rrdset->id, rd->rrdset->name, rd->id, rd->name);
    struct mongoeng_collect_handle *handle;
    struct mongoengine_instance *ctx;
    struct pg_cache_page_index *page_index;

    ctx = get_mongoeng_ctx_from_host(rd->rrdset->rrdhost);
    handle = &rd->state->handle.mongoeng;
    handle->ctx = ctx;

    handle->descr = NULL;
    handle->prev_descr = NULL;
    handle->unaligned_page = 0;

    //handle->name

    /*char collection_name[1024];
    snprintf(collection_name, sizeof(collection_name), "%s-%s", rd->rrdset->id, rd->id);
    replace_char(collection_name, '.', '-');

    bson_error_t error;
    const bson_t* MONGOC_TIMESERIES_PARAM = BCON_NEW ("timeseries", "{", "timeField", "t", "}");

    uv_mutex_lock(&ctx->lock);
    handle->collection = mongoc_database_create_collection(ctx->database, collection_name, MONGOC_TIMESERIES_PARAM, &error);
    if (!handle->collection) {
        handle->collection = mongoc_database_get_collection(ctx->database, collection_name);
        if (!handle->collection) {
            info("MongoDB mongoeng_store_metric_init ERROR %s", error.message);
        }
        //return EXIT_FAILURE;
    }
    uv_mutex_unlock(&ctx->lock);*/
    /*page_index = rd->state->page_index;
    uv_rwlock_wrlock(&page_index->lock);
    ++page_index->writers;
    uv_rwlock_wrunlock(&page_index->lock);*/
}

void mongoeng_store_metric_next(RRDDIM *rd, usec_t point_in_time, storage_number number)
{
    //info("MongoDB mongoeng_store_metric_next %s %s %llu", rd->id, rd->name, point_in_time);
    struct mongoeng_collect_handle *handle;
    struct mongoengine_instance *ctx;
    storage_number *page;
    handle = &rd->state->handle.mongoeng;
    ctx = handle->ctx;

    bson_t *doc = bson_new ();
    //bson_oid_t oid;
    //bson_oid_init (&oid, NULL);
    BSON_APPEND_DATE_TIME (doc, "t", point_in_time/1000);
    BSON_APPEND_INT32 (doc, "v", number);
    BSON_APPEND_UTF8 (doc, "d", rd->rrdset->id);
    BSON_APPEND_UTF8 (doc, "m", rd->id);

    uv_mutex_lock(&ctx->lock);

    if (unlikely(!ctx->op)) {
        info("MongoDB mongoeng_store_metric_next ERROR no op");
        uv_mutex_unlock(&ctx->lock);
        bson_destroy (doc);
        return;
    }

    mongoc_bulk_operation_insert (ctx->op, doc);

    /*bson_error_t error;
    if (!mongoc_collection_insert_one (
           handle->collection, doc, NULL, NULL, &error)) {
        info("MongoDB mongoeng_store_metric_next ERROR %s", error.message);
    }*/
    uv_mutex_unlock(&ctx->lock);

    bson_destroy (doc);

}

/*
 * Releases the database reference from the handle for storing metrics.
 * Returns 1 if it's safe to delete the dimension.
 */
int mongoeng_store_metric_finalize(RRDDIM *rd)
{
    //error("MongoDB mongoeng_store_metric_finalize %s %s", rd->id, rd->name);
    struct mongoeng_collect_handle *handle;
    struct mongoengine_instance *ctx;

    handle = &rd->state->handle.mongoeng;
    ctx = handle->ctx;

    if (handle->collection)
        mongoc_collection_destroy(handle->collection);

    return 0;
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
                                         struct mongoeng_region_info **region_info_arrayp, unsigned *max_intervalp, struct context_param *context_param_list)
{

}

/*
 * Gets a handle for loading metrics from the database.
 * The handle must be released with mongoeng_load_metric_final().
 */
void mongoeng_load_metric_init(RRDDIM *rd, struct rrddim_query_handle *rrdimm_handle, time_t start_time, time_t end_time)
{
    error("MongoDB mongoeng_load_metric_init %s %s %ld %ld", rd->id, rd->name, start_time, end_time);
    struct mongoeng_query_handle *handle;
    struct mongoengine_instance *ctx;
    unsigned pages_nr;

    ctx = get_mongoeng_ctx_from_host(rd->rrdset->rrdhost);
    rrdimm_handle->rd = rd;
    rrdimm_handle->start_time = start_time;
    rrdimm_handle->end_time = end_time;
    handle = &rrdimm_handle->mongoeng;
    handle->next_page_time = start_time;
    handle->now = start_time;
    handle->position = 0;
    handle->ctx = ctx;
}

/* Returns the metric and sets its timestamp into current_time */
storage_number mongoeng_load_metric_next(struct rrddim_query_handle *rrdimm_handle, time_t *current_time)
{
    error("MongoDB mongoeng_load_metric_next %s %s %ld %ld", rrdimm_handle->rd->id, rrdimm_handle->rd->name, rrdimm_handle->start_time, rrdimm_handle->end_time);
    struct mongoeng_query_handle *handle;
    struct mongoengine_instance *ctx;
    struct mongoeng_page_descr *descr;
    storage_number *page, ret;
    unsigned position, entries;
    usec_t next_page_time = 0, current_position_time, page_end_time = 0;
    uint32_t page_length;

    /*handle = &rrdimm_handle->mongoeng;
    if (unlikely(INVALID_TIME == handle->next_page_time)) {
        return SN_EMPTY_SLOT;
    }
    ctx = handle->ctx;

    handle->next_page_time = INVALID_TIME;*/
    return SN_EMPTY_SLOT;
}

int mongoeng_load_metric_is_finished(struct rrddim_query_handle *rrdimm_handle)
{
    error("MongoDB mongoeng_load_metric_is_finished %s %s %ld %ld", rrdimm_handle->rd->id, rrdimm_handle->rd->name, rrdimm_handle->start_time, rrdimm_handle->end_time);
    struct mongoeng_query_handle *handle;

    /*handle = &rrdimm_handle->mongoeng;
    return (INVALID_TIME == handle->next_page_time);*/
    return 1;
}

/*
 * Releases the database reference from the handle for loading metrics.
 */
void mongoeng_load_metric_finalize(struct rrddim_query_handle *rrdimm_handle)
{
    error("MongoDB mongoeng_load_metric_finalize %s %s %ld %ld", rrdimm_handle->rd->id, rrdimm_handle->rd->name, rrdimm_handle->start_time, rrdimm_handle->end_time);
    struct mongoeng_query_handle *handle;
    struct mongoengine_instance *ctx;
    struct mongoeng_page_descr *descr;

    handle = &rrdimm_handle->mongoeng;
    ctx = handle->ctx;
}

time_t mongoeng_metric_latest_time(RRDDIM *rd)
{
    info("MongoDB mongoeng_metric_latest_time.");
    /*struct pg_cache_page_index *page_index;

    page_index = rd->state->page_index;

    return page_index->latest_time / USEC_PER_SEC;*/
    return 0;
}
time_t mongoeng_metric_oldest_time(RRDDIM *rd)
{
    info("MongoDB mongoeng_metric_oldest_time.");
    /*struct pg_cache_page_index *page_index;

    page_index = rd->state->page_index;

    return page_index->oldest_time / USEC_PER_SEC;*/
    return 0;
}

int mongoeng_metric_latest_time_by_uuid(uuid_t *dim_uuid, time_t *first_entry_t, time_t *last_entry_t)
{
    info("MongoDB mongoeng_metric_latest_time_by_uuid.");
    struct mongoengine_instance *ctx;

    ctx = get_mongoeng_ctx_from_host(localhost);
    if (unlikely(!ctx)) {
        error("Failed to fetch multidb context");
        return 1;
    }

    return 1;
}

/*
 * Returns 0 on success, negative on error
 */
int mongoeng_init(RRDHOST *host, struct mongoengine_instance **ctxp, char *dbfiles_path, unsigned page_cache_mb,
                unsigned disk_space_mb)
{
    info("MongoDB mongoeng_init.");
    struct mongoengine_instance *ctx;
    //int error = 0;
    int32_t mongodb_default_socket_timeout = (int32_t)(global_backend_update_every >= 2)?(global_backend_update_every * MSEC_PER_SEC - 500):1000;

    if (NULL == ctxp) {
        ctx = &mongodb_ctx;
        memset(ctx, 0, sizeof(*ctx));
    } else {
        *ctxp = ctx = callocz(1, sizeof(*ctx));
    }
    if (NULL == host)
        strncpyz(ctx->machine_guid, registry_get_this_machine_guid(), GUID_LEN);
    else
        strncpyz(ctx->machine_guid, host->machine_guid, GUID_LEN);

    ctx->quiesce = NO_QUIESCE;
    ctx->metalog_ctx = NULL; /* only set this after the metadata log has finished initializing */
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

    const char *uri_string = "mongodb://localhost:27017";
    uri = mongoc_uri_new_with_error(uri_string, &error);
    if(unlikely(!uri)) {
        error("BACKEND: failed to parse URI: %s. Error message: %s", uri_string, error.message);
        return 1;
    }

    int32_t socket_timeout = mongoc_uri_get_option_as_int32(uri, MONGOC_URI_SOCKETTIMEOUTMS, mongodb_default_socket_timeout);
    if(!mongoc_uri_set_option_as_int32(uri, MONGOC_URI_SOCKETTIMEOUTMS, socket_timeout)) {
        error("BACKEND: failed to set %s to the value %d", MONGOC_URI_SOCKETTIMEOUTMS, socket_timeout);
        return 1;
    };

    uv_mutex_init(&ctx->lock);

    ctx->mongo_client = mongoc_client_new_from_uri(uri);
    if(unlikely(!ctx->mongo_client)) {
        error("BACKEND: failed to create a new client");
        return 1;
    }

    if(!mongoc_client_set_appname(ctx->mongo_client, "netdata")) {
        error("BACKEND: failed to set client appname");
    }

    ctx->database = mongoc_client_get_database (ctx->mongo_client, "netdata");
    if(unlikely(!ctx->database)) {
        error("BACKEND: failed to create database");
        return 1;
    }

    const bson_t* MONGOC_TIMESERIES_PARAM = BCON_NEW ("timeseries", "{", "timeField", "t", "}");
    const char* COLLECTION_NAME = "metrics";
    ctx->collection = mongoc_database_create_collection(ctx->database, COLLECTION_NAME, MONGOC_TIMESERIES_PARAM, &error);
    if (!ctx->collection) {
        ctx->collection = mongoc_database_get_collection(ctx->database, COLLECTION_NAME);
        if (!ctx->collection) {
            info("MongoDB mongoeng_store_metric_init ERROR %s", error.message);
        }
        //return EXIT_FAILURE;
    }
    bson_destroy(MONGOC_TIMESERIES_PARAM);

    ctx->op = mongoc_collection_create_bulk_operation_with_opts (ctx->collection, NULL);

    //mongoc_collection_t* mongodb_collection = mongoc_client_get_collection(mongodb_client, database_string, collection_string);

    mongoc_uri_destroy(uri);

    completion_init(&ctx->mongoeng_completion);
    fatal_assert(0 == uv_thread_create(&ctx->worker_config.thread, mongoeng_worker, &ctx->worker_config));
    /* wait for worker thread to initialize */
    completion_wait_for(&ctx->mongoeng_completion);
    completion_destroy(&ctx->mongoeng_completion);
    uv_thread_set_name_np(ctx->worker_config.thread, "MONGOENGINE");
    if (ctx->worker_config.error) {
        goto error_after_mongoeng_worker;
    }
    /*error = metalog_init(NULL, ctx);
    if (error) {
        error("Failed to initialize metadata log file event loop.");
        goto error_after_mongoeng_worker;
    }*/

    return 0;

error_after_mongoeng_worker:
    //finalize_rrd_files(ctx);
error_after_init_rrd_files:
    //free_page_cache(ctx);
    if (ctx != &mongodb_ctx) {
        freez(ctx);
        *ctxp = NULL;
    }
    //rrd_stat_atomic_add(&mongoeng_reserved_file_descriptors, -mongoeng_FD_BUDGET_PER_INSTANCE);
    return UV_EIO;
}

/*
 * Returns 0 on success, 1 on error
 */
int mongoeng_exit(struct mongoengine_instance *ctx)
{
    info("MongoDB mongoeng_exit.");
    struct mongoeng_cmd cmd;

    if (NULL == ctx) {
        return 1;
    }

    /* TODO: add page to page cache */
    cmd.opcode = MONGOENGINE_SHUTDOWN;
    mongoeng_enq_cmd(&ctx->worker_config, &cmd);

    fatal_assert(0 == uv_thread_join(&ctx->worker_config.thread));

    mongoc_bulk_operation_destroy(ctx->op);
    mongoc_collection_destroy(ctx->collection);
    mongoc_database_destroy(ctx->database);
    mongoc_client_destroy(ctx->mongo_client);

    //metalog_exit(ctx->metalog_ctx);

    if (ctx != &mongodb_ctx) {
        freez(ctx);
    }
    //rrd_stat_atomic_add(&mongoeng_reserved_file_descriptors, -mongoeng_FD_BUDGET_PER_INSTANCE);
    return 0;
}

void mongoeng_prepare_exit(struct mongoengine_instance *ctx)
{
    info("MongoDB mongoeng_prepare_exit.");
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

    //metalog_prepare_exit(ctx->metalog_ctx);
}

