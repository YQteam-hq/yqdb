/*
 * yq_security.h — Security utilities for yq-DB
 *
 * 版本   : 1.0.0
 * 语言   : C11
 * 格式版本: 1（见 FORMAT.md）
 *
 * 设计约束（改动本头文件前请先读）：
 *   1. 本头文件提供安全相关的工具函数和宏
 *   2. 包含内存损坏检测、安全内存清零、输入验证等功能
 *   3. 所有函数都是线程安全的
 *   4. 不暴露任何内部实现细节
 */

#ifndef YQ_SECURITY_H
#define YQ_SECURITY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 安全常量
 * ═══════════════════════════════════════════════════════════════════════ */

#define YQ_SECURITY_CANARY_VALUE    0xDEADBEEF
#define YQ_SECURITY_MAGIC_VALUE     0xCAFEBABE
#define YQ_SECURITY_MAX_STACK_DEPTH  1024
#define YQ_SECURITY_MAX_RECURSION   1000

/* ═══════════════════════════════════════════════════════════════════════
 * 内存损坏检测
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_security_canary {
    uint32_t canary;
    uint32_t magic;
    uint32_t size;
    uint32_t reserved[3];
} yq_security_canary;

/* 初始化安全canary */
static inline void yq_security_canary_init(yq_security_canary *canary, uint32_t size) {
    if (!canary) return;
    canary->canary = YQ_SECURITY_CANARY_VALUE;
    canary->magic = YQ_SECURITY_MAGIC_VALUE;
    canary->size = size;
}

/* 检查canary是否被破坏 */
static inline int yq_security_canary_check(const yq_security_canary *canary) {
    if (!canary) return 0;
    return canary->canary == YQ_SECURITY_CANARY_VALUE && 
           canary->magic == YQ_SECURITY_MAGIC_VALUE;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 安全内存清零
 * ═══════════════════════════════════════════════════════════════════════ */

/* 安全清零内存块 */
static inline void yq_security_zero(void *ptr, size_t len) {
    if (!ptr || len == 0) return;
    if (len > SIZE_MAX / 2) return; /* 防止整数溢出 */
    
    /* 使用volatile防止编译器优化 */
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) *p++ = 0;
}

/* 安全清零结构体 */
static inline void yq_security_zero_struct(void *ptr, size_t size) {
    if (!ptr || size == 0) return;
    yq_security_zero(ptr, size);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 输入验证
 * ═══════════════════════════════════════════════════════════════════════ */

/* 验证指针是否对齐 */
static inline int yq_security_is_aligned(const void *ptr, size_t alignment) {
    if (!ptr || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return 0;
    }
    return ((uintptr_t)ptr & (alignment - 1)) == 0;
}

/* 验证内存范围是否有效 */
static inline int yq_security_validate_range(const void *ptr, size_t size) {
    if (!ptr) return 0;
    if (size == 0) return 1; /* 空范围是有效的 */
    
    /* 检查整数溢出 */
    if (size > SIZE_MAX - (uintptr_t)ptr) {
        return 0;
    }
    
    return 1;
}

/* 验证字符串长度 */
static inline int yq_security_validate_string(const char *str, size_t max_len) {
    if (!str) return 0;
    if (max_len == 0) return 0;
    
    /* 检查字符串长度是否超过最大值 */
    if (strlen(str) >= max_len) {
        return 0;
    }
    
    return 1;
}

/* 验证slice结构 */
static inline int yq_security_validate_slice(const void *data, size_t size, size_t max_size) {
    if (!data) {
        return size == 0; /* 空slice是有效的 */
    }
    if (size > max_size) {
        return 0;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 线程安全检查
 * ═══════════════════════════════════════════════════════════════════════ */

/* 检查递归深度 */
static inline int yq_security_check_recursion(int depth) {
    if (depth < 0 || depth > YQ_SECURITY_MAX_RECURSION) {
        return 0;
    }
    return 1;
}

/* 检查栈深度 */
static inline int yq_security_check_stack(int depth) {
    if (depth < 0 || depth > YQ_SECURITY_MAX_STACK_DEPTH) {
        return 0;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 错误处理安全
 * ═══════════════════════════════════════════════════════════════════════ */

/* 安全的错误信息缓冲区 */
#define YQ_SECURITY_ERROR_MAX 256

/* 安全的错误信息处理 */
static inline void yq_security_safe_error(char *buffer, size_t buffer_size, const char *error) {
    if (!buffer || buffer_size == 0 || !error) {
        return;
    }
    
    /* 限制错误信息长度 */
    size_t error_len = strlen(error);
    if (error_len >= buffer_size) {
        error_len = buffer_size - 1;
    }
    
    /* 复制错误信息 */
    memcpy(buffer, error, error_len);
    buffer[error_len] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════
 * 内存分配安全
 * ═══════════════════════════════════════════════════════════════════════ */

/* 检查内存分配大小 */
static inline int yq_security_check_allocation_size(size_t size) {
    if (size == 0) return 0; /* 零大小分配通常是不安全的 */
    if (size > SIZE_MAX / 2) return 0; /* 防止整数溢出 */
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 位操作安全
 * ═══════════════════════════════════════════════════════════════════════ */

/* 安全的位移操作 */
static inline int yq_security_safe_shift(size_t value, int shift) {
    if (shift < 0) return 0; /* 负位移不安全 */
    if (shift >= (int)(sizeof(size_t) * 8)) return 0; /* 过度位移 */
    
    size_t result = value << shift;
    if (result >> shift != value) {
        return 0; /* 溢出检测 */
    }
    
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 宏定义
 * ═══════════════════════════════════════════════════════════════════════ */

/* 安全的内存分配 */
#define YQ_SECURITY_SAFE_ALLOC(ptr, size) \
    do { \
        if (!yq_security_check_allocation_size(size)) { \
            return YQ_ERR_INVAL; \
        } \
        ptr = malloc(size); \
        if (!ptr) { \
            return YQ_ERR_NOMEM; \
        } \
    } while (0)

/* 安全的内存释放 */
#define YQ_SECURITY_SAFE_FREE(ptr) \
    do { \
        if (ptr) { \
            yq_security_zero(ptr, sizeof(*(ptr))); \
            free(ptr); \
            ptr = NULL; \
        } \
    } while (0)

/* 安全的内存复制 */
#define YQ_SECURITY_SAFE_COPY(dest, src, size) \
    do { \
        if (!yq_security_validate_range(src, size) || !yq_security_validate_range(dest, size)) { \
            return YQ_ERR_INVAL; \
        } \
        memcpy(dest, src, size); \
    } while (0)

/* 安全的字符串复制 */
#define YQ_SECURITY_SAFE_STR(dest, src, max_len) \
    do { \
        if (!yq_security_validate_string(src, max_len)) { \
            return YQ_ERR_INVAL; \
        } \
        strncpy(dest, src, max_len - 1); \
        dest[max_len - 1] = '\0'; \
    } while (0)

/* 安全的输入验证 */
#define YQ_SECURITY_VALIDATE_INPUT(ptr, size, max_size) \
    do { \
        if (!yq_security_validate_range(ptr, size) || size > max_size) { \
            return YQ_ERR_INVAL; \
        } \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_SECURITY_H */