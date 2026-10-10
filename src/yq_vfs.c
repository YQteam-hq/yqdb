#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "yq_vfs.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

struct yq_file {
    HANDLE handle;
    int rdwr;
};

static int win32_error(void) {
    DWORD err = GetLastError();
    (void)err;
    return YQ_ERR_IO;
}

yq_file *yq_file_open(const char *path, int create, int rdwr) {
    yq_file *f = (yq_file *)malloc(sizeof(yq_file));
    if (!f) return NULL;
    f->rdwr = rdwr;

    DWORD access = rdwr ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    DWORD disp = create ? OPEN_ALWAYS : OPEN_EXISTING;

    /*
     * 必须带 FILE_SHARE_DELETE：checkpoint 用 MoveFileExA(...,
     * MOVEFILE_REPLACE_EXISTING) 替换 <db>.log，而该调用需要先删除目标文件。
     * 若句柄未共享删除权限（旧实现只有 READ|WRITE），替换会被挡下并返回
     * ERROR_SHARING_VIOLATION，checkpoint 随之报 YQ_ERR_IO。
     */
    f->handle = CreateFileA(path, access,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f->handle == INVALID_HANDLE_VALUE) {
        free(f);
        return NULL;
    }
    return f;
}

int yq_file_close(yq_file *f) {
    if (!f) return YQ_OK;
    CloseHandle(f->handle);
    free(f);
    return YQ_OK;
}

int yq_file_pwrite(yq_file *f, const void *buf, size_t len, uint64_t offset) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    uint64_t off = offset;

    while (remaining > 0) {
        size_t chunk = remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining;
        OVERLAPPED ov = {0};
        ov.Offset = (DWORD)(off & 0xFFFFFFFF);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD written = 0;
        if (!WriteFile(f->handle, p, (DWORD)chunk, &written, &ov)) {
            return win32_error();
        }
        p += written;
        off += written;
        remaining -= written;
        if (written == 0) {
            return YQ_ERR_IO;
        }
    }
    return YQ_OK;
}

int yq_file_pread(yq_file *f, void *buf, size_t len, uint64_t offset) {
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    uint64_t off = offset;

    while (remaining > 0) {
        size_t chunk = remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining;
        OVERLAPPED ov = {0};
        ov.Offset = (DWORD)(off & 0xFFFFFFFF);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD read = 0;
        if (!ReadFile(f->handle, p, (DWORD)chunk, &read, &ov)) {
            return win32_error();
        }
        p += read;
        off += read;
        remaining -= read;
        if (read == 0) {
            return YQ_ERR_IO;
        }
    }
    return YQ_OK;
}

int yq_file_sync(yq_file *f) {
    if (!FlushFileBuffers(f->handle)) {
        return win32_error();
    }
    return YQ_OK;
}

int yq_file_truncate(yq_file *f, uint64_t size) {
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(f->handle, pos, NULL, FILE_BEGIN)) {
        return win32_error();
    }
    if (!SetEndOfFile(f->handle)) {
        return win32_error();
    }
    return YQ_OK;
}

uint64_t yq_file_size(yq_file *f) {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->handle, &sz)) {
        return 0;
    }
    return (uint64_t)sz.QuadPart;
}

