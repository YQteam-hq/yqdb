#ifndef YQ_VFS_H
#define YQ_VFS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "yq.h"

typedef struct yq_file yq_file;

yq_file *yq_file_open(const char *path, int create, int rdwr);
int yq_file_close(yq_file *f);
int yq_file_pwrite(yq_file *f, const void *buf, size_t len, uint64_t offset);
int yq_file_pread(yq_file *f, void *buf, size_t len, uint64_t offset);
int yq_file_sync(yq_file *f);
int yq_file_truncate(yq_file *f, uint64_t size);
uint64_t yq_file_size(yq_file *f);
int yq_file_lock(yq_file *f, int exclusive);
int yq_file_unlock(yq_file *f);
void *yq_file_mmap(yq_file *f, uint64_t offset, size_t len);
int yq_file_munmap(void *ptr, size_t len);

#endif
