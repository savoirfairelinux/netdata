// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_MONGODB_DBENGINEAPI_H
#define NETDATA_MONGODB_DBENGINEAPI_H

#include "dbengine.h"

#define MONGOENG_NR_STATS (37)
#define MONGOENG_TIMESERIES_ALREADY_EXISTS (48)

extern char *mongoengine_uri;
extern char *mongoengine_database;
extern int mongoengine_timeout;
extern int mongoengine_expiration;
extern struct mongoengine_instance mongodb_ctx;

extern void mongoeng_metric_init(RRDDIM *rd);
extern void mongoeng_store_metric_init(RRDDIM *rd);
extern void mongoeng_store_metric_next(RRDDIM *rd, usec_t point_in_time, storage_number number);
extern int mongoeng_store_metric_finalize(RRDDIM *rd);
extern unsigned 
    mongoeng_variable_step_boundaries(RRDSET *st, time_t start_time, time_t end_time, 
                                      struct rrdr_region_info **region_info_arrayp, unsigned *max_intervalp, struct context_param *context_param_list);
extern void mongoeng_load_metric_init(RRDDIM *rd, struct rrddim_query_handle *rrdimm_handle,
                                      time_t start_time, time_t end_time);
extern storage_number mongoeng_load_metric_next(struct rrddim_query_handle *rrdimm_handle, time_t *current_time);
extern int mongoeng_load_metric_is_finished(struct rrddim_query_handle *rrdimm_handle);
extern void mongoeng_load_metric_finalize(struct rrddim_query_handle *rrdimm_handle);
extern time_t mongoeng_metric_latest_time(RRDDIM *rd);
extern time_t mongoeng_metric_oldest_time(RRDDIM *rd);

/* must call once before using anything */
extern STORAGE_ENGINE_INSTANCE* mongoeng_init(STORAGE_ENGINE* eng, RRDHOST *host);

extern int mongoeng_exit(struct mongoengine_instance *ctx);
extern void mongoeng_prepare_exit(struct mongoengine_instance *ctx);

extern RRDR* mongoeng_query(
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

struct mongoeng_value {
    storage_number v;
    time_t t;
};
struct mongoeng_region {
    unsigned interval;
    unsigned value_count;
    struct mongoeng_value *values;
};
struct mongoeng_dim_data {
    usec_t oldest_time;
    usec_t latest_time;
    uv_mutex_t regions_lock;
    // temporary store for query data
    unsigned region_count;
    struct mongoeng_region *regions;
};
struct mongoeng_host_data {
    mongoc_collection_t *collection;
    uv_mutex_t bulk_write_lock;
    mongoc_bulk_operation_t *op;
};

#endif /* NETDATA_MONGODB_DBENGINEAPI_H */
