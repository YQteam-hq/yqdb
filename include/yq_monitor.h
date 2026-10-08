/*
 * yq_monitor.h — yq-DB Advanced Monitoring and Analytics System
 *
 * Version   : 1.0.0
 * Language   : C11
 * Format version: 1 (see FORMAT.md)
 *
 * Design constraints (read before modifying this header):
 *   1. This header is the sole external ABI contract. Monitor layer must not enter this file.
 *   2. All structs must have struct_size field, new fields can only be appended at the end.
 *   3. Error code values, once published, are fixed and cannot be rearranged or reused.
 *   4. No internal structures exposed, all handles are opaque types.
 */

#ifndef YQ_MONITOR_H
#define YQ_MONITOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * Monitoring Support (Optional)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Monitoring support is optional. To enable monitoring features, define YQ_ENABLE_MONITOR
 * before including yq.h or use -DYQ_ENABLE_MONITOR for compilation.
 */
#ifndef YQ_ENABLE_MONITOR
#define YQ_ENABLE_MONITOR 0
#endif

#if YQ_ENABLE_MONITOR

/* ═══════════════════════════════════════════════════════════════════════
 * Error Code Extensions
 * ═══════════════════════════════════════════════════════════════════════ */

/* Basic error codes (from main yq.h) */
#define YQ_OK              0   /* Success */
#define YQ_ERR             1   /* Generic error */
#define YQ_ERR_NOMEM       2   /* Memory allocation failed */
#define YQ_ERR_INVAL       3   /* Invalid parameter */
#define YQ_ERR_NOTFOUND    4   /* Not found */
#define YQ_ERR_EXISTS      5   /* Already exists */

typedef enum yq_monitor_rc {
    YQ_MONITOR_OK                = 0,   /* Success */
    YQ_MONITOR_ERR               = 200,  /* Generic monitoring error */
    YQ_MONITOR_ERR_CONFIG        = 201,  /* Configuration error */
    YQ_MONITOR_ERR_STATS         = 202,  /* Statistics error */
    YQ_MONITOR_ERR_METRICS       = 203,  /* Metrics error */
    YQ_MONITOR_ERR_ALERT         = 204,  /* Alert error */
    YQ_MONITOR_ERR_EXPORT        = 205,  /* Export error */
    YQ_MONITOR_ERR_SAMPLING      = 206,  /* Sampling error */
    YQ_MONITOR_ERR_AGGREGATION   = 207,  /* Aggregation error */
    YQ_MONITOR_ERR_VISUALIZATION = 208,  /* Visualization error */
    YQ_MONITOR_ERR_REPORT        = 209   /* Report error */
} yq_monitor_rc;

/* ═══════════════════════════════════════════════════════════════════════
 * Monitoring Levels
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_monitor_level {
    YQ_MONITOR_LEVEL_OFF    = 0,  /* Monitoring off */
    YQ_MONITOR_LEVEL_BASIC  = 1,  /* Basic monitoring (key metrics) */
    YQ_MONITOR_LEVEL_NORMAL = 2,  /* Normal monitoring (standard metrics) */
    YQ_MONITOR_LEVEL_DETAILED = 3, /* Detailed monitoring (all metrics) */
    YQ_MONITOR_LEVEL_DEBUG  = 4   /* Debug monitoring (includes debug info) */
} yq_monitor_level;

/* ═══════════════════════════════════════════════════════════════════════
 * Metric Types
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_monitor_type {
    YQ_MONITOR_TYPE_COUNTER   = 0,  /* Counter (monotonic increasing) */
    YQ_MONITOR_TYPE_GAUGE     = 1,  /* Gauge (can increase or decrease) */
    YQ_MONITOR_TYPE_HISTOGRAM = 2,  /* Histogram (distribution statistics) */
    YQ_MONITOR_TYPE_TIMER     = 3,  /* Timer (time statistics) */
    YQ_MONITOR_TYPE_SET       = 4   /* Set (unique count) */
} yq_monitor_type;

/* ═══════════════════════════════════════════════════════════════════════
 * Data Types
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_monitor_data_type {
    YQ_MONITOR_DATA_INT64    = 0,  /* 64-bit integer */
    YQ_MONITOR_DATA_DOUBLE   = 1,  /* Double precision floating point */
    YQ_MONITOR_DATA_BOOL     = 2,  /* Boolean */
    YQ_MONITOR_DATA_STRING   = 3,  /* String */
    YQ_MONITOR_DATA_JSON     = 4,  /* JSON object */
    YQ_MONITOR_DATA_BINARY   = 5   /* Binary data */
} yq_monitor_data_type;

