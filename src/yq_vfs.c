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

#define YQ_MAX_FILE_SIZE (1ULL << 40) /* 1TB limit */
#define YQ_MAX_MMAP_SIZE (1ULL << 30) /* 1GB limit */
#define YQ_MAX_IO_SIZE (1ULL << 31) /* 2GB limit */

struct yq_file {
    HANDLE handle;
    int rdwr;
    uint64_t max_size;
};

static int win32_error(void) {
    DWORD err = GetLastError();
    (void)err;
    return YQ_ERR_IO;
}

yq_file *yq_file_open(const char *path, int create, int rdwr) {
    /* Validate input parameters */
    if (!path) return NULL;
    
    /* Validate path length (Windows API has MAX_PATH limit) */
    if (strlen(path) >= MAX_PATH) {
        return NULL;
    }
    
    /* Validate create and rdwr flags */
    if (create < 0 || create > 1 || rdwr < 0 || rdwr > 1) {
        return NULL;
    }
    
    yq_file *f = (yq_file *)malloc(sizeof(yq_file));
    if (!f) return NULL;
    
    memset(f, 0, sizeof(yq_file));
    f->rdwr = rdwr;
    f->max_size = YQ_MAX_FILE_SIZE;

    DWORD access = rdwr ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    DWORD disp = create ? OPEN_ALWAYS : OPEN_EXISTING;

    f->handle = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f->handle == INVALID_HANDLE_VALUE) {
        free(f);
        return NULL;
    }
    return f;
}

int yq_file_close(yq_file *f) {
    /* Validate input parameters */
    if (!f) return YQ_OK;
    
    /* Validate handle before closing */
    if (f->handle == INVALID_HANDLE_VALUE) {
        free(f);
        return YQ_ERR_INVAL;
    }
    
    CloseHandle(f->handle);
    free(f);
    return YQ_OK;
}

