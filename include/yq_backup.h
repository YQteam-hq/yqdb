/*
 * yq_backup.h — yq-DB Backup and Recovery System
 *
 * Version   : 1.0.0
 * Language   : C11
 * Format version: 1 (see FORMAT.md)
 *
 * Design constraints (read before modifying this header):
 *   1. This header is the sole external ABI contract. Backup layer must not enter this file.
 *   2. All structs must have struct_size field, new fields can only be appended at the end.
 *   3. Error code values, once published, are fixed and cannot be rearranged or reused.
 *   4. No internal structures exposed, all handles are opaque types.
 */

#ifndef YQ_BACKUP_H
#define YQ_BACKUP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Support (Optional)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Backup support is optional. To enable backup features, define YQ_ENABLE_BACKUP
 * before including yq.h or use -DYQ_ENABLE_BACKUP for compilation.
 */
#ifndef YQ_ENABLE_BACKUP
#define YQ_ENABLE_BACKUP 0
#endif

#if YQ_ENABLE_BACKUP

/* ═══════════════════════════════════════════════════════════════════════
 * Error Code Extensions
 *
 * 基础错误码（YQ_OK / YQ_ERR / YQ_ERR_NOMEM / YQ_ERR_INVAL /
 * YQ_ERR_NOTFOUND / YQ_ERR_EXISTS）统一由 yq.h 的枚举 yq_rc 定义，
 * 此处不再重复声明。曾经用宏重定义会把枚举值覆盖成错误数值
 * （例如 YQ_ERR_NOTFOUND 被改成 4，而规范值是 6），从而让
 * yq_strerror() 返回错误的描述字符串。
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_backup_rc {
    YQ_BACKUP_OK                = 0,   /* Success */
    YQ_BACKUP_ERR               = 300,  /* Generic backup error */
    YQ_BACKUP_ERR_CONFIG        = 301,  /* Configuration error */
    YQ_BACKUP_ERR_CREATE        = 302,  /* Backup creation error */
    YQ_BACKUP_ERR_RESTORE       = 303,  /* Restore error */
    YQ_BACKUP_ERR_VERIFY        = 304,  /* Verification error */
    YQ_BACKUP_ERR_ENCRYPT       = 305,  /* Encryption error */
    YQ_BACKUP_ERR_DECRYPT       = 306,  /* Decryption error */
    YQ_BACKUP_ERR_COMPRESS      = 307,  /* Compression error */
    YQ_BACKUP_ERR_DECOMPRESS    = 308,  /* Decompression error */
    YQ_BACKUP_ERR_STORAGE       = 309,  /* Storage error */
    YQ_BACKUP_ERR_NETWORK       = 310,  /* Network error */
    YQ_BACKUP_ERR_TIMEOUT       = 311,  /* Timeout error */
    YQ_BACKUP_ERR_PARTIAL       = 312,  /* Partial backup error */
    YQ_BACKUP_ERR_INCONSISTENT   = 313,  /* Inconsistent backup error */
    /*
     * 下面两个由实现（src/yq_backup.c）使用，此前漏定义，
     * 使该文件一旦真正参与编译就报 "undeclared"。
     */
    YQ_BACKUP_ERR_NOMEM         = 314,  /* Out of memory */
    YQ_BACKUP_ERR_INVAL         = 315   /* Invalid argument */
} yq_backup_rc;

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Types
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_backup_type {
    YQ_BACKUP_TYPE_FULL        = 0,  /* Full backup */
    YQ_BACKUP_TYPE_INCREMENTAL = 1,  /* Incremental backup */
    YQ_BACKUP_TYPE_DIFFERENTIAL = 2, /* Differential backup */
    YQ_BACKUP_TYPE_SNAPSHOT   = 3   /* Point-in-time snapshot */
} yq_backup_type;

