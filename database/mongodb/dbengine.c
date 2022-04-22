// SPDX-License-Identifier: GPL-3.0-or-later
#define NETDATA_MONGODB_INTERNALS

#include "dbengine.h"
#include <mongoc/mongoc.h>

void mongoeng_init_cmd_queue(struct mongoengine_worker_config* wc)
{
    wc->cmd_queue.head = wc->cmd_queue.tail = 0;
    wc->queue_size = 0;
    fatal_assert(0 == uv_cond_init(&wc->cmd_cond));
    fatal_assert(0 == uv_mutex_init(&wc->cmd_mutex));
}

void mongoeng_enq_cmd(struct mongoengine_worker_config* wc, struct mongoeng_cmd *cmd)
{
    unsigned queue_size;

    /* wait for free space in queue */
    uv_mutex_lock(&wc->cmd_mutex);
    while ((queue_size = wc->queue_size) == DBENG_CMD_Q_MAX_SIZE) {
        uv_cond_wait(&wc->cmd_cond, &wc->cmd_mutex);
    }
    fatal_assert(queue_size < DBENG_CMD_Q_MAX_SIZE);
    /* enqueue command */
    wc->cmd_queue.cmd_array[wc->cmd_queue.tail] = *cmd;
    wc->cmd_queue.tail = wc->cmd_queue.tail != DBENG_CMD_Q_MAX_SIZE - 1 ?
                         wc->cmd_queue.tail + 1 : 0;
    wc->queue_size = queue_size + 1;
    uv_mutex_unlock(&wc->cmd_mutex);

    /* wake up event loop */
    fatal_assert(0 == uv_async_send(&wc->async));
}

struct mongoeng_cmd mongoeng_deq_cmd(struct mongoengine_worker_config* wc)
{
    struct mongoeng_cmd ret;
    unsigned queue_size;

    uv_mutex_lock(&wc->cmd_mutex);
    queue_size = wc->queue_size;
    if (queue_size == 0) {
        ret.opcode = MONGOENGINE_NOOP;
    } else {
        /* dequeue command */
        ret = wc->cmd_queue.cmd_array[wc->cmd_queue.head];
        if (queue_size == 1) {
            wc->cmd_queue.head = wc->cmd_queue.tail = 0;
        } else {
            wc->cmd_queue.head = wc->cmd_queue.head != DBENG_CMD_Q_MAX_SIZE - 1 ?
                                 wc->cmd_queue.head + 1 : 0;
        }
        wc->queue_size = queue_size - 1;

        /* wake up producers */
        uv_cond_signal(&wc->cmd_cond);
    }
    uv_mutex_unlock(&wc->cmd_mutex);

    return ret;
}


void mongo_async_cb(uv_async_t *handle)
{
    uv_stop(handle->loop);
    uv_update_time(handle->loop);
}


/* Queues write command when timer expires */
#define TIMER_PERIOD_MS (1000)

void mongo_timer_cb(uv_timer_t* handle)
{
    struct mongoengine_worker_config* wc = handle->data;

    struct mongoeng_cmd write_cmd;
    write_cmd.opcode = MONGOENGINE_BULK_WRITE;
    mongoeng_enq_cmd(wc, &write_cmd);

    uv_stop(handle->loop);
    uv_update_time(handle->loop);
}

#define MAX_CMD_BATCH_SIZE (256)

