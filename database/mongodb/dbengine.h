// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_MONGODBENGINE_H
#define NETDATA_MONGODBENGINE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "daemon/common.h"
#include "../rrd.h"
#include "../engine/rrdenginelib.h"
#include "../storage_engine.h"
#include "dbengineapi.h"

/* Forward declarations */
struct mongoengine_instance;

struct mongoeng_collect_handle {
    struct mongoeng_host_data *host_data;
};

struct mongoeng_query_handle {
    unsigned region_index;
    unsigned value_index;
    struct mongoeng_dim_data *data;
};

enum mongoengine_opcode {
    /* can be used to return empty status or flush the command queue */
    MONGOENGINE_NOOP = 0,

    MONGOENGINE_QUERY_SET,
    MONGOENGINE_QUERY_TIME_LATEST,
    MONGOENGINE_QUERY_TIME_OLDEST,
    MONGOENGINE_BULK_WRITE,
    MONGOENGINE_CREATE_COLLECTION,
    MONGOENGINE_SHUTDOWN,
    MONGOENGINE_QUIESCE,
    MONGOENGINE_TEST,

    MONGOENGINE_MAX_OPCODE
};

#define DBENG_CMD_Q_MAX_SIZE (2048)
        
struct set_query {
    RRDSET *st;
    long long start;
    long long end;
};

struct mongoeng_cmd {
    enum mongoengine_opcode opcode;
    struct completion *completion;
    union {        
        struct rrddim_query_handle *handle;
        RRDDIM *rd;
        struct set_query *set_query;
    };
};

struct mongoeng_cmdqueue {
    unsigned head, tail;
    struct mongoeng_cmd cmd_array[DBENG_CMD_Q_MAX_SIZE];
};

struct mongoengine_worker_config {
    struct mongoengine_instance *ctx;

    uv_thread_t thread;
    uv_loop_t* loop;
    uv_async_t async;

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
    // TODO
};

#define NO_QUIESCE  (0) /* initial state when all operations function normally */
#define SET_QUIESCE (1) /* set it before shutting down the instance, quiesce long running operations */
#define QUIESCED    (2) /* is set after all threads have finished running */

typedef struct _mongoc_client_t mongoc_client_t;
typedef struct _mongoc_database_t mongoc_database_t;
typedef struct _mongoc_bulk_operation_t mongoc_bulk_operation_t;

struct mongoengine_instance {
    STORAGE_ENGINE_INSTANCE parent;
    struct mongoengine_worker_config worker_config;
    struct completion mongoeng_completion;
    RRDHOST *host; /* the legacy host, or NULL for multi-host DB */
    
    uint8_t quiesce; /* set to SET_QUIESCE before shutdown of the engine */

    uv_mutex_t client_lock; // could be removed in favor of connection pooling
    mongoc_client_t* mongo_client;
    mongoc_database_t* database;

    uv_mutex_t collection_create_lock; // only needed for collection creation in metric init

    struct mongoengine_statistics stats;
};

extern void mongoeng_worker(void* arg);
extern void mongoeng_enq_cmd(struct mongoengine_worker_config* wc, struct mongoeng_cmd *cmd);
extern struct mongoeng_cmd mongoeng_deq_cmd(struct mongoengine_worker_config* wc);

#endif /* NETDATA_MONGODBENGINE_H */
