/*
 * Surface Memory Recycling Engine
 *
 * Prototype user-mode memory pool.
 * Designed as a foundation for:
 *  - buffer reuse
 *  - memory pressure management
 *  - hot/cold allocations
 *  - fragmentation reduction
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#define MAX_BLOCKS          1024
#define MIN_BLOCK_SIZE      4096
#define MAX_BLOCK_SIZE      (64 * 1024 * 1024)

typedef enum {
    MEMORY_HOT,
    MEMORY_WARM,
    MEMORY_COLD
} MemoryTemperature;

typedef struct {
    void *address;
    size_t size;

    bool allocated;
    bool reusable;

    uint64_t last_used;
    uint32_t use_count;

    MemoryTemperature temperature;
} MemoryBlock;

typedef struct {
    MemoryBlock blocks[MAX_BLOCKS];

    size_t total_reserved;
    size_t total_used;
    size_t total_recycled;

    CRITICAL_SECTION lock;
} MemoryRecycler;


/* ---------------------------------------------------------
 * Initialization
 * --------------------------------------------------------- */

void recycler_init(MemoryRecycler *r)
{
    ZeroMemory(r, sizeof(MemoryRecycler));
    InitializeCriticalSection(&r->lock);
}


/* ---------------------------------------------------------
 * Allocate memory
 * --------------------------------------------------------- */

void *recycler_alloc(
    MemoryRecycler *r,
    size_t size)
{
    if (size < MIN_BLOCK_SIZE ||
        size > MAX_BLOCK_SIZE)
        return NULL;

    EnterCriticalSection(&r->lock);

    /*
     * First attempt:
     * recycle an existing unused block.
     */
    for (int i = 0; i < MAX_BLOCKS; i++) {

        MemoryBlock *b = &r->blocks[i];

        if (!b->allocated &&
            b->reusable &&
            b->size >= size) {

            b->allocated = true;
            b->last_used = GetTickCount64();
            b->use_count++;

            r->total_used += b->size;

            LeaveCriticalSection(&r->lock);

            return b->address;
        }
    }

    /*
     * No reusable block.
     * Allocate a new region.
     */

    void *memory = VirtualAlloc(
        NULL,
        size,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );

    if (!memory) {
        LeaveCriticalSection(&r->lock);
        return NULL;
    }

    for (int i = 0; i < MAX_BLOCKS; i++) {

        MemoryBlock *b = &r->blocks[i];

        if (b->address == NULL) {

            b->address = memory;
            b->size = size;
            b->allocated = true;
            b->reusable = true;

            b->last_used = GetTickCount64();
            b->use_count = 1;
            b->temperature = MEMORY_HOT;

            r->total_reserved += size;
            r->total_used += size;

            break;
        }
    }

    LeaveCriticalSection(&r->lock);

    return memory;
}


/* ---------------------------------------------------------
 * Release memory back into recycler
 * --------------------------------------------------------- */

void recycler_free(
    MemoryRecycler *r,
    void *address)
{
    if (!address)
        return;

    EnterCriticalSection(&r->lock);

    for (int i = 0; i < MAX_BLOCKS; i++) {

        MemoryBlock *b = &r->blocks[i];

        if (b->address == address) {

            b->allocated = false;
            b->last_used = GetTickCount64();
            b->temperature = MEMORY_COLD;

            if (r->total_used >= b->size)
                r->total_used -= b->size;

            break;
        }
    }

    LeaveCriticalSection(&r->lock);
}


/* ---------------------------------------------------------
 * Recycle cold memory
 * --------------------------------------------------------- */

size_t recycler_trim(
    MemoryRecycler *r,
    uint64_t idle_ms)
{
    size_t recycled = 0;
    uint64_t now = GetTickCount64();

    EnterCriticalSection(&r->lock);

    for (int i = 0; i < MAX_BLOCKS; i++) {

        MemoryBlock *b = &r->blocks[i];

        if (!b->address ||
            b->allocated)
            continue;

        if ((now - b->last_used) < idle_ms)
            continue;

        /*
         * Return physical pages to Windows.
         */

        if (VirtualFree(
                b->address,
                0,
                MEM_RELEASE)) {

            recycled += b->size;

            b->address = NULL;
            b->size = 0;
            b->reusable = false;
            b->temperature = MEMORY_COLD;
        }
    }

    r->total_recycled += recycled;

    LeaveCriticalSection(&r->lock);

    return recycled;
}


/* ---------------------------------------------------------
 * Memory statistics
 * --------------------------------------------------------- */

void recycler_stats(
    MemoryRecycler *r)
{
    EnterCriticalSection(&r->lock);

    printf("\n--- Surface Memory Recycler ---\n");
    printf("Reserved : %zu MB\n",
           r->total_reserved / (1024 * 1024));

    printf("Used     : %zu MB\n",
           r->total_used / (1024 * 1024));

    printf("Recycled : %zu MB\n",
           r->total_recycled / (1024 * 1024));

    LeaveCriticalSection(&r->lock);
}


/* ---------------------------------------------------------
 * Shutdown
 * --------------------------------------------------------- */

void recycler_shutdown(
    MemoryRecycler *r)
{
    EnterCriticalSection(&r->lock);

    for (int i = 0; i < MAX_BLOCKS; i++) {

        if (r->blocks[i].address) {

            VirtualFree(
                r->blocks[i].address,
                0,
                MEM_RELEASE
            );

            r->blocks[i].address = NULL;
        }
    }

    LeaveCriticalSection(&r->lock);

    DeleteCriticalSection(&r->lock);
}



