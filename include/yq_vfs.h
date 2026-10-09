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
/*
 * Atomically replace `to` with `from`.
 *
 * Used by checkpoint to swap a freshly written log into place: on POSIX this
 * is rename(2), on Win32 MoveFileEx(REPLACE_EXISTING). The destination is
 * replaced in a single step, so a crash either sees the old log or the new
 * one, never a half-written file.
 *
 * On failure the source file is left behind and the destination is untouched;
 * the caller is responsible for removing the source.
 */
int yq_file_rename(const char *from, const char *to);
int yq_file_lock(yq_file *f, int exclusive);
/*
 * Non-blocking variant of yq_file_lock().
 *
 * Returns YQ_OK when the lock was taken, YQ_ERR_BUSY when another holder has
 * it, YQ_ERR_IO on a real failure. yq_file_lock() blocks until the lock is
 * available, which makes it impossible to honour a lock timeout on top of it.
 */
int yq_file_lock_nb(yq_file *f, int exclusive);
int yq_file_unlock(yq_file *f);
void *yq_file_mmap(yq_file *f, uint64_t offset, size_t len);
int yq_file_munmap(void *ptr, size_t len);

#endif