/* ═══════════════════════════════════════════════════════════════════════
 * Compression Types
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_backup_compression {
    YQ_BACKUP_COMPRESS_NONE    = 0,  /* No compression */
    YQ_BACKUP_COMPRESS_SNAPPY  = 1,  /* Snappy compression */
    YQ_BACKUP_COMPRESS_LZ4     = 2,  /* LZ4 compression */
    YQ_BACKUP_COMPRESS_ZSTD    = 3,  /* Zstandard compression */
    YQ_BACKUP_COMPRESS_ZLIB    = 4,  /* Zlib compression */
    YQ_BACKUP_COMPRESS_GZIP    = 5   /* Gzip compression */
} yq_backup_compression;

/* ═══════════════════════════════════════════════════════════════════════
 * Encryption Types
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_backup_encryption {
    YQ_BACKUP_ENCRYPTION_NONE  = 0,  /* No encryption */
    YQ_BACKUP_ENCRYPTION_AES256 = 1, /* AES-256 encryption */
    YQ_BACKUP_ENCRYPTION_CHACHA20 = 2, /* ChaCha20 encryption */
    YQ_BACKUP_ENCRYPTION_XOR   = 3   /* XOR encryption */
} yq_backup_encryption;

/* ═══════════════════════════════════════════════════════════════════════
 * Storage Types
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_backup_storage {
    YQ_BACKUP_STORAGE_LOCAL    = 0,  /* Local file system */
    YQ_BACKUP_STORAGE_REMOTE   = 1,  /* Remote storage */
    YQ_BACKUP_STORAGE_S3       = 2,  /* Amazon S3 */
    YQ_BACKUP_STORAGE_AZURE    = 3,  /* Azure Blob Storage */
    YQ_BACKUP_STORAGE_GCS      = 4,  /* Google Cloud Storage */
    YQ_BACKUP_STORAGE_FTP      = 5,  /* FTP server */
    YQ_BACKUP_STORAGE_SFTP     = 6   /* SFTP server */
} yq_backup_storage;

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Status
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_backup_status {
    YQ_BACKUP_STATUS_PENDING   = 0,  /* Backup pending */
    YQ_BACKUP_STATUS_RUNNING   = 1,  /* Backup in progress */
    YQ_BACKUP_STATUS_COMPLETED = 2,  /* Backup completed */
    YQ_BACKUP_STATUS_FAILED    = 3,  /* Backup failed */
    YQ_BACKUP_STATUS_CANCELLED = 4,  /* Backup cancelled */
    YQ_BACKUP_STATUS_VERIFYING = 5   /* Backup verifying */
} yq_backup_status;

/* ═══════════════════════════════════════════════════════════════════════
 * Opaque Handles
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_backup      yq_backup;
typedef struct yq_backup_job  yq_backup_job;
typedef struct yq_backup_info yq_backup_info;
typedef struct yq_restore_job yq_restore_job;

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Configuration
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_backup_config {
    uint32_t struct_size;      /* Must be sizeof(yq_backup_config) */
    uint32_t enabled;          /* Whether backup enabled */
    uint32_t auto_backup;      /* Whether auto backup enabled */
    uint32_t backup_type;      /* Backup type (yq_backup_type) */
    uint32_t compression;      /* Compression type (yq_backup_compression) */
    uint32_t encryption;       /* Encryption type (yq_backup_encryption) */
    uint32_t storage_type;     /* Storage type (yq_backup_storage) */
    uint32_t schedule_interval_ms; /* Schedule interval (milliseconds) */
    uint32_t retention_count;   /* Retention count */
    uint32_t retention_days;   /* Retention days */
    uint32_t max_file_size_mb; /* Max file size (MB) */
    uint32_t chunk_size_kb;    /* Chunk size (KB) */
    uint32_t verify_after_backup; /* Verify after backup */
    uint32_t encrypt_key;      /* Encrypt key */
    uint32_t reserved[8];      /* Must be 0 */
} yq_backup_config;

