#ifndef YQ_RECOVER_H
#define YQ_RECOVER_H
#include <stddef.h>
#include <stdint.h>
#include "yq.h"
#include "yq_wal.h"
#include "yq_memtable.h"

typedef struct yq_recover_ctx yq_recover_ctx;

int yq_recover(yq_wal *wal, yq_memtable *mt);
int yq_recover_replay(yq_recover_ctx *ctx);

#endif
