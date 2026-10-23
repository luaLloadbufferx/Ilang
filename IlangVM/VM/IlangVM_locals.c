/**
 * @file IlangVM_locals.c
 * @brief Ilang locals system (implementation of IlangVM_locals.h).
 */

/* Required */
#include "IlangVM_locals.h"
#include "IlangConf.h"
#include "IlangVM.h"
#include "IlangVM_panic.h"

#include <stddef.h>

/** @brief Number of slots currently in use on the data stack. */
static size_t stack_used(const iState* state) {
    return (size_t)(state->top - state->stack);
}

/**
 * @brief Returns a pointer to `stack[index]` if that slot is live (below `top`).
 *
 * The check is done on the INDEX, before any pointer is formed: computing `stack + offset` first and range-checking
 * the pointer afterwards would be undefined behaviour once the offset is out of range.
 *
 * @param state The VM.
 * @param index Index into the data stack.
 * @return The slot, or NULL.
 */
static Value* live_slot(iState* state, uint32_t index) {
    if ((size_t)index >= stack_used(state)) {
        return NULL;
    }

    return &state->stack[index];
}

/**
 * @brief Resolves a parameter offset to a stack index. Parameter N lives at `bp - (N + 1)`.
 *
 * Computing `offset + 1` directly would wrap for `offset == UINT32_MAX`.
 *
 * @param[in]  state  The VM.
 * @param[in]  offset Parameter number.
 * @param[out] index  Receives the stack index.
 * @return false if that would fall below the stack base.
 */
static bool param_index(const iState* state, uint32_t offset, uint32_t* index) {
    size_t base = (size_t)(state->bp - state->stack);

    if ((size_t)offset >= base) {
        return false;
    }

    *index = (uint32_t)(base - 1u - (size_t)offset);
    return true;
}

/* Reserves `locals` slots at the top of the data stack (see IlangVM_locals.h) */
void l_enter(iState* state, uint32_t locals) {
    size_t used = stack_used(state);

    /* Check up front so a failed ENTER doesn't leave a half-allocated frame. */
    if (used > MAX_STACK_SIZE || (size_t)locals > MAX_STACK_SIZE - used) {
        IlangVM_panic(state, "Data stack overflow!");
        return;
    }

    for (uint32_t i = 0; i < locals; i++) {
        /* Locals start as INT 0 (the whole value is zeroed, not just the tag). */
        *state->top++ = (Value){ .tag = T_INT, .data.i32 = 0 };
    }
}

/* Releases the top `locals` slots of the data stack */
void l_leave(iState* state, uint32_t locals) {
    if ((size_t)locals > stack_used(state)) {
        IlangVM_panic(state, "Data stack underflow!");
        return;
    }

    state->top -= locals;
}

/* Reads the live stack slot at absolute index `offset`; panics and returns T_NONE if there is none */
Value l_get_local(iState* state, uint32_t offset) {
    Value* slot = live_slot(state, offset);

    if (slot == NULL) {
        Value none = { .tag = T_NONE };

        IlangVM_panic(state, "Invalid local access!");
        return none;
    }

    return *slot;
}

/* Writes the live stack slot at absolute index `offset`; panics if there is none */
void l_set_local(iState* state, uint32_t offset, Value value) {
    Value* slot = live_slot(state, offset);

    if (slot == NULL) {
        IlangVM_panic(state, "Invalid local access!");
        return;
    }

    *slot = value;
}

/* Reads parameter `offset` of the current frame (counted down from the base pointer) */
Value l_get_param(iState* state, uint32_t offset) {
    uint32_t index;
    Value* slot = NULL;

    if (param_index(state, offset, &index)) {
        slot = live_slot(state, index);
    }

    if (slot == NULL) {
        Value none = { .tag = T_NONE };

        IlangVM_panic(state, "Invalid parameter access!");
        return none;
    }

    return *slot;
}

/* Writes parameter `offset` of the current frame (counted down from the base pointer) */
void l_set_param(iState* state, uint32_t offset, Value value) {
    uint32_t index;
    Value* slot = NULL;

    if (param_index(state, offset, &index)) {
        slot = live_slot(state, index);
    }

    if (slot == NULL) {
        IlangVM_panic(state, "Invalid parameter access!");
        return;
    }

    *slot = value;
}