/* ═══════════════════════════════════════════════════════════════════════
 * Remote Storage Configuration
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_remote_config {
    uint32_t struct_size;      /* Must be sizeof(yq_remote_config) */
    uint32_t enabled;          /* Whether remote storage enabled */
    char endpoint[256];       /* Storage endpoint */
    char bucket[128];         /* Bucket name */
    char path[256];           /* Storage path */
    uint32_t timeout_ms;      /* Timeout (milliseconds) */
    uint32_t retry_count;     /* Retry count */
    uint32_t auth_enabled;    /* Whether authentication enabled */
    char access_key[128];     /* Access key */
    char secret_key[128];     /* Secret key */
    char region[64];          /* Region */
    uint32_t reserved[8];     /* Must be 0 */
} yq_remote_config;

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Information
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_backup_info {
    uint32_t struct_size;      /* Must be sizeof(yq_backup_info) */
    char name[64];             /* Backup name */
    char filename[256];        /* Backup filename */
    char checksum[64];         /* Checksum */
    uint64_t timestamp;       /* Timestamp */
    uint64_t size_bytes;      /* Size in bytes */
    uint32_t type;            /* Backup type (yq_backup_type) */
    uint32_t compression;     /* Compression type */
    uint32_t encryption;       /* Encryption type */
    uint32_t status;          /* Status (yq_backup_status) */
    uint32_t progress;        /* Progress (0-100) */
    uint32_t duration_ms;      /* Duration (milliseconds) */
    uint32_t file_count;      /* File count */
    uint32_t reserved[8];     /* Must be 0 */
} yq_backup_info;

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Statistics
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_backup_stats {
    uint32_t struct_size;      /* Must be sizeof(yq_backup_stats) */
    uint64_t total_backups;    /* Total backups */
    uint64_t successful_backups; /* Successful backups */
    uint64_t failed_backups;   /* Failed backups */
    /*
     * 恢复侧计数。实现（src/yq_backup.c 的 yq_backup_wait）会累加这两个字段，
     * 此前结构体里没有它们，导致该文件一旦真正参与编译即报 "has no member"。
     */
    uint64_t successful_restores; /* Successful restores */
    uint64_t failed_restores;     /* Failed restores */
    uint64_t total_bytes;      /* Total bytes backed up */
    uint64_t last_backup_time; /* Last backup time */
    uint32_t avg_duration_ms;  /* Average duration */
    uint32_t compression_ratio; /* Compression ratio */
    uint32_t recovery_rate;   /* Recovery rate */
    uint32_t storage_usage;    /* Storage usage */
    uint32_t reserved[8];     /* Must be 0 */
} yq_backup_stats;

/* ═══════════════════════════════════════════════════════════════════════
 * Restore Configuration
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_restore_config {
    uint32_t struct_size;      /* Must be sizeof(yq_restore_config) */
    uint32_t enabled;          /* Whether restore enabled */
    uint32_t overwrite;       /* Whether overwrite existing data */
    uint32_t verify_data;      /* Whether verify data integrity */
    uint32_t validate_only;   /* Whether validate only */
    uint32_t skip_corrupted;   /* Whether skip corrupted data */
    uint32_t max_errors;      /* Maximum errors allowed */
    uint32_t timeout_ms;      /* Timeout (milliseconds) */
    uint32_t retry_count;     /* Retry count */
    uint32_t reserved[8];     /* Must be 0 */
} yq_restore_config;

/* ═══════════════════════════════════════════════════════════════════════
 * Restore Statistics
 * ╎═════════════════════════════════════════════════════════════════════ */

typedef struct yq_restore_stats {
    uint32_t struct_size;      /* Must be sizeof(yq_restore_stats) */
    uint64_t total_restores;   /* Total restores */
    uint64_t successful_restores; /* Successful restores */
    uint64_t failed_restores;  /* Failed restores */
    uint64_t total_bytes;      /* Total bytes restored */
    uint64_t last_restore_time; /* Last restore time */
    uint32_t avg_duration_ms;  /* Average duration */
    uint32_t recovery_rate;   /* Recovery rate */
    uint32_t corrupted_blocks; /* Corrupted blocks */
    uint32_t reserved[8];     /* Must be 0 */
} yq_restore_stats;

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Initialize backup system
 */
int yq_backup_init(yq_backup_config *config, yq_backup **out);

/*
 * Shutdown backup system
 */
int yq_backup_close(yq_backup *backup);