/* ═══════════════════════════════════════════════════════════════════════
 * Opaque Handles
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_monitor      yq_monitor;
typedef struct yq_metric       yq_metric;
typedef struct yq_alert        yq_alert;
typedef struct yq_exporter     yq_exporter;
typedef struct yq_sampler      yq_sampler;
typedef struct yq_aggregator   yq_aggregator;

/* ═══════════════════════════════════════════════════════════════════════
 * Monitoring Configuration
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_monitor_config {
    uint32_t struct_size;      /* Must be sizeof(yq_monitor_config) */
    uint32_t enabled;          /* Whether monitoring is enabled */
    uint32_t level;            /* Monitoring level (yq_monitor_level) */
    uint32_t collection_interval_ms;  /* Collection interval (milliseconds) */
    uint32_t retention_hours;   /* Data retention time (hours) */
    uint32_t max_metrics;      /* Maximum number of metrics */
    uint32_t max_alerts;       /* Maximum number of alerts */
    uint32_t export_interval_ms; /* Export interval (milliseconds) */
    uint32_t buffer_size;      /* Buffer size */
    uint32_t enable_sampling;  /* Whether to enable sampling */
    uint32_t sample_rate;      /* Sample rate (1-100) */
    uint32_t enable_aggregation; /* Whether to enable aggregation */
    uint32_t aggregation_interval_ms; /* Aggregation interval (milliseconds) */
    uint32_t reserved[8];      /* Must be 0 */
} yq_monitor_config;

/* ═══════════════════════════════════════════════════════════════════════
 * Metric Definition
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_metric_definition {
    uint32_t struct_size;      /* Must be sizeof(yq_metric_definition) */
    char name[64];             /* Metric name */
    char description[128];     /* Metric description */
    uint32_t type;            /* Metric type (yq_monitor_type) */
    uint32_t data_type;       /* Data type (yq_monitor_data_type) */
    uint32_t unit;            /* Unit (e.g., bytes, milliseconds, count) */
    uint32_t tags_count;      /* Number of tags */
    char tags[256];           /* Tags (JSON format) */
    uint32_t reserved[8];     /* Must be 0 */
} yq_metric_definition;

/* ═══════════════════════════════════════════════════════════════════════
 * Metric Data
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_metric_data {
    uint32_t struct_size;      /* Must be sizeof(yq_metric_data) */
    uint64_t timestamp;       /* Timestamp (milliseconds) */
    char name[64];             /* Metric name */
    int64_t int_value;         /* Integer value */
    double double_value;      /* Double value */
    uint32_t bool_value;      /* Boolean value */
    uint32_t string_len;      /* String length */
    const char *string_value;  /* String value */
    uint32_t binary_len;      /* Binary data length */
    const void *binary_value;  /* Binary data value */
    uint32_t tags_count;      /* Number of tags */
    char tags[256];           /* Tags (JSON format) */
    uint32_t reserved[8];     /* Must be 0 */
} yq_metric_data;

/* ═══════════════════════════════════════════════════════════════════════
 * Alert Configuration
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef enum yq_alert_severity {
    YQ_ALERT_SEVERITY_LOW     = 0,  /* Low severity */
    YQ_ALERT_SEVERITY_MEDIUM  = 1,  /* Medium severity */
    YQ_ALERT_SEVERITY_HIGH   = 2,  /* High severity */
    YQ_ALERT_SEVERITY_CRITICAL = 3  /* Critical severity */
} yq_alert_severity;

typedef enum yq_alert_condition {
    YQ_ALERT_CONDITION_GT = 0,   /* Greater than */
    YQ_ALERT_CONDITION_LT = 1,   /* Less than */
    YQ_ALERT_CONDITION_GTE = 2,  /* Greater than or equal */
    YQ_ALERT_CONDITION_LTE = 3,  /* Less than or equal */
    YQ_ALERT_CONDITION_EQ  = 4,  /* Equal */
    YQ_ALERT_CONDITION_NEQ = 5   /* Not equal */
} yq_alert_condition;

