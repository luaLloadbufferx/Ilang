/**
 * @file IlangVM_Mem.c
 * @brief Ilang virtual machine main memory manager (implementation of IlangVM_Mem.h).
 *
 * @section mem_table Block table invariant
 * `blocks` is always sorted by address (strictly increasing), so a block is found with a binary search instead of a
 * linear scan.
 *
 * Freeing does not remove an entry from the middle of the table (a memmove of the whole tail per free would be
 * quadratic when many blocks are live and freed out of order). The entry stays as a tombstone (size 0, data NULL): it
 * never matches a lookup and keeps its address, so the order is preserved. A freed LAST entry is simply dropped, and
 * the table is compacted once more than half of it is dead.
 *
 * Normally `next_address` only grows and new blocks are appended. The virtual address space is only 32 bits, though,
 * so a program that allocates and frees a lot (string temporaries!) would eventually run out of fresh addresses even
 * though almost nothing is live. When that happens memory_reserve() falls back to memory_find_gap(), which reuses the
 * space of freed blocks and inserts the new block at its sorted position.
 *
 * @section mem_pool Small-buffer pool
 * Small host buffers are recycled through per-size-class free lists instead of going through calloc/free every time.
 * Class `c` holds buffers of exactly `(c + 1) * 16` bytes. A recycled buffer is zeroed again (VM memory starts zeroed).
 *
 * Address 0 means "failure / null" and is never handed out, so `ILANG_VM_MEMORY_START` must be non-zero.
 */

/* Required */
#include "IlangVM_Mem.h"
#include "IlangConf.h"

#include <stdlib.h>
#include <string.h>

/* The tunables (ILANG_VM_MEMORY_LIMIT, ILANG_VM_MEMORY_INITIAL_CAPACITY, ILANG_VM_MEMORY_START and the pool sizes
   ILANG_VM_POOL_CLASSES / ILANG_VM_POOL_MAX_PER_CLASS) are all defined in IlangConf.h. Address 0 means "failure / null"
   and is never handed out, so ILANG_VM_MEMORY_START must be non-zero. */

/*
 * Block table invariant: `blocks` is always sorted by address (strictly increasing), so a block is found with a binary
 * search instead of a linear scan.
 *
 * Freeing does not remove an entry from the middle of the table (a memmove of the whole tail per free would be
 * quadratic when many blocks are live and freed out of order). The entry stays as a tombstone (size 0, data NULL): it
 * never matches a lookup and keeps its address, so the order is preserved. A freed LAST entry is simply dropped, and the
 * table is compacted once more than half of it is dead.
 *
 * Normally next_address only grows and new blocks are appended. The virtual address space is only 32 bits, though, so
 * a program that allocates and frees a lot (string temporaries!) would eventually run out of fresh addresses even
 * though almost nothing is live. When that happens memory_reserve() falls back to memory_find_gap(), which reuses the
 * space of freed blocks and inserts the new block at its sorted position.
 */


/* Small host buffers are recycled through per-size-class free lists instead of going through calloc/free every time.
 * Class c holds buffers of exactly (c + 1) * 16 bytes. A recycled buffer is zeroed again (VM memory starts zeroed). */
/** @brief Head of the free list of each size class (the first bytes of a free buffer hold the next pointer). */
static void*    pool_head[ILANG_VM_POOL_CLASSES];
/** @brief Number of buffers currently in each free list. */
static uint32_t pool_count[ILANG_VM_POOL_CLASSES];

/**
 * @brief Returns a zeroed host buffer of `size` bytes, taken from the pool when it is small enough.
 * @param size Buffer size in bytes; must not be 0 (memory_reserve() rejects empty blocks before it gets here).
 * @return The buffer, or NULL if the host is out of memory.
 */
static uint8_t* host_alloc_zeroed(uint32_t size) {
    if (size <= ILANG_VM_POOL_CLASSES * 16u) {
        uint32_t c = (size - 1u) >> 4;
        void* p = pool_head[c];
        if (p != NULL) {
            pool_head[c] = *(void**)p;
            pool_count[c]--;
            memset(p, 0, (size_t)(c + 1u) * 16u);
            return (uint8_t*)p;
        }
        return (uint8_t*)calloc((size_t)(c + 1u) * 16u, 1);
    }
    return (uint8_t*)calloc(size, 1);
}

