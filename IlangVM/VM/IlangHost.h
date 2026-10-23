/**
 * @file IlangHost.h
 * @brief Writing C functions that Ilang programs can call ("host functions"), and calling Ilang functions from C.
 *
 * This header is all a host library needs: it does not include the VM's own headers, and a library never links against
 * the VM. Everything it can do goes through the function table (IlangHostAPI) that the VM hands to its `ilang_open()`.
 *
 * @section host_ilang In Ilang
 * @code{.unparsed}
 * extern fn dot(a: float[], b: float[]) -> float;
 * extern fn linspace(lo: float, hi: float, n: int) -> float[];
 *
 * fn main() { set x = linspace(0.0, 1.0, 5); print(dot(x, x)); }
 * @endcode
 *
 * @section host_c In C
 * @code
 * #include "IlangHost.h"
 * static const IlangHostAPI* api;
 *
 * static bool dot(IlangCall* c) {
 *     IlangArray a, b;
 *     if (!api->arg_array(c, 0, &a) || !api->arg_array(c, 1, &b)) return false;
 *     if (a.length != b.length) return api->error(c, "the arrays have different lengths");
 *     const float* x = a.data; const float* y = b.data;
 *     float s = 0.0f;
 *     for (uint32_t i = 0; i < a.length; i++) s += x[i] * y[i];
 *     api->ret_float(c, s);
 *     return true;
 * }
 *
 * ILANG_EXPORT bool ilang_open(const IlangHostAPI* host, IlangRegistry* reg) {
 *     if (!ILANG_HOST_API_OK(host)) return false;
 *     api = host;
 *     return api->define(reg, "dot", "float(float[], float[])", dot, NULL);
 * }
 * @endcode
 *
 * Build and run:
 * @code{.sh}
 * gcc -O2 -shared -fPIC mylib.c -o mylib.so      # Windows: gcc -O2 -shared mylib.c -o mylib.dll
 * ilangvm --lib ./mylib.so prog.ic
 * @endcode
 *
 * @section host_rules Rules
 *  - Signatures are written like Ilang types: `"ret(param, param, ...)"` with `int uint float bool byte address
 *    string`, any of `int`..`byte` followed by one or more `[]` (arrays, arrays of arrays), and `void` as a return type
 *    only. Spaces are ignored. `string` and `byte[]` are interchangeable. The VM refuses to load a program whose
 *    `extern fn` declaration does not match the signature the host defined (or names a function nobody defined).
 *  - A host function returns true on success. On failure it returns `api->error(c, "message")` (which is false): the
 *    program then stops with `IlangVM Panic! host function 'name': message`.
 *  - Arguments are BORROWED: array / string data may be read and changed in place, but the function must not keep the
 *    pointers after it returns, and must not store new row addresses into an argument's array of arrays.
 *  - A returned string / array must be made during this call with `new_array` / `new_string`; so must the rows of a
 *    returned array of arrays (each used once, none left 0). Blocks made during the call and not returned are freed
 *    automatically.
 *  - Pointers into VM memory (IlangArray::data) stay valid for the whole call, also across `new_array` / `new_string`.
 *  - A non-void function must call exactly one `ret_*` before returning true. Reading an argument with the wrong
 *    accessor (`arg_int` on a float, ...) is reported as a panic once the function returns.
 *
 * @section host_callback Calling back into Ilang
 * The program marks the functions C may call with `export fn`:
 * @code{.unparsed}
 * export fn compare(a: int, b: int) -> int { return a - b; }
 * @endcode
 * and a host function calls them by name with values built by the `ilang_*` helpers below:
 * @code
 * IlangValue args[2] = { ilang_int(3), ilang_int(5) }, r;
 * if (!api->call(c, "compare", args, 2, &r)) return false;   // the program panicked, or the call was wrong
 * int32_t order = r.as.i;
 * @endcode
 *  - The arguments are borrowed by the Ilang function. Arrays / strings passed must be alive (arguments of the host
 *    function, or made with `new_array` / `new_string`).
 *  - A string / array the Ilang function returns belongs to the caller, like one made with `new_array`: inside a host
 *    function it is freed when the host function returns (unless it is returned), elsewhere it lives until `release()`.
 *  - After a call, look at argument arrays again with `arg_array` / `row`: the Ilang code may have replaced rows.
 *  - A program that embeds the VM does the same outside any host function: `IlangVM_start()`, then `IlangVM_call()` or
 *    `api->call(IlangVM_context(state), ...)`, then `IlangVM_finish()` (see `IlangVM.h`).
 */
