// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_STORAGEENGINEAPI_H
#define NETDATA_STORAGEENGINEAPI_H

#include "rrd.h"

typedef struct storage_engine STORAGE_ENGINE;
typedef struct storage_engine_instance STORAGE_ENGINE_INSTANCE;

// ------------------------------------------------------------------------
// function pointers that handle storage engine instance creation and destruction
struct storage_engine_ops {
    STORAGE_ENGINE_INSTANCE*(*create)(STORAGE_ENGINE* engine, RRDHOST *host);
    void(*exit)(STORAGE_ENGINE_INSTANCE*);
    void(*destroy)(STORAGE_ENGINE_INSTANCE*);
};

// ------------------------------------------------------------------------
// function pointers that handle RRDSET creation, destruction and query
struct rrdset_ops {
    RRDSET*(*create)(const char *id, const char *fullid, const char *filename, long entries, int update_every);
    void(*destroy)(RRDSET *rd);

    //
    RRDR*(*query)(
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
        , struct context_param *context_param_list);
};

// ------------------------------------------------------------------------
// function pointers that handle RRDDIM creation and destruction
struct rrddim_ops {
    RRDDIM*(*create)(RRDSET *st, const char *id, const char *filename, collected_number multiplier,
                          collected_number divisor, RRD_ALGORITHM algorithm);
    void(*init)(RRDDIM* rd);
    void(*destroy)(RRDDIM *rd);
};

// ------------------------------------------------------------------------
// function pointers for all APIs provided by a storge engine
typedef struct storage_engine_api {
    struct storage_engine_ops engine_ops;
    struct rrdset_ops set_ops;
    struct rrddim_ops dim_ops;
    struct rrddim_collect_ops collect_ops;
    struct rrddim_query_ops query_ops;
} STORAGE_ENGINE_API;

struct storage_engine {
    RRD_MEMORY_MODE id;
    const char* name;
    STORAGE_ENGINE_API api;

    // True if this storage engine only supports a single host per instance
    bool instance_per_host;
    STORAGE_ENGINE_INSTANCE* multidb_instance;
};

// Abstract structure to be extended by implementations
struct storage_engine_instance {
    STORAGE_ENGINE* engine;
};

STORAGE_ENGINE* engine_get(RRD_MEMORY_MODE mmode);
STORAGE_ENGINE* engine_find(const char* name);

// Iterator over existing engines
STORAGE_ENGINE* engine_foreach_init();
STORAGE_ENGINE* engine_foreach_next(STORAGE_ENGINE* it);

// ------------------------------------------------------------------------
// Retreive or create a storage engine instance for mmode and host
// If this engine supports multidb (instance_per_host is false), a global instance
// is returned.
// If force_new is true, a new instance will be created regardless of instance_per_host.
STORAGE_ENGINE_INSTANCE* engine_new(STORAGE_ENGINE* engine, RRDHOST *host, bool force_new);
void engine_delete(STORAGE_ENGINE_INSTANCE* engine);

#endif