int yq_file_rename(const char *from, const char *to) {
    if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_lock(yq_file *f, int exclusive) {
    DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    OVERLAPPED ov = {0};
    if (!LockFileEx(f->handle, flags, 0, 1, 0, &ov)) {
        return win32_error();
    }
    return YQ_OK;
}

int yq_file_lock_nb(yq_file *f, int exclusive) {
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (exclusive) flags |= LOCKFILE_EXCLUSIVE_LOCK;
    OVERLAPPED ov = {0};
    if (!LockFileEx(f->handle, flags, 0, 1, 0, &ov)) {
        return win32_error();
    }
    return YQ_OK;
}

int yq_file_unlock(yq_file *f) {
    OVERLAPPED ov = {0};
    if (!UnlockFileEx(f->handle, 0, 1, 0, &ov)) {
        return win32_error();
    }
    return YQ_OK;
}

void *yq_file_mmap(yq_file *f, uint64_t offset, size_t len) {
    LARGE_INTEGER off;
    off.QuadPart = 0;
    SetFilePointerEx(f->handle, off, NULL, FILE_END);
    LARGE_INTEGER cur;
    GetFileSizeEx(f->handle, &cur);
    if (cur.QuadPart < (LONG64)offset + (LONG64)len) {
        LARGE_INTEGER ex;
        ex.QuadPart = (LONG64)offset + (LONG64)len;
        SetFilePointerEx(f->handle, ex, NULL, FILE_BEGIN);
        SetEndOfFile(f->handle);
    }

    HANDLE mapping = CreateFileMappingA(f->handle, NULL,
                                        f->rdwr ? PAGE_READWRITE : PAGE_READONLY,
                                        0, 0, NULL);
    if (!mapping) return NULL;

    LARGE_INTEGER mv_off;
    mv_off.QuadPart = offset;
    void *ptr = MapViewOfFile(mapping,
                              f->rdwr ? FILE_MAP_WRITE : FILE_MAP_READ,
                              (DWORD)(mv_off.HighPart),
                              (DWORD)(mv_off.LowPart),
                              len);
    CloseHandle(mapping);
    if (!ptr) return NULL;
    return ptr;
}

int yq_file_munmap(void *ptr, size_t len) {
    (void)len;
    UnmapViewOfFile(ptr);
    return YQ_OK;
}

#else

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>

struct yq_file {
    int fd;
    int rdwr;
};

yq_file *yq_file_open(const char *path, int create, int rdwr) {
    yq_file *f = (yq_file *)malloc(sizeof(yq_file));
    if (!f) return NULL;
    f->rdwr = rdwr;

    int flags = rdwr ? (O_RDWR) : (O_RDONLY);
    if (create) flags |= O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int mode = 0644;

    f->fd = open(path, flags, mode);
    if (f->fd < 0) {
        free(f);
        return NULL;
    }
    return f;
}

int yq_file_close(yq_file *f) {
    if (!f) return YQ_OK;
    close(f->fd);
    free(f);
    return YQ_OK;
}

int yq_file_pwrite(yq_file *f, const void *buf, size_t len, uint64_t offset) {
    ssize_t ret = pwrite(f->fd, buf, len, (off_t)offset);
    if (ret < 0 || (size_t)ret != len) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_pread(yq_file *f, void *buf, size_t len, uint64_t offset) {
    ssize_t ret = pread(f->fd, buf, len, (off_t)offset);
    if (ret < 0 || (size_t)ret != len) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_sync(yq_file *f) {
    if (fdatasync(f->fd) < 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_truncate(yq_file *f, uint64_t size) {
    if (ftruncate(f->fd, (off_t)size) < 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_rename(const char *from, const char *to) {
    if (rename(from, to) != 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

uint64_t yq_file_size(yq_file *f) {
    struct stat st;
    if (fstat(f->fd, &st) < 0) {
        return 0;
    }
    return (uint64_t)st.st_size;
}

int yq_file_lock(yq_file *f, int exclusive) {
    int type = exclusive ? LOCK_EX : LOCK_SH;
    if (flock(f->fd, type) < 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_lock_nb(yq_file *f, int exclusive) {
    int type = (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
    int rc;
    do {
        rc = flock(f->fd, type);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return YQ_ERR_BUSY;
        }
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_unlock(yq_file *f) {
    if (flock(f->fd, LOCK_UN) < 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

void *yq_file_mmap(yq_file *f, uint64_t offset, size_t len) {
    int prot = f->rdwr ? (PROT_READ | PROT_WRITE) : (PROT_READ);
    void *ptr = mmap(NULL, len, prot, MAP_SHARED, f->fd, (off_t)offset);
    if (ptr == MAP_FAILED) return NULL;
    return ptr;
}

int yq_file_munmap(void *ptr, size_t len) {
    munmap(ptr, len);
    return YQ_OK;
}

#endif
