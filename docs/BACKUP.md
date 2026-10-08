# yq-DB Backup and Recovery System

## Overview

The yq-DB Backup and Recovery System provides comprehensive backup and restore capabilities for your yq-DB databases. It supports multiple backup types, compression algorithms, encryption methods, and storage options.

## Features

### Backup Types
- **Full Backup**: Complete database backup
- **Incremental Backup**: Only changed data since last backup
- **Differential Backup**: Changes since last full backup
- **Snapshot Backup**: Point-in-time snapshot

### Compression Options
- No compression
- Snappy compression
- LZ4 compression
- Zstandard compression
- Zlib compression
- Gzip compression

### Encryption Options
- No encryption
- AES-256 encryption
- ChaCha20 encryption
- XOR encryption

### Storage Options
- Local file system
- Remote storage (S3, Azure, GCS)
- FTP server
- SFTP server

## Quick Start

### Basic Backup

```c
#include "yq.h"
#include "yq_backup.h"

int main() {
    // Enable backup support
    #define YQ_ENABLE_BACKUP 1
    #include "yq.h"
    
    // Initialize backup system
    yq_backup_config config = {
        .struct_size = sizeof(yq_backup_config),
        .enabled = 1,
        .auto_backup = 1,
        .backup_type = YQ_BACKUP_TYPE_FULL,
        .compression = YQ_BACKUP_COMPRESS_SNAPPY,
        .encryption = YQ_BACKUP_ENCRYPTION_NONE,
        .storage_type = YQ_BACKUP_STORAGE_LOCAL
    };
    
    yq_backup *backup;
    yq_backup_init(&config, &backup);
    
    // Create backup job
    yq_backup_job *job;
    yq_backup_create_full(backup, "/path/to/backup.yq", &job);
    
    // Start backup
    yq_backup_start(backup, job);
    
    // Wait for completion
    yq_backup_wait(backup, job, 0);
    
    // Cleanup
    yq_backup_close(backup);
    
    return 0;
}
```

### Basic Restore

```c
int main() {
    // Initialize backup system
    yq_backup_config config = {
        .struct_size = sizeof(yq_backup_config),
        .enabled = 1
    };
    
    yq_backup *backup;
    yq_backup_init(&config, &backup);
    
    // Create restore job
    yq_restore_job *job;
    yq_restore_create(backup, "/path/to/backup.yq", "/path/to/restore", &job);
    
    // Start restore
    yq_restore_start(backup, job);
    
    // Wait for completion
    yq_restore_wait(backup, job, 0);
    
    // Cleanup
    yq_backup_close(backup);
    
    return 0;
}
```

## Advanced Configuration

### Remote Storage Configuration

```c
yq_remote_config remote_config = {
    .struct_size = sizeof(yq_remote_config),
    .enabled = 1,
    .endpoint = "s3://my-bucket/backups",
    .bucket = "my-bucket",
    .path = "backups/",
    .auth_enabled = 1,
    .access_key = "your-access-key",
    .secret_key = "your-secret-key"
};

yq_backup_configure_remote(backup, &remote_config);
```

### Schedule Configuration

```c
// Schedule backup every 24 hours
yq_backup_schedule(backup, 24 * 60 * 60 * 1000, "daily_backup", &job);
```

### Retention Policy

```c
// Keep last 7 backups or 30 days, whichever comes first
yq_backup_set_retention(backup, 7, 30);
```

## API Reference

### Core Functions

- `yq_backup_init()` - Initialize backup system
- `yq_backup_close()` - Shutdown backup system
- `yq_backup_configure()` - Configure backup settings
- `yq_backup_get_stats()` - Get backup statistics

### Backup Operations

- `yq_backup_create_full()` - Create full backup
- `yq_backup_create_incremental()` - Create incremental backup
- `yq_backup_create_differential()` - Create differential backup
- `yq_backup_create_snapshot()` - Create snapshot backup

### Restore Operations

- `yq_restore_create()` - Create restore job
- `yq_restore_start()` - Start restore operation
- `yq_restore_wait()` - Wait for restore completion

### Job Management

