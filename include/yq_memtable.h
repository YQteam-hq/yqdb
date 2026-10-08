#ifndef YQ_MEMTABLE_H
#define YQ_MEMTABLE_H
#include <stddef.h>
#include <stdint.h>
#include "yq.h"

typedef struct yq_memtable yq_memtable;
typedef struct yq_memtable_iter yq_memtable_iter;

yq_memtable *yq_memtable_create(size_t max_bytes);
void yq_memtable_destroy(yq_memtable *mt);
int yq_memtable_put(yq_memtable *mt, yq_slice key, yq_slice val);
int yq_memtable_del(yq_memtable *mt, yq_slice key);
int yq_memtable_get(yq_memtable *mt, yq_slice key, yq_slice *out);
int yq_memtable_size(yq_memtable *mt);
int yq_memtable_full(yq_memtable *mt);
size_t yq_memtable_bytes(yq_memtable *mt);
int yq_memtable_iter_open(yq_memtable *mt, yq_memtable_iter **out);
void yq_memtable_iter_close(yq_memtable_iter *it);
int yq_memtable_iter_first(yq_memtable_iter *it);
int yq_memtable_iter_next(yq_memtable_iter *it);
int yq_memtable_iter_last(yq_memtable_iter *it);
int yq_memtable_iter_prev(yq_memtable_iter *it);
int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out);
int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out);
int yq_memtable_iter_valid(yq_memtable_iter *it);

/*
 * 清空内存表并复用 arena。
 *
 * 返回 YQ_OK 成功；YQ_ERR_NOMEM 表示重建跳表头失败（内存表进入空且不可用
 * 状态，调用方应放弃该表）。
 *
 * 语义变化提示：reset 会使此前打开的所有迭代器失效。失效后的迭代器
 * first/next/last/prev/key/val 返回 YQ_ERR_CURSOR，iter_valid 返回 0，
 * 不会读到 reset 之前已丢弃的数据。原因见实现处的 gen 说明。
 */
int yq_memtable_reset(yq_memtable *mt);

#endif
