/**
 * @file IlangVM_fetch.c
 * @brief Ilang bytecode fetching: error paths (see IlangVM_fetch.h).
 *
 * Both functions halt the VM themselves, so a failed fetch stops execution even if IlangVM_panic() happens to return
 * instead of exiting.
 */

/* Required */
#include "IlangVM_fetch.h"
#include "IlangVM_panic.h"

/*
 * Both functions halt the VM themselves, so a failed fetch stops execution
 * even if IlangVM_panic() happens to return instead of exiting.
 */

void fetch_eof(iState* state) {
    IlangVM_panic(state, "Unexpected EOF!");

    state->halt = true;
    state->ip = state->code_size;
}

void fetch_bad_register(iState* state, uint8_t index) {
    IlangVM_panicf(state, "Register index r%u is out of range (the VM has %u registers)!", (unsigned)index, (unsigned)REG_COUNT);

    state->halt = true;
}