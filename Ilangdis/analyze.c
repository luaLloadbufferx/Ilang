/**
 * @file analyze.c
 * @brief Finds the structure of the code: instructions, functions and their names, labels, cross references.
 *
 * Everything here is heuristic, because a `.ic` file has no symbol table, but the heuristics are tuned to what `Ilangc`
 * emits (see `Compiler::compile()` and `FuncGen` in `Ilangc/codegen.hpp`):
 *
 * @code{.unparsed}
 * image = header | import table | [EXPT section] | startup stub | [init stub] | functions, callee first
 * stub     = [ENTER n; typed zeroes; CALL <globals>]  CALL main  HALT
 * function = [ENTER n] body  epilogue: [LEAVE n] RET  [STRLIT "runtime error: ... in 'name'"; ABORT]...
 * tail call to another function = LEAVE n; JMP <function start>
 * @endcode
 */
#include "ilangdis.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/** @brief The start of every message of a runtime-error stub emitted by the compiler (`emitFailStub()`). */
static const char kFailPrefix[] = "runtime error: ";

/**
 * @brief Bytes of the string a STRLIT instruction carries.
 * @param p The program.
 * @param i Index of a STRLIT.
 */
static const char* strlit_text(const DisProgram* p, size_t i) {
    return (const char*)p->image->data + p->insns[i].data_off;
}

/**
 * @brief Whether the instruction at @p i begins one of the compiler's failure stubs: `STRLIT "runtime error: ..."` and
 *        an ABORT of the same register.
 * @param p The program.
 * @param i Instruction index.
 */
static bool is_failstub(const DisProgram* p, size_t i) {
    const DisInsn* s;

    if (i + 1 >= p->insn_count) return false;
    s = &p->insns[i];
    return s->op == STRLIT && p->insns[i + 1].op == ABORT && p->insns[i + 1].reg[0] == s->reg[0] &&
           s->value >= sizeof kFailPrefix - 1 && memcmp(strlit_text(p, i), kFailPrefix, sizeof kFailPrefix - 1) == 0;
}

/**
 * @brief Reads the function name out of a failure stub's message (`runtime error: <what> in '<name>'`).
 * @param[in]  p    The program.
 * @param[in]  i    Index of the stub's STRLIT.
 * @param[out] name Receives the name.
 * @param[in]  size Size of @p name.
 * @return false if the message has no name in it.
 */
static bool failstub_name(const DisProgram* p, size_t i, char* name, size_t size) {
    const char* s = strlit_text(p, i);
    size_t len = p->insns[i].value, k, n, j;

    if (len < 6 || s[len - 1] != '\'') return false;
    for (k = len - 6; memcmp(s + k, " in '", 5) != 0; k--) {  /* the last " in '" in the message */
        if (k == 0) return false;
    }
    n = len - 1 - (k + 5);
    if (n == 0 || n >= size) return false;
    for (j = 0; j < n; j++) {
        unsigned char c = (unsigned char)s[k + 5 + j];
        if (c <= 0x20 || c >= 0x7F) return false;
        name[j] = (char)c;
    }
    name[n] = '\0';
    if (strcmp(name, "<globals>") == 0) snprintf(name, size, "_globals");
    return true;
}

/**
 * @brief Finds a function by its exact name.
 * @param p    The program.
 * @param name The name.
 * @return Its index, or -1.
 */
static int32_t func_by_name(const DisProgram* p, const char* name) {
    size_t f;

    for (f = 0; f < p->func_count; f++) {
        if (p->funcs[f].name && strcmp(p->funcs[f].name, name) == 0) return (int32_t)f;
    }
    return -1;
}

/**
 * @brief Gives a function a name, unless it already has a better one or the name is taken.
 * @param p    The program.
 * @param f    Function index.
 * @param name The candidate.
 * @param src  Where the candidate comes from; it replaces the current name only if it is more reliable.
 */
static void set_name(DisProgram* p, int32_t f, const char* name, DisNameSource src) {
    DisFunc* fn = &p->funcs[f];
    int32_t other = func_by_name(p, name);

    if (other >= 0 && other != f) return;
    if (fn->name && fn->source >= src) return;
    free(fn->name);
    fn->name = dis_xstrndup(name, strlen(name));
    fn->source = src;
}