void process_query_set(struct set_query *set_query) {
    bson_t *pipeline;
    mongoc_cursor_t *cursor;
    bson_error_t error;
    const bson_t *doc;
    bson_iter_t iter, array_iter, inner_iter;
    time_t time;
    storage_number value;
    char *dim_id;
    RRDSET *st;
    struct mongoeng_host_data *host_data;
    struct mongoeng_dim_data *dim_data;
    struct mongoeng_region *current_region;
    struct mongoeng_value *current_value;
    unsigned interval;

    RRDDIM *rd = NULL;
    unsigned region_memory_count = 256;
    unsigned value_memory_count = 32768;
    unsigned current_interval = 0; // also used to track if a region was found for a dimension

    st = set_query->st;
    long long start_time = set_query->start * MSEC_PER_SEC;
    long long end_time = set_query->end * MSEC_PER_SEC;

    host_data = st->rrdhost->mongoeng_data;
    if(!host_data) {
        error("MongoDB process_query_set failed to retrieve host data");
        return;
    }

    struct mongoengine_instance *ctx = (struct mongoengine_instance *) st->rrdhost->rrdeng_ctx;
    if (unlikely(!ctx)) {
        error("MongoDB process_query_set failed to fetch context");
        return;
    }

    pipeline = BCON_NEW("pipeline", "[", 
                          "{", "$match", "{", 
                            "m.s", st->id, 
                            "t", "{",
                              "$gte", BCON_DATE_TIME(start_time), 
                              "$lte", BCON_DATE_TIME(end_time), 
                            "}",
                          "}", "}", 
                          "{", "$sort", "{", "t", BCON_INT32(1), "}", "}",
                          "{", "$group", "{", 
                            "_id", "$m.d", "d", "{", "$push", "{", "r", "$m.r", "v", "$m.v", "t", "$t", "}", "}", 
                          "}", "}", 
                        "]");

    uv_mutex_lock(&ctx->client_lock);
    cursor = mongoc_collection_aggregate(host_data->collection, MONGOC_QUERY_NONE, pipeline, NULL, NULL);
    
    // iterate each queried dimension grouping
    while (mongoc_cursor_next (cursor, &doc)) {
        if(bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_UTF8(&iter)) {

            // get matching dimension from set
            dim_id = bson_iter_utf8(&iter, NULL);
            for((rd) = (st)->dimensions; (rd) ; (rd) = (rd)->next) {
                if(!strcmp(rd->id, dim_id))
                    break;
            }

            // extract queried dimension data
            if(rd && !strcmp(rd->id, dim_id) && rd->state->mongoeng_data && bson_iter_find(&iter, "d") && 
               BSON_ITER_HOLDS_ARRAY(&iter) && bson_iter_recurse(&iter, &array_iter)) {

                dim_data = rd->state->mongoeng_data;
                uv_mutex_lock(&dim_data->regions_lock);
                dim_data->regions = callocz(region_memory_count, sizeof(struct mongoeng_region));

                // iterate dimension data 
                while(bson_iter_next(&array_iter) && BSON_ITER_HOLDS_DOCUMENT(&array_iter) && 
                      bson_iter_recurse(&array_iter, &inner_iter)) {
                    
                    if(bson_iter_find(&inner_iter, "r") && BSON_ITER_HOLDS_INT32(&inner_iter)) {
                        // check collection interval and create a new region if it differs from the previous
                        interval = bson_iter_int32(&inner_iter);
                        if(current_interval != interval) {
                            if(++dim_data->region_count > region_memory_count) {
                                region_memory_count *= 2;
                                dim_data->regions = reallocz(dim_data->regions, 
                                                             sizeof(struct mongoeng_region) * region_memory_count);
                            }
                            // resize the previous region's values (if this is not the first)
                            if(current_interval) {
                                current_region->values = 
                                                reallocz(current_region->values, 
                                                         sizeof(struct mongoeng_value) * current_region->value_count);
                            }
                            value_memory_count = 32768;
                            current_interval = interval;
                            current_region = &dim_data->regions[dim_data->region_count - 1];
                            current_region->interval = interval;
                            current_region->value_count = 0;
                            current_region->values = callocz(value_memory_count, sizeof(struct mongoeng_value));
                        }
                    }

                    if(current_interval && bson_iter_find(&inner_iter, "v") && 
                       BSON_ITER_HOLDS_INT32(&inner_iter)) {

                        value = bson_iter_int32(&inner_iter);
                        if(bson_iter_find(&inner_iter, "t") && BSON_ITER_HOLDS_DATE_TIME(&inner_iter)) {
                            time = bson_iter_time_t(&inner_iter);
                            if(++current_region->value_count > value_memory_count) {
                                value_memory_count *= 2;
                                current_region->values = reallocz(current_region->values, 
                                                                  sizeof(struct mongoeng_value) * value_memory_count);
                            }
                            current_value = &current_region->values[current_region->value_count - 1];
                            current_value->v = value;
                            current_value->t = time * USEC_PER_SEC;
                        }
                    }
                }
                // resize the last region's values and the regions for this dimension if any region was found
                if(current_interval) {
                    current_region->values = reallocz(current_region->values, 
                                                      sizeof(struct mongoeng_value) * current_region->value_count);
                    dim_data->regions = reallocz(dim_data->regions, 
                                                 sizeof(struct mongoeng_region) * dim_data->region_count);
                }
                uv_mutex_unlock(&dim_data->regions_lock);
            }
            rd = NULL;
            region_memory_count = 256;
            value_memory_count = 32768;
            current_interval = 0;
        }
    }
    if (mongoc_cursor_error (cursor, &error)) {
        error("MongoDB process_query_set ERROR %s\n", error.message);
    }
    uv_mutex_unlock(&ctx->client_lock);

    mongoc_cursor_destroy (cursor);
    bson_destroy (pipeline);
}

