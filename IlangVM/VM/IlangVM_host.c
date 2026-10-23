/**
 * @file IlangVM_host.c
 * @brief Host functions: C functions an Ilang program calls with CALLC, and the embedding API
 *        (`export fn`, IlangVM_start() / IlangVM_call() / IlangVM_finish()).
 *
 * See IlangHost.h for the library side and IlangVM.h for the import table and the instruction.
 *
 * The file has four parts: signatures (parsing, comparing and printing the `"float(float[], int)"` strings), defining
 * host functions, binding a program's imports when it is loaded, and the IlangCall object together with the
 * IlangHostAPI functions (`a_*`) that a host function sees.
 */

#include "IlangVM.h"
#include "IlangVM_fetch.h"
#include "IlangVM_panic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Signatures                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Normalizes a type: `string` and `byte[]` are the same thing to a host function.
 * @param[in,out] type  Type tag; T_STRINGADDRESS becomes T_BYTE.
 * @param[in,out] depth Array levels; a string becomes depth 1.
 */
static void sig_norm(uint8_t* type, uint8_t* depth) {
    if (*type == T_STRINGADDRESS && *depth == 0) {
        *type = T_BYTE;
        *depth = 1;
    }
}

/**
 * @brief Same type and depth, with string and `byte[]` treated as one.
 * @return true if (`ta`, `da`) and (`tb`, `db`) are the same type.
 */
static bool sig_type_eq(uint8_t ta, uint8_t da, uint8_t tb, uint8_t db) {
    sig_norm(&ta, &da);
    sig_norm(&tb, &db);
    return ta == tb && da == db;
}

/**
 * @brief Two signatures are equal if the return types and every parameter type match (`string` == `byte[]`).
 * @param a First signature.
 * @param b Second signature.
 */
static bool sig_eq(const iHostSig* a, const iHostSig* b) {
    uint8_t i;

    if (a->argc != b->argc) return false;
    if (!sig_type_eq(a->ret_type, a->ret_depth, b->ret_type, b->ret_depth)) return false;
    for (i = 0; i < a->argc; i++) {
        if (!sig_type_eq(a->arg_type[i], a->arg_depth[i], b->arg_type[i], b->arg_depth[i])) return false;
    }
    return true;
}

/**
 * @brief Whether a (type, depth) pair can appear in a signature.
 * @param type  Type tag.
 * @param depth Array levels.
 * @param ret   true for a return type (void is allowed too).
 */
static bool sig_type_valid(uint8_t type, uint8_t depth, bool ret) {
    switch (type) {
    case T_NONE: return ret && depth == 0;
    case T_STRINGADDRESS: return depth == 0;
    case T_INT: case T_UINT: case T_FLOAT: case T_BOOL: case T_ADDRESS: case T_BYTE: return true;
    default: return false;
    }
}

/**
 * @brief The Ilang name of a type tag, for messages.
 * @param type Type tag.
 * @return A static string such as "int" or "void"; "?" for an unknown tag.
 */
static const char* tag_name(uint8_t type) {
    switch (type) {
    case T_NONE: return "void";
    case T_INT: return "int";
    case T_UINT: return "uint";
    case T_FLOAT: return "float";
    case T_BOOL: return "bool";
    case T_ADDRESS: return "address";
    case T_STRINGADDRESS: return "string";
    case T_BYTE: return "byte";
    default: return "?";
    }
}

/**
 * @brief Appends a type such as `float[][]` to `buf`.
 * @param[in,out] buf   Text buffer (always kept terminated).
 * @param[in]     cap   Size of `buf` in bytes.
 * @param[in]     type  Type tag.
 * @param[in]     depth Array levels.
 */
static void put_type(char* buf, size_t cap, uint8_t type, uint8_t depth) {
    size_t n = strlen(buf);
    uint8_t d;

    snprintf(buf + n, cap - n, "%s", tag_name(type));
    for (d = 0; d < depth; d++) {
        n = strlen(buf);
        snprintf(buf + n, cap - n, "[]");
    }
}

/**
 * @brief Writes a signature as text, e.g. `float(float[], int)`.
 * @param[in]  s   The signature.
 * @param[out] buf Receives the text.
 * @param[in]  cap Size of `buf` in bytes.
 */
static void sig_format(const iHostSig* s, char* buf, size_t cap) {
    uint8_t i;

    buf[0] = '\0';
    put_type(buf, cap, s->ret_type, s->ret_depth);
    strncat(buf, "(", cap - strlen(buf) - 1);
    for (i = 0; i < s->argc; i++) {
        if (i) strncat(buf, ", ", cap - strlen(buf) - 1);
        put_type(buf, cap, s->arg_type[i], s->arg_depth[i]);
    }
    strncat(buf, ")", cap - strlen(buf) - 1);
}

/**
 * @brief Moves `*p` past spaces, tabs and line breaks.
 * @param[in,out] p Position in the text.
 */
static void skip_spaces(const char** p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

/**
 * @brief Parses one type (`int`, `float[][]`, `string`, `void`) at `*p` and moves `*p` past it.
 * @param[in,out] p     Position in the text.
 * @param[out]    type  Receives the type tag.
 * @param[out]    depth Receives the number of `[]` pairs.
 * @return false if the text is not a type.
 */
static bool parse_type(const char** p, uint8_t* type, uint8_t* depth) {
    static const struct { const char* name; uint8_t tag; } names[] = {
        { "int", T_INT }, { "uint", T_UINT }, { "float", T_FLOAT }, { "bool", T_BOOL }, { "byte", T_BYTE },
        { "address", T_ADDRESS }, { "string", T_STRINGADDRESS }, { "void", T_NONE }
    };
    const char* s;
    size_t len, k;
    unsigned d = 0;

    skip_spaces(p);
    s = *p;
    while ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || (**p >= '0' && **p <= '9') || **p == '_') (*p)++;
    len = (size_t)(*p - s);

    for (k = 0; k < sizeof names / sizeof names[0]; k++) {
        if (strlen(names[k].name) == len && memcmp(names[k].name, s, len) == 0) break;
    }
    if (k == sizeof names / sizeof names[0]) return false;

    for (;;) {
        skip_spaces(p);
        if ((*p)[0] != '[') break;
        (*p)++;
        skip_spaces(p);
        if (**p != ']') return false;
        (*p)++;
        if (++d > 255) return false;
    }

    *type = names[k].tag;
    *depth = (uint8_t)d;
    return true;
}

/**
 * @brief Parses a signature such as `"float(float[], int)"`.
 * @param[in]  text The signature text.
 * @param[out] sig  Receives the parsed signature.
 * @return false if it does not parse or uses a type a host function cannot have.
 */