/**
 * @brief Gives a buffer back to the pool if its size class has room, otherwise to `free()`.
 * @param data Buffer to release (may be NULL).
 * @param size The size it was allocated with.
 */
static void host_free(uint8_t* data, uint32_t size) {
    if (data == NULL) return;
    if (size <= ILANG_VM_POOL_CLASSES * 16u && size > 0) {
        uint32_t c = (size - 1u) >> 4;
        if (pool_count[c] < ILANG_VM_POOL_MAX_PER_CLASS) {
            *(void**)data = pool_head[c];
            pool_head[c] = data;
            pool_count[c]++;
            return;
        }
    }
    free(data);
}

/**
 * @brief Rounds up to a multiple of 4. Fails instead of wrapping around.
 * @param[in]  value The value to round.
 * @param[out] out   Receives the rounded value.
 * @return false if the result does not fit in 32 bits.
 */
static bool align4(uint32_t value, uint32_t* out) {
    if (value > UINT32_MAX - 3u) {
        return false;
    }

    *out = (value + 3u) & ~3u;
    return true;
}

/**
 * @brief Doubles the capacity of the block table (or creates it).
 * @param[in,out] memory The memory manager.
 * @return false if the table cannot grow (overflow or out of host memory); the table is unchanged.
 */
static bool memory_grow_blocks(iMemory* memory) {
    uint32_t new_capacity;
    size_t bytes;
    iMemoryBlock* new_blocks;

    if (memory->block_capacity == 0) {
        new_capacity = ILANG_VM_MEMORY_INITIAL_CAPACITY;
    }
    else {
        if (memory->block_capacity > UINT32_MAX / 2u) {
            return false;
        }
        new_capacity = memory->block_capacity * 2u;
    }

    /* Byte size of the new table; detect wrap-around (only possible where size_t is 32-bit). */
    bytes = (size_t)new_capacity * sizeof(iMemoryBlock);

    if (bytes / sizeof(iMemoryBlock) != new_capacity) {
        return false;
    }

    new_blocks = (iMemoryBlock*)realloc(memory->blocks, bytes);

    if (new_blocks == NULL) {
        return false;
    }

    memory->blocks = new_blocks;
    memory->block_capacity = new_capacity;

    return true;
}

/**
 * @brief Binary search: finds the block that contains `address`. Updates the `last_block` lookup cache on a hit.
 * @param[in,out] memory  The memory manager.
 * @param[in]     address Any virtual address.
 * @return The block, or NULL.
 */
static iMemoryBlock* memory_find_block(iMemory* memory, uint32_t address) {
    uint32_t lo = 0;
    uint32_t hi = memory->block_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        iMemoryBlock* block = &memory->blocks[mid];

        if (address < block->address) {
            hi = mid;
        }
        else if (address - block->address >= block->size) {
            lo = mid + 1u;
        }
        else {
            memory->last_block = mid; /* see memory_ptr_fast() */
            return block;
        }
    }

    return NULL;
}

/**
 * @brief Finds the block that fully contains `[address, address + size)`.
 *
 * An access may NOT straddle two blocks.
 *
 * @param[in,out] memory  The memory manager.
 * @param[in]     address First virtual address.
 * @param[in]     size    Length of the range in bytes.
 * @param[out]    offset  On success, the position of `address` inside the block.
 * @return The block, or NULL if `size` is 0 or the range is not inside one live block.
 */
static iMemoryBlock* memory_find_range(iMemory* memory, uint32_t address,
    uint32_t size, uint32_t* offset) {
    iMemoryBlock* block;
    uint32_t off;

    if (memory == NULL || size == 0) {
        return NULL;
    }

    block = memory_find_block(memory, address);
    if (block == NULL) {
        return NULL;
    }

    off = address - block->address; /* always < block->size */

    if (size > block->size - off) {
        return NULL;
    }

    *offset = off;
    return block;
}

