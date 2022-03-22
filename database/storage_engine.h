// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_STORAGEENGINEAPI_H
#define NETDATA_STORAGEENGINEAPI_H

#include "rrd.h"

typedef struct storage_engine_api STORAGE_ENGINE_API;
typedef struct storage_engine STORAGE_ENGINE;
typedef struct storage_engine_instance STORAGE_ENGINE_INSTANCE;

typedef STORAGE_ENGINE_INSTANCE*(*storage_engine_init)(RRDHOST *host);
typedef void(*storage_engine_destroy)(STORAGE_ENGINE_INSTANCE*);

typedef RRDDIM*(*storage_engine_dimension_init)(RRDSET *st, const char *id, const char *filename);
typedef void(*storage_engine_dimension_destroy)(RRDDIM *rd);

typedef RRDSET*(*storage_engine_set_init)(const char *id, const char *fullid, const char *filename);
typedef void(*storage_engine_set_destroy)(RRDSET *rd);

struct storage_engine_api {
    storage_engine_init init;
    storage_engine_destroy exit;
    storage_engine_destroy destroy;

    storage_engine_set_init set_init;
    storage_engine_set_destroy set_destroy;

    storage_engine_dimension_init dimension_init;
    storage_engine_dimension_destroy dimension_destroy;

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
    struct metalog_instance *metalog_ctx;
    char machine_guid[GUID_LEN + 1]; /* the unique ID of the corresponding host, or localhost for multihost DB */
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
STORAGE_ENGINE_INSTANCE* engine_new(RRD_MEMORY_MODE mmode, RRDHOST *host, bool force_new);
void engine_delete(STORAGE_ENGINE_INSTANCE* engine);

#endif