typedef struct yq_alert_rule {
    uint32_t struct_size;      /* Must be sizeof(yq_alert_rule) */
    char name[64];             /* Alert name */
    char description[128];     /* Alert description */
    uint32_t severity;         /* Severity (yq_alert_severity) */
    char metric_name[64];      /* Metric name */
    uint32_t condition;       /* Condition (yq_alert_condition) */
    double threshold;         /* Threshold */
    uint32_t duration_ms;     /* Duration (milliseconds) */
    uint32_t enabled;         /* Whether enabled */
    uint32_t cooldown_ms;     /* Cooldown time (milliseconds) */
    uint32_t notification_enabled; /* Whether notifications enabled */
    char notification_channels[256]; /* Notification channels (JSON format) */
    uint32_t reserved[8];     /* Must be 0 */
} yq_alert_rule;

/* ═══════════════════════════════════════════════════════════════════════
 * Alert Status
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_alert_status {
    uint32_t struct_size;      /* Must be sizeof(yq_alert_status) */
    char name[64];             /* Alert name */
    uint32_t severity;         /* Severity */
    uint32_t state;           /* State (0: normal, 1: triggered, 2: resolved) */
    uint64_t trigger_time;    /* Trigger time */
    uint64_t last_time;       /* Last trigger time */
    uint32_t trigger_count;   /* Trigger count */
    double current_value;     /* Current value */
    double threshold;         /* Threshold */
    char message[256];       /* Alert message */
    uint32_t reserved[8];     /* Must be 0 */
} yq_alert_status;

/* ═══════════════════════════════════════════════════════════════════════
 * Export Configuration
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef enum yq_export_format {
    YQ_EXPORT_FORMAT_JSON    = 0,  /* JSON format */
    YQ_EXPORT_FORMAT_PROMETHEUS = 1, /* Prometheus format */
    YQ_EXPORT_FORMAT_INFLUXDB = 2,  /* InfluxDB format */
    YQ_EXPORT_FORMAT_GRAPHITE = 3,  /* Graphite format */
    YQ_EXPORT_FORMAT_CSV     = 4,  /* CSV format */
    YQ_EXPORT_FORMAT_XML     = 5   /* XML format */
} yq_export_format;

typedef struct yq_export_config {
    uint32_t struct_size;      /* Must be sizeof(yq_export_config) */
    uint32_t enabled;          /* Whether export enabled */
    uint32_t format;          /* Export format (yq_export_format) */
    char endpoint[256];       /* Export endpoint */
    uint32_t interval_ms;     /* Export interval (milliseconds) */
    uint32_t timeout_ms;      /* Timeout (milliseconds) */
    uint32_t retry_count;     /* Retry count */
    uint32_t auth_enabled;    /* Whether authentication enabled */
    char auth_token[128];    /* Authentication token */
    uint32_t compression_enabled; /* Whether compression enabled */
    uint32_t metrics_filter[32];  /* Metrics filter */
    uint32_t reserved[8];     /* Must be 0 */
} yq_export_config;

/* ═══════════════════════════════════════════════════════════════════════
 * Sampling Configuration
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_sampler_config {
    uint32_t struct_size;      /* Must be sizeof(yq_sampler_config) */
    uint32_t enabled;          /* Whether sampling enabled */
    uint32_t sample_rate;     /* Sample rate (1-100) */
    uint32_t buffer_size;     /* Buffer size */
    uint32_t flush_interval_ms; /* Flush interval (milliseconds) */
    uint32_t random_seed;     /* Random seed */
    uint32_t stratified_enabled; /* Whether stratified sampling enabled */
    uint32_t time_window_ms;  /* Time window (milliseconds) */
    uint32_t reserved[8];     /* Must be 0 */
} yq_sampler_config;

/* ═══════════════════════════════════════════════════════════════════════
 * Aggregation Configuration
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef enum yq_aggregation_type {
    YQ_AGGREGATION_TYPE_SUM     = 0,  /* Sum */
    YQ_AGGREGATION_TYPE_AVG     = 1,  /* Average */
    YQ_AGGREGATION_TYPE_MIN     = 2,  /* Minimum */
    YQ_AGGREGATION_TYPE_MAX     = 3,  /* Maximum */
    YQ_AGGREGATION_TYPE_COUNT   = 4,  /* Count */
    YQ_AGGREGATION_TYPE_STDDEV  = 5,  /* Standard deviation */
    YQ_AGGREGATION_TYPE_PERCENTILE = 6 /* Percentile */
} yq_aggregation_type;

