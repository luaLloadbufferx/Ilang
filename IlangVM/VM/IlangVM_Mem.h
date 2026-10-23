/**
 * @file IlangVM_Mem.h
 * @brief IlangVM virtual memory manager.
 *
 * The VM never hands out host pointers to the program. Programs see 32-bit virtual addresses; each address is the
 * start of a block (iMemoryBlock) kept in a table sorted by address, and every access is bounds-checked against the
 * block that contains it. Address 0 is never valid, so it doubles as "null" and as the failure value of
 * memory_reserve().
 */
#pragma once

/* Required */
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief A block of VM memory: one live allocation.
 */
typedef struct iMemoryBlock {
    uint32_t address;  /**< Virtual address of the first byte. */
    uint32_t size;  /**< Size of the block in bytes. */
    uint8_t* data;  /**< Host pointer to the data of the block. */
} iMemoryBlock;

/**
 * @brief The virtual memory manager.
 *
 * Created by memory_create() and destroyed by memory_destroy(). Freed blocks stay in `blocks[]` as tombstones (size 0,
 * data NULL) so the table stays sorted without moving entries; see IlangVM_Mem.c for the invariants.
 */
typedef struct iMemory {
    iMemoryBlock* blocks;  /**< Memory blocks (always sorted by address). */
    uint32_t block_count;  /**< Number of used entries in `blocks[]` (live blocks plus tombstones). */
    uint32_t block_capacity;  /**< Allocated length of `blocks[]`, in entries. */
    uint32_t next_address;  /**< The next virtual address that is available. */
    uint32_t total_bytes;  /**< Total size of all live blocks (limited by ILANG_VM_MEMORY_LIMIT). */
    uint32_t dead;  /**< Tombstones in `blocks[]` (freed entries: size 0, data NULL); they keep the table sorted without memmove. */
    uint32_t last_block; /**< Table index of the block found last: a one-entry lookup cache. It is only a hint (frees and
                              inserts shift indices), so every use re-checks that it still matches. */
} iMemory;

/* Manager functions */
/**
 * @brief Creates an empty memory.
 * @return The new memory, or NULL if the host is out of memory.
 */
iMemory* memory_create(void);
/**
 * @brief Destroys the memory and frees every block that is still live.
 * @param[in] memory The memory to destroy (may be NULL).
 */
void memory_destroy(iMemory* memory);

/**
 * @brief Reserves memory (zeroed).
 * @param[in,out] memory The memory manager.
 * @param[in]     size   Bytes to reserve.
 * @return The virtual address of the new block, or 0 on failure or when the memory limit would be exceeded.
 */
uint32_t memory_reserve(iMemory* memory, uint32_t size);
/**
 * @brief How many more bytes can be reserved before the memory limit is hit.
 * @param[in] memory The memory manager.
 */
uint32_t memory_available(const iMemory* memory);
/**
 * @brief Frees memory.
 * @param[in,out] memory  The memory manager.
 * @param[in]     address Start address of the block to free.
 * @return false if `address` is not the start of a live block (bad address / double free).
 */
bool memory_free(iMemory* memory, uint32_t address);

/**
 * @brief Accounts for a block that is allocated and freed again before anything else can look at it (a string
 *        literal that is used once), without creating a block.
 *
 * It hands out exactly the address memory_reserve(size) would, advances `next_address` and counts the bytes as live, but
 * creates no block. It returns 0 and changes nothing when the real allocation would not be the plain "append at
 * next_address" case or could fail; the caller then does the real thing. memory_phantom_release() ends it.
 *
 * @param[in,out] memory The memory manager.
 * @param[in]     size   Size of the pretend block in bytes.
 * @return The address memory_reserve() would have returned, or 0 if the phantom could not be made.
 */
uint32_t memory_phantom_acquire(iMemory* memory, uint32_t size);
/**
 * @brief Ends a phantom block made by memory_phantom_acquire().
 * @param[in,out] memory The memory manager.
 * @param[in]     size   The size that was passed to memory_phantom_acquire().
 */
void memory_phantom_release(iMemory* memory, uint32_t size);

/**
 * @brief Safely reads memory.
 * @param[in]  memory      The memory manager.
 * @param[in]  address     First virtual address to read.
 * @param[out] destination Receives `size` bytes.
 * @param[in]  size        Number of bytes to read.
 * @return false if the range is not fully inside one live block.
 */
bool memory_read(iMemory* memory, uint32_t address, void* destination, uint32_t size);
/**
 * @brief Safely writes memory.
 * @param[in,out] memory  The memory manager.
 * @param[in]     address First virtual address to write.
 * @param[in]     source  The `size` bytes to store.
 * @param[in]     size    Number of bytes to write.
 * @return false if the range is not fully inside one live block.
 */
bool memory_write(iMemory* memory, uint32_t address, const void* source, uint32_t size);

/**
 * @brief Direct host pointer to `[address, address + size)`.
 *
 * The pointer is only valid until that block is freed.
 *
 * @param[in] memory  The memory manager.
 * @param[in] address First virtual address.
 * @param[in] size    Length of the range in bytes.
 * @return The pointer, or NULL if the range is not fully inside ONE live block (or `size` is 0).
 */
uint8_t* memory_ptr(iMemory* memory, uint32_t address, uint32_t size);

/**
 * @brief Size in bytes of the live block that STARTS exactly at `address`.
 * @param[in] memory  The memory manager.
 * @param[in] address Start address of a block.
 * @return The size, or 0 if there is no such block.
 */
uint32_t memory_block_size(iMemory* memory, uint32_t address);

/**
 * @brief Hot-path accessor: same contract as memory_ptr() for `size` > 0, but a repeat access to the block that was
 *        used last (the common case: a loop over one array, several fields of one struct) is resolved inline without
 *        a function call or a binary search.
 *
 * @param[in] memory  The memory manager.
 * @param[in] address First virtual address.
 * @param[in] size    Length of the range in bytes.
 * @return The pointer, or NULL if the range is not fully inside ONE live block.
 */
static inline uint8_t* memory_ptr_fast(iMemory* memory, uint32_t address, uint32_t size) {
    if (memory->last_block < memory->block_count) {
        const iMemoryBlock* block = &memory->blocks[memory->last_block];
        uint32_t offset = address - block->address; /* wraps (and so fails the next test) if address < block start */

        if (offset < block->size && size <= block->size - offset) {
            return block->data + offset;
        }
    }

    return memory_ptr(memory, address, size);
}