/**
 * @brief Drops the tombstones (freed entries), keeping the order.
 *
 * O(block_count); called only when more than half the table is dead, so a free costs O(1) amortised instead of a
 * memmove of the whole tail.
 *
 * @param[in,out] memory The memory manager.
 */
static void memory_compact(iMemory* memory) {
    uint32_t w = 0;

    for (uint32_t r = 0; r < memory->block_count; r++) {
        if (memory->blocks[r].size != 0) {
            if (w != r) memory->blocks[w] = memory->blocks[r];
            w++;
        }
    }

    memory->block_count = w;
    memory->dead = 0;
    memory->last_block = 0;
}

/**
 * @brief Finds the first hole of `aligned_size` bytes between live blocks (or after the last one).
 *
 * Compacts the table first, so that it contains live blocks only.
 *
 * @param[in,out] memory       The memory manager.
 * @param[in]     aligned_size Requested size, already rounded up to a multiple of 4.
 * @param[out]    index        The table index the new block must take.
 * @return The address of the hole, or 0 if there is none.
 */
static uint32_t memory_find_gap(iMemory* memory, uint32_t aligned_size, uint32_t* index) {
    uint32_t cursor = ILANG_VM_MEMORY_START;

    memory_compact(memory); /* the gap search wants a table of live blocks only */

    for (uint32_t i = 0; i < memory->block_count; i++) {
        const iMemoryBlock* block = &memory->blocks[i];
        uint32_t block_end;

        if (block->address >= cursor && block->address - cursor >= aligned_size) {
            *index = i;
            return cursor;
        }

        if (!align4(block->size, &block_end) || block_end > UINT32_MAX - block->address) {
            return 0;
        }

        cursor = block->address + block_end;
    }

    if (aligned_size <= UINT32_MAX - cursor) {
        *index = memory->block_count;
        return cursor;
    }

    return 0;
}

iMemory* memory_create(void) {
    iMemory* memory = (iMemory*)malloc(sizeof(iMemory));

    if (memory == NULL) {
        return NULL;
    }

    memory->blocks = NULL;
    memory->block_count = 0;
    memory->block_capacity = 0;
    memory->next_address = ILANG_VM_MEMORY_START;
    memory->total_bytes = 0;
    memory->last_block = 0;
    memory->dead = 0;

    return memory;
}

void memory_destroy(iMemory* memory) {
    if (memory == NULL) {
        return;
    }

    for (uint32_t i = 0; i < memory->block_count; i++) {
        host_free(memory->blocks[i].data, memory->blocks[i].size);
    }

    free(memory->blocks);
    free(memory);
}

uint32_t memory_available(const iMemory* memory) {
    if (memory == NULL || memory->total_bytes >= ILANG_VM_MEMORY_LIMIT) {
        return 0;
    }

    return ILANG_VM_MEMORY_LIMIT - memory->total_bytes;
}

/**
 * @details Everything is validated before anything is allocated. The fast path hands out `next_address` and appends
 *          the block; once the 32-bit address space is used up, the block goes into a hole found by memory_find_gap()
 *          and is inserted at its sorted position. The block's data is zeroed.
 */
uint32_t memory_reserve(iMemory* memory, uint32_t size) {
    uint32_t address;
    uint32_t aligned_size;
    uint32_t index;
    uint8_t* data;
    bool append;
    iMemoryBlock* block;

    if (memory == NULL || size == 0) {
        return 0;
    }

    /* All of this is checked BEFORE allocating anything. */
    if (!align4(memory->next_address, &address)) {
        return 0;
    }

    if (!align4(size, &aligned_size)) {
        return 0;
    }

    /* Don't let a program take more host memory than we allow. */
    if (size > ILANG_VM_MEMORY_LIMIT - memory->total_bytes) {
        return 0;
    }

    /* Fast path: hand out the next fresh address. Slow path (virtual address space used up):
       reuse a hole left by freed blocks. */
    append = (aligned_size <= UINT32_MAX - address);
    index = memory->block_count;

    if (!append) {
        address = memory_find_gap(memory, aligned_size, &index);

        if (address == 0) {
            return 0;
        }
    }

    if (memory->block_count >= memory->block_capacity) {
        if (memory->dead != 0) {   /* (never on the gap path: memory_find_gap has already compacted) */
            memory_compact(memory);
            index = memory->block_count;
        }
        if (memory->block_count >= memory->block_capacity && !memory_grow_blocks(memory)) {
            return 0;
        }
    }

    /* Zeroed, so programs never see host garbage. */
    data = host_alloc_zeroed(size);

    if (data == NULL) {
        return 0;
    }

    if (index < memory->block_count) {
        memmove(&memory->blocks[index + 1u], &memory->blocks[index],
            (size_t)(memory->block_count - index) * sizeof(iMemoryBlock));
    }

    block = &memory->blocks[index];

    block->address = address;
    block->size = size;
    block->data = data;

    memory->block_count++;
    if (append) {
        memory->next_address = address + aligned_size;
    }
    memory->total_bytes += size;

    return address;
}