typedef struct yq_aggregation_config {
    uint32_t struct_size;      /* Must be sizeof(yq_aggregation_config) */
    uint32_t enabled;          /* Whether aggregation enabled */
    uint32_t type;            /* Aggregation type (yq_aggregation_type) */
    uint32_t interval_ms;     /* Aggregation interval (milliseconds) */
    uint32_t window_size;     /* Window size */
    uint32_t percentile;      /* Percentile value (if applicable) */
    uint32_t downsample_enabled; /* Whether downsampling enabled */
    uint32_t downsample_factor; /* Downsampling factor */
    uint32_t reserved[8];     /* Must be 0 */
} yq_aggregation_config;

/* ═══════════════════════════════════════════════════════════════════════
 * Monitor Statistics
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_monitor_stats {
    uint32_t struct_size;      /* Must be sizeof(yq_monitor_stats) */
    uint64_t total_metrics;    /* Total metrics count */
    uint64_t active_metrics;   /* Active metrics count */
    uint64_t total_alerts;     /* Total alerts count */
    uint64_t active_alerts;    /* Active alerts count */
    uint64_t total_samples;    /* Total samples count */
    uint64_t export_count;    /* Export count */
    uint64_t export_bytes;    /* Export bytes */
    uint64_t alert_count;     /* Alert trigger count */
    uint64_t last_collection_time; /* Last collection time */
    uint32_t collection_interval_ms; /* Collection interval */
    uint32_t memory_usage;    /* Memory usage */
    uint32_t buffer_usage;    /* Buffer usage */
    uint32_t reserved[8];     /* Must be 0 */
} yq_monitor_stats;

/* ═══════════════════════════════════════════════════════════════════════
 * Monitor Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Initialize monitoring system
 */
int yq_monitor_init(yq_monitor_config *config, yq_monitor **out);

/*
 * Shutdown monitoring system
 */
int yq_monitor_close(yq_monitor *monitor);

/*
 * Get monitoring statistics
 */
int yq_monitor_get_stats(yq_monitor *monitor, yq_monitor_stats *stats);

/*
 * Set monitoring level
 */
int yq_monitor_set_level(yq_monitor *monitor, uint32_t level);

/*
 * Get monitoring level
 */
int yq_monitor_get_level(yq_monitor *monitor, uint32_t *level);

/*
 * Enable/disable monitoring
 */
int yq_monitor_set_enabled(yq_monitor *monitor, uint32_t enabled);

/*
 * Check if monitoring is enabled
 */
int yq_monitor_is_enabled(yq_monitor *monitor, uint32_t *enabled);

/* ═══════════════════════════════════════════════════════════════════════
 * Metric Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Register metric definition
 */
int yq_monitor_register_metric(yq_monitor *monitor, yq_metric_definition *def);

/*
 * Unregister metric
 */
int yq_monitor_unregister_metric(yq_monitor *monitor, const char *name);

/*
 * Get metric definition
 */
int yq_monitor_get_metric_definition(yq_monitor *monitor, const char *name, yq_metric_definition *def);

/*
 * List all metric definitions
 */
int yq_monitor_list_metrics(yq_monitor *monitor, char **metrics, size_t *count);

/*
 * Record metric data
 */
int yq_monitor_record_metric(yq_monitor *monitor, yq_metric_data *data);

/*
 * Get metric data
 */
int yq_monitor_get_metric_data(yq_monitor *monitor, const char *name, uint64_t start_time, 
                              uint64_t end_time, yq_metric_data **data, size_t *count);

/*
 * Increment counter value
 */
int yq_monitor_increment_counter(yq_monitor *monitor, const char *name, int64_t value, 
                                const char *tags);

/*
 * Set gauge value
 */
int yq_monitor_set_gauge(yq_monitor *monitor, const char *name, double value, 
                        const char *tags);

/*
 * Record histogram data
 */
int yq_monitor_record_histogram(yq_monitor *monitor, const char *name, double value, 
                               const char *tags);

/*
 * Record timer data
 */
int yq_monitor_record_timer(yq_monitor *monitor, const char *name, double value, 
                           const char *tags);

/*
 * Add to set
 */
int yq_monitor_add_to_set(yq_monitor *monitor, const char *name, const char *value, 
                         const char *tags);

/* ═══════════════════════════════════════════════════════════════════════
 * Alert Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Add alert rule
 */
int yq_monitor_add_alert_rule(yq_monitor *monitor, yq_alert_rule *rule);

/*
 * Remove alert rule
 */
int yq_monitor_remove_alert_rule(yq_monitor *monitor, const char *name);

/*
 * Get alert rule
 */
int yq_monitor_get_alert_rule(yq_monitor *monitor, const char *name, yq_alert_rule *rule);

/*
 * List all alert rules
 */
int yq_monitor_list_alert_rules(yq_monitor *monitor, char **rules, size_t *count);

