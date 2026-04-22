#include <stdint.h>
#include <stdlib.h>

#define HOP_IMPLEMENTATION
#include "libhop.h"

typedef struct {
    uint32_t allocCount;
    uint32_t freeCount;
} ArenaStats;

static void* ArenaGrow(void* ctx, uint32_t minSize, uint32_t* outSize) {
    ArenaStats* stats = (ArenaStats*)ctx;
    uint32_t    size = minSize < 128u ? 128u : minSize;
    void*       p = malloc((size_t)size);
    if (p == NULL) {
        *outSize = 0;
        return NULL;
    }
    stats->allocCount++;
    *outSize = size;
    return p;
}

static void ArenaFree(void* ctx, void* block, uint32_t blockSize) {
    ArenaStats* stats = (ArenaStats*)ctx;
    (void)blockSize;
    free(block);
    stats->freeCount++;
}

int main(void) {
    uint8_t    storage[32];
    ArenaStats stats = { 0 };
    HOPArena    arena;
    void*      p0;
    void*      p1;
    void*      p2;

    HOPArenaInitEx(&arena, storage, (uint32_t)sizeof(storage), &stats, ArenaGrow, ArenaFree);

    p0 = HOPArenaAlloc(&arena, 16u, 8u);
    p1 = HOPArenaAlloc(&arena, 128u, 8u);
    if (p0 == NULL || p1 == NULL || stats.allocCount == 0) {
        return 1;
    }

    HOPArenaReset(&arena);
    p2 = HOPArenaAlloc(&arena, 64u, 8u);
    if (p2 == NULL) {
        return 2;
    }

    HOPArenaDispose(&arena);
    if (stats.freeCount != stats.allocCount) {
        return 3;
    }
    return 0;
}
