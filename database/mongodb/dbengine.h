// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_MONGODBENGINE_H
#define NETDATA_MONGODBENGINE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "daemon/common.h"
#include "../rrd.h"
#include "../engine/rrdenginelib.h"

/* Forward declarations */
struct mongoengine_instance;

typedef enum {
    MONGOENGINE_STATUS_UNINITIALIZED = 0,
    MONGOENGINE_STATUS_INITIALIZING,
    MONGOENGINE_STATUS_INITIALIZED
} mongoengine_state_t;

enum mongoengine_opcode {
    /* can be used to return empty status or flush the command queue */
    MONGOENGINE_NOOP = 0,

    MONGOENGINE_READ_PAGE,
    MONGOENGINE_READ_EXTENT,
    MONGOENGINE_COMMIT_PAGE,
    MONGOENGINE_FLUSH_PAGES,
    MONGOENGINE_SHUTDOWN,
    MONGOENGINE_INVALIDATE_OLDEST_MEMORY_PAGE,
    MONGOENGINE_QUIESCE,

    MONGOENGINE_MAX_OPCODE
};

#define RRDENG_CMD_Q_MAX_SIZE (2048)

struct mongoeng_cmd {
    enum mongoengine_opcode opcode;
    struct completion *completion;
};

struct mongoeng_cmdqueue {
    unsigned head, tail;
    struct mongoeng_cmd cmd_array[RRDENG_CMD_Q_MAX_SIZE];
};

struct mongoengine_worker_config {
    struct mongoengine_instance *ctx;

    uv_thread_t thread;
    uv_loop_t* loop;
    uv_async_t async;

    /* file deletion thread */
    uv_thread_t *now_deleting_files;
    unsigned long cleanup_thread_deleting_files; /* set to 0 when now_deleting_files is still running */

    /* dirty page deletion thread */
    uv_thread_t *now_invalidating_dirty_pages;
    /* set to 0 when now_invalidating_dirty_pages is still running */
    unsigned long cleanup_thread_invalidating_dirty_pages;
    unsigned inflight_dirty_pages;

    /* FIFO command queue */
    uv_mutex_t cmd_mutex;
    uv_cond_t cmd_cond;
    volatile unsigned queue_size;
    struct mongoeng_cmdqueue cmd_queue;

    int error;
};

/*
 * Debug statistics not used by code logic.
 * They only describe operations since DB engine instance load time.
 */
struct mongoengine_statistics {
    /*rrdeng_stats_t metric_API_producers;
    rrdeng_stats_t metric_API_consumers;
    rrdeng_stats_t pg_cache_insertions;
    rrdeng_stats_t pg_cache_deletions;
    rrdeng_stats_t pg_cache_hits;
    rrdeng_stats_t pg_cache_misses;
    rrdeng_stats_t pg_cache_backfills;
    rrdeng_stats_t pg_cache_evictions;
    rrdeng_stats_t before_decompress_bytes;
    rrdeng_stats_t after_decompress_bytes;
    rrdeng_stats_t before_compress_bytes;
    rrdeng_stats_t after_compress_bytes;
    rrdeng_stats_t io_write_bytes;
    rrdeng_stats_t io_write_requests;
    rrdeng_stats_t io_read_bytes;
    rrdeng_stats_t io_read_requests;
    rrdeng_stats_t io_write_extent_bytes;
    rrdeng_stats_t io_write_extents;
    rrdeng_stats_t io_read_extent_bytes;
    rrdeng_stats_t io_read_extents;
    rrdeng_stats_t datafile_creations;
    rrdeng_stats_t datafile_deletions;
    rrdeng_stats_t journalfile_creations;
    rrdeng_stats_t journalfile_deletions;
    rrdeng_stats_t page_cache_descriptors;
    rrdeng_stats_t io_errors;
    rrdeng_stats_t fs_errors;
    rrdeng_stats_t pg_cache_over_half_dirty_events;
    rrdeng_stats_t flushing_pressure_page_deletions;*/
};

#define NO_QUIESCE  (0) /* initial state when all operations function normally */
#define SET_QUIESCE (1) /* set it before shutting down the instance, quiesce long running operations */
#define QUIESCED    (2) /* is set after all threads have finished running */

struct mongoc_client_t;
struct mongoc_database_t;
struct mongoc_collection_t;

struct mongoengine_instance {
    struct metalog_instance *metalog_ctx;
    struct mongoengine_worker_config worker_config;
    struct completion mongoeng_completion;
    RRDHOST *host; /* the legacy host, or NULL for multi-host DB */
    char machine_guid[GUID_LEN + 1]; /* the unique ID of the corresponding host, or localhost for multihost DB */
    uint64_t disk_space;
    uint64_t max_disk_space;
    unsigned last_fileno; /* newest index of datafile and journalfile */

    uint8_t quiesce; /* set to SET_QUIESCE before shutdown of the engine */

    uv_mutex_t lock;
    struct mongoc_client_t* mongo_client;
    struct mongoc_database_t* database;
    struct mongoc_collection_t* collection;
    struct mongoc_bulk_operation_t* op;

    struct mongoengine_statistics stats;
};

extern void mongoeng_test_quota(struct mongoengine_worker_config* wc);
extern void mongoeng_worker(void* arg);
extern void mongoeng_enq_cmd(struct mongoengine_worker_config* wc, struct mongoeng_cmd *cmd);
extern struct mongoeng_cmd mongoeng_deq_cmd(struct mongoengine_worker_config* wc);

#endif /* NETDATA_MONGODBENGINE_H */