#ifndef ILANG_HOST_H
#define ILANG_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Version of the IlangHostAPI table. A library checks it with ILANG_HOST_API_OK() before using the table. */
#define ILANG_HOST_API_VERSION 1u

/**
 * @brief Element types (the same numbers as the VM's operand tags).
 */
enum {
    ILANG_INT = 1,       /**< 32-bit signed integer. */
    ILANG_UINT = 2,      /**< 32-bit unsigned integer. */
    ILANG_FLOAT = 3,     /**< 32-bit float. */
    ILANG_BOOL = 4,      /**< 1 byte per element, 0 or 1. */
    ILANG_ADDRESS = 5,   /**< A raw VM address (4 bytes). */
    ILANG_STRING = 6,    /**< Signatures only: a string argument is seen as ILANG_BYTE, depth 1. */
    ILANG_BYTE = 7       /**< Unsigned 8-bit integer. */
};

/** @brief One running call of a host function (opaque). It is the first argument of every API function. */
typedef struct IlangCall IlangCall;
/** @brief Where `ilang_open()` defines its functions (opaque); passed to IlangHostAPI::define. */
typedef struct IlangRegistry IlangRegistry;

/**
 * @brief A host function.
 *
 * It returns true on success, or `return api->error(c, "message");` (which is false) on failure.
 *
 * @param call The running call; pass it to every API function.
 */
typedef bool (*IlangHostFn)(IlangCall* call);

/**
 * @brief A view of an Ilang array or string.
 *
 *  - depth 1: `data` points at `length` elements of `type` (`int32_t`, `uint32_t`, `float`, `uint8_t` for bool and
 *    byte, `uint32_t` for address). A string is type ILANG_BYTE, depth 1, and is NOT zero-terminated.
 *  - depth 2+: an array of arrays; `data` points at `length` `uint32_t` row addresses. Use `api->row()` to look at a row.
 */
typedef struct IlangArray {
    uint32_t addr;      /**< VM address of the block (what the Ilang program holds). */
    uint32_t length;    /**< Number of elements. */
    void* data;         /**< The elements (see the description of IlangArray for their type). */
    uint8_t type;       /**< Innermost element type, ILANG_INT .. ILANG_BYTE. */
    uint8_t depth;      /**< 1 = flat array, 2 = array of arrays, ... */
    uint8_t elem_size;  /**< Bytes per element of THIS level: 4, or 1 for a flat bool / byte array. */
    uint8_t reserved_;  /**< Padding; do not use. */
} IlangArray;

/**
 * @brief A value passed to or returned from an Ilang function called from C.
 *
 * For an array or string, `as.addr` is its VM address; `api->view()` turns it into an IlangArray. A string has type
 * ILANG_STRING, depth 0 (or ILANG_BYTE, depth 1: the two are the same thing). Type 0 means "no value" (the result of a
 * void function).
 */
typedef struct IlangValue {
    uint8_t type;       /**< ILANG_INT .. ILANG_BYTE, ILANG_STRING, or 0. */
    uint8_t depth;      /**< Array levels: 0 for a scalar or a string. */
    uint8_t reserved_[2];  /**< Padding; do not use. */
    union {
        int32_t i;      /**< ILANG_INT. */
        uint32_t u;     /**< ILANG_UINT. */
        float f;        /**< ILANG_FLOAT. */
        uint8_t b;      /**< ILANG_BOOL (0 / 1) and ILANG_BYTE. */
        uint32_t addr;  /**< ILANG_ADDRESS, and every array or string. */
    } as;  /**< The payload; the member that is valid is given by `type`. */
} IlangValue;

/**
 * @brief A zeroed value of a type (the shared internal helper of the constructors below).
 * @param type  One of the ILANG_* element types, or 0.
 * @param depth Array levels (0 for a scalar or string).
 * @return The value, with its payload cleared.
 */