/*
 * Configure backup system
 */
int yq_backup_configure(yq_backup *backup, yq_backup_config *config);

/*
 * Configure remote storage
 */
int yq_backup_configure_remote(yq_backup *backup, yq_remote_config *config);

/*
 * Get backup statistics
 */
int yq_backup_get_stats(yq_backup *backup, yq_backup_stats *stats);

/*
 * Set backup enabled
 */
int yq_backup_set_enabled(yq_backup *backup, uint32_t enabled);

/*
 * Check if backup is enabled
 */
int yq_backup_is_enabled(yq_backup *backup, uint32_t *enabled);

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Operations API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Create backup job
 */
int yq_backup_create(yq_backup *backup, yq_backup_job **job);

/*
 * Start backup job
 */
int yq_backup_start(yq_backup *backup, yq_backup_job *job);

/*
 * Stop backup job
 */
int yq_backup_stop(yq_backup *backup, yq_backup_job *job);

/*
 * Cancel backup job
 */
int yq_backup_cancel(yq_backup *backup, yq_backup_job *job);

/*
 * Wait for backup job completion
 */
int yq_backup_wait(yq_backup *backup, yq_backup_job *job, uint32_t timeout_ms);

/*
 * Get backup job status
 */
int yq_backup_get_job_status(yq_backup *backup, yq_backup_job *job, 
                             yq_backup_info *info);

/*
 * List backup jobs
 */
int yq_backup_list_jobs(yq_backup *backup, yq_backup_info **jobs, size_t *count);

/*
 * Delete backup job
 */
int yq_backup_delete_job(yq_backup *backup, yq_backup_job *job);

/*
 * Create full backup
 */
int yq_backup_create_full(yq_backup *backup, const char *filename, 
                          yq_backup_job **job);

/*
 * Create incremental backup
 */
int yq_backup_create_incremental(yq_backup *backup, const char *filename, 
                                  yq_backup_job **job);

/*
 * Create differential backup
 */
int yq_backup_create_differential(yq_backup *backup, const char *filename, 
                                  yq_backup_job **job);

/*
 * Create snapshot backup
 */
int yq_backup_create_snapshot(yq_backup *backup, const char *filename, 
                             yq_backup_job **job);

/* ═══════════════════════════════════════════════════════════════════════
 * Restore Operations API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Create restore job
 */
int yq_restore_create(yq_backup *backup, const char *filename, 
                     yq_restore_job **job);

/*
 * Start restore job
 */
int yq_restore_start(yq_backup *backup, yq_restore_job *job);

/*
 * Stop restore job
 */
int yq_restore_stop(yq_backup *backup, yq_restore_job *job);

/*
 * Cancel restore job
 */
int yq_restore_cancel(yq_backup *backup, yq_restore_job *job);

/*
 * Wait for restore job completion
 */
int yq_restore_wait(yq_backup *backup, yq_restore_job *job, uint32_t timeout_ms);

/*
 * Get restore job status
 */
int yq_restore_get_job_status(yq_backup *backup, yq_restore_job *job, 
                              yq_backup_info *info);

/*
 * List restore jobs
 */
int yq_restore_list_jobs(yq_backup *backup, yq_backup_info **jobs, size_t *count);

/*
 * Delete restore job
 */
int yq_restore_delete_job(yq_backup *backup, yq_restore_job *job);

/*
 * Restore from backup
 */
int yq_restore_from_backup(yq_backup *backup, const char *filename, 
                          const char *target_dir, yq_restore_job **job);

/*
 * Restore with configuration
 */
int yq_restore_with_config(yq_backup *backup, const char *filename, 
                           yq_restore_config *config, yq_restore_job **job);

/* ═══════════════════════════════════════════════════════════════════════
 * Backup Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * List available backups
 */
int yq_backup_list(yq_backup *backup, yq_backup_info **backups, size_t *count);

/*
 * Get backup information
 */
int yq_backup_get_info(yq_backup *backup, const char *filename, 
                      yq_backup_info *info);

/*
 * Delete backup
 */
int yq_backup_delete(yq_backup *backup, const char *filename);