- `yq_backup_get_job_status()` - Get job status
- `yq_backup_list_jobs()` - List all jobs
- `yq_backup_cancel()` - Cancel job

### Utility Functions

- `yq_backup_checksum()` - Calculate backup checksum
- `yq_backup_size()` - Get backup size
- `yq_backup_check_integrity()` - Check backup integrity

## Error Handling

```c
int result = yq_backup_create_full(backup, "/path/to/backup.yq", &job);
if (result != YQ_BACKUP_OK) {
    printf("Backup failed: %s\n", yq_backup_strerror(result));
    return 1;
}
```

## Performance Considerations

- Use compression to reduce backup size
- Use encryption for sensitive data
- Schedule backups during low-traffic periods
- Monitor backup statistics regularly

## Best Practices

1. **Regular Backups**: Schedule regular backups based on your data change rate
2. **Multiple Copies**: Keep multiple backup copies in different locations
3. **Testing**: Regularly test restore procedures
4. **Monitoring**: Monitor backup success rates and performance
5. **Retention**: Set appropriate retention policies to balance space and recovery needs

## Troubleshooting

### Common Issues

1. **Backup Fails**: Check disk space and permissions
2. **Restore Fails**: Verify backup integrity and target directory
3. **Performance Issues**: Adjust compression and encryption settings

### Debug Information

Enable debug logging by setting the log level in the configuration.

## Integration with Other Features

The backup system integrates seamlessly with other yq-DB features:

- **Compression**: Built-in compression support
- **Encryption**: Optional encryption for security
- **Monitoring**: Backup statistics available through monitoring API
- **Pub/Sub**: Backup events can be published to topics

## Example: Complete Backup Workflow

```c
#include "yq.h"
#include "yq_backup.h"

int main() {
    // Enable backup support
    #define YQ_ENABLE_BACKUP 1
    #include "yq.h"
    
    // Initialize backup system
    yq_backup_config config = {
        .struct_size = sizeof(yq_backup_config),
        .enabled = 1,
        .auto_backup = 1,
        .backup_type = YQ_BACKUP_TYPE_FULL,
        .compression = YQ_BACKUP_COMPRESS_ZSTD,
        .encryption = YQ_BACKUP_ENCRYPTION_AES256,
        .storage_type = YQ_BACKUP_STORAGE_LOCAL,
        .verify_after_backup = 1
    };
    
    yq_backup *backup;
    if (yq_backup_init(&config, &backup) != YQ_BACKUP_OK) {
        printf("Failed to initialize backup system\n");
        return 1;
    }
    
    // Create and start backup job
    yq_backup_job *job;
    if (yq_backup_create_full(backup, "/tmp/my_database_backup.yq", &job) != YQ_BACKUP_OK) {
        printf("Failed to create backup job\n");
        yq_backup_close(backup);
        return 1;
    }
    
    if (yq_backup_start(backup, job) != YQ_BACKUP_OK) {
        printf("Failed to start backup job\n");
        yq_backup_close(backup);
        return 1;
    }
    
    // Wait for completion with progress monitoring
    yq_backup_info info;
    while (1) {
        yq_backup_get_job_status(backup, job, &info);
        printf("Backup progress: %d%%\n", info.progress);
        
        if (info.status == YQ_BACKUP_STATUS_COMPLETED) {
            printf("Backup completed successfully\n");
            break;
        } else if (info.status == YQ_BACKUP_STATUS_FAILED) {
            printf("Backup failed\n");
            break;
        }
        
        sleep(1);
    }
    
    // Verify backup
    if (yq_backup_check_integrity("/tmp/my_database_backup.yq") != YQ_BACKUP_OK) {
        printf("Backup integrity check failed\n");
    } else {
        printf("Backup integrity verified\n");
    }
    
    // Get backup statistics
    yq_backup_stats stats;
    yq_backup_get_stats(backup, &stats);
    printf("Total backups: %lu\n", stats.total_backups);
    printf("Successful backups: %lu\n", stats.successful_backups);
    
    // Cleanup
    yq_backup_close(backup);
    
    return 0;
}
```

This comprehensive backup and recovery system ensures your yq-DB data is safe and can be restored when needed.