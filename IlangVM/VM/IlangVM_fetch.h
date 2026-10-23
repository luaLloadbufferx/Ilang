/**
 * @file IlangVM_fetch.h
 * @brief Ilang bytecode fetching: bounds-checked readers for the operands of an instruction.
 *
 * Bytecode format rules:
 *  - every multi-byte value is stored LITTLE-ENDIAN, whatever the host is
 *  - floats are IEEE-754 binary32
 *
 * The readers below assemble values byte by byte, so they behave the same on every host (compilers turn this into a
 * single load on little-endian CPUs).
 *
 * Every reader is bounds-checked. On failure it reports the error, HALTS the VM and returns 0, so callers that check
 * `state->halt` (or the bool results of fetch_reg()) never act on a half-read instruction.
 */
#pragma once

/* Required */
#include "IlangVM.h"
#include "IlangConf.h"

#include <string.h>

/*
 * Bytecode format rules:
 *   - every multi-byte value is stored LITTLE-ENDIAN, whatever the host is
 *   - floats are IEEE-754 binary32
 *
 * The readers below assemble values byte by byte, so they behave the same on
 * every host (compilers turn this into a single load on little-endian CPUs).
 *
 * Every reader is bounds-checked. On failure it reports the error, HALTS the
 * VM and returns 0, so callers that check `state->halt` (or the bool results
 * of fetch_reg) never act on a half-read instruction.
 */

/**
 * @brief Cold path (IlangVM_fetch.c): reports "Unexpected EOF!" and halts the VM. Kept out of the hot loop.
 * @param[in,out] state The VM.
 */
void fetch_eof(iState* state);
/**
 * @brief Cold path (IlangVM_fetch.c): reports an out-of-range register index and halts the VM.
 * @param[in,out] state The VM.
 * @param[in]     index The offending register index.
 */
void fetch_bad_register(iState* state, uint8_t index);

/**
 * @brief True if `count` more bytes can be read. Written so it can never wrap around.
 * @param state The VM.
 * @param count Number of bytes wanted.
 */
static inline bool fetch_available(iState* state, uint32_t count) {
    return state->ip <= state->code_size && count <= state->code_size - state->ip;
}

/**
 * @brief Reads one byte and advances `ip`.
 * @param[in,out] state The VM.
 * @return The byte, or 0 (after halting the VM) at the end of the code.
 */
static inline uint8_t fetch_byte(iState* state) {
    if (!fetch_available(state, 1)) {
        fetch_eof(state);
        return 0;
    }

    return state->code[state->ip++];
}

/**
 * @brief Reads a little-endian 16-bit value and advances `ip`.
 * @param[in,out] state The VM.
 * @return The value, or 0 (after halting the VM) at the end of the code.
 */
static inline uint16_t fetch_ushort(iState* state) {
    const uint8_t* p;

    if (!fetch_available(state, 2)) {
        fetch_eof(state);
        return 0;
    }

    p = state->code + state->ip;
    state->ip += 2;

    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

/**
 * @brief Reads a little-endian 32-bit unsigned value and advances `ip`.
 * @param[in,out] state The VM.
 * @return The value, or 0 (after halting the VM) at the end of the code.
 */
static inline uint32_t fetch_uint(iState* state) {
    const uint8_t* p;

    if (!fetch_available(state, 4)) {
        fetch_eof(state);
        return 0;
    }

    p = state->code + state->ip;
    state->ip += 4;

    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/**
 * @brief Reads a little-endian 32-bit signed value (two's complement) and advances `ip`.
 * @param[in,out] state The VM.
 * @return The value, or 0 (after halting the VM) at the end of the code.
 */
static inline int32_t fetch_int(iState* state) {
    /* Two's complement reinterpretation of the same 32 bits. */
    return (int32_t)fetch_uint(state);
}

/**
 * @brief Reads an IEEE-754 binary32 float and advances `ip`.
 * @param[in,out] state The VM.
 * @return The value, or 0 (after halting the VM) at the end of the code.
 */
static inline float fetch_float(iState* state) {
    uint32_t bits = fetch_uint(state);
    float value;

    memcpy(&value, &bits, sizeof value);
    return value;
}

/**
 * @brief Fetches a register index and validates it against REG_COUNT.
 * @param[in,out] state The VM.
 * @param[out]    out   Receives the register index.
 * @return false (and halts the VM) on EOF or an out-of-range index.
 */
static inline bool fetch_reg(iState* state, uint8_t* out) {
    uint8_t index;

    if (!fetch_available(state, 1)) {
        fetch_eof(state);
        return false;
    }

    index = state->code[state->ip++];

    if ((size_t)index >= REG_COUNT) {
        fetch_bad_register(state, index);
        return false;
    }

    *out = index;
    return true;
}