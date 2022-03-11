// SPDX-License-Identifier: GPL-3.0-or-later

#include "storage_engine.h"
#include "rrddim_mem.h"
#ifdef ENABLE_DBENGINE
#include "engine/rrdengineapi.h"
#endif

STORAGE_ENGINE_INSTANCE* multidb_ctx = NULL;

static void dimension_destroy_freez(RRDDIM *rd) {
    freez(rd);
}
static void dimension_destroy_unmap(RRDDIM *rd) {
    munmap(rd, rd->memsize);
}

// Initialize a rrdset with calloc and the provided memory mode
static RRDSET* set_init_calloc(RRD_MEMORY_MODE mode) {
    RRDSET* st = callocz(1, sizeof(RRDSET));
    st->rrd_memory_mode = mode;
    return st;
}

#define SET_INIT(MMODE) \
static RRDSET* rrdset_init_##MMODE(const char *id, const char *fullid, const char *filename) { \
    return set_init_calloc(RRD_MEMORY_MODE_##MMODE); \
}
SET_INIT(NONE)
SET_INIT(ALLOC)
SET_INIT(DBENGINE)

// Initialize a rrddim with calloc and the provided memory mode
static RRDDIM* dim_init_calloc(RRDSET *st, RRD_MEMORY_MODE mode) {
    RRDDIM* rd = callocz(1, sizeof(RRDDIM));
    rd->rrd_memory_mode = mode;
    return rd;
}

#define DIM_INIT(MMODE) \
static RRDDIM* rrddim_init_##MMODE(RRDSET *st, const char *id, const char *filename) { \
    return dim_init_calloc((st), RRD_MEMORY_MODE_##MMODE); \
}
DIM_INIT(NONE)
DIM_INIT(ALLOC)

static const struct rrddim_collect_ops im_collect_ops = {
    .init = rrddim_collect_init,
    .store_metric = rrddim_collect_store_metric,
    .finalize = rrddim_collect_finalize
};

static const struct rrddim_query_ops im_query_ops = {
    .init = rrddim_query_init,
    .next_metric = rrddim_query_next_metric,
    .is_finished = rrddim_query_is_finished,
    .finalize = rrddim_query_finalize,
    .latest_time = rrddim_query_latest_time,
    .oldest_time = rrddim_query_oldest_time
};

STORAGE_ENGINE engines[] = {
    {
        .id = RRD_MEMORY_MODE_NONE,
        .name = RRD_MEMORY_MODE_NONE_NAME,
        .api = {
            .init = NULL,
            .exit = NULL,
            .destroy = NULL,
            .set_init = rrdset_init_NONE,
            .set_destroy = dimension_destroy_freez,
            .dimension_init = rrddim_init_NONE,
            .dimension_destroy = dimension_destroy_freez,
            .collect_ops = im_collect_ops,
            .query_ops = im_query_ops
        },
        .instance_per_host = true
    },
    {
        .id = RRD_MEMORY_MODE_RAM,
        .name = RRD_MEMORY_MODE_RAM_NAME,
        .api = {
            .init = NULL,
            .exit = NULL,
            .destroy = NULL,
            .set_init = rrdset_init_ram,
            .set_destroy = dimension_destroy_unmap,
            .dimension_init = rrddim_init_ram,
            .dimension_destroy = dimension_destroy_unmap,
            .collect_ops = im_collect_ops,
            .query_ops = im_query_ops
        },
        .instance_per_host = true
    },
    {
        .id = RRD_MEMORY_MODE_MAP,
        .name = RRD_MEMORY_MODE_MAP_NAME,
        .api = {
            .init = NULL,
            .exit = NULL,
            .destroy = NULL,
            .set_init = rrdset_init_map,
            .set_destroy = dimension_destroy_unmap,
            .dimension_init = rrddim_init_map,
            .dimension_destroy = dimension_destroy_unmap,
            .collect_ops = im_collect_ops,
            .query_ops = im_query_ops
        },
        .instance_per_host = true
    },
    {
        .id = RRD_MEMORY_MODE_SAVE,
        .name = RRD_MEMORY_MODE_SAVE_NAME,
        .api = {
            .init = NULL,
            .exit = NULL,
            .destroy = NULL,
            .set_init = rrdset_init_save,
            .set_destroy = dimension_destroy_unmap,
            .dimension_init = rrddim_init_save,
            .dimension_destroy = dimension_destroy_unmap,
            .collect_ops = im_collect_ops,
            .query_ops = im_query_ops
        },
        .instance_per_host = true
    },
    {
        .id = RRD_MEMORY_MODE_ALLOC,
        .name = RRD_MEMORY_MODE_ALLOC_NAME,
        .api = {
            .init = NULL,
            .exit = NULL,
            .destroy = NULL,
            .set_init = rrdset_init_ALLOC,
            .set_destroy = dimension_destroy_unmap,
            .dimension_init = rrddim_init_ALLOC,
            .dimension_destroy = dimension_destroy_unmap,
            .collect_ops = im_collect_ops,
            .query_ops = im_query_ops
        },
        .instance_per_host = true
    },
    { .id = RRD_MEMORY_MODE_NONE, .name = NULL }
};

STORAGE_ENGINE* find_engine(const char* name)
{
    for (STORAGE_ENGINE* it = engines; it->name; it++) {
        if (strcmp(it->name, name) == 0)
            return it;
    }
    return NULL;
}

STORAGE_ENGINE* get_engine(RRD_MEMORY_MODE mmode)
{
    for (STORAGE_ENGINE* it = engines; it->name; it++) {
        if (it->id == mmode)
            return it;
    }
    return NULL;
}

STORAGE_ENGINE_INSTANCE* engine_new(RRD_MEMORY_MODE mmode, RRDHOST *host)
{
    STORAGE_ENGINE* eng = get_engine(mmode);
    STORAGE_ENGINE_INSTANCE* instance = NULL;
    if (eng && eng->api.init) {
        instance = eng->api.init(host);
    }
    if (instance) {
        instance->engine = eng;
    }
    return instance;
}

void engine_delete(STORAGE_ENGINE_INSTANCE* instance) {
    if (instance) {
        STORAGE_ENGINE* eng = instance->engine;
        if (eng && eng->api.destroy) {
            eng->api.destroy(instance);
        }
    }
}