/**
 * @details Only the exact start of a live block can be freed. The last table entry is dropped (with any tombstones
 *          that end up at the end); any other entry becomes a tombstone, and the table is compacted once more than
 *          half of it is dead.
 */
bool memory_free(iMemory* memory, uint32_t address) {
    iMemoryBlock* block;
    uint32_t index;

    if (memory == NULL || address == 0) {
        return false;
    }

    block = memory_find_block(memory, address);

    /* Only the exact start of a live block can be freed. */
    if (block == NULL || block->address != address) {
        return false;
    }

    index = (uint32_t)(block - memory->blocks);

    memory->total_bytes -= block->size;
    host_free(block->data, block->size);

    if (index + 1u == memory->block_count) {
        /* last entry: just drop it (and any tombstones that are now at the end) */
        memory->block_count--;
        while (memory->block_count > 0 && memory->blocks[memory->block_count - 1u].size == 0) {
            memory->block_count--;
            memory->dead--;
        }
    }
    else {
        /* tombstone: size 0 never matches a lookup, the address keeps the table sorted */
        block->size = 0;
        block->data = NULL;
        memory->dead++;

        if (memory->dead > 32u && memory->dead * 2u > memory->block_count) {
            memory_compact(memory);
        }
    }

    return true;
}

uint32_t memory_phantom_acquire(iMemory* memory, uint32_t size) {
    uint32_t address, aligned_size;

    /* the same checks, in the same order, as the append path of memory_reserve() */
    if (memory == NULL || size == 0 || memory->block_count >= memory->block_capacity) {
        return 0;
    }

    if (!align4(memory->next_address, &address) || !align4(size, &aligned_size)) {
        return 0;
    }

    if (size > ILANG_VM_MEMORY_LIMIT - memory->total_bytes || aligned_size > UINT32_MAX - address) {
        return 0;
    }

    memory->next_address = address + aligned_size;
    memory->total_bytes += size;

    return address;
}

void memory_phantom_release(iMemory* memory, uint32_t size) {
    memory->total_bytes -= size;
}

bool memory_read(iMemory* memory, uint32_t address, void* destination, uint32_t size) {
    iMemoryBlock* block;
    uint32_t offset;

    if (destination == NULL) {
        return false;
    }

    block = memory_find_range(memory, address, size, &offset);
    if (block == NULL) {
        return false;
    }

    memcpy(destination, block->data + offset, size);
    return true;
}

bool memory_write(iMemory* memory, uint32_t address, const void* source, uint32_t size) {
    iMemoryBlock* block;
    uint32_t offset;

    if (source == NULL) {
        return false;
    }

    block = memory_find_range(memory, address, size, &offset);
    if (block == NULL) {
        return false;
    }

    memcpy(block->data + offset, source, size);
    return true;
}

uint8_t* memory_ptr(iMemory* memory, uint32_t address, uint32_t size) {
    iMemoryBlock* block;
    uint32_t offset;

    block = memory_find_range(memory, address, size, &offset);
    if (block == NULL) {
        return NULL;
    }

    return block->data + offset;
}

uint32_t memory_block_size(iMemory* memory, uint32_t address) {
    iMemoryBlock* block;

    if (memory == NULL || address == 0) {
        return 0;
    }

    block = memory_find_block(memory, address);
    if (block == NULL || block->address != address) {
        return 0;
    }

    return block->size;
}