/*
 * Verify backup
 */
int yq_backup_verify(yq_backup *backup, const char *filename, 
                     yq_backup_job **job);

/*
 * Compress backup
 */
int yq_backup_compress(yq_backup *backup, const char *filename, 
                      yq_backup_compression compression, 
                      yq_backup_job **job);

/*
 * Decompress backup
 */
int yq_backup_decompress(yq_backup *backup, const char *filename, 
                        yq_backup_job **job);

/*
 * Encrypt backup
 */
int yq_backup_encrypt(yq_backup *backup, const char *filename, 
                     uint32_t encryption_key, 
                     yq_backup_job **job);

/*
 * Decrypt backup
 */
int yq_backup_decrypt(yq_backup *backup, const char *filename, 
                     uint32_t encryption_key, 
                     yq_backup_job **job);

/*
 * Upload backup to remote storage
 */
int yq_backup_upload(yq_backup *backup, const char *filename, 
                    yq_backup_job **job);

/*
 * Download backup from remote storage
 */
int yq_backup_download(yq_backup *backup, const char *remote_path, 
                      const char *local_filename, 
                      yq_backup_job **job);

/* ═══════════════════════════════════════════════════════════════════════
 * Schedule Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Schedule backup
 */
int yq_backup_schedule(yq_backup *backup, uint32_t interval_ms, 
                       const char *name, yq_backup_job **job);

/*
 * Unschedule backup
 */
int yq_backup_unschedule(yq_backup *backup, const char *name);

/*
 * List scheduled backups
 */
int yq_backup_list_scheduled(yq_backup *backup, yq_backup_info **schedules, 
                            size_t *count);

/*
 * Get next scheduled backup time
 */
int yq_backup_get_next_schedule(yq_backup *backup, uint64_t *next_time);

/*
 * Trigger scheduled backup
 */
int yq_backup_trigger_schedule(yq_backup *backup, const char *name, 
                              yq_backup_job **job);

/* ═══════════════════════════════════════════════════════════════════════
 * Retention Management API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Set retention policy
 */
int yq_backup_set_retention(yq_backup *backup, uint32_t count, uint32_t days);

/*
 * Get retention policy
 */
int yq_backup_get_retention(yq_backup *backup, uint32_t *count, uint32_t *days);

/*
 * Clean expired backups
 */
int yq_backup_clean_expired(yq_backup *backup);

/*
 * List expired backups
 */
int yq_backup_list_expired(yq_backup *backup, yq_backup_info **expired, 
                          size_t *count);

/*
 * Prune old backups
 */
int yq_backup_prune(yq_backup *backup, uint32_t keep_count);

/* ═══════════════════════════════════════════════════════════════════════
 * Utility API
 * ╎═════════════════════════════════════════════════════════════════════ */

/*
 * Get error message
 */
const char *yq_backup_strerror(int error_code);

/*
 * Calculate backup checksum
 */
int yq_backup_checksum(const char *filename, char *checksum, size_t size);

/*
 * Calculate backup size
 */
int yq_backup_size(const char *filename, uint64_t *size);

/*
 * Check backup integrity
 */
int yq_backup_check_integrity(const char *filename);

/*
 * Get backup progress
 */
int yq_backup_get_progress(yq_backup *backup, const char *filename, 
                          uint32_t *progress);

/*
 * Estimate backup size
 */
int yq_backup_estimate_size(yq_backup *backup, uint64_t *estimated_size);

/*
 * Get backup recommendations
 */
int yq_backup_get_recommendations(yq_backup *backup, char **recommendations, 
                                 size_t *count);

/*
 * Validate backup configuration
 */
int yq_backup_validate_config(yq_backup_config *config, uint32_t *valid);

/*
 * Optimize backup performance
 */
int yq_backup_optimize(yq_backup *backup);

/*
 * Get backup health status
 */
int yq_backup_get_health(yq_backup *backup, uint32_t *health_status);

#endif /* YQ_ENABLE_BACKUP */

#ifdef __cplusplus
}
#endif

#endif /* YQ_BACKUP_H */