/**
 * @file util.c
 * @brief Allocation helpers for Ilangdis. A command line tool has no way to carry on without memory, so these end the
 *        program instead of returning NULL.
 */
#include "ilangdis.h"

#include <stdlib.h>
#include <string.h>

/** @brief Reports that memory ran out and ends the program with status 1. */
static void out_of_memory(void) {
    fputs("Ilangdis: out of memory\n", stderr);
    exit(1);
}

void* dis_xmalloc(size_t size) {
    void* p = malloc(size ? size : 1);

    if (!p) out_of_memory();
    return p;
}

void* dis_xcalloc(size_t count, size_t size) {
    void* p = calloc(count ? count : 1, size ? size : 1);

    if (!p) out_of_memory();
    return p;
}

void* dis_xrealloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size ? size : 1);

    if (!p) out_of_memory();
    return p;
}

char* dis_xstrndup(const char* text, size_t length) {
    char* copy = (char*)dis_xmalloc(length + 1);

    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

void dis_reserve(void* array, size_t* capacity, size_t element, size_t need) {
    void** slot = (void**)array;
    size_t cap = *capacity;

    if (need <= cap) return;
    if (cap < 16) cap = 16;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2 / element) out_of_memory();
        cap *= 2;
    }
    *slot = dis_xrealloc(*slot, cap * element);
    *capacity = cap;
}