/*
 * Get alert status
 */
int yq_monitor_get_alert_status(yq_monitor *monitor, const char *name, yq_alert_status *status);

/*
 * List all alert statuses
 */
int yq_monitor_list_alert_statuses(yq_monitor *monitor, yq_alert_status **statuses, 
                                   size_t *count);

/*
 * Enable/disable alert rule
 */
int yq_monitor_set_alert_enabled(yq_monitor *monitor, const char *name, uint32_t enabled);

/*
 * Trigger alert
 */
int yq_monitor_trigger_alert(yq_monitor *monitor, const char *name, const char *message);

/*
 * Resolve alert
 */
int yq_monitor_resolve_alert(yq_monitor *monitor, const char *name);

/* ═══════════════════════════════════════════════════════════════════════
 * Export Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Configure export
 */
int yq_monitor_configure_export(yq_monitor *monitor, yq_export_config *config);

/*
 * Start exporter
 */
int yq_monitor_start_exporter(yq_monitor *monitor);

/*
 * Stop exporter
 */
int yq_monitor_stop_exporter(yq_monitor *monitor);

/*
 * Manually export data
 */
int yq_monitor_export_data(yq_monitor *monitor, uint32_t format, const char *filename);

/*
 * Get export statistics
 */
int yq_monitor_get_export_stats(yq_monitor *monitor, uint64_t *export_count, 
                                uint64_t *export_bytes);

/* ═══════════════════════════════════════════════════════════════════════
 * Sampling Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Configure sampler
 */
int yq_monitor_configure_sampler(yq_monitor *monitor, yq_sampler_config *config);

/*
 * Start sampler
 */
int yq_monitor_start_sampler(yq_monitor *monitor);

/*
 * Stop sampler
 */
int yq_monitor_stop_sampler(yq_monitor *monitor);

/*
 * Sample metric data
 */
int yq_monitor_sample_metric(yq_monitor *monitor, yq_metric_data *data, 
                            uint32_t *sampled);

/* ═══════════════════════════════════════════════════════════════════════
 * Aggregation Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Configure aggregator
 */
int yq_monitor_configure_aggregator(yq_monitor *monitor, yq_aggregation_config *config);

/*
 * Start aggregator
 */
int yq_monitor_start_aggregator(yq_monitor *monitor);

/*
 * Stop aggregator
 */
int yq_monitor_stop_aggregator(yq_monitor *monitor);

/*
 * Get aggregated data
 */
int yq_monitor_get_aggregated_data(yq_monitor *monitor, const char *name, 
                                  uint64_t start_time, uint64_t end_time, 
                                  yq_metric_data **data, size_t *count);

/* ═══════════════════════════════════════════════════════════════════════
 * Data Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Clean up expired data
 */
int yq_monitor_cleanup_data(yq_monitor *monitor);

/*
 * Get data storage information
 */
int yq_monitor_get_storage_info(yq_monitor *monitor, uint64_t *total_bytes, 
                               uint64_t *oldest_timestamp, uint64_t *newest_timestamp);

/*
 * Backup monitoring data
 */
int yq_monitor_backup_data(yq_monitor *monitor, const char *filename);

/*
 * Restore monitoring data
 */
int yq_monitor_restore_data(yq_monitor *monitor, const char *filename);

/* ═══════════════════════════════════════════════════════════════════════
 * Utility API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Get error message
 */
const char *yq_monitor_strerror(int error_code);

/*
 * Get system metrics
 */
int yq_monitor_get_system_metrics(yq_monitor *monitor, yq_metric_data **data, size_t *count);

/*
 * Get database metrics
 */
int yq_monitor_get_database_metrics(yq_monitor *monitor, yq_metric_data **data, size_t *count);

/*
 * Get performance metrics
 */
int yq_monitor_get_performance_metrics(yq_monitor *monitor, yq_metric_data **data, size_t *count);

/*
 * Get memory metrics
 */
int yq_monitor_get_memory_metrics(yq_monitor *monitor, yq_metric_data **data, size_t *count);

/*
 * Get network metrics
 */
int yq_monitor_get_network_metrics(yq_monitor *monitor, yq_metric_data **data, size_t *count);

/*
 * Get disk metrics
 */
int yq_monitor_get_disk_metrics(yq_monitor *monitor, yq_metric_data **data, size_t *count);

#endif /* YQ_ENABLE_MONITOR */

#ifdef __cplusplus
}
#endif

#endif /* YQ_MONITOR_H */