void process_bulk_write(struct mongoengine_instance *ctx) {
    RRDHOST *host;
    bson_t reply;
    bson_error_t error;
    struct mongoeng_host_data *mongoeng_data;

    uv_mutex_lock(&ctx->client_lock);
    rrd_rdlock();

    rrdhost_foreach_read(host) {
        mongoeng_data = host->mongoeng_data;
        if(mongoeng_data && mongoeng_data->collection) {
            uv_mutex_lock(&mongoeng_data->bulk_write_lock);
            if(mongoeng_data->op) {
                mongoc_bulk_operation_execute (mongoeng_data->op, &reply, &error);
                bson_destroy (&reply);
                mongoc_bulk_operation_destroy (mongoeng_data->op);
            }                
            mongoeng_data->op = mongoc_collection_create_bulk_operation_with_opts (mongoeng_data->collection, NULL);
            uv_mutex_unlock(&mongoeng_data->bulk_write_lock);
        }
    }

    rrd_unlock();
    uv_mutex_unlock(&ctx->client_lock);
}

time_t query_time(RRDDIM *rd, struct mongoeng_host_data *mongoeng_data, int sort) {
    bson_t *filter;
    bson_t *opts;
    mongoc_cursor_t *cursor;
    bson_error_t error;
    const bson_t *doc;
    bson_iter_t iter;
    time_t time = 0;

    struct mongoengine_instance *ctx = (struct mongoengine_instance *) rd->rrdset->rrdhost->rrdeng_ctx;
    if (unlikely(!ctx)) {
        error("MongoDB query_time failed to fetch context");
        return time;
    }

    filter = BCON_NEW("m.s", rd->rrdset->id, "m.d", rd->id);
    opts = BCON_NEW("projection", "{",
                        "_id", BCON_BOOL(false),
                        "t", BCON_BOOL(true),
                    "}",
                    "sort", "{", "t", BCON_INT32(sort), "}",
                    "limit", BCON_INT64(1));

    uv_mutex_lock(&ctx->client_lock);
    cursor = mongoc_collection_find_with_opts(mongoeng_data->collection, filter, opts, NULL);
    if (mongoc_cursor_next (cursor, &doc)) {
        if(bson_iter_init_find(&iter, doc, "t") && BSON_ITER_HOLDS_DATE_TIME(&iter)) {
            time = bson_iter_time_t(&iter);
        }
    }

    if (mongoc_cursor_error (cursor, &error)) {
        error("MongoDB mongoeng_metric_latest_time ERROR %s\n", error.message);
    }
    uv_mutex_unlock(&ctx->client_lock);

    mongoc_cursor_destroy (cursor);
    bson_destroy (opts);
    bson_destroy (filter);
    return time;
}

void process_query_latest(RRDDIM *rd) {
    struct mongoeng_dim_data *dim_data;
    struct mongoeng_host_data *host_data;

    dim_data = rd->state->mongoeng_data;
    if(!dim_data) {
        error("MongoDB process_query_latest failed to retrieve dim data");
        return;
    }

    host_data = rd->rrdset->rrdhost->mongoeng_data;
    if(!host_data) {
        error("MongoDB process_query_latest failed to retrieve host data");
        return;
    }
    dim_data->latest_time = query_time(rd, host_data, -1);
}

void process_query_oldest(RRDDIM *rd) {
    struct mongoeng_dim_data *dim_data;
    struct mongoeng_host_data *host_data;

    dim_data = rd->state->mongoeng_data;
    if(!dim_data) {
        error("MongoDB process_query_oldest failed to retrieve dim data");
        return;
    }

    host_data = rd->rrdset->rrdhost->mongoeng_data;
    if(!host_data) {
        error("MongoDB process_query_oldest failed to retrieve host data");
        return;
    }
    dim_data->oldest_time = query_time(rd, host_data, 1);
}

