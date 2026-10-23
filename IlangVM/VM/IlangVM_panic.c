/**
 * @file IlangVM_panic.c
 * @brief Ilang virtual machine error reporting and panic system (implementation of IlangVM_panic.h).
 */

/* Required */
#include "IlangVM_panic.h"

#include <stdarg.h>
#include <stdio.h>

/** @details Sets `state->halt`, keeps the message in `state->panic_message` and prints it to stderr (stdout is flushed first). */
void IlangVM_panic(iState* state, const char* message) {
    state->halt = true;
    snprintf(state->panic_message, sizeof state->panic_message, "%s", message);

    fflush(stdout);
    fprintf(stderr, "IlangVM Panic! %s\n", message);
}

void IlangVM_panicf(iState* state, const char* format, ...) {
    char message[ILANG_VM_PANIC_MESSAGE_SIZE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);

    IlangVM_panic(state, message);
}