static inline IlangValue ilang_value_(uint8_t type, uint8_t depth) {
    IlangValue v;
    v.type = type; v.depth = depth; v.reserved_[0] = v.reserved_[1] = 0; v.as.u = 0;
    return v;
}
/** @brief An `int` argument for `api->call()`. */
static inline IlangValue ilang_int(int32_t x) { IlangValue v = ilang_value_(ILANG_INT, 0); v.as.i = x; return v; }
/** @brief A `uint` argument for `api->call()`. */
static inline IlangValue ilang_uint(uint32_t x) { IlangValue v = ilang_value_(ILANG_UINT, 0); v.as.u = x; return v; }
/** @brief A `float` argument for `api->call()`. */
static inline IlangValue ilang_float(float x) { IlangValue v = ilang_value_(ILANG_FLOAT, 0); v.as.f = x; return v; }
/** @brief A `bool` argument for `api->call()`. */
static inline IlangValue ilang_bool(bool x) { IlangValue v = ilang_value_(ILANG_BOOL, 0); v.as.b = x ? 1 : 0; return v; }
/** @brief A `byte` argument for `api->call()`. */
static inline IlangValue ilang_byte(uint8_t x) { IlangValue v = ilang_value_(ILANG_BYTE, 0); v.as.b = x; return v; }
/** @brief An `address` argument for `api->call()` (a raw VM address). */
static inline IlangValue ilang_address(uint32_t x) { IlangValue v = ilang_value_(ILANG_ADDRESS, 0); v.as.addr = x; return v; }
/**
 * @brief An array or a string argument for `api->call()`, from a view (`arg_array`, `row`, `new_array`, `new_string`, `view`).
 * @param a The view; its VM address and type are copied.
 */
static inline IlangValue ilang_array(const IlangArray* a) { IlangValue v = ilang_value_(a->type, a->depth); v.as.addr = a->addr; return v; }

/**
 * @brief The function table the VM hands to `ilang_open()`.
 *
 * Fields are only ever appended, so a library checks `version` and `size` (see ILANG_HOST_API_OK) before it relies on
 * one. All functions take the running IlangCall as their first argument (except `define`, which takes the registry).
 */
