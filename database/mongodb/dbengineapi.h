// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_MONGODB_DBENGINEAPI_H
#define NETDATA_MONGODB_DBENGINEAPI_H

#include "dbengine.h"

#define MONGOENG_MIN_PAGE_CACHE_SIZE_MB (8)
#define MONGOENG_MIN_DISK_SPACE_MB (64)

#define MONGOENG_NR_STATS (37)

#define MONGOENG_FD_BUDGET_PER_INSTANCE (50)

extern int default_mongoeng_page_cache_mb;
extern int default_mongoeng_disk_quota_mb;
extern uint8_t mongoeng_drop_metrics_under_page_cache_pressure;

extern char *mongoengine_uri;
extern char *mongoengine_database;
extern int mongoengine_timeout;
extern int mongoengine_expiration;
extern struct mongoengine_instance mongodb_ctx;

struct mongoeng_region_info {
    time_t start_time;
    int update_every;
    unsigned points;
};

extern RRDDIM* mongoeng_metric_init(RRDSET *rrdset, const char *id, const char *filename);
extern void mongoeng_store_metric_init(RRDDIM *rd);
extern void mongoeng_store_metric_next(RRDDIM *rd, usec_t point_in_time, storage_number number);
extern int mongoeng_store_metric_finalize(RRDDIM *rd);
extern unsigned mongoeng_variable_step_boundaries(RRDSET *st, time_t start_time, time_t end_time);
//, struct mongoeng_region_info **region_info_arrayp, unsigned *max_intervalp, struct context_param *context_param_list);

extern void mongoeng_load_metric_init(RRDDIM *rd, struct rrddim_query_handle *rrdimm_handle,
                                    time_t start_time, time_t end_time);
extern storage_number mongoeng_load_metric_next(struct rrddim_query_handle *rrdimm_handle, time_t *current_time);
extern int mongoeng_load_metric_is_finished(struct rrddim_query_handle *rrdimm_handle);
extern void mongoeng_load_metric_finalize(struct rrddim_query_handle *rrdimm_handle);
extern time_t mongoeng_query_metric_latest_time(RRDDIM *rd);
extern time_t mongoeng_metric_latest_time(RRDDIM *rd);
extern time_t mongoeng_query_metric_oldest_time(RRDDIM *rd);
extern time_t mongoeng_metric_oldest_time(RRDDIM *rd);

/* must call once before using anything */
extern STORAGE_ENGINE_INSTANCE* mongoeng_init(RRDHOST *host);
//, char *dbfiles_path, unsigned page_cache_mb, unsigned disk_space_mb);

extern int mongoeng_exit(struct mongoengine_instance *ctx);
extern void mongoeng_prepare_exit(struct mongoengine_instance *ctx);
extern int mongoeng_metric_latest_time_by_uuid(uuid_t *dim_uuid, time_t *first_entry_t, time_t *last_entry_t);

#endif /* NETDATA_MONGODB_DBENGINEAPI_H */
