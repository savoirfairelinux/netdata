// SPDX-License-Identifier: GPL-3.0-or-later
#define NETDATA_MONGODB_INTERNALS

#include "dbengine.h"
#include <mongoc/mongoc.h>

void mongoeng_init_cmd_queue(struct mongoengine_worker_config* wc)
{
    info("MongoDB mongoeng_init_cmd_queue");
    wc->cmd_queue.head = wc->cmd_queue.tail = 0;
    wc->queue_size = 0;
    fatal_assert(0 == uv_cond_init(&wc->cmd_cond));
    fatal_assert(0 == uv_mutex_init(&wc->cmd_mutex));
}

void mongoeng_enq_cmd(struct mongoengine_worker_config* wc, struct mongoeng_cmd *cmd)
{
    info("MongoDB mongoeng_enq_cmd. %d", cmd->opcode);
    unsigned queue_size;

    /* wait for free space in queue */
    uv_mutex_lock(&wc->cmd_mutex);
    while ((queue_size = wc->queue_size) == RRDENG_CMD_Q_MAX_SIZE) {
        uv_cond_wait(&wc->cmd_cond, &wc->cmd_mutex);
    }
    fatal_assert(queue_size < RRDENG_CMD_Q_MAX_SIZE);
    /* enqueue command */
    wc->cmd_queue.cmd_array[wc->cmd_queue.tail] = *cmd;
    wc->cmd_queue.tail = wc->cmd_queue.tail != RRDENG_CMD_Q_MAX_SIZE - 1 ?
                         wc->cmd_queue.tail + 1 : 0;
    wc->queue_size = queue_size + 1;
    uv_mutex_unlock(&wc->cmd_mutex);

    /* wake up event loop */
    fatal_assert(0 == uv_async_send(&wc->async));
}

struct mongoeng_cmd mongoeng_deq_cmd(struct mongoengine_worker_config* wc)
{
    info("MongoDB mongoeng_deq_cmd.");
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
            wc->cmd_queue.head = wc->cmd_queue.head != RRDENG_CMD_Q_MAX_SIZE - 1 ?
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
    debug(D_RRDENGINE, "%s called, active=%d.", __func__, uv_is_active((uv_handle_t *)handle));
}


/* Flushes dirty pages when timer expires */
#define TIMER_PERIOD_MS (1000)

void mongo_timer_cb(uv_timer_t* handle)
{
    struct mongoengine_worker_config* wc = handle->data;
    struct mongoengine_instance *ctx = wc->ctx;

    uv_stop(handle->loop);
    uv_update_time(handle->loop);
    //if (unlikely(ctx->metalog_ctx && !ctx->metalog_ctx->initialized))
    //    return; /* Wait for the metadata log to initialize */
    //rrdeng_test_quota(wc);
    debug(D_RRDENGINE, "%s: timeout reached.", __func__);
    /*if (likely(!wc->now_deleting_files && !wc->now_invalidating_dirty_pages)) {

    }*/
    info("MongoDB mongo_timer_cb.");
   //load_configuration_dynamic();
#ifdef NETDATA_INTERNAL_CHECKS
    /*{
        char buf[4096];
        debug(D_RRDENGINE, "%s", get_rrdeng_statistics(wc->ctx, buf, sizeof(buf)));
    }*/
#endif
}

#define MAX_CMD_BATCH_SIZE (256)

void mongoeng_worker(void* arg)
{
    info("MongoDB mongoeng_worker.");
    struct mongoengine_worker_config* wc = arg;
    struct mongoengine_instance *ctx = wc->ctx;
    uv_loop_t* loop;
    int shutdown, ret;
    enum mongoengine_opcode opcode;
    uv_timer_t timer_req;
    struct mongoeng_cmd cmd;
    unsigned cmd_batch_size;

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

    wc->now_deleting_files = NULL;
    wc->cleanup_thread_deleting_files = 0;

    wc->now_invalidating_dirty_pages = NULL;
    wc->cleanup_thread_invalidating_dirty_pages = 0;
    wc->inflight_dirty_pages = 0;

    /* dirty page flushing timer */
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
        //mongoeng_cleanup_finished_threads(wc);

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
            info("MongoDB mongoeng_deq_cmd -> %d.", cmd.opcode);
            opcode = cmd.opcode;
            ++cmd_batch_size;

            switch (opcode) {
            case MONGOENGINE_NOOP:
                /* the command queue was empty, do nothing */

                uv_mutex_lock(&ctx->lock);

                if (ctx->op) {
                    bson_t reply;
                    bson_error_t error;
                    ret = mongoc_bulk_operation_execute (ctx->op, &reply, &error);
                    char *str = bson_as_canonical_extended_json (&reply, NULL);
                    info ("%s\n", str);
                    bson_free (str);
                    bson_destroy (&reply);
                    mongoc_bulk_operation_destroy (ctx->op);
                }

                ctx->op = mongoc_collection_create_bulk_operation_with_opts (ctx->collection, NULL);

                uv_mutex_unlock(&ctx->lock);

                break;
            case MONGOENGINE_SHUTDOWN:
                shutdown = 1;
                break;
            case MONGOENGINE_QUIESCE:
                //ctx->drop_metrics_under_page_cache_pressure = 0;
                ctx->quiesce = SET_QUIESCE;
                fatal_assert(0 == uv_timer_stop(&timer_req));
                uv_close((uv_handle_t *)&timer_req, NULL);
                //while (do_flush_pages(wc, 1, NULL)) {
                //    ; /* Force flushing of all committed pages. */
                //}
                //wal_flush_transaction_buffer(wc);
                //if (!mongoeng_threads_alive(wc)) {
                    ctx->quiesce = QUIESCED;
                    completion_mark_complete(&ctx->mongoeng_completion);
                //}
                break;
            case MONGOENGINE_READ_PAGE:
                //do_read_extent(wc, &cmd.read_page.page_cache_descr, 1, 0);
                break;
            case MONGOENGINE_READ_EXTENT:
                //do_read_extent(wc, cmd.read_extent.page_cache_descr, cmd.read_extent.page_count, 1);
                break;
            case MONGOENGINE_COMMIT_PAGE:
                //do_commit_transaction(wc, STORE_DATA, NULL);
                break;
            case MONGOENGINE_FLUSH_PAGES: {
                if (wc->now_invalidating_dirty_pages) {
                    /* Do not flush if the disk cannot keep up */
                    completion_mark_complete(cmd.completion);
                } else {
                    //(void)do_flush_pages(wc, 1, cmd.completion);
                }
                break;
            case MONGOENGINE_INVALIDATE_OLDEST_MEMORY_PAGE:
                //rrdeng_invalidate_oldest_committed(wc);
                break;
            }
            default:
                debug(D_RRDENGINE, "%s: default.", __func__);
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

    //while (do_flush_pages(wc, 1, NULL)) {
    //    ; /* Force flushing of all committed pages. */
    //}
    //wal_flush_transaction_buffer(wc);
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
