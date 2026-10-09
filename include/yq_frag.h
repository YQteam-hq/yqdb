/*
 * yq_frag.h — Memory fragmentation reduction for yq-DB
 *
 * 版本   : 1.0.0
 * 语言   : C11
 * 格式版本: 1（见 FORMAT.md）
 *
 * 设计约束（改动本头文件前请先读）：
 *   1. 本头文件提供内存碎片减少技术
 *   2. 实现内存碎片分析工具
 *   3. 提供自动碎片整理功能
 *   4. 支持多种碎片整理策略
 *   5. 提供碎片统计和监控
 */

#ifndef YQ_FRAG_H
#define YQ_FRAG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 内存碎片配置常量
 * ═══════════════════════════════════════════════════════════════════════ */

#define YQ_FRAG_MAX_BLOCK_SIZE      (1 << 20)  /* 1MB */
#define YQ_FRAG_MIN_BLOCK_SIZE      16
#define YQ_FRAG_ANALYSIS_INTERVAL   300  /* 5分钟 */
#define YQ_FRAG_DEFRAG_THRESHOLD    0.3  /* 30% 碎片率阈值 */
#define YQ_FRAG_MAX_DEFRAG_SIZE     (1 << 22)  /* 4MB */
#define YQ_FRAG_CACHE_SIZE          128

/* ═══════════════════════════════════════════════════════════════════════
 * 内存碎片统计结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_frag_stats {
    size_t total_memory;
    size_t used_memory;
    size_t free_memory;
    size_t fragmented_memory;
    size_t fragmentation_ratio;
    size_t allocation_count;
    size_t free_count;
    size_t defrag_count;
    size_t defrag_success;
    size_t defrag_failed;
    size_t peak_memory;
    size_t average_block_size;
    double average_fragmentation;
    double defrag_efficiency;
} yq_frag_stats;

/* ═══════════════════════════════════════════════════════════════════════
 * 内存块信息结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_frag_block {
    void *address;
    size_t size;
    bool allocated;
    uint64_t timestamp;
    uint64_t access_count;
    struct yq_frag_block *prev;
    struct yq_frag_block *next;
    struct yq_frag_block *buddy;
    struct yq_frag_block *free_list_next;
} yq_frag_block;

/* ═══════════════════════════════════════════════════════════════════════
 * 内存碎片分析器结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_frag_analyzer {
    yq_frag_block *blocks;
    yq_frag_block *free_blocks;
    yq_frag_block *allocated_blocks;
    size_t block_count;
    size_t free_block_count;
    size_t allocated_block_count;
    yq_frag_stats stats;
    uint64_t last_analysis;
    uint64_t analysis_interval;
    bool auto_defrag;
    double defrag_threshold;
    size_t max_defrag_size;
} yq_frag_analyzer;

/* ═══════════════════════════════════════════════════════════════════════
 * 内存碎片整理策略
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    YQ_FRAG_STRATEGY_COMPACT,    /* 紧凑整理 */
    YQ_FRAG_STRATEGY_COALESCE,   /* 合并相邻空闲块 */
    YQ_FRAG_STRATEGY_RECLAIM,    /* 回收碎片 */
    YQ_FRAG_STRATEGY_OPTIMAL      /* 最优整理 */
} yq_frag_strategy;

/* ═══════════════════════════════════════════════════════════════════════
 * 内存碎片函数
 * ═══════════════════════════════════════════════════════════════════════ */

/* 创建碎片分析器 */
yq_frag_analyzer *yq_frag_analyzer_create(size_t total_memory);

/* 销毁碎片分析器 */
void yq_frag_analyzer_destroy(yq_frag_analyzer *analyzer);

/* 注册内存分配 */
int yq_frag_register_alloc(yq_frag_analyzer *analyzer, void *address, size_t size);

/* 注册内存释放 */
int yq_frag_register_free(yq_frag_analyzer *analyzer, void *address, size_t size);

/* 分析内存碎片 */
int yq_frag_analyze(yq_frag_analyzer *analyzer, yq_frag_stats *stats);

/* 执行碎片整理 */
int yq_frag_defrag(yq_frag_analyzer *analyzer, yq_frag_strategy strategy);

/* 设置自动碎片整理 */
void yq_frag_set_auto_defrag(yq_frag_analyzer *analyzer, bool enabled, double threshold, size_t max_size);

/* 获取碎片统计 */
void yq_frag_get_stats(yq_frag_analyzer *analyzer, yq_frag_stats *stats);

/* 重置碎片统计 */
void yq_frag_reset_stats(yq_frag_analyzer *analyzer);

/* 生成碎片报告 */
void yq_frag_generate_report(yq_frag_analyzer *analyzer, FILE *output);

/* ═══════════════════════════════════════════════════════════════════════
 * 内存碎片宏
 * ═══════════════════════════════════════════════════════════════════════ */

/* 快速碎片检查 */
#define yq_frag_check_fragments(analyzer, threshold) \
    (analyzer && analyzer->stats.fragmentation_ratio > threshold)

/* 快速碎片整理 */
#define yq_frag_quick_defrag(analyzer) \
    yq_frag_defrag(analyzer, YQ_FRAG_STRATEGY_COALESCE)

/* ═══════════════════════════════════════════════════════════════════════
 * 内存碎片工具函数
 * ═══════════════════════════════════════════════════════════════════════ */

/* 计算碎片率 */
static inline double yq_frag_calculate_fragmentation_ratio(yq_frag_analyzer *analyzer) {
    if (!analyzer) return 0.0;
    
    if (analyzer->stats.total_memory == 0) return 0.0;
    
    return (double)analyzer->stats.fragmented_memory / analyzer->stats.total_memory;
}

/* 验证内存块 */
static inline bool yq_frag_validate_block(void *address, size_t size) {
    if (!address) return false;
    if (size == 0 || size > YQ_FRAG_MAX_BLOCK_SIZE) return false;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 内存碎片调试
 * ═══════════════════════════════════════════════════════════════════════ */

/* 碎片分析器断言 */
#define YQ_FRAG_ASSERT(analyzer, address) \
    do { \
        if (!(analyzer) || !(address)) { \
            return -1; \
        } \
    } while (0)

/* 碎片分析器边界检查 */
#define YQ_FRAG_BOUND_CHECK(analyzer, address, size) \
    do { \
        if (!(analyzer) || !(address) || (size) == 0 || (size) > YQ_FRAG_MAX_BLOCK_SIZE) { \
            return -1; \
        } \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_FRAG_H */