int yq_file_pwrite(yq_file *f, const void *buf, size_t len, uint64_t offset) {
    /* Validate input parameters */
    if (!f || !buf || len == 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate file handle */
    if (f->handle == INVALID_HANDLE_VALUE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size bounds */
    if (len > YQ_MAX_IO_SIZE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate offset bounds */
    if (offset > f->max_size || len > f->max_size - offset) {
        return YQ_ERR_INVAL;
    }
    
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    uint64_t off = offset;

    while (remaining > 0) {
        size_t chunk = remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining;
        
        /* Validate chunk size doesn't exceed remaining space */
        if (off + chunk > f->max_size) {
            chunk = f->max_size - off;
            if (chunk == 0) {
                return YQ_ERR_IO;
            }
        }
        
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
    /* Validate input parameters */
    if (!f || !buf || len == 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate file handle */
    if (f->handle == INVALID_HANDLE_VALUE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size bounds */
    if (len > YQ_MAX_IO_SIZE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate offset bounds */
    if (offset > f->max_size || len > f->max_size - offset) {
        return YQ_ERR_INVAL;
    }
    
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    uint64_t off = offset;

    while (remaining > 0) {
        size_t chunk = remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining;
        
        /* Validate chunk size doesn't exceed remaining space */
        if (off + chunk > f->max_size) {
            chunk = f->max_size - off;
            if (chunk == 0) {
                return YQ_ERR_IO;
            }
        }
        
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
    /* Validate input parameters */
    if (!f) return YQ_ERR_INVAL;
    
    /* Validate file handle */
    if (f->handle == INVALID_HANDLE_VALUE) {
        return YQ_ERR_INVAL;
    }
    
    if (!FlushFileBuffers(f->handle)) {
        return win32_error();
    }
    return YQ_OK;
}

int yq_file_truncate(yq_file *f, uint64_t size) {
    /* Validate input parameters */
    if (!f) return YQ_ERR_INVAL;
    
    /* Validate file handle */
    if (f->handle == INVALID_HANDLE_VALUE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size bounds */
    if (size > f->max_size) {
        return YQ_ERR_INVAL;
    }
    
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
    /* Validate input parameters */
    if (!f) return 0;
    
    /* Validate file handle */
    if (f->handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->handle, &sz)) {
        return 0;
    }
    
    /* Validate size bounds */
    if ((uint64_t)sz.QuadPart > f->max_size) {
        return 0;
    }
    
    return (uint64_t)sz.QuadPart;
}

int yq_file_lock(yq_file *f, int exclusive) {
    /* Validate input parameters */
    if (!f) return YQ_ERR_INVAL;
    
    /* Validate file handle */
    if (f->handle == INVALID_HANDLE_VALUE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate exclusive flag */
    if (exclusive < 0 || exclusive > 1) {
        return YQ_ERR_INVAL;
    }
    
    DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    OVERLAPPED ov = {0};
    if (!LockFileEx(f->handle, flags, 0, 1, 0, &ov)) {
        return win32_error();
    }
    return YQ_OK;
}

int yq_file_unlock(yq_file *f) {
    /* Validate input parameters */
    if (!f) return YQ_ERR_INVAL;
    
    /* Validate file handle */
    if (f->handle == INVALID_HANDLE_VALUE) {
        return YQ_ERR_INVAL;
    }
    
    OVERLAPPED ov = {0};
    if (!UnlockFileEx(f->handle, 0, 1, 0, &ov)) {
        return win32_error();
    }
    return YQ_OK;
}

void *yq_file_mmap(yq_file *f, uint64_t offset, size_t len) {
    /* Validate input parameters */
    if (!f) return NULL;
    
    /* Validate file handle */
    if (f->handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    
    /* Validate size bounds */
    if (len > YQ_MAX_MMAP_SIZE) {
        return NULL;
    }
    
    /* Validate offset bounds */
    if (offset > f->max_size || len > f->max_size - offset) {
        return NULL;
    }
    
    /* Ensure file is at least as large as requested mapping */
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
    /* Validate input parameters */
    if (!ptr) return YQ_ERR_INVAL;
    
    /* Validate size bounds */
    if (len > YQ_MAX_MMAP_SIZE) {
        return YQ_ERR_INVAL;
    }
    
    UnmapViewOfFile(ptr);
    return YQ_OK;
}

#else

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <limits.h>

#define YQ_MAX_FILE_SIZE (1ULL << 40) /* 1TB limit */
#define YQ_MAX_MMAP_SIZE (1ULL << 30) /* 1GB limit */
#define YQ_MAX_IO_SIZE (1ULL << 31) /* 2GB limit */

struct yq_file {
    int fd;
    int rdwr;
    uint64_t max_size;
};

yq_file *yq_file_open(const char *path, int create, int rdwr) {
    /* Validate input parameters */
    if (!path) return NULL;
    
    /* Validate path length */
    if (strlen(path) >= PATH_MAX) {
        return NULL;
    }
    
    /* Validate create and rdwr flags */
    if (create < 0 || create > 1 || rdwr < 0 || rdwr > 1) {
        return NULL;
    }
    
    yq_file *f = (yq_file *)malloc(sizeof(yq_file));
    if (!f) return NULL;
    
    memset(f, 0, sizeof(yq_file));
    f->rdwr = rdwr;
    f->max_size = YQ_MAX_FILE_SIZE;

    int flags = rdwr ? (O_RDWR) : (O_RDONLY);
    if (create) flags |= O_CREAT;
    int mode = 0644;

    f->fd = open(path, flags, mode);
    if (f->fd < 0) {
        free(f);
        return NULL;
    }
    return f;
}

int yq_file_close(yq_file *f) {
    /* Validate input parameters */
    if (!f) return YQ_OK;
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        free(f);
        return YQ_ERR_INVAL;
    }
    
    close(f->fd);
    free(f);
    return YQ_OK;
}

int yq_file_pwrite(yq_file *f, const void *buf, size_t len, uint64_t offset) {
    /* Validate input parameters */
    if (!f || !buf || len == 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size bounds */
    if (len > YQ_MAX_IO_SIZE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate offset bounds */
    if (offset > f->max_size || len > f->max_size - offset) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate offset fits in off_t */
    if (offset > (uint64_t)INT64_MAX) {
        return YQ_ERR_INVAL;
    }
    
    ssize_t ret = pwrite(f->fd, buf, len, (off_t)offset);
    if (ret < 0 || (size_t)ret != len) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_pread(yq_file *f, void *buf, size_t len, uint64_t offset) {
    /* Validate input parameters */
    if (!f || !buf || len == 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size bounds */
    if (len > YQ_MAX_IO_SIZE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate offset bounds */
    if (offset > f->max_size || len > f->max_size - offset) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate offset fits in off_t */
    if (offset > (uint64_t)INT64_MAX) {
        return YQ_ERR_INVAL;
    }
    
    ssize_t ret = pread(f->fd, buf, len, (off_t)offset);
    if (ret < 0 || (size_t)ret != len) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_sync(yq_file *f) {
    /* Validate input parameters */
    if (!f) return YQ_ERR_INVAL;
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        return YQ_ERR_INVAL;
    }
    
    if (fdatasync(f->fd) < 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_truncate(yq_file *f, uint64_t size) {
    /* Validate input parameters */
    if (!f) return YQ_ERR_INVAL;
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size bounds */
    if (size > f->max_size) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size fits in off_t */
    if (size > (uint64_t)INT64_MAX) {
        return YQ_ERR_INVAL;
    }
    
    if (ftruncate(f->fd, (off_t)size) < 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

uint64_t yq_file_size(yq_file *f) {
    /* Validate input parameters */
    if (!f) return 0;
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        return 0;
    }
    
    struct stat st;
    if (fstat(f->fd, &st) < 0) {
        return 0;
    }
    
    /* Validate size bounds */
    if ((uint64_t)st.st_size > f->max_size) {
        return 0;
    }
    
    return (uint64_t)st.st_size;
}

int yq_file_lock(yq_file *f, int exclusive) {
    /* Validate input parameters */
    if (!f) return YQ_ERR_INVAL;
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate exclusive flag */
    if (exclusive < 0 || exclusive > 1) {
        return YQ_ERR_INVAL;
    }
    
    int type = exclusive ? LOCK_EX : LOCK_SH;
    if (flock(f->fd, type) < 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

int yq_file_unlock(yq_file *f) {
    /* Validate input parameters */
    if (!f) return YQ_ERR_INVAL;
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        return YQ_ERR_INVAL;
    }
    
    if (flock(f->fd, LOCK_UN) < 0) {
        return YQ_ERR_IO;
    }
    return YQ_OK;
}

void *yq_file_mmap(yq_file *f, uint64_t offset, size_t len) {
    /* Validate input parameters */
    if (!f) return NULL;
    
    /* Validate file descriptor */
    if (f->fd < 0) {
        return NULL;
    }
    
    /* Validate size bounds */
    if (len > YQ_MAX_MMAP_SIZE) {
        return NULL;
    }
    
    /* Validate offset bounds */
    if (offset > f->max_size || len > f->max_size - offset) {
        return NULL;
    }
    
    /* Validate offset fits in off_t */
    if (offset > (uint64_t)INT64_MAX) {
        return NULL;
    }
    
    int prot = f->rdwr ? (PROT_READ | PROT_WRITE) : (PROT_READ);
    void *ptr = mmap(NULL, len, prot, MAP_SHARED, f->fd, (off_t)offset);
    if (ptr == MAP_FAILED) return NULL;
    return ptr;
}

int yq_file_munmap(void *ptr, size_t len) {
    /* Validate input parameters */
    if (!ptr) return YQ_ERR_INVAL;
    
    /* Validate size bounds */
    if (len > YQ_MAX_MMAP_SIZE) {
        return YQ_ERR_INVAL;
    }
    
    munmap(ptr, len);
    return YQ_OK;
}

#endif