static bool sig_parse(const char* text, iHostSig* sig) {
    const char* p = text;

    memset(sig, 0, sizeof *sig);
    if (!parse_type(&p, &sig->ret_type, &sig->ret_depth) || !sig_type_valid(sig->ret_type, sig->ret_depth, true)) return false;
    skip_spaces(&p);
    if (*p++ != '(') return false;
    skip_spaces(&p);
    if (*p != ')') {
        for (;;) {
            if (sig->argc >= ILANG_HOST_MAX_ARGS) return false;
            if (!parse_type(&p, &sig->arg_type[sig->argc], &sig->arg_depth[sig->argc])) return false;
            if (!sig_type_valid(sig->arg_type[sig->argc], sig->arg_depth[sig->argc], false)) return false;
            sig->argc++;
            skip_spaces(&p);
            if (*p == ',') { p++; continue; }
            break;
        }
    }
    if (*p++ != ')') return false;
    skip_spaces(&p);
    return *p == '\0';
}

/* ------------------------------------------------------------------ */
/* Defining host functions                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief A host function name must be a valid Ilang identifier of at most 255 bytes.
 * @param name Zero-terminated name.
 */
static bool name_valid(const char* name) {
    size_t i, n = strlen(name);

    if (n == 0 || n > 255) return false;
    for (i = 0; i < n; i++) {
        char c = name[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    return true;
}

/**
 * @brief Looks a host function up by name.
 * @param s    The VM.
 * @param name The name (`len` bytes, not necessarily terminated).
 * @param len  Length of `name`.
 * @return The function, or NULL.
 */
static const iHostFunc* host_find(const iState* s, const char* name, size_t len) {
    uint32_t i;

    for (i = 0; i < s->host_count; i++) {
        if (strlen(s->host_funcs[i].name) == len && memcmp(s->host_funcs[i].name, name, len) == 0) return &s->host_funcs[i];
    }
    return NULL;
}

/**
 * @details The name must be a valid Ilang identifier, the signature must parse and the name must not be defined yet;
 *          otherwise the reason is written to stderr and false is returned. The name is copied. Any bound imports are
 *          dropped, because they point into the table of definitions, which may move: a program is bound after all
 *          definitions.
 */
bool IlangVM_define(iState* s, const char* name, const char* signature, IlangHostFn fn, void* user) {
    iHostSig sig;
    iHostFunc* f;
    char* copy;

    if (!s || !name || !signature || !fn) return false;

    if (!name_valid(name)) {
        fprintf(stderr, "IlangVM: cannot define host function '%s': not a valid Ilang name\n", name);
        return false;
    }
    if (!sig_parse(signature, &sig)) {
        fprintf(stderr, "IlangVM: cannot define host function '%s': bad signature \"%s\" "
            "(expected e.g. \"float(float[], int)\"; at most %d parameters)\n", name, signature, ILANG_HOST_MAX_ARGS);
        return false;
    }
    if (host_find(s, name, strlen(name))) {
        fprintf(stderr, "IlangVM: host function '%s' is defined twice\n", name);
        return false;
    }

    if (s->host_count == s->host_capacity) {
        uint32_t cap = s->host_capacity ? s->host_capacity * 2u : 16u;
        iHostFunc* grown = (iHostFunc*)realloc(s->host_funcs, (size_t)cap * sizeof *grown);
        if (!grown) return false;
        s->host_funcs = grown;
        s->host_capacity = cap;
    }

    copy = (char*)malloc(strlen(name) + 1);
    if (!copy) return false;
    strcpy(copy, name);

    f = &s->host_funcs[s->host_count++];
    f->name = copy;
    f->sig = sig;
    f->fn = fn;
    f->user = user;

    /* the imports point into host_funcs, which may just have moved: a program is bound after all definitions */
    free((void*)s->imports);
    s->imports = NULL;
    s->import_count = 0;
    return true;
}

/**
 * @brief IlangHostAPI::define. The registry a library receives in `ilang_open()` is the iState itself.
 */
static bool api_define(IlangRegistry* reg, const char* name, const char* signature, IlangHostFn fn, void* user) {
    return IlangVM_define((iState*)reg, name, signature, fn, user);
}

bool IlangVM_open_library(iState* s, IlangOpenFn open) {
    if (!s || !open) return false;
    return open(IlangVM_host_api(), (IlangRegistry*)s);
}

void IlangVM_host_reset(iState* s) {
    uint32_t i;

    if (!s) return;
    for (i = 0; i < s->host_count; i++) free(s->host_funcs[i].name);
    free(s->host_funcs);
    free((void*)s->imports);
    s->host_funcs = NULL;
    s->host_count = s->host_capacity = 0;
    s->imports = NULL;
    s->import_count = 0;
}

/* ------------------------------------------------------------------ */
/* Binding a program's imports (IlangVM_load)                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Records why IlangVM_load() failed (`state->load_error`) and returns false, so callers can `return load_fail(...)`.
 * @param s      The VM.
 * @param format printf-style format.
 */
static bool load_fail(iState* s, const char* format, ...) {
    va_list args;

    va_start(args, format);
    vsnprintf(s->load_error, sizeof s->load_error, format, args);
    va_end(args);
    return false;
}

/**
 * @details Reads the import table that follows the header (layout: see iHeader in IlangVM.h) and binds each entry to
 *          the host function of the same name, which must have been defined with the same signature. On success
 *          `*code_start` is the offset just behind the table; on failure `state->load_error` says what is wrong.
 */
bool host_bind_imports(iState* s, const uint8_t* code, uint32_t size, uint32_t* code_start) {
    uint32_t pos = (uint32_t)sizeof(iHeader);
    uint32_t count, i;

    free((void*)s->imports);
    s->imports = NULL;
    s->import_count = 0;

    if (size - pos < 4u) return load_fail(s, "the file ends inside its import table");
    count = (uint32_t)code[pos] | ((uint32_t)code[pos + 1] << 8) | ((uint32_t)code[pos + 2] << 16) | ((uint32_t)code[pos + 3] << 24);
    pos += 4;
    if (count > 65536u) return load_fail(s, "the import table is malformed (%u entries)", (unsigned)count);

    if (count) {
        s->imports = (const iHostFunc**)calloc(count, sizeof *s->imports);
        if (!s->imports) return load_fail(s, "out of memory");
    }

    for (i = 0; i < count; i++) {
        const char* name;
        uint8_t nlen, a;
        iHostSig want;
        const iHostFunc* f;

        if (pos >= size) return load_fail(s, "the file ends inside its import table");
        nlen = code[pos++];
        if (nlen == 0 || size - pos < (uint32_t)nlen + 3u) return load_fail(s, "the import table is malformed");
        name = (const char*)code + pos;
        pos += nlen;

        memset(&want, 0, sizeof want);
        want.ret_type = code[pos++];
        want.ret_depth = code[pos++];
        want.argc = code[pos++];
        if (want.argc > ILANG_HOST_MAX_ARGS || size - pos < 2u * want.argc) return load_fail(s, "the import table is malformed");
        if (!sig_type_valid(want.ret_type, want.ret_depth, true)) return load_fail(s, "the import table is malformed");
        for (a = 0; a < want.argc; a++) {
            want.arg_type[a] = code[pos++];
            want.arg_depth[a] = code[pos++];
            if (!sig_type_valid(want.arg_type[a], want.arg_depth[a], false)) return load_fail(s, "the import table is malformed");
        }

        f = host_find(s, name, nlen);
        if (!f) {
            char ws[512];
            sig_format(&want, ws, sizeof ws);
            return load_fail(s, "the program needs host function '%.*s' (%s), which no loaded library defines",
                (int)nlen, name, ws);
        }
        if (!sig_eq(&f->sig, &want)) {
            char ws[256], hs[256];
            sig_format(&want, ws, sizeof ws);
            sig_format(&f->sig, hs, sizeof hs);
            return load_fail(s, "host function '%.*s': the program declares it as %s, but the host defines %s",
                (int)nlen, name, ws, hs);
        }
        s->imports[i] = f;
    }

    s->import_count = count;
    *code_start = pos;
    return true;
}

/* ------------------------------------------------------------------ */
/* Calls                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief One call between the VM and a host function, or the root context (IlangVM_context()) used outside any host
 *        function.
 */
struct IlangCall {
    iState* s;  /**< The VM. */
    const iHostFunc* fn;   /**< The function being called; NULL for the root context (IlangVM_context(): outside any host function). */
    const Value* args;  /**< The arguments of the call (registers first, then the stack ones). */
    bool root;  /**< This is the root context. */

    bool has_result;  /**< A `ret_*` function has set `result`. */
    Value result;  /**< The result of the call. */

    bool failed;  /**< The call has failed; `error` says why. */
    char error[ILANG_VM_HOST_ERROR_SIZE];  /**< The first problem found (see call_fail()). */

    /**
     * @brief Blocks made by `new_array` / `new_string` during this call (and values an Ilang function returned to the
     *        host); the ones that end up outside the result are freed.
     */
    uint32_t* made;
    uint8_t* mark;  /**< Per entry of `made`: the entry is part of the returned value (set by claim()). */
    uint32_t made_count,      /**< Number of entries in `made`. */
             made_capacity;   /**< Allocated length of `made`, in entries. */
};

/**
 * @brief Records a failure of the call.
 *
 * Inside a host function the first problem is kept and turned into a panic when the function returns. In the root
 * context there is nothing to return to: the message is only kept for `last_error()`.
 *
 * @param c      The call.
 * @param format printf-style format of the message.
 */
static void call_fail(IlangCall* c, const char* format, ...) {
    va_list args;

    if (c->failed && !c->root) return; /* keep the first problem */
    c->failed = true;
    va_start(args, format);
    vsnprintf(c->error, sizeof c->error, format, args);
    va_end(args);
}

/**
 * @brief The argument accessors and `ret_*` only make sense inside a host function; records an error otherwise.
 * @param c        The call.
 * @param accessor Name of the API function, for the message.
 * @return true inside a host function.
 */
static bool in_host_function(IlangCall* c, const char* accessor) {
    if (c->fn) return true;
    call_fail(c, "%s: not inside a host function", accessor);
    return false;
}

/**
 * @brief Checks that argument `i` exists, is a scalar of `type` and really holds that tag; records an error otherwise.
 * @param c        The call.
 * @param i        Argument index.
 * @param type     Expected type tag.
 * @param accessor Name of the API function, for the message.
 * @return true if the argument can be read.
 */
static bool arg_is(IlangCall* c, uint32_t i, uint8_t type, const char* accessor) {
    const iHostSig* sig;
    char want[64] = "";

    if (!in_host_function(c, accessor)) return false;
    sig = &c->fn->sig;

    if (i >= sig->argc) {
        call_fail(c, "%s(%u): it has only %u argument%s", accessor, (unsigned)i, (unsigned)sig->argc, sig->argc == 1 ? "" : "s");
        return false;
    }
    if (sig->arg_depth[i] != 0 || sig->arg_type[i] != type) {
        put_type(want, sizeof want, sig->arg_type[i], sig->arg_depth[i]);
        call_fail(c, "%s(%u): argument %u is of type %s", accessor, (unsigned)i, (unsigned)i, want);
        return false;
    }
    if (c->args[i].tag != (Types)type) {
        call_fail(c, "argument %u does not hold a %s (bad bytecode?)", (unsigned)i, tag_name(type));
        return false;
    }
    return true;
}

/* ---- the IlangHostAPI functions (a_*): IlangHost.h says what each one promises ---- */

/** @brief IlangHostAPI::argc. */
static uint32_t a_argc(IlangCall* c) { return c->fn ? c->fn->sig.argc : 0u; }
/** @brief IlangHostAPI::name. */
static const char* a_name(IlangCall* c) { return c->fn ? c->fn->name : ""; }
/** @brief IlangHostAPI::user. */
static void* a_user(IlangCall* c) { return c->fn ? c->fn->user : NULL; }

/** @brief IlangHostAPI::arg_int. */
static int32_t a_arg_int(IlangCall* c, uint32_t i) { return arg_is(c, i, T_INT, "arg_int") ? c->args[i].data.i32 : 0; }
/** @brief IlangHostAPI::arg_uint. */
static uint32_t a_arg_uint(IlangCall* c, uint32_t i) { return arg_is(c, i, T_UINT, "arg_uint") ? c->args[i].data.u32 : 0; }
/** @brief IlangHostAPI::arg_float. */
static float a_arg_float(IlangCall* c, uint32_t i) { return arg_is(c, i, T_FLOAT, "arg_float") ? c->args[i].data.f32 : 0.0f; }
/** @brief IlangHostAPI::arg_bool. */
static bool a_arg_bool(IlangCall* c, uint32_t i) { return arg_is(c, i, T_BOOL, "arg_bool") ? c->args[i].data.u8 != 0 : false; }
/** @brief IlangHostAPI::arg_byte. */
static uint8_t a_arg_byte(IlangCall* c, uint32_t i) { return arg_is(c, i, T_BYTE, "arg_byte") ? c->args[i].data.u8 : 0; }
/** @brief IlangHostAPI::arg_address. */
static uint32_t a_arg_address(IlangCall* c, uint32_t i) { return arg_is(c, i, T_ADDRESS, "arg_address") ? c->args[i].data.u32 : 0; }

/**
 * @brief Fills `out` for the array block at `addr` of the given innermost type and depth.
 * @param[in,out] c     The call (an error is recorded on failure).
 * @param[in]     addr  VM address of the block.
 * @param[in]     type  Innermost element type.
 * @param[in]     depth Array levels.
 * @param[out]    out   Receives the view.
 * @param[in]     what  Description of the value, for messages.
 * @return false (and records why) if there is no such block or it is too small for its length.
 */
static bool view(IlangCall* c, uint32_t addr, uint8_t type, uint8_t depth, IlangArray* out, const char* what) {
    uint32_t size = memory_block_size(c->s->memory, addr);
    uint32_t len;
    uint8_t elem = (depth > 1 || (type != T_BOOL && type != T_BYTE)) ? 4 : 1;
    uint8_t* p;

    if (size < 4u) {
        call_fail(c, "%s is not a live array (address 0x%08X)", what, (unsigned)addr);
        return false;
    }
    p = memory_ptr(c->s->memory, addr, size);
    memcpy(&len, p, 4);
    if ((uint64_t)len * elem > (uint64_t)size - 4u) {
        call_fail(c, "%s is damaged: its length %u does not fit its block", what, (unsigned)len);
        return false;
    }

    out->addr = addr;
    out->length = len;
    out->data = p + 4;
    out->type = type;
    out->depth = depth;
    out->elem_size = elem;
    out->reserved_ = 0;
    return true;
}

/** @brief IlangHostAPI::arg_array. */
static bool a_arg_array(IlangCall* c, uint32_t i, IlangArray* out) {
    const iHostSig* sig;
    uint8_t type, depth;
    char what[32];

    memset(out, 0, sizeof *out);
    if (!in_host_function(c, "arg_array")) return false;
    sig = &c->fn->sig;
    if (i >= sig->argc) {
        call_fail(c, "arg_array(%u): it has only %u argument%s", (unsigned)i, (unsigned)sig->argc, sig->argc == 1 ? "" : "s");
        return false;
    }
    type = sig->arg_type[i];
    depth = sig->arg_depth[i];
    sig_norm(&type, &depth);
    if (depth == 0) {
        char have[64] = "";
        put_type(have, sizeof have, sig->arg_type[i], sig->arg_depth[i]);
        call_fail(c, "arg_array(%u): argument %u is of type %s, not an array or string", (unsigned)i, (unsigned)i, have);
        return false;
    }
    if (c->args[i].tag != T_ADDRESS && c->args[i].tag != T_STRINGADDRESS) {
        call_fail(c, "argument %u does not hold an address (bad bytecode?)", (unsigned)i);
        return false;
    }
    snprintf(what, sizeof what, "argument %u", (unsigned)i);
    return view(c, c->args[i].data.u32, type, depth, out, what);
}

/** @brief IlangHostAPI::row. */
static bool a_row(IlangCall* c, const IlangArray* a, uint32_t i, IlangArray* out) {
    uint32_t row;
    char what[48];

    memset(out, 0, sizeof *out);
    if (!a || a->depth < 2) {
        call_fail(c, "row(): the array is not an array of arrays");
        return false;
    }
    if (i >= a->length) {
        call_fail(c, "row(%u): the array has %u row%s", (unsigned)i, (unsigned)a->length, a->length == 1 ? "" : "s");
        return false;
    }
    memcpy(&row, (const uint8_t*)a->data + 4u * i, 4);
    snprintf(what, sizeof what, "row %u", (unsigned)i);
    if (row == 0) {
        call_fail(c, "row %u is empty (0)", (unsigned)i);
        return false;
    }
    return view(c, row, a->type, (uint8_t)(a->depth - 1), out, what);
}

/* ---- blocks the host owns: made by new_array / new_string, or returned to it by an Ilang function ---- */

/**
 * @brief Makes room for one more entry in `c->made`; records an error and returns false if the host is out of memory.
 * @param c The call.
 */
static bool own_room(IlangCall* c) {
    if (c->made_count == c->made_capacity) {
        uint32_t cap = c->made_capacity ? c->made_capacity * 2u : 8u;
        uint32_t* grown = (uint32_t*)realloc(c->made, (size_t)cap * sizeof *grown);
        if (!grown) { call_fail(c, "out of host memory"); return false; }
        c->made = grown;
        c->made_capacity = cap;
    }
    return true;
}

/**
 * @brief Records `addr` and, for an array of arrays, every row below it as owned (a value an Ilang function returned).
 * @param c     The call.
 * @param addr  VM address of the outer block.
 * @param depth Array levels of the value.
 */
static bool own_tree(IlangCall* c, uint32_t addr, uint32_t depth) {
    if (!own_room(c)) return false;
    c->made[c->made_count++] = addr;
    if (depth > 1) {
        uint32_t size = memory_block_size(c->s->memory, addr), n, i, row;
        const uint8_t* p;
        if (size < 4u) return true;
        p = memory_ptr(c->s->memory, addr, size);
        memcpy(&n, p, 4);
        if ((uint64_t)n * 4u > (uint64_t)size - 4u) return true;
        for (i = 0; i < n; i++) {
            memcpy(&row, p + 4u + 4u * i, 4);
            if (row != 0 && !own_tree(c, row, depth - 1u)) return false;
        }
    }
    return true;
}

static long made_index(const IlangCall* c, uint32_t addr);

/**
 * @brief Frees an owned value (rows first) and forgets it.
 * @param c     The call.
 * @param addr  VM address of the outer block.
 * @param depth Array levels of the value.
 * @return false (and records why) if the value does not belong to the host.
 */
static bool release_tree(IlangCall* c, uint32_t addr, uint32_t depth) {
    long k = made_index(c, addr);

    if (k < 0) {
        call_fail(c, "release(): the value at 0x%08X does not belong to the host", (unsigned)addr);
        return false;
    }
    if (depth > 1) {
        uint32_t size = memory_block_size(c->s->memory, addr), n, i, row;
        const uint8_t* p;
        if (size >= 4u) {
            p = memory_ptr(c->s->memory, addr, size);
            memcpy(&n, p, 4);
            if ((uint64_t)n * 4u <= (uint64_t)size - 4u) {
                for (i = 0; i < n; i++) {
                    memcpy(&row, p + 4u + 4u * i, 4);
                    if (row != 0 && made_index(c, row) >= 0 && !release_tree(c, row, depth - 1u)) return false;
                }
            }
        }
        k = made_index(c, addr);
    }
    (void)memory_free(c->s->memory, addr);
    c->made[k] = c->made[--c->made_count];
    return true;
}

/** @brief IlangHostAPI::new_array. Reserves a zeroed block `[uint32 length][elements...]` and remembers it as owned by the call. */
static bool a_new_array(IlangCall* c, uint8_t type, uint8_t depth, uint32_t length, IlangArray* out) {
    uint8_t elem;
    uint64_t bytes;
    uint32_t addr;

    memset(out, 0, sizeof *out);
    if (type == T_STRINGADDRESS) { type = T_BYTE; if (depth == 0) depth = 1; }
    if (depth == 0 || type < T_INT || type > T_BYTE || type == T_STRINGADDRESS) {
        call_fail(c, "new_array(): bad element type %u / depth %u", (unsigned)type, (unsigned)depth);
        return false;
    }
    elem = (depth > 1 || (type != T_BOOL && type != T_BYTE)) ? 4 : 1;
    bytes = 4ull + (uint64_t)length * elem;
    if (bytes > 0xFFFFFFFFull) {
        call_fail(c, "new_array(): %u elements is too many", (unsigned)length);
        return false;
    }

    if (!c->s->memory) {
        call_fail(c, "new_array(): no program is loaded");
        return false;
    }
    if (!own_room(c)) return false;

    addr = memory_reserve(c->s->memory, (uint32_t)bytes);
    if (addr == 0) {
        call_fail(c, "new_array(): out of VM memory (%u bytes wanted)", (unsigned)bytes);
        return false;
    }
    c->made[c->made_count++] = addr;
    memcpy(memory_ptr(c->s->memory, addr, 4), &length, 4);
    return view(c, addr, type, depth, out, "a new array");
}

/** @brief IlangHostAPI::new_string. */
static bool a_new_string(IlangCall* c, const void* bytes, uint32_t length, IlangArray* out) {
    if (!a_new_array(c, T_BYTE, 1, length, out)) return false;
    if (length) {
        if (!bytes) { call_fail(c, "new_string(): no bytes given"); return false; }
        memcpy(out->data, bytes, length);
    }
    return true;
}

/**
 * @brief Stores the result of the call; the result must be what the declaration says, otherwise it is recorded as a failure.
 * @param c        The call.
 * @param type     Type tag of the scalar being returned.
 * @param v        The value.
 * @param accessor Name of the `ret_*` function, for messages.
 */
static void set_result(IlangCall* c, uint8_t type, Value v, const char* accessor) {
    const iHostSig* sig;
    char want[64] = "";

    if (!in_host_function(c, accessor)) return;
    sig = &c->fn->sig;
    if (c->has_result) { call_fail(c, "%s: the function already gave a result", accessor); return; }
    if (sig->ret_depth != 0 || sig->ret_type != type) {
        put_type(want, sizeof want, sig->ret_type, sig->ret_depth);
        call_fail(c, "%s: the function returns %s", accessor, want);
        return;
    }
    c->has_result = true;
    c->result = v;
}

/** @brief IlangHostAPI::ret_int. */
static void a_ret_int(IlangCall* c, int32_t v) { set_result(c, T_INT, (Value){ .tag = T_INT, .data.i32 = v }, "ret_int"); }
/** @brief IlangHostAPI::ret_uint. */
static void a_ret_uint(IlangCall* c, uint32_t v) { set_result(c, T_UINT, (Value){ .tag = T_UINT, .data.u32 = v }, "ret_uint"); }
/** @brief IlangHostAPI::ret_float. */
static void a_ret_float(IlangCall* c, float v) { set_result(c, T_FLOAT, (Value){ .tag = T_FLOAT, .data.f32 = v }, "ret_float"); }
/** @brief IlangHostAPI::ret_bool. */
static void a_ret_bool(IlangCall* c, bool v) { set_result(c, T_BOOL, (Value){ .tag = T_BOOL, .data.u8 = v ? 1 : 0 }, "ret_bool"); }
/** @brief IlangHostAPI::ret_byte. */
static void a_ret_byte(IlangCall* c, uint8_t v) { set_result(c, T_BYTE, (Value){ .tag = T_BYTE, .data.u8 = v }, "ret_byte"); }
/** @brief IlangHostAPI::ret_address. */
static void a_ret_address(IlangCall* c, uint32_t v) { set_result(c, T_ADDRESS, (Value){ .tag = T_ADDRESS, .data.u32 = v }, "ret_address"); }

/** @brief IlangHostAPI::ret_array. The view must match the declared return type. */
static void a_ret_array(IlangCall* c, const IlangArray* a) {
    const iHostSig* sig;
    uint8_t type, depth;
    char want[64] = "", have[64] = "";

    if (!in_host_function(c, "ret_array")) return;
    sig = &c->fn->sig;
    type = sig->ret_type;
    depth = sig->ret_depth;
    if (c->has_result) { call_fail(c, "ret_array: the function already gave a result"); return; }
    sig_norm(&type, &depth);
    if (!a || depth == 0 || a->type != type || a->depth != depth) {
        put_type(want, sizeof want, sig->ret_type, sig->ret_depth);
        if (a) put_type(have, sizeof have, a->type, a->depth);
        call_fail(c, "ret_array: the function returns %s%s%s", want, a ? ", not " : "", have);
        return;
    }
    c->has_result = true;
    c->result = (Value){ .tag = sig->ret_type == T_STRINGADDRESS ? T_STRINGADDRESS : T_ADDRESS, .data.u32 = a->addr };
}

/** @brief IlangHostAPI::error. */
static bool a_error(IlangCall* c, const char* message) {
    call_fail(c, "%s", message ? message : "failed");
    return false;
}

/* ------------------------------------------------------------------ */
/* Calls into the program (`export fn`)                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Looks an `export fn` up by name.
 * @param s    The VM.
 * @param name Name of the exported function.
 * @return The export, or NULL.
 */
static const iExport* export_find(const iState* s, const char* name) {
    uint32_t i;

    for (i = 0; i < s->export_count; i++) {
        if (strcmp(s->exports[i].name, name) == 0) return &s->exports[i];
    }
    return NULL;
}

/**
 * @brief Calls the function at `e` like CALL would, with the arguments already turned into tagged values, and runs the
 *        interpreter (nested, if a host function is calling) until it returns.
 *
 * Every register, flag and the instruction pointer are put back afterwards, so the code that was running (a CALLC)
 * sees no change.
 *
 * @param[in,out] s    The VM.
 * @param[in]     e    The exported function.
 * @param[in]     args The arguments (`nreg` of them go to registers, the rest to the data stack).
 * @param[out]    r0   Receives register 0 (the result) if the call succeeded.
 * @return false if there is no room on the stacks or the program panicked.
 */
static bool vm_call(iState* s, const iExport* e, const Value* args, Value* r0) {
    Value saved_r[REG_COUNT];
    const uint32_t saved_ip = s->ip;
    const bool zf = s->zF, sf = s->sF, cf = s->cF, of = s->oF;
    Value* const saved_top = s->top;
    Value* const saved_bp = s->bp;
    uint32_t* const saved_call = s->callTop;
    const uint32_t argc = e->sig.argc, nstack = argc - e->nreg;
    uint32_t i;
    bool ok;

    if ((size_t)(s->top - s->stack) + nstack > MAX_STACK_SIZE) {
        snprintf(s->panic_message, sizeof s->panic_message, "not enough stack for the arguments");
        return false;
    }
    if ((size_t)(s->callTop - s->callStack) + 2 > MAX_CALL_DEPTH) {
        snprintf(s->panic_message, sizeof s->panic_message, "call stack overflow");
        return false;
    }

    memcpy(saved_r, s->r, sizeof saved_r);
    for (i = 0; i < e->nreg; i++) s->r[i] = args[i];
    for (i = e->nreg; i < argc; i++) *s->top++ = args[i];

    /* CALL, returning to the end of the code: that is where the interpreter stops */
    *s->callTop++ = s->code_size;
    *s->callTop++ = (uint32_t)(s->bp - s->stack);
    s->bp = s->top;
    s->ip = e->address;

    s->host_depth++;
    ok = vm_run(s);
    s->host_depth--;

    ok = ok && !s->halt && s->ip == s->code_size && s->callTop == saved_call;
    if (ok) {
        *r0 = s->r[0];
        s->top = saved_top; /* the callee popped its frame; this drops the pushed arguments, like the caller's LEAVE */
        s->bp = saved_bp;
    }
    memcpy(s->r, saved_r, sizeof saved_r);
    s->ip = saved_ip;
    s->zF = zf; s->sF = sf; s->cF = cf; s->oF = of;
    return ok;
}

/**
 * @brief IlangHostAPI::call: calls an `export fn` by name.
 *
 * The arguments are checked against the exported signature, the function runs through vm_call(), and r0 is converted
 * into `*result`. A string or array it returns becomes owned by the caller.
 */
static bool a_call(IlangCall* c, const char* name, const IlangValue* args, uint32_t argc, IlangValue* result) {
    iState* s = c->s;
    const iExport* e;
    Value vals[ILANG_HOST_MAX_ARGS];
    Value r0;
    uint32_t i;

    if (result) *result = ilang_value_(0, 0);
    if (!name) { call_fail(c, "call(): no function name"); return false; }

    e = export_find(s, name);
    if (!e) {
        call_fail(c, "call(\"%s\"): the program has no `export fn %s`", name, name);
        return false;
    }
    if (argc != e->sig.argc || (argc && !args)) {
        call_fail(c, "call(\"%s\"): it takes %u argument%s, %u given", name, (unsigned)e->sig.argc,
            e->sig.argc == 1 ? "" : "s", (unsigned)argc);
        return false;
    }
    if (!s->memory || s->halt) {
        call_fail(c, "call(\"%s\"): the program is not running%s", name, s->halt ? " (it stopped with a panic)" : "");
        return false;
    }
    if (c->root && !s->started && !s->running) {
        call_fail(c, "call(\"%s\"): run IlangVM_start() first (it sets up the global variables)", name);
        return false;
    }
    if (s->host_depth >= ILANG_VM_MAX_HOST_DEPTH) {
        call_fail(c, "call(\"%s\"): calls between Ilang and C are nested more than %d deep", name, ILANG_VM_MAX_HOST_DEPTH);
        return false;
    }

    for (i = 0; i < argc; i++) {
        uint8_t wt = e->sig.arg_type[i], wd = e->sig.arg_depth[i], ht = args[i].type, hd = args[i].depth;
        uint8_t nwt = wt, nwd = wd;
        sig_norm(&nwt, &nwd);
        sig_norm(&ht, &hd);
        if (nwt != ht || nwd != hd) {
            char want[64] = "", have[64] = "";
            put_type(want, sizeof want, wt, wd);
            put_type(have, sizeof have, args[i].type, args[i].depth);
            call_fail(c, "call(\"%s\"): argument %u is of type %s, but the function takes %s", name, (unsigned)i, have, want);
            return false;
        }
        if (nwd > 0) {
            IlangArray tmp;
            char what[48];
            snprintf(what, sizeof what, "call(\"%s\"): argument %u", name, (unsigned)i);
            if (!view(c, args[i].as.addr, nwt, nwd, &tmp, what)) return false;
            vals[i] = (Value){ .tag = wt == T_STRINGADDRESS ? T_STRINGADDRESS : T_ADDRESS, .data.u32 = args[i].as.addr };
        }
        else switch (wt) {
            case T_INT: vals[i] = (Value){ .tag = T_INT, .data.i32 = args[i].as.i }; break;
            case T_UINT: vals[i] = (Value){ .tag = T_UINT, .data.u32 = args[i].as.u }; break;
            case T_FLOAT: vals[i] = (Value){ .tag = T_FLOAT, .data.f32 = args[i].as.f }; break;
            case T_BOOL: vals[i] = (Value){ .tag = T_BOOL, .data.u8 = args[i].as.b ? 1 : 0 }; break;
            case T_BYTE: vals[i] = (Value){ .tag = T_BYTE, .data.u8 = args[i].as.b }; break;
            default: vals[i] = (Value){ .tag = T_ADDRESS, .data.u32 = args[i].as.addr }; break;
        }
    }

    if (!vm_call(s, e, vals, &r0)) {
        call_fail(c, "call(\"%s\"): %s", name, s->panic_message[0] ? s->panic_message : "the program stopped");
        return false;
    }

    if (e->sig.ret_type == T_NONE) return true;
    if (e->sig.ret_depth > 0 || e->sig.ret_type == T_STRINGADDRESS) {
        uint8_t t = e->sig.ret_type, d = e->sig.ret_depth;
        sig_norm(&t, &d);
        if (!own_tree(c, r0.data.u32, d)) return false;   /* the caller owns what the function returned */
        if (result) { *result = ilang_value_(e->sig.ret_type == T_STRINGADDRESS ? ILANG_STRING : e->sig.ret_type, e->sig.ret_depth); result->as.addr = r0.data.u32; }
        return true;
    }
    if (result) {
        *result = ilang_value_(e->sig.ret_type, 0);
        switch (e->sig.ret_type) {
        case T_INT: result->as.i = r0.data.i32; break;
        case T_FLOAT: result->as.f = r0.data.f32; break;
        case T_BOOL: case T_BYTE: result->as.b = r0.data.u8; break;
        default: result->as.u = r0.data.u32; break;
        }
    }
    return true;
}

/** @brief IlangHostAPI::view. */
static bool a_view(IlangCall* c, const IlangValue* v, IlangArray* out) {
    uint8_t t, d;

    memset(out, 0, sizeof *out);
    if (!v) { call_fail(c, "view(): no value"); return false; }
    t = v->type;
    d = v->depth;
    sig_norm(&t, &d);
    if (d == 0) { call_fail(c, "view(): the value is not an array or a string"); return false; }
    if (!c->s->memory) { call_fail(c, "view(): no program is loaded"); return false; }
    return view(c, v->as.addr, t, d, out, "view(): the value");
}

/** @brief IlangHostAPI::release. */
static bool a_release(IlangCall* c, const IlangValue* v) {
    uint8_t t, d;

    if (!v) { call_fail(c, "release(): no value"); return false; }
    t = v->type;
    d = v->depth;
    sig_norm(&t, &d);
    if (d == 0) { call_fail(c, "release(): the value is not an array or a string"); return false; }
    if (!c->s->memory) return true; /* the program has ended: its memory is gone already */
    return release_tree(c, v->as.addr, d);
}

/** @brief IlangHostAPI::last_error. */
static const char* a_last_error(IlangCall* c) {
    return c->failed ? c->error : "";
}

/**
 * @brief The function table handed to libraries; the initializers follow the order of the fields in IlangHostAPI.
 */
static const IlangHostAPI g_api = {
    ILANG_HOST_API_VERSION,
    (uint32_t)sizeof(IlangHostAPI),
    api_define,
    a_argc, a_name, a_user,
    a_arg_int, a_arg_uint, a_arg_float, a_arg_bool, a_arg_byte, a_arg_address, a_arg_array, a_row,
    a_new_array, a_new_string,
    a_ret_int, a_ret_uint, a_ret_float, a_ret_bool, a_ret_byte, a_ret_address, a_ret_array,
    a_error,
    a_call, a_view, a_release, a_last_error
};

const IlangHostAPI* IlangVM_host_api(void) {
    return &g_api;
}

/**
 * @brief Index of `addr` in the blocks made during the call.
 * @return The index, or -1.
 */
static long made_index(const IlangCall* c, uint32_t addr) {
    uint32_t i;

    for (i = 0; i < c->made_count; i++) if (c->made[i] == addr) return (long)i;
    return -1;
}

/**
 * @brief Marks the returned array and (for an array of arrays) every row below it.
 *
 * They must all have been made during this call, each exactly once, so that the program gets values it alone owns.
 *
 * @param c     The call.
 * @param addr  VM address of the returned block.
 * @param depth Array levels of the returned value.
 * @param what  Description of the value, for messages.
 * @return false (and records why) if something was not made during this call or appears twice.
 */
static bool claim(IlangCall* c, uint32_t addr, uint32_t depth, const char* what) {
    long k = made_index(c, addr);

    if (k < 0) {
        call_fail(c, "%s was not made by new_array / new_string during this call "
            "(an argument cannot be returned as it is: copy it into a new array)", what);
        return false;
    }
    if (c->mark[k]) {
        call_fail(c, "%s appears twice in the returned array", what);
        return false;
    }
    c->mark[k] = 1;

    if (depth > 1) {
        uint32_t n, i, row;
        const uint8_t* p = memory_ptr(c->s->memory, addr, 4);
        memcpy(&n, p, 4);
        p = memory_ptr(c->s->memory, addr, 4u + 4u * n);
        for (i = 0; i < n; i++) {
            char sub[64];
            memcpy(&row, p + 4u + 4u * i, 4);
            snprintf(sub, sizeof sub, "row %u of the result", (unsigned)i);
            if (row == 0) {
                call_fail(c, "%s was never filled (it is 0)", sub);
                return false;
            }
            if (!claim(c, row, depth - 1u, sub)) return false;
        }
    }
    return true;
}

/**
 * @details Calls the host function IMPORT with its arguments (the first NREG in r0.., the rest on top of the data
 *          stack). Afterwards the result must match the declaration, and a returned string or array must have been made
 *          during the call (see claim()). Blocks the function made and did not return are freed here. Any failure
 *          becomes a panic that names the function.
 *
 *          Encoding: `[CALLC][IMPORT:u16][NREG:u8]`.
 */
void op_callc(iState* s) {
    /* [CALLC][IMPORT:u16][NREG:u8] */
    uint16_t idx = fetch_ushort(s);
    uint8_t nreg = fetch_byte(s);
    const iHostFunc* f;
    Value args[ILANG_HOST_MAX_ARGS];
    uint32_t nstack, i;
    IlangCall c;
    bool ok;

    if (s->halt) return;

    if (idx >= s->import_count) {
        IlangVM_panicf(s, "CALLC names import %u, but the program imports %u function%s!",
            (unsigned)idx, (unsigned)s->import_count, s->import_count == 1 ? "" : "s");
        return;
    }
    f = s->imports[idx];

    if (nreg > f->sig.argc || nreg > REG_COUNT) {
        IlangVM_panicf(s, "CALLC to '%s' passes %u arguments in registers, but it takes %u!", f->name, (unsigned)nreg, (unsigned)f->sig.argc);
        return;
    }
    nstack = f->sig.argc - nreg;
    if ((uint32_t)(s->top - s->stack) < nstack) {
        IlangVM_panicf(s, "CALLC to '%s': its arguments are missing from the stack!", f->name);
        return;
    }
    for (i = 0; i < nreg; i++) args[i] = s->r[i];
    for (i = 0; i < nstack; i++) args[nreg + i] = s->top[(ptrdiff_t)i - (ptrdiff_t)nstack];

    memset(&c, 0, sizeof c);
    c.s = s;
    c.fn = f;
    c.args = args;

    ok = f->fn(&c);

    if (s->halt) {
        /* the program panicked inside a call the host function made back into it: that panic was reported already */
        free(c.made);
        return;
    }

    if (!ok && !c.failed) call_fail(&c, "failed (it returned false without calling error())");
    if (ok && !c.failed && f->sig.ret_type != T_NONE && !c.has_result) call_fail(&c, "returned without a result");

    /* ownership: everything in the result must be new; everything new that is not in it is freed */
    if (c.made_count) {
        c.mark = (uint8_t*)calloc(c.made_count, 1);
        if (!c.mark) call_fail(&c, "out of host memory");
        else if (!c.failed && c.has_result && f->sig.ret_depth + (f->sig.ret_type == T_STRINGADDRESS) > 0) {
            uint8_t type = f->sig.ret_type, depth = f->sig.ret_depth;
            sig_norm(&type, &depth);
            (void)claim(&c, c.result.data.u32, depth, "the result");
        }
        for (i = 0; i < c.made_count; i++) {
            if (!c.mark || !c.mark[i] || c.failed) (void)memory_free(s->memory, c.made[i]);
        }
        free(c.mark);
        free(c.made);
    }
    else if (!c.failed && c.has_result && (f->sig.ret_depth > 0 || f->sig.ret_type == T_STRINGADDRESS)) {
        call_fail(&c, "the result was not made by new_array / new_string during this call "
            "(an argument cannot be returned as it is: copy it into a new array)");
    }

    if (c.failed) {
        IlangVM_panicf(s, "host function '%s': %s", f->name, c.error);
        return;
    }

    if (f->sig.ret_type != T_NONE) s->r[0] = c.result;
}

/* ------------------------------------------------------------------ */
/* Exports (the EXPT section) and the embedding API                    */
/* ------------------------------------------------------------------ */

/** @brief Reads a little-endian uint32 at `p`. */
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief Parses the body of an EXPT section: the address of the initializer stub, then the exported functions (layout:
 *        see iHeader in IlangVM.h). Addresses must lie inside the code.
 * @param[in,out] s          The VM.
 * @param[in]     p          Start of the section body.
 * @param[in]     len        Length of the body.
 * @param[in]     code_start Offset where the code starts.
 * @param[in]     code_size  Size of the whole image.
 * @return false on a malformed section (`load_error` says why).
 */
static bool read_exports(iState* s, const uint8_t* p, uint32_t len, uint32_t code_start, uint32_t code_size) {
    uint32_t pos = 8, count, i;

    if (len < 8u) return load_fail(s, "the EXPT section is malformed");
    s->init_address = rd32(p);
    count = rd32(p + 4);
    if (s->init_address < code_start || s->init_address >= code_size) return load_fail(s, "the EXPT section is malformed");
    if (count > 65536u) return load_fail(s, "the EXPT section is malformed");
    if (count) {
        s->exports = (iExport*)calloc(count, sizeof *s->exports);
        if (!s->exports) return load_fail(s, "out of memory");
    }

    for (i = 0; i < count; i++) {
        iExport* e = &s->exports[i];
        uint8_t nlen, a;

        if (len - pos < 1u) return load_fail(s, "the EXPT section is malformed");
        nlen = p[pos++];
        if (nlen == 0 || len - pos < (uint32_t)nlen + 8u) return load_fail(s, "the EXPT section is malformed");
        e->name = (char*)malloc((size_t)nlen + 1);
        if (!e->name) return load_fail(s, "out of memory");
        memcpy(e->name, p + pos, nlen);
        e->name[nlen] = '\0';
        s->export_count = i + 1; /* so host_end_program frees what was read so far */
        pos += nlen;
        e->address = rd32(p + pos);
        pos += 4;
        e->nreg = p[pos++];
        e->sig.ret_type = p[pos++];
        e->sig.ret_depth = p[pos++];
        e->sig.argc = p[pos++];
        if (e->address < code_start || e->address >= code_size || e->sig.argc > ILANG_HOST_MAX_ARGS ||
            e->nreg > e->sig.argc || e->nreg > REG_COUNT || len - pos < 2u * e->sig.argc ||
            !sig_type_valid(e->sig.ret_type, e->sig.ret_depth, true)) {
            return load_fail(s, "the EXPT section is malformed (function '%s')", e->name);
        }
        for (a = 0; a < e->sig.argc; a++) {
            e->sig.arg_type[a] = p[pos++];
            e->sig.arg_depth[a] = p[pos++];
            if (!sig_type_valid(e->sig.arg_type[a], e->sig.arg_depth[a], false))
                return load_fail(s, "the EXPT section is malformed (function '%s')", e->name);
        }
    }
    s->has_init = true;
    return true;
}

/**
 * @details Walks the `{ tag, length, bytes }` sections between the import table and the entry point and reads the ones
 *          it knows (EXPT); tags it does not know are skipped.
 */
bool host_read_sections(iState* s, const uint8_t* code, uint32_t from, uint32_t to, uint32_t size) {
    uint32_t pos = from;

    while (pos < to) {
        uint32_t len;
        const uint8_t* tag;

        if (to - pos < 8u) return load_fail(s, "the extension sections before the code are malformed");
        tag = code + pos;
        len = rd32(code + pos + 4);
        if (len > to - pos - 8u) return load_fail(s, "the extension sections before the code are malformed");
        if (memcmp(tag, "EXPT", 4) == 0) {
            if (s->has_init) return load_fail(s, "the program has two EXPT sections");
            if (!read_exports(s, code + pos + 8, len, to, size)) return false;
        }
        /* any other tag: an extension this VM does not know, skipped */
        pos += 8u + len;
    }
    return true;
}

/** @details Frees what belongs to the loaded program: its exports and the root call context. */
void host_end_program(iState* s) {
    uint32_t i;

    for (i = 0; i < s->export_count; i++) free(s->exports[i].name);
    free(s->exports);
    s->exports = NULL;
    s->export_count = 0;
    s->has_init = false;
    s->started = false;
    s->host_depth = 0;
    if (s->root_call) {
        free(s->root_call->made);
        free(s->root_call);
        s->root_call = NULL;
    }
}

IlangCall* IlangVM_context(iState* s) {
    if (!s) return NULL;
    if (!s->root_call) {
        s->root_call = (IlangCall*)calloc(1, sizeof *s->root_call);
        if (!s->root_call) return NULL;
        s->root_call->s = s;
        s->root_call->root = true;
    }
    return s->root_call;
}

bool IlangVM_start(iState* s) {
    bool ok;

    if (!s || !s->memory) return false;
    if (!s->has_init) {
        snprintf(s->panic_message, sizeof s->panic_message, "the program has no `export fn`: there is nothing to call");
        return false;
    }
    if (s->started) return true;

    s->ip = s->init_address;
    ok = vm_run(s);            /* the stub runs the global initializers and stops with HALT */
    if (!ok) return false;
    s->halt = false;           /* HALT ended the stub, not the program */
    s->started = true;
    return true;
}

bool IlangVM_call(iState* s, const char* name, const IlangValue* args, uint32_t argc, IlangValue* result) {
    IlangCall* c = IlangVM_context(s);
    return c ? a_call(c, name, args, argc, result) : false;
}

void IlangVM_finish(iState* s) {
    if (!s || !s->memory) return;
    vm_teardown(s);
}