int32_t dis_find_insn(const DisProgram* p, uint32_t addr) {
    size_t lo = 0, hi = p->insn_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (p->insns[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return (lo < p->insn_count && p->insns[lo].addr == addr) ? (int32_t)lo : -1;
}

/**
 * @brief Orders cross references by target, then by source.
 * @param pa First DisXref.
 * @param pb Second DisXref.
 */
static int xref_order(const void* pa, const void* pb) {
    const DisXref* a = (const DisXref*)pa;
    const DisXref* b = (const DisXref*)pb;

    if (a->to != b->to) return a->to < b->to ? -1 : 1;
    if (a->from != b->from) return a->from < b->from ? -1 : 1;
    return 0;
}

const DisXref* dis_xrefs_to(const DisProgram* p, uint32_t insn, size_t* count) {
    size_t lo = 0, hi = p->xref_count, first;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (p->xrefs[mid].to < insn) lo = mid + 1;
        else hi = mid;
    }
    first = lo;
    while (lo < p->xref_count && p->xrefs[lo].to == insn) lo++;
    *count = lo - first;
    return *count ? &p->xrefs[first] : NULL;
}

/**
 * @brief Step 1: decodes every instruction from the entry point to the end of the file.
 * @param p The program (its `image` is set).
 */
static void sweep(DisProgram* p) {
    const DisImage* img = p->image;
    size_t capacity = 0;
    uint32_t addr = img->entry;

    while (addr < img->size) {
        DisInsn* in;

        dis_reserve(&p->insns, &capacity, sizeof *p->insns, p->insn_count + 1);
        in = &p->insns[p->insn_count++];
        addr += dis_decode(img, addr, in);
        if (in->op == 0) p->bad_bytes++;
    }
}

/**
 * @brief Step 2: resolves the target address of every jump and call to an instruction index.
 * @param p The program.
 */
static void resolve_targets(DisProgram* p) {
    size_t i;

    for (i = 0; i < p->insn_count; i++) {
        DisInsn* in = &p->insns[i];

        if (!in->has_target) continue;
        in->target_idx = dis_find_insn(p, in->target);
        if (in->target_idx < 0) p->bad_targets++;
    }
}

/**
 * @brief Step 3: marks the first instruction of every function and builds the function table.
 *
 * A function starts at the entry point, after every HALT, after every RET (and the failure stubs behind it), and at
 * every address the rest of the file names as a function: static call targets, the init stub, the exports.
 *
 * @param p The program.
 */
static void find_functions(DisProgram* p) {
    const DisImage* img = p->image;
    bool* start = (bool*)dis_xcalloc(p->insn_count + 1, sizeof *start);
    size_t i, cap = 0;
    uint32_t e;
    int32_t idx;

    if (p->insn_count == 0) {
        free(start);
        return;
    }
    start[0] = true;
    for (i = 0; i < p->insn_count; i++) {
        const DisInsn* in = &p->insns[i];

        if (in->op == HALT) {
            start[i + 1] = true;
        }
        else if (in->op == RET) {
            size_t j = i + 1;

            while (is_failstub(p, j)) j += 2;
            start[j] = true;
        }
        else if (in->op == CALL && in->has_target && in->target_idx >= 0) {
            start[in->target_idx] = true;
        }
    }
    if (img->has_expt && (idx = dis_find_insn(p, img->init_address)) >= 0) start[idx] = true;
    for (e = 0; e < img->export_count; e++) {
        if ((idx = dis_find_insn(p, img->exports[e].address)) >= 0) start[idx] = true;
    }

    for (i = 0; i < p->insn_count; i++) {
        DisFunc* f;

        if (start[i]) {
            const DisInsn* first = &p->insns[i];

            dis_reserve(&p->funcs, &cap, sizeof *p->funcs, p->func_count + 1);
            f = &p->funcs[p->func_count++];
            memset(f, 0, sizeof *f);
            f->first = (uint32_t)i;
            f->start = first->addr;
            f->export_index = -1;
            f->has_frame = first->op == ENTER;
            f->frame = f->has_frame ? first->value : 0;
            f->body = (uint32_t)i + (f->has_frame ? 1u : 0u);
            p->insns[i].flags |= DIS_F_FUNC_START;
        }
        f = &p->funcs[p->func_count - 1];
        p->insns[i].func = (int32_t)(p->func_count - 1);
        f->count++;
        f->end = p->insns[i].addr + p->insns[i].len;
    }
    free(start);
}

/**
 * @brief Step 4: names the functions.
 *
 * In order of reliability: the entry and init stubs and the exports (the file says so); `main` and `_globals` (their
 * place in the startup stub); the name inside the function's own failure stub; the address.
 *
 * @param p The program.
 */
static void name_functions(DisProgram* p) {
    const DisImage* img = p->image;
    size_t f, i;
    uint32_t e;
    int32_t idx;

    if (p->func_count == 0) return;

    p->funcs[0].is_entry = true;
    set_name(p, 0, "_start", DIS_NAME_EXPORT);
    if (img->has_expt && (idx = dis_find_insn(p, img->init_address)) >= 0) {
        int32_t fi = p->insns[idx].func;

        if (p->funcs[fi].start == img->init_address) {
            p->funcs[fi].is_init = true;
            set_name(p, fi, "_init", DIS_NAME_EXPORT);
        }
    }
    for (e = 0; e < img->export_count; e++) {
        idx = dis_find_insn(p, img->exports[e].address);
        if (idx < 0 || p->funcs[p->insns[idx].func].start != img->exports[e].address) continue;
        set_name(p, p->insns[idx].func, img->exports[e].name, DIS_NAME_EXPORT);
        if (p->funcs[p->insns[idx].func].export_index < 0) p->funcs[p->insns[idx].func].export_index = (int32_t)e;
    }

    /* The stubs: with globals the first CALL runs the initializers, a later one (or the only one) calls main. */
    for (f = 0; f < p->func_count; f++) {
        const DisFunc* stub = &p->funcs[f];
        unsigned calls = 0;

        if (!stub->is_entry && !stub->is_init) continue;
        for (i = stub->first; i < stub->first + stub->count; i++) {
            const DisInsn* in = &p->insns[i];

            if (in->op != CALL || !in->has_target || in->target_idx < 0) continue;
            set_name(p, p->insns[in->target_idx].func, (calls == 0 && stub->has_frame) ? "_globals" : "main", DIS_NAME_STARTUP);
            calls++;
        }
    }

    for (f = 0; f < p->func_count; f++) {
        for (i = p->funcs[f].first; i < p->funcs[f].first + p->funcs[f].count; i++) {
            char name[256];

            if (is_failstub(p, i) && failstub_name(p, i, name, sizeof name)) {
                set_name(p, (int32_t)f, name, DIS_NAME_FAILSTUB);
                break;
            }
        }
    }

    for (f = 0; f < p->func_count; f++) {
        char name[32];

        if (p->funcs[f].name) continue;
        snprintf(name, sizeof name, "func_%04X", (unsigned)p->funcs[f].start);
        set_name(p, (int32_t)f, name, DIS_NAME_GENERATED);
    }
}

/**
 * @brief Step 5: records every jump and call as a cross reference and marks jump targets.
 * @param p The program.
 */
static void find_xrefs(DisProgram* p) {
    size_t i, cap = 0;

    for (i = 0; i < p->insn_count; i++) {
        DisInsn* in = &p->insns[i];
        DisXrefKind kind;
        DisInsn* to;
        DisXref* x;

        if ((in->op != CALL && !dis_is_jump(in->op)) || in->target_idx < 0) continue;
        to = &p->insns[in->target_idx];

        if (in->op == CALL) {
            kind = DIS_XREF_CALL;
        }
        else if (in->op == JMP && (to->flags & DIS_F_FUNC_START)) {
            kind = DIS_XREF_TAIL;
            in->flags |= DIS_F_TAIL_CALL;
        }
        else {
            kind = in->op == JMP ? DIS_XREF_JUMP : DIS_XREF_COND;
        }
        if (in->op != CALL && kind != DIS_XREF_TAIL) {
            to->flags |= ((uint32_t)in->target_idx > i) ? DIS_F_TARGET_FWD : DIS_F_TARGET_BACK;
        }

        dis_reserve(&p->xrefs, &cap, sizeof *p->xrefs, p->xref_count + 1);
        x = &p->xrefs[p->xref_count++];
        x->to = (uint32_t)in->target_idx;
        x->from = (uint32_t)i;
        x->kind = kind;
    }
    if (p->xref_count > 1) qsort(p->xrefs, p->xref_count, sizeof *p->xrefs, xref_order);
}

/**
 * @brief Step 6: gives every jump target a label named after its role.
 * @param p The program.
 */
static void find_labels(DisProgram* p) {
    size_t i, cap = 0;
    int32_t current = -1;
    unsigned n_plain = 0, n_loop = 0, n_epi = 0, n_oob = 0, n_parse = 0, n_fail = 0;

    for (i = 0; i < p->insn_count; i++) {
        DisInsn* in = &p->insns[i];
        const DisFunc* fn = &p->funcs[in->func];
        DisLabel* l;
        unsigned* counter = NULL;
        const char* base = ".L";

        if (in->func != current) {
            current = in->func;
            n_plain = n_loop = n_epi = n_oob = n_parse = n_fail = 0;
        }
        if (!(in->flags & (DIS_F_TARGET_FWD | DIS_F_TARGET_BACK))) continue;

        dis_reserve(&p->labels, &cap, sizeof *p->labels, p->label_count + 1);
        l = &p->labels[p->label_count];
        l->insn = (uint32_t)i;
        l->func = in->func;

        if (is_failstub(p, i)) {
            const char* s = strlit_text(p, i);
            size_t len = in->value;

            if (len >= 40 && memcmp(s + 15, "index out of bounds", 19) == 0) {
                l->kind = DIS_LABEL_OOB; base = ".oob_fail"; counter = &n_oob;
            }
            else if (len >= 15 + 14 && memcmp(s + 15, "cannot convert", 14) == 0) {
                l->kind = DIS_LABEL_PARSE; base = ".parse_fail"; counter = &n_parse;
            }
            else {
                l->kind = DIS_LABEL_FAIL; base = ".fail"; counter = &n_fail;
            }
        }
        else if (in->op == RET || (in->op == LEAVE && i + 1 < p->insn_count && p->insns[i + 1].op == RET)) {
            l->kind = DIS_LABEL_EPILOGUE; base = ".epilogue"; counter = &n_epi;
        }
        else if (fn->has_frame && i == fn->body && (in->flags & DIS_F_TARGET_BACK)) {
            l->kind = DIS_LABEL_BODY; base = ".body";  /* there is only one body start */
        }
        else if (in->flags & DIS_F_TARGET_BACK) {
            l->kind = DIS_LABEL_LOOP; base = ".loop"; counter = &n_loop;
        }
        else {
            l->kind = DIS_LABEL_PLAIN; base = ".L"; counter = &n_plain;
        }

        if (l->kind == DIS_LABEL_LOOP || l->kind == DIS_LABEL_PLAIN) {
            snprintf(l->name, sizeof l->name, "%s%u", base, ++*counter);
        }
        else if (counter && ++*counter > 1) {
            snprintf(l->name, sizeof l->name, "%s%u", base, *counter);
        }
        else {
            snprintf(l->name, sizeof l->name, "%s", base);
        }
        in->label = (int32_t)p->label_count++;
    }
}

void dis_analyze(DisProgram* p, const DisImage* img) {
    memset(p, 0, sizeof *p);
    p->image = img;
    sweep(p);
    resolve_targets(p);
    find_functions(p);
    name_functions(p);
    find_xrefs(p);
    find_labels(p);
}

void dis_program_free(DisProgram* p) {
    size_t f;

    for (f = 0; f < p->func_count; f++) free(p->funcs[f].name);
    free(p->insns);
    free(p->funcs);
    free(p->labels);
    free(p->xrefs);
    memset(p, 0, sizeof *p);
}

int32_t dis_find_function(const DisProgram* p, const char* text) {
    int32_t byname = func_by_name(p, text);
    char* rest = NULL;
    unsigned long v;
    size_t f;

    if (byname >= 0) return byname;
    if (!*text) return -1;
    v = strtoul(text, &rest, 0);
    if (*rest) return -1;
    for (f = 0; f < p->func_count; f++) {
        if (v >= p->funcs[f].start && v < p->funcs[f].end) return (int32_t)f;
    }
    return -1;
}

void dis_describe_position(const DisProgram* p, uint32_t insn, char* buf, size_t size) {
    const DisInsn* in = &p->insns[insn];
    const DisFunc* fn = &p->funcs[in->func];

    if (in->addr == fn->start) snprintf(buf, size, "%s", fn->name);
    else snprintf(buf, size, "%s+0x%X", fn->name, (unsigned)(in->addr - fn->start));
}

bool dis_rename_function(DisProgram* p, const char* which, const char* name, char* error, size_t error_size) {
    int32_t f = dis_find_function(p, which);
    int32_t other;
    const char* c;

    if (f < 0) {
        snprintf(error, error_size, "no function '%s' (--functions lists them)", which);
        return false;
    }
    if (!*name || strlen(name) > 200 || (!isalpha((unsigned char)*name) && *name != '_')) {
        snprintf(error, error_size, "'%s' is not a usable name: use letters, digits and underscores, and do not start with a digit", name);
        return false;
    }
    for (c = name; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_') {
            snprintf(error, error_size, "'%s' is not a usable name: use letters, digits and underscores, and do not start with a digit", name);
            return false;
        }
    }
    other = func_by_name(p, name);
    if (other >= 0 && other != f) {
        snprintf(error, error_size, "the name '%s' is already used by the function at 0x%04X", name, (unsigned)p->funcs[other].start);
        return false;
    }
    free(p->funcs[f].name);
    p->funcs[f].name = dis_xstrndup(name, strlen(name));
    p->funcs[f].source = DIS_NAME_USER;
    return true;
}
