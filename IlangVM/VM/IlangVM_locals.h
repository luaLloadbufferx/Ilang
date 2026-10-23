/**
 * @file IlangVM_locals.h
 * @brief Ilang locals: stack frame setup and access to locals and parameters.
 *
 * Locals live on the data stack above the base pointer; parameters live just below it (parameter N at
 * `bp - (N + 1)`). Every access is bounds-checked against the live part of the stack and panics when it is out of range.
 */
#pragma once

/* Required */
#include "IlangVM.h"

/* API */
/**
 * @brief Reserves `locals` slots at the top of the data stack. Each new slot holds the T_INT value 0.
 * @param[in,out] state  The VM.
 * @param[in]     locals Number of slots to reserve; panics ("Data stack overflow!") if they do not fit.
 */
void l_enter(iState* state, uint32_t locals);
/**
 * @brief Releases the top `locals` slots of the data stack.
 * @param[in,out] state  The VM.
 * @param[in]     locals Number of slots to release; panics ("Data stack underflow!") if fewer are in use.
 */
void l_leave(iState* state, uint32_t locals);

/**
 * @brief Reads the live stack slot at absolute index `offset`.
 * @param[in,out] state  The VM.
 * @param[in]     offset Index into the data stack.
 * @return The slot, or a T_NONE value after a panic if there is no such live slot.
 */
Value l_get_local(iState* state, uint32_t offset);
/**
 * @brief Writes the live stack slot at absolute index `offset`; panics if there is none.
 * @param[in,out] state  The VM.
 * @param[in]     offset Index into the data stack.
 * @param[in]     value  The value to store.
 */
void l_set_local(iState* state, uint32_t offset, Value value);

/**
 * @brief Reads parameter `offset` of the current frame (it lives at `bp - (offset + 1)`).
 * @param[in,out] state  The VM.
 * @param[in]     offset Parameter number, 0 = the one nearest to the base pointer.
 * @return The parameter, or a T_NONE value after a panic if there is no such live slot.
 */
Value l_get_param(iState* state, uint32_t offset);
/**
 * @brief Writes parameter `offset` of the current frame.
 * @param[in,out] state  The VM.
 * @param[in]     offset Parameter number, 0 = the one nearest to the base pointer.
 * @param[in]     value  The value to store.
 */
void l_set_param(iState* state, uint32_t offset, Value value);