# yq-DB Backup and Recovery Quick Start Guide

## Quick Setup

### 1. Enable Backup Support

```c
#define YQ_ENABLE_BACKUP 1
#include "yq.h"
#include "yq_backup.h"
```

### 2. Basic Backup

```c
// Initialize backup system
yq_backup_config config = {
    .struct_size = sizeof(yq_backup_config),
    .enabled = 1,
    .backup_type = YQ_BACKUP_TYPE_FULL
};

yq_backup *backup;
yq_backup_init(&config, &backup);

// Create backup
yq_backup_job *job;
yq_backup_create_full(backup, "my_backup.yq", &job);

// Start backup
yq_backup_start(backup, job);

// Wait for completion
yq_backup_wait(backup, job, 0);

// Cleanup
yq_backup_close(backup);
```

### 3. Basic Restore

```c
// Initialize backup system
yq_backup_config config = {
    .struct_size = sizeof(yq_backup_config),
    .enabled = 1
};

yq_backup *backup;
yq_backup_init(&config, &backup);

// Create restore job
yq_restore_job *job;
yq_restore_create(backup, "my_backup.yq", "/restore/path", &job);

// Start restore
yq_restore_start(backup, job);

// Wait for completion
yq_restore_wait(backup, job, 0);

// Cleanup
yq_backup_close(backup);
```

## Common Use Cases

### Daily Full Backup

```c
// Schedule daily backup
yq_backup_schedule(backup, 24 * 60 * 60 * 1000, "daily_backup", &job);
```

### Compressed Backup

```c
yq_backup_config config = {
    .struct_size = sizeof(yq_backup_config),
    .enabled = 1,
    .backup_type = YQ_BACKUP_TYPE_FULL,
    .compression = YQ_BACKUP_COMPRESS_ZSTD
};
```

### Encrypted Backup

```c
yq_backup_config config = {
    .struct_size = sizeof(yq_backup_config),
    .enabled = 1,
    .backup_type = YQ_BACKUP_TYPE_FULL,
    .encryption = YQ_BACKUP_ENCRYPTION_AES256,
    .encrypt_key = 0x1234567890ABCDEF
};
```

### Remote Backup to S3

```c
yq_remote_config s3_config = {
    .struct_size = sizeof(yq_remote_config),
    .enabled = 1,
    .endpoint = "s3://my-bucket/backups",
    .bucket = "my-bucket",
    .path = "backups/",
    .auth_enabled = 1,
    .access_key = "your-access-key",
    .secret_key = "your-secret-key"
};

yq_backup_configure_remote(backup, &s3_config);
```

## Build Instructions

### Compile with Backup Support

```bash
cmake -DYQ_ENABLE_BACKUP=1 .
make
```

### Run Tests

```bash
./yq_test_basic
./yq_test_integration
```

## Next Steps

1. Read the full [BACKUP.md](BACKUP.md) documentation
2. Explore [advanced features](BACKUP.md#advanced-configuration)
3. Learn about [best practices](BACKUP.md#best-practices)
4. Check [troubleshooting guide](BACKUP.md#troubleshooting)