// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_STORAGEENGINEAPI_H
#define NETDATA_STORAGEENGINEAPI_H

#include "rrd.h"

typedef struct storage_engine_api STORAGE_ENGINE_API;
typedef struct storage_engine STORAGE_ENGINE;
typedef struct storage_engine_instance STORAGE_ENGINE_INSTANCE;

struct storage_engine_ops {
    STORAGE_ENGINE_INSTANCE*(*create)(STORAGE_ENGINE* engine, RRDHOST *host);
    void(*exit)(STORAGE_ENGINE_INSTANCE*);
    void(*destroy)(STORAGE_ENGINE_INSTANCE*);
};

struct rrdset_ops {
    RRDSET*(*create)(const char *id, const char *fullid, const char *filename, long entries, int update_every);
    void(*destroy)(RRDSET *rd);
};

struct rrddim_ops {
    RRDDIM*(*create)(RRDSET *st, const char *id, const char *filename, collected_number multiplier,
                          collected_number divisor, RRD_ALGORITHM algorithm);
    void(*init)(RRDDIM* rd);
    void(*destroy)(RRDDIM *rd);
};

struct storage_engine_api {
    struct storage_engine_ops engine_ops;
    struct rrdset_ops set_ops;
    struct rrddim_ops dim_ops;
    struct rrddim_collect_ops collect_ops;
    struct rrddim_query_ops query_ops;
};

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
