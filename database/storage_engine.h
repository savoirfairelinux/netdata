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

/* Default global database instance */
extern STORAGE_ENGINE_INSTANCE* multidb_ctx;

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
};

// Abstract structure to be extended by implementations
struct storage_engine_instance {
    // Mendatory first field
    STORAGE_ENGINE* engine;
};

STORAGE_ENGINE* get_engine(RRD_MEMORY_MODE mmode);
STORAGE_ENGINE* find_engine(const char* name);

STORAGE_ENGINE_INSTANCE* engine_new(RRD_MEMORY_MODE mmode, RRDHOST *host);
void engine_delete(STORAGE_ENGINE_INSTANCE* engine);

#endif