typedef struct IlangHostAPI {
    uint32_t version;   /**< ILANG_HOST_API_VERSION of the VM. */
    uint32_t size;      /**< sizeof(IlangHostAPI) in the VM; later versions only append fields. */

    /* ---- registration (from ilang_open) ---- */
    /**
     * @brief Registers a host function (call it from `ilang_open`).
     * @param reg       The registry passed to `ilang_open`.
     * @param name      The name Ilang code uses in its `extern fn` declaration.
     * @param signature Signature string such as `"float(float[], float[])"`.
     * @param fn        The C function.
     * @param user      A pointer passed back by `user()` (may be NULL).
     * @return false if the signature does not parse or the name is already defined (the reason goes to stderr).
     */
    bool (*define)(IlangRegistry* reg, const char* name, const char* signature, IlangHostFn fn, void* user);

    /* ---- the current call ---- */
    uint32_t (*argc)(IlangCall* c);  /**< Number of arguments of the current call. */
    const char* (*name)(IlangCall* c);  /**< The name the current function was defined with. */
    void* (*user)(IlangCall* c);        /**< The `user` pointer given to `define()`. */

    /* ---- arguments (index 0 = first) ---- */
    int32_t (*arg_int)(IlangCall* c, uint32_t i);  /**< Argument `i` (0 = first) as an `int`. */
    uint32_t (*arg_uint)(IlangCall* c, uint32_t i);  /**< Argument `i` as a `uint`. */
    float (*arg_float)(IlangCall* c, uint32_t i);  /**< Argument `i` as a `float`. */
    bool (*arg_bool)(IlangCall* c, uint32_t i);  /**< Argument `i` as a `bool`. */
    uint8_t (*arg_byte)(IlangCall* c, uint32_t i);  /**< Argument `i` as a `byte`. */
    uint32_t (*arg_address)(IlangCall* c, uint32_t i);  /**< Argument `i` as an `address`. */
    bool (*arg_array)(IlangCall* c, uint32_t i, IlangArray* out);                 /**< Argument `i` (an array or a string) as a view in `out`; returns false (and records an error) if it is not one. */
    bool (*row)(IlangCall* c, const IlangArray* a, uint32_t i, IlangArray* out);  /**< Row `i` of the array of arrays `a`, as a view in `out`. */

    /* ---- making values (zeroed; length set) ---- */
    /**
     * @brief Makes a new zeroed array; the VM owns it until it is returned or the call ends.
     * @param c      The running call.
     * @param type   Innermost element type, ILANG_INT .. ILANG_BYTE.
     * @param depth  Nesting (1 = flat array).
     * @param length Number of elements.
     * @param out    Receives the view of the new array.
     * @return false if the memory limit is hit or the arguments are wrong.
     */
    bool (*new_array)(IlangCall* c, uint8_t type, uint8_t depth, uint32_t length, IlangArray* out);
    /**
     * @brief Makes a new string holding a copy of `length` bytes.
     * @param c      The running call.
     * @param bytes  The bytes to copy (may be NULL when `length` is 0).
     * @param length Number of bytes.
     * @param out    Receives the view of the new string.
     * @return false if the memory limit is hit.
     */
    bool (*new_string)(IlangCall* c, const void* bytes, uint32_t length, IlangArray* out);

    /* ---- the result ---- */
    void (*ret_int)(IlangCall* c, int32_t v);  /**< Sets the result of the call to an `int`. */
    void (*ret_uint)(IlangCall* c, uint32_t v);  /**< Sets the result of the call to a `uint`. */
    void (*ret_float)(IlangCall* c, float v);  /**< Sets the result of the call to a `float`. */
    void (*ret_bool)(IlangCall* c, bool v);  /**< Sets the result of the call to a `bool`. */
    void (*ret_byte)(IlangCall* c, uint8_t v);  /**< Sets the result of the call to a `byte`. */
    void (*ret_address)(IlangCall* c, uint32_t v);  /**< Sets the result of the call to an `address`. */
    void (*ret_array)(IlangCall* c, const IlangArray* a);  /**< Sets the result of the call to an array or string made by `new_array` / `new_string` during this call. */

    /* ---- failure: records the message and returns false, so `return api->error(c, "...");` ---- */
    /**
     * @brief Fails the call: records the message and returns false, so `return api->error(c, "...");` works.
     * @param c       The running call.
     * @param message The text shown in the panic message.
     * @return Always false.
     */
    bool (*error)(IlangCall* c, const char* message);

    /**
     * @brief Calls one of the program's `export fn` functions by name.
     * @param c      The running call.
     * @param name   Name of the exported function.
     * @param args   The arguments (see the `ilang_*` constructors).
     * @param argc   Number of arguments.
     * @param result Receives the return value (type 0 for a void function).
     * @return false if the call was wrong (see `last_error`) or the program panicked.
     */
    bool (*call)(IlangCall* c, const char* name, const IlangValue* args, uint32_t argc, IlangValue* result);
    bool (*view)(IlangCall* c, const IlangValue* v, IlangArray* out);   /**< Turns an array / string IlangValue into an IlangArray view in `out`. */
    bool (*release)(IlangCall* c, const IlangValue* v);                 /**< Frees a value the host owns (made by `new_array`, or returned by `call`) before the call ends. */
    const char* (*last_error)(IlangCall* c);                            /**< Why the last failed call / `call()` failed. */
} IlangHostAPI;

/**
 * @brief A library's entry point, looked up by name (`ilang_open`) when the VM loads it.
 * @param api The VM's function table.
 * @param reg The registry to define functions in.
 * @return true on success.
 */
typedef bool (*IlangOpenFn)(const IlangHostAPI* api, IlangRegistry* reg);

/** @brief True if the VM's API is one this header can use (same version, and a table at least as large as this header's). */
#define ILANG_HOST_API_OK(api) ((api) != NULL && (api)->version == ILANG_HOST_API_VERSION && (api)->size >= sizeof(IlangHostAPI))

/**
 * @def ILANG_EXPORT
 * @brief Marks `ilang_open()` as exported from a shared library (`__declspec(dllexport)` on Windows, default
 *        visibility on GCC/clang, nothing elsewhere).
 */
#if defined(_WIN32)
#define ILANG_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define ILANG_EXPORT __attribute__((visibility("default")))
#else
#define ILANG_EXPORT
#endif

#ifdef __cplusplus
}
#endif

#endif /* ILANG_HOST_H */