void mongoeng_worker(void* arg)
{
    uv_loop_t* loop;
    int shutdown, ret;
    enum mongoengine_opcode opcode;
    uv_timer_t timer_req;
    struct mongoeng_cmd cmd;
    unsigned cmd_batch_size;
    
    struct mongoengine_worker_config* wc = arg;
    struct mongoengine_instance *ctx = wc->ctx;

    mongoeng_init_cmd_queue(wc);

    loop = wc->loop = mallocz(sizeof(uv_loop_t));
    ret = uv_loop_init(loop);
    if (ret) {
        error("uv_loop_init(): %s", uv_strerror(ret));
        goto error_after_loop_init;
    }
    loop->data = wc;

    ret = uv_async_init(wc->loop, &wc->async, mongo_async_cb);
    if (ret) {
        error("uv_async_init(): %s", uv_strerror(ret));
        goto error_after_async_init;
    }
    wc->async.data = wc;

    /* bulk write timer */
    ret = uv_timer_init(loop, &timer_req);
    if (ret) {
        error("uv_timer_init(): %s", uv_strerror(ret));
        goto error_after_timer_init;
    }
    timer_req.data = wc;

    wc->error = 0;
    /* wake up initialization thread */
    completion_mark_complete(&ctx->mongoeng_completion);

    fatal_assert(0 == uv_timer_start(&timer_req, mongo_timer_cb, TIMER_PERIOD_MS, TIMER_PERIOD_MS));
    shutdown = 0;
    while (likely(shutdown == 0)) {
        uv_run(loop, UV_RUN_DEFAULT);

        /* wait for commands */
        cmd_batch_size = 0;
        do {
            /*
             * Avoid starving the loop when there are too many commands coming in.
             * mongo_timer_cb will interrupt the loop again to allow serving more commands.
             */
            if (unlikely(cmd_batch_size >= MAX_CMD_BATCH_SIZE))
                break;

            cmd = mongoeng_deq_cmd(wc);
            opcode = cmd.opcode;
            ++cmd_batch_size;

            switch (opcode) {
            case MONGOENGINE_NOOP:
                /* the command queue was empty, do nothing */
                break;
            case MONGOENGINE_SHUTDOWN:
                shutdown = 1;
                break;
            case MONGOENGINE_QUIESCE:
                ctx->quiesce = SET_QUIESCE;
                fatal_assert(0 == uv_timer_stop(&timer_req));
                uv_close((uv_handle_t *)&timer_req, NULL);
                ctx->quiesce = QUIESCED;
                completion_mark_complete(&ctx->mongoeng_completion);
                break;
            case MONGOENGINE_QUERY_SET:
                process_query_set(cmd.set_query);
                completion_mark_complete(cmd.completion);
                break;
            case MONGOENGINE_QUERY_TIME_LATEST:
                process_query_latest(cmd.rd);
                completion_mark_complete(cmd.completion);
                break;
            case MONGOENGINE_QUERY_TIME_OLDEST:
                process_query_oldest(cmd.rd);
                completion_mark_complete(cmd.completion);
                break;
            case MONGOENGINE_BULK_WRITE:
                process_bulk_write(ctx);
                break;    
            case MONGOENGINE_TEST:
                completion_mark_complete(cmd.completion);
            default:
                break;
            }
        } while (opcode != MONGOENGINE_NOOP);
    }

    /* cleanup operations of the event loop */
    info("Shutting down MongoDB engine event loop.");

    /*
     * uv_async_send after uv_close does not seem to crash in linux at the moment,
     * it is however undocumented behaviour and we need to be aware if this becomes
     * an issue in the future.
     */
    uv_close((uv_handle_t *)&wc->async, NULL);

    uv_run(loop, UV_RUN_DEFAULT);

    info("Shutting down MongoDB engine event loop complete.");
    /* TODO: don't let the API block by waiting to enqueue commands */
    uv_cond_destroy(&wc->cmd_cond);
/*  uv_mutex_destroy(&wc->cmd_mutex); */
    fatal_assert(0 == uv_loop_close(loop));
    freez(loop);

    return;

error_after_timer_init:
    uv_close((uv_handle_t *)&wc->async, NULL);
error_after_async_init:
    fatal_assert(0 == uv_loop_close(loop));
error_after_loop_init:
    freez(loop);

    wc->error = UV_EAGAIN;
    /* wake up initialization thread */
    completion_mark_complete(&ctx->mongoeng_completion);
}
