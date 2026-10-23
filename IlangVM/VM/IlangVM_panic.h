/**
 * @file IlangVM_panic.h
 * @brief Ilang virtual machine error reporting and panic system.
 *
 * A panic stops the VM (`state->halt`) and prints one line to stderr:
 * @code{.unparsed}
 * IlangVM Panic! Division by zero!
 * @endcode
 * Whatever the program printed to stdout so far is flushed first, so the message lands where the program stopped.
 * The message always goes to stderr (never to a dialog box), so it also works without a console.
 */
#pragma once

/* Required */
#include "IlangVM.h"


/**
 * @brief Halts the execution and prints the message.
 *
 * The message is also kept in `state->panic_message`.
 *
 * @param[in,out] state   The VM.
 * @param[in]     message The text to report (cut to `ILANG_VM_PANIC_MESSAGE_SIZE - 1` bytes in `panic_message`).
 */
void IlangVM_panic(iState* state, const char* message);

/**
 * @brief Same as IlangVM_panic(), with printf-style formatting. The message is cut at 511 bytes.
 * @param[in,out] state  The VM.
 * @param[in]     format printf-style format.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void IlangVM_panicf(iState* state, const char* format, ...);
