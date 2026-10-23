/**
 * @file IlangVM_cast.h
 * @brief Ilang casting system: the conversions behind the CAST instruction.
 */
#pragma once

/* Required */
#include "IlangVM.h"

/**
 * @brief Converts `value` to `target_type`.
 *
 * The conversion rules are listed in IlangVM_cast.c. On an unsupported conversion it panics and returns a T_NONE value.
 *
 * @param[in,out] state       The VM (used to report a panic).
 * @param[in]     value       The value to convert.
 * @param[in]     target_type T_INT, T_UINT, T_FLOAT, T_BOOL, T_BYTE or T_ADDRESS.
 * @return The converted value, or a T_NONE value after a panic.
 */
Value l_cast(iState* state, Value value, uint8_t target_type);