/**
 * @file IlangVM.c
 * @brief IlangVM core: the opcode implementations, the interpreter loops and the load / execute entry points.
 *
 * The file is organised as: small helpers; the operand and value primitives; the opcode implementations (`op_*`), one
 * function per instruction group; the portable `switch` interpreter, the computed-goto interpreter and the shared
 * slow-path dispatcher; and finally IlangVM_load(), IlangVM_execute() and vm_teardown(). The default loop on gcc and
 * clang is the quickening interpreter in IlangVM_quick.inc.
 *
 * Optional build switches (see IlangConf.h for the full list): `ILANG_VM_TRACE`, `ILANG_VM_MAX_INSTRUCTIONS`,
 * `ILANG_VM_STRICT_TYPES`, `ILANG_VM_NO_QUICK`, `ILANG_VM_COMPUTED_GOTO`, `ILANG_VM_NO_FILE_IO`.
 */


/* fwrite_unlocked (used to print) needs this before the first system header; it is harmless elsewhere */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

/* Required */
#include "IlangVM.h"
#include "IlangVM_Mem.h"
#include "IlangVM_fetch.h"
#include "IlangVM_locals.h"
#include "IlangVM_panic.h"
#include "IlangVM_cast.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/**
 * @def ilang_fwrite
 * @brief `fwrite_unlocked` where available: output goes through one thread, so glibc's per-call stdio locking is pure
 *        overhead on the print path.
 */
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
#define ilang_fwrite fwrite_unlocked
#else
#define ilang_fwrite fwrite
#endif

/**
 * @def ILANG_VM_QUICK
 * @brief Defined when the quickening interpreter (IlangVM_quick.inc) is the default loop (gcc and clang).
 *
 * It has no per-instruction trace or budget, and it does not check register tags, so builds with
 * `ILANG_VM_TRACE`, `ILANG_VM_MAX_INSTRUCTIONS` or `ILANG_VM_STRICT_TYPES` (or with `ILANG_VM_NO_QUICK`) keep using the
 * original loops.
 */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(ILANG_VM_NO_QUICK) && !defined(ILANG_VM_TRACE) \
    && !defined(ILANG_VM_MAX_INSTRUCTIONS) && !defined(ILANG_VM_STRICT_TYPES)
#define ILANG_VM_QUICK 1
#endif

/**
 * @def ILANG_NOINLINE
 * @brief Marks slow-path instructions as out of line: the compiler inlines every static function that is called once,
 *        and a bigger IlangVM_execute() would leave less of the interpreter's state in registers.
 */
#if defined(_MSC_VER)
#define ILANG_NOINLINE static __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define ILANG_NOINLINE static __attribute__((noinline))
#else
#define ILANG_NOINLINE static
#endif

/**
 * @def ILANG_DISPATCH
 * @brief Attributes of the dispatch loop itself: kept out of line (with LTO it would otherwise be folded into main()),
 *        placed with the hot code and aligned to a cache line.
 *
 * On gcc -O3 -flto the unaligned, inlined loop measured ~9% slower on the sieve for no change in instruction count:
 * it is purely a code-layout effect.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ILANG_DISPATCH __attribute__((noinline, hot, aligned(64)))
#else
#define ILANG_DISPATCH
#endif

/**
 * @def ILANG_COMPUTED_GOTO
 * @brief Selects the computed-goto interpreter loop.
 *
 * There are two interpreter loops with identical behaviour: a portable `switch` one (the fallback, and the only one
 * MSVC can build), and one using computed gotos ("labels as values", a GNU extension: gcc and clang) that jumps straight
 * from the end of one instruction to the next, so every instruction has its own indirect branch.
 *
 * The goto loop is OPT-IN: define `ILANG_VM_COMPUTED_GOTO` (gcc/clang only) to use it. Which loop is faster depends on
 * the CPU and compiler (with the instruction pointer in a local, see FastCtx, it measured ~15% faster than the switch
 * loop in a gcc 13 PGO build over ten mixed programs), so time both.
 */
#if (defined(__GNUC__) || defined(__clang__)) && defined(ILANG_VM_COMPUTED_GOTO)
#define ILANG_COMPUTED_GOTO 1
#endif

/**
 * @def ILANG_GOTO_ATTR
 * @brief Keeps gcc from merging the identical "fetch + jump" tails of a computed-goto loop back into one shared
 *        indirect jump, which would defeat the purpose (the `no-gcse` and `no-crossjumping` passes do it).
 */
#if defined(__GNUC__) && !defined(__clang__)
#define ILANG_GOTO_ATTR __attribute__((optimize("no-gcse", "no-crossjumping")))
#else
#define ILANG_GOTO_ATTR
#endif

/**
 * @def ILANG_INLINE
 * @brief Hot helpers must really be inlined (they are called with constant arguments, which then fold away).
 */
#if defined(_MSC_VER)
#define ILANG_INLINE static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define ILANG_INLINE static inline __attribute__((always_inline))
#else
#define ILANG_INLINE static inline
#endif


/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void print_string(iState* s, const char* op, Value v); /* strings section below */

/** @brief True for both address tags: a plain heap address and a string block. */
static inline bool is_address(Value v) {
    return v.tag == T_ADDRESS || v.tag == T_STRINGADDRESS;
}

#ifdef ILANG_VM_STRICT_TYPES
/**
 * @brief STRICT_TYPES: does a register's tag satisfy the type an instruction asks for? UINT and ADDRESS are
 *        interchangeable.
 * @param have The tag the register holds.
 * @param want The type the instruction asks for.
 */
static bool tags_compatible(Types have, uint8_t want) {
    if ((uint8_t)have == want) {
        return true;
    }
    return (have == T_UINT || have == T_ADDRESS) && (want == T_UINT || want == T_ADDRESS);
}
#endif

/**
 * @brief Fetches an immediate of the given type from the instruction stream.
 * @param[in,out] s    The VM.
 * @param[in]     type A T_* tag.
 * @param[out]    out  Receives the tagged value.
 * @return false if the code ends early (the VM is halted) or the type cannot be an immediate (a panic).
 */
static bool fetch_imm(iState* s, uint8_t type, Value* out) {
    Value v = { .tag = (Types)type };

    switch (type) {
    case T_INT:
        v.data.i32 = fetch_int(s);
        break;
    case T_UINT:
    case T_ADDRESS:
        v.data.u32 = fetch_uint(s);
        break;
    case T_FLOAT:
        v.data.f32 = fetch_float(s);
        break;
    case T_BOOL:
        v.data.u8 = (fetch_byte(s) != 0);
        break;
    case T_BYTE:
        v.data.u8 = fetch_byte(s);
        break;
    default:
        IlangVM_panicf(s, "Unsupported immediate type %u!", (unsigned)type);
        return false;
    }

    /* A truncated immediate halts the VM (see IlangVM_fetch.h). */
    if (s->halt) {
        return false;
    }

    *out = v;
    return true;
}

/**
 * @brief Fetches one operand: either a register's value or an immediate.
 * @param[in,out] s      The VM.
 * @param[in]     is_reg true if the operand is a register index, false if it is an inline immediate.
 * @param[in]     type   The type of the instruction (the immediate's type; checked against the register under
 *                       ILANG_VM_STRICT_TYPES).
 * @param[out]    out    Receives the operand value.
 * @return false (with the VM halted) on a malformed operand.
 */
static bool fetch_operand(iState* s, bool is_reg, uint8_t type, Value* out) {
    if (is_reg) {
        uint8_t idx;

        if (!fetch_reg(s, &idx)) return false;

        *out = s->r[idx];

#ifdef ILANG_VM_STRICT_TYPES
        if (!tags_compatible(out->tag, type)) {
            IlangVM_panic(s, "Register type does not match instruction type!");
            return false;
        }
#endif
        return true;
    }

    return fetch_imm(s, type, out);
}

/**
 * @brief Fetches both operands of a binary instruction, first then second.
 *
 * They come from one byte stream, so the order matters; it is sequenced explicitly (`&&`) instead of being left to the
 * evaluation order of an expression.
 *
 * @param[in,out] s    The VM.
 * @param[in]     mode An OpType: whether each operand is a register or an immediate.
 * @param[in]     type The operand type.
 * @param[out]    a    First operand.
 * @param[out]    b    Second operand.
 * @return false (with the VM halted) on a malformed operand or an invalid mode.
 */
static bool fetch_operands(iState* s, uint8_t mode, uint8_t type, Value* a, Value* b) {
    bool a_reg, b_reg;

    switch (mode) {
    case REG_REG: a_reg = true;  b_reg = true;  break;
    case REG_IMM: a_reg = true;  b_reg = false; break;
    case IMM_REG: a_reg = false; b_reg = true;  break;
    case IMM_IMM: a_reg = false; b_reg = false; break;
    default:
        IlangVM_panicf(s, "Invalid operand mode %u!", (unsigned)mode);
        return false;
    }

    return fetch_operand(s, a_reg, type, a) && fetch_operand(s, b_reg, type, b);
}

/**
 * @brief Size in bytes of a type when stored in VM memory.
 * @param type A T_* tag.
 * @return 1 for T_BYTE / T_BOOL, 4 for T_INT / T_UINT / T_FLOAT / T_ADDRESS, 0 if unsupported.
 */
static uint32_t mem_type_size(uint8_t type) {
    switch (type) {
    case T_BYTE:
    case T_BOOL:
        return 1;
    case T_INT:
    case T_UINT:
    case T_FLOAT:
    case T_ADDRESS:
        return 4;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* MOV                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief MOV: copies a register or an immediate into a register.
 *
 * Encoding: `[MOV][MODE][IMM_TYPE][TO][FROM_REG | IMMEDIATE]`; MODE is REG_REG or REG_IMM.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_mov(iState* s) {
    /* [MOV][MODE][IMM_TYPE][TO][FROM_REG | IMMEDIATE]   MODE is REG_REG or REG_IMM */
    uint8_t mode = fetch_byte(s);
    uint8_t imm_type = fetch_byte(s);
    uint8_t dst;

    if (!fetch_reg(s, &dst)) return;

    switch (mode) {
    case REG_REG: {
        uint8_t src;
        if (!fetch_reg(s, &src)) return;
        s->r[dst] = s->r[src];
        break;
    }
    case REG_IMM: {
        Value v;
        if (!fetch_imm(s, imm_type, &v)) return;
        s->r[dst] = v;
        break;
    }
    default:
        IlangVM_panic(s, "Unknown MOV \"TYPE\" code!");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ADD / SUB / MUL / DIV / MOD                                         */
/* ------------------------------------------------------------------ */

/** @brief Which arithmetic instruction is being computed. */
typedef enum ArithOp {
    ARITH_ADD,  /**< ADD. */
    ARITH_SUB,  /**< SUB. */
    ARITH_MUL,  /**< MUL. */
    ARITH_DIV,  /**< DIV. */
    ARITH_MOD  /**< MOD. */
} ArithOp;

/**
 * @brief Outcome of the arithmetic.
 *
 * The arithmetic itself is "pure": it never panics, it returns a status instead. The slow path (op_arith()) turns a
 * status into the panic message; the fast path (fast_arith) just declines the instruction and lets the slow path report
 * it. So there is exactly one implementation of the semantics.
 */
typedef enum ArithStatus {
    AR_OK,  /**< The result was computed. */
    AR_DIV_ZERO,  /**< Division by zero. */
    AR_MOD_ZERO,  /**< Modulo by zero. */
    AR_FLOAT_MOD,  /**< Modulo of floats (not defined). */
    AR_BAD_TYPE  /**< The type does not support arithmetic. */
} ArithStatus;

/**
 * @brief The panic message for an arithmetic status.
 * @param st A status other than AR_OK.
 */
static const char* arith_message(ArithStatus st) {
    switch (st) {
    case AR_DIV_ZERO:   return "Division by zero!";
    case AR_MOD_ZERO:   return "Modulo by zero!";
    case AR_FLOAT_MOD:  return "Modulo operation not supported for floating point types!";
    default:            return "Attempt to perform arithmetic on unsupported values!";
    }
}

/**
 * @brief Unsigned arithmetic (UINT, ADDRESS, BYTE, BOOL all go through this). Wraps on overflow.
 * @param[in]  op  The operation.
 * @param[in]  a   Left operand.
 * @param[in]  b   Right operand.
 * @param[out] out Receives the result (only written for AR_OK).
 */
ILANG_INLINE ArithStatus arith_u32(ArithOp op, uint32_t a, uint32_t b, uint32_t* out) {
    switch (op) {
    case ARITH_ADD: *out = a + b; return AR_OK;
    case ARITH_SUB: *out = a - b; return AR_OK;
    case ARITH_MUL: *out = a * b; return AR_OK;
    case ARITH_DIV:
        if (b == 0) return AR_DIV_ZERO;
        *out = a / b;
        return AR_OK;
    case ARITH_MOD:
        if (b == 0) return AR_MOD_ZERO;
        *out = a % b;
        return AR_OK;
    }
    return AR_BAD_TYPE;
}

/**
 * @brief Signed arithmetic. Wraps on overflow instead of invoking undefined behaviour (`INT_MIN / -1` is `INT_MIN`,
 *        `INT_MIN % -1` is 0).
 * @param[in]  op  The operation.
 * @param[in]  a   Left operand.
 * @param[in]  b   Right operand.
 * @param[out] out Receives the result (only written for AR_OK).
 */
ILANG_INLINE ArithStatus arith_i32(ArithOp op, int32_t a, int32_t b, int32_t* out) {
    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;

    switch (op) {
    case ARITH_ADD: *out = (int32_t)(ua + ub); return AR_OK;
    case ARITH_SUB: *out = (int32_t)(ua - ub); return AR_OK;
    case ARITH_MUL: *out = (int32_t)(ua * ub); return AR_OK;
    case ARITH_DIV:
    case ARITH_MOD:
        if (b == 0) return (op == ARITH_MOD) ? AR_MOD_ZERO : AR_DIV_ZERO;
        if (b == -1) {
            /* INT_MIN / -1 and INT_MIN % -1 are undefined behaviour in C. */
            *out = (op == ARITH_DIV) ? (int32_t)(0u - ua) : 0;
            return AR_OK;
        }
        *out = (op == ARITH_DIV) ? (a / b) : (a % b);
        return AR_OK;
    }
    return AR_BAD_TYPE;
}

/**
 * @brief Float arithmetic. Division by zero is reported instead of producing an infinity; modulo is not defined for floats.
 * @param[in]  op  The operation.
 * @param[in]  a   Left operand.
 * @param[in]  b   Right operand.
 * @param[out] out Receives the result (only written for AR_OK).
 */
ILANG_INLINE ArithStatus arith_f32(ArithOp op, float a, float b, float* out) {
    switch (op) {
    case ARITH_ADD: *out = a + b; return AR_OK;
    case ARITH_SUB: *out = a - b; return AR_OK;
    case ARITH_MUL: *out = a * b; return AR_OK;
    case ARITH_DIV:
        if (b == 0.0f) return AR_DIV_ZERO;
        *out = a / b;
        return AR_OK;
    case ARITH_MOD:
        return AR_FLOAT_MOD;
    }
    return AR_BAD_TYPE;
}

/**
 * @brief Computes `a op b` in `type`.
 *
 * Never panics: it returns a status, and `*out` is only written when that is AR_OK. T_BYTE results wrap to 8 bits and
 * T_BOOL results are normalized to 0 / 1.
 *
 * @param[in]  op   The operation.
 * @param[in]  type Operand type tag.
 * @param[in]  a    Left operand.
 * @param[in]  b    Right operand.
 * @param[out] out  Receives the tagged result.
 */
ILANG_INLINE ArithStatus arith_compute(ArithOp op, uint8_t type, Value a, Value b, Value* out) {
    Value r = { .tag = (Types)type };
    ArithStatus st;
    uint32_t u;

    switch (type) {
    case T_INT:
        st = arith_i32(op, a.data.i32, b.data.i32, &r.data.i32);
        break;

    case T_UINT:
    case T_ADDRESS:
        st = arith_u32(op, a.data.u32, b.data.u32, &r.data.u32);
        break;

    case T_FLOAT:
        st = arith_f32(op, a.data.f32, b.data.f32, &r.data.f32);
        break;

    case T_BYTE:
        st = arith_u32(op, a.data.u8, b.data.u8, &u);
        r.data.u8 = (uint8_t)u;
        break;

    case T_BOOL:
        st = arith_u32(op, a.data.u8, b.data.u8, &u);
        r.data.u8 = (u != 0);
        break;

    default:
        return AR_BAD_TYPE;
    }

    if (st == AR_OK) {
        *out = r;
    }
    return st;
}

/**
 * @brief ADD / SUB / MUL / DIV / MOD: `TO = A op B` in TYPE.
 *
 * Encoding: `[ADD|SUB|MUL|DIV|MOD][MODE][TYPE][TO][A][B]`; A and B are registers or immediates of TYPE, as MODE says.
 * Integers wrap; division or modulo by zero and modulo of floats panic.
 *
 * @param[in,out] s  The VM; `ip` points at the operands.
 * @param[in]     op Which operation.
 */
static void op_arith(iState* s, ArithOp op) {
    /* [ADD|SUB|MUL|DIV|MOD][MODE][TYPE][TO][A][B]   A and B are registers or immediates of TYPE, as MODE says */
    uint8_t mode = fetch_byte(s);
    uint8_t type = fetch_byte(s);
    uint8_t to;
    Value a, b, result;
    ArithStatus st;

    if (!fetch_reg(s, &to)) return;
    if (!fetch_operands(s, mode, type, &a, &b)) return;

    st = arith_compute(op, type, a, b, &result);
    if (st != AR_OK) {
        IlangVM_panic(s, arith_message(st));
        return;
    }

    s->r[to] = result;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief RESERVEMEM: `TO` = address of a new zeroed block of SIZE bytes. Panics if the memory cannot be reserved.
 *
 * Encoding: `[RESERVEMEM][TO_REG][SIZE:uint32]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_reservemem(iState* s) {
    uint8_t to;
    uint32_t size, address;

    if (!fetch_reg(s, &to)) return;
    size = fetch_uint(s);
    if (s->halt) return;

    address = memory_reserve(s->memory, size);
    if (address == 0) {
        IlangVM_panic(s, "Failed to reserve VM memory!");
        return;
    }

    s->r[to] = (Value){ .tag = T_ADDRESS, .data.u32 = address };
}

/**
 * @brief FREEMEM: frees the block whose address is in a register. Panics if the register is not an address or the
 *        address is invalid or already freed.
 *
 * Encoding: `[FREEMEM][ADDRESS_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_freemem(iState* s) {
    uint8_t addr_reg;

    if (!fetch_reg(s, &addr_reg)) return;

    if (!is_address(s->r[addr_reg])) {
        IlangVM_panic(s, "FREEMEM requires an address!");
        return;
    }

    if (!memory_free(s->memory, s->r[addr_reg].data.u32)) {
        IlangVM_panicf(s, "FREEMEM on an invalid or already freed address 0x%08" PRIX32 "!", s->r[addr_reg].data.u32);
        return;
    }
}

/**
 * @brief LOAD: reads one element of TYPE (1 byte for T_BOOL / T_BYTE, 4 bytes otherwise) from VM memory.
 *
 * Encoding: `[LOAD][TYPE][ADDRESS_REG][TO_REG]`. A loaded T_BOOL is normalized to 0 / 1. Panics if the register is not
 * an address, the type is unsupported or the access is outside one live block.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_load(iState* s) {
    /* [LOAD][TYPE][ADDRESS_REG][TO_REG] */
    uint8_t type = fetch_byte(s);
    uint8_t addr_reg, to;
    uint32_t size;
    Value result;

    if (!fetch_reg(s, &addr_reg)) return;
    if (!fetch_reg(s, &to)) return;

    if (!is_address(s->r[addr_reg])) {
        IlangVM_panic(s, "LOAD requires an address!");
        return;
    }

    size = mem_type_size(type);
    if (size == 0) {
        IlangVM_panic(s, "LOAD received an unsupported type!");
        return;
    }

    result = (Value){ .tag = (Types)type };

    /* All union members live at offset 0, so reading `size` bytes into
       `data` fills the right member for every supported type. */
    if (!memory_read(s->memory, s->r[addr_reg].data.u32, &result.data, size)) {
        IlangVM_panicf(s, "LOAD attempted to access unmapped memory (%u bytes at 0x%08" PRIX32 ")!",
            (unsigned)size, s->r[addr_reg].data.u32);
        return;
    }

    if (type == T_BOOL) {
        result.data.u8 = (result.data.u8 != 0);
    }

    s->r[to] = result;
}

/**
 * @brief STORE: writes one element of TYPE to VM memory.
 *
 * Encoding: `[STORE][TYPE][ADDRESS_REG][FROM_REG]`. The tag of the value must equal TYPE. Panics if the register is
 * not an address, the types do not match, the type is unsupported or the access is outside one live block.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_store(iState* s) {
    /* [STORE][TYPE][ADDRESS_REG][FROM_REG] */
    uint8_t type = fetch_byte(s);
    uint8_t addr_reg, from;
    uint32_t size;
    Value value;

    if (!fetch_reg(s, &addr_reg)) return;
    if (!fetch_reg(s, &from)) return;

    value = s->r[from];

    if (!is_address(s->r[addr_reg])) {
        IlangVM_panic(s, "STORE requires an address!");
        return;
    }

    if (value.tag != type) {
        IlangVM_panic(s, "STORE value type does not match requested type!");
        return;
    }

    size = mem_type_size(type);
    if (size == 0) {
        IlangVM_panic(s, "STORE received an unsupported type!");
        return;
    }

    if (type == T_BOOL) {
        value.data.u8 = (value.data.u8 != 0);
    }

    if (!memory_write(s->memory, s->r[addr_reg].data.u32, &value.data, size)) {
        IlangVM_panicf(s, "STORE attempted to access unmapped memory (%u bytes at 0x%08" PRIX32 ")!",
            (unsigned)size, s->r[addr_reg].data.u32);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Locals / stack frame                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief LOADLOCAL: `TO` = the data stack slot whose absolute index is in a register (see LEA).
 *
 * Encoding: `[LOADLOCAL][OFFSET_REG][TO]`. Panics if there is no such live slot.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_loadlocal(iState* s) {
    uint8_t offset_reg, to;

    if (!fetch_reg(s, &offset_reg)) return;
    if (!fetch_reg(s, &to)) return;

    s->r[to] = l_get_local(s, s->r[offset_reg].data.u32);
}

/**
 * @brief STORELOCAL: stores a register or an immediate into the data stack slot whose absolute index is in a register.
 *
 * Encoding: `[STORELOCAL][OFFSET_REG][IMMTYPE][IS_REGISTER][FROM_REG | IMMEDIATE]`. Panics if there is no such live slot.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_storelocal(iState* s) {
    /* [STORELOCAL][OFFSET_REG][IMMTYPE][IS_REGISTER][FROM_REG | IMMEDIATE] */
    uint8_t offset_reg, imm_type;
    bool is_reg;
    Value val;

    if (!fetch_reg(s, &offset_reg)) return;
    imm_type = fetch_byte(s);
    is_reg = fetch_byte(s) != 0;

    if (is_reg) {
        uint8_t from;
        if (!fetch_reg(s, &from)) return;
        val = s->r[from];
    }
    else if (!fetch_imm(s, imm_type, &val)) {
        return;
    }

    l_set_local(s, s->r[offset_reg].data.u32, val);
}

/**
 * @brief LEA: `TO` = the absolute stack index of a local or a parameter of the current frame (a T_ADDRESS value that
 *        LOADLOCAL / STORELOCAL understand; it is not an address in VM memory).
 *
 * Encoding: `[LEA][TO_REG][MODE (0 = local, 1 = parameter)][OFFSET:uint32]`. A local is at `bp + OFFSET`, a parameter at
 * `bp - (OFFSET + 1)`. Indices are computed, not pointers, so no out-of-bounds pointer is ever formed.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_lea(iState* s) {
    /* [LEA][TO_REG][MODE (0 = local, 1 = parameter)][OFFSET] */
    uint8_t to = 0;
    uint8_t mode;
    uint32_t offset;
    ptrdiff_t base, index;

    if (!fetch_reg(s, &to)) return;
    mode = fetch_byte(s);
    offset = fetch_uint(s);
    if (s->halt) return;

    /* Work with indices rather than pointers so we never form an
       out-of-bounds pointer (which is UB even if never dereferenced). */
    base = s->bp - s->stack;
    index = (mode == 0) ? base + (ptrdiff_t)offset
                        : base - ((ptrdiff_t)offset + 1);

    s->r[to] = (Value){ .tag = T_ADDRESS, .data.u32 = (uint32_t)index };
}

/**
 * @brief ENTER: reserves AMOUNT zeroed (T_INT 0) local slots at the top of the data stack.
 *
 * Encoding: `[ENTER][AMOUNT:uint32]`.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
static void op_enter(iState* s) {
    uint32_t locals = fetch_uint(s);

    if (s->halt) return;
    l_enter(s, locals);
}

/**
 * @brief LEAVE: releases the top AMOUNT slots of the data stack.
 *
 * Encoding: `[LEAVE][AMOUNT:uint32]`.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
static void op_leave(iState* s) {
    uint32_t locals = fetch_uint(s);

    if (s->halt) return;
    l_leave(s, locals);
}

/* ------------------------------------------------------------------ */
/* CMP                                                                 */
/* ------------------------------------------------------------------ */

/** @brief The result of a comparison: the zero, sign, carry and overflow flags that iState keeps (zF, sF, cF, oF). */
typedef struct CmpFlags {
    bool z,   /**< Zero flag. */
         s,   /**< Sign flag. */
         c,   /**< Carry flag. */
         o;   /**< Overflow flag. */
} CmpFlags;

/** @brief Outcome of cmp_compute(). NaN operands and unsupported types are turned into panics by the caller. */
typedef enum CmpStatus {
    CM_OK,  /**< The flags were computed. */
    CM_NAN,  /**< A float operand is NaN (unordered, which the flags cannot express). */
    CM_BAD_TYPE  /**< The type cannot be compared. */
} CmpStatus;

/**
 * @brief Pure comparison: computes the flags without touching the VM (see ArithStatus for why).
 *
 * The flags are set so that the SAME jump instructions work for every type: `JE: zF`, `JLT: sF != oF`,
 * `JGT: !zF && sF == oF` (etc.). Unsigned types mirror the carry into the sign flag; floats are compared exactly (no
 * epsilon), and a NaN operand is an error.
 *
 * @param[in]  type Operand type tag.
 * @param[in]  a    Left operand.
 * @param[in]  b    Right operand.
 * @param[out] f    Receives the flags.
 */
ILANG_INLINE CmpStatus cmp_compute(uint8_t type, Value a, Value b, CmpFlags* f) {
    f->z = f->s = f->c = f->o = false;

    switch (type) {
    case T_INT: {
        uint32_t ua = (uint32_t)a.data.i32;
        uint32_t ub = (uint32_t)b.data.i32;
        int32_t result = (int32_t)(ua - ub); /* wraps; no signed-overflow UB */

        f->z = (result == 0);
        f->s = (result < 0);
        f->c = (ua < ub);
        f->o = (a.data.i32 >= 0 && b.data.i32 < 0 && result < 0) ||
               (a.data.i32 < 0 && b.data.i32 >= 0 && result >= 0);
        return CM_OK;
    }

    case T_UINT:
    case T_ADDRESS:
        /*
         * Unsigned: "less than" is the carry/borrow, NOT the sign bit of the
         * difference. Mirroring it into sF (with oF = false) makes JLT/JGT/
         * JGE/JLE give the unsigned answer (e.g. 5 < 0xFFFFFFFF).
         */
        f->z = (a.data.u32 == b.data.u32);
        f->c = (a.data.u32 < b.data.u32);
        f->s = f->c;
        return CM_OK;

    case T_FLOAT:
        /*
         * Exact IEEE comparison (no epsilon: an epsilon makes JE and JLT both
         * true for the same operands). NaN is unordered, which these flags
         * can't express, so comparing NaN is an error.
         */
        if (a.data.f32 != a.data.f32 || b.data.f32 != b.data.f32) {
            return CM_NAN;
        }
        f->z = (a.data.f32 == b.data.f32);
        f->s = (a.data.f32 < b.data.f32);
        f->c = f->s;
        return CM_OK;

    case T_BOOL:
    case T_BYTE: {
        int result = (int)a.data.u8 - (int)b.data.u8;

        f->z = (result == 0);
        f->s = (result < 0);
        f->c = (a.data.u8 < b.data.u8);
        return CM_OK;
    }

    default:
        return CM_BAD_TYPE;
    }
}

/**
 * @brief CMP: compares A and B and sets the zero, sign, carry and overflow flags (the conditional jumps read them).
 *
 * Encoding: `[CMP][MODE][TYPE][A][B]`. All four flags are cleared first. Panics when a float operand is NaN or the type
 * cannot be compared.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_cmp(iState* s) {
    /* [CMP][MODE][TYPE][A][B] */
    uint8_t mode = fetch_byte(s);
    uint8_t type = fetch_byte(s);
    Value a, b;
    CmpFlags f;
    CmpStatus st;

    s->zF = false;
    s->sF = false;
    s->cF = false;
    s->oF = false;

    if (!fetch_operands(s, mode, type, &a, &b)) return;

    st = cmp_compute(type, a, b, &f);
    if (st != CM_OK) {
        IlangVM_panic(s, st == CM_NAN ? "Attempt to compare a NaN!"
                                      : "Attempt to compare unsupported types!");
        return;
    }

    s->zF = f.z;
    s->sF = f.s;
    s->cF = f.c;
    s->oF = f.o;
}

/* ------------------------------------------------------------------ */
/* Jumps / calls                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Fetches a jump/call target: an immediate address or an address register.
 * @param[in,out] s       The VM.
 * @param[in]     use_reg true if the operand is a register index, false for an inline 32-bit value.
 * @param[out]    out     Receives the target.
 * @param[in]     err     Panic message if the register does not hold an address.
 * @return false (with the VM halted or panicked) on failure.
 */
static bool fetch_target(iState* s, bool use_reg, uint32_t* out, const char* err) {
    if (use_reg) {
        uint8_t idx;
        Value target;

        if (!fetch_reg(s, &idx)) return false;

        target = s->r[idx];
        if (!is_address(target)) {
            IlangVM_panic(s, err);
            return false;
        }

        *out = target.data.u32;
        return true;
    }

    *out = fetch_uint(s);
    return !s->halt;
}

/**
 * @brief Shared by JMP / JE / JNE / JGT / JLT / JGE / JLE.
 *
 * Encoding: `[Jxx][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) | register]`. The operand is ALWAYS consumed,
 * even when the condition is false, so the instruction pointer ends up after the instruction either way. A relative
 * target is added to the address of the next instruction.
 *
 * @param[in,out] s         The VM; `ip` points at the operands.
 * @param[in]     condition Whether the jump is taken.
 */
static void op_jump(iState* s, bool condition) {
    /* [Jxx][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) | register] */
    bool relative = fetch_byte(s) != 0;
    bool use_reg = fetch_byte(s) != 0;
    uint32_t target;

    if (!fetch_target(s, use_reg, &target,
            "Attempt to jump to a non-address holding register!")) {
        return;
    }

    if (!condition) return;

    s->ip = relative ? (s->ip + target) : target;
}

/**
 * @brief CALL: saves the return address and the caller's frame base on the call stack, starts a new frame at the top
 *        of the data stack and jumps to the target.
 *
 * Encoding: `[CALL][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) | register]`. A call pushes two entries on the
 * call stack (return ip + saved bp); panics with "Call stack overflow!" when they do not fit.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_call(iState* s) {
    /* [CALL][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) | register] */
    bool relative = fetch_byte(s) != 0;
    bool use_reg = fetch_byte(s) != 0;
    uint32_t target;

    if (!fetch_target(s, use_reg, &target,
            "Attempt to call a non-address holding register!")) {
        return;
    }

    /* A call pushes two entries (return ip + saved bp). */
    if ((size_t)(s->callTop - s->callStack) + 2 > MAX_CALL_DEPTH) {
        IlangVM_panic(s, "Call stack overflow!");
        return;
    }

    *s->callTop++ = s->ip;
    *s->callTop++ = (uint32_t)(s->bp - s->stack);
    s->bp = s->top;

    s->ip = relative ? (s->ip + target) : target;
}

/**
 * @brief RET: restores the caller's frame base and resumes after the CALL. Panics on call stack underflow.
 * @param[in,out] s The VM.
 */
static void op_ret(iState* s) {
    uint32_t old_bp_offset, return_ip;

    if ((size_t)(s->callTop - s->callStack) < 2) {
        IlangVM_panic(s, "Call stack underflow!");
        return;
    }

    old_bp_offset = *--s->callTop;
    return_ip = *--s->callTop;

    s->bp = s->stack + old_bp_offset;
    s->ip = return_ip;
}

/* ------------------------------------------------------------------ */
/* AND / OR / XOR                                                      */
/* ------------------------------------------------------------------ */

/** @brief Which bitwise instruction is being computed. */
typedef enum BitOp {
    BIT_AND,  /**< AND. */
    BIT_OR,  /**< OR. */
    BIT_XOR  /**< XOR. */
} BitOp;

/** @brief Panic message for an unsupported operand type, indexed by BitOp. */
static const char* const bit_unsupported_msg[] = {
    "AND received unsupported type!",
    "OR received unsupported type!",
    "XOR received unsupported type!"
};

/**
 * @brief Applies a bitwise operation to two 32-bit values.
 * @param op Which operation.
 * @param a  Left operand.
 * @param b  Right operand.
 */
ILANG_INLINE uint32_t bit_apply(BitOp op, uint32_t a, uint32_t b) {
    switch (op) {
    case BIT_AND: return a & b;
    case BIT_OR:  return a | b;
    case BIT_XOR: return a ^ b;
    }
    return 0;
}

/** @brief Only INT, UINT and BYTE are valid bitwise (and shift) types. */
ILANG_INLINE bool bit_type_ok(uint8_t type) {
    return type == T_INT || type == T_UINT || type == T_BYTE;
}

/**
 * @brief Computes `a op b` in `type`, which must satisfy bit_type_ok().
 * @param op   Which operation.
 * @param type Operand type tag.
 * @param a    Left operand.
 * @param b    Right operand.
 * @return The tagged result.
 */
ILANG_INLINE Value bit_compute(BitOp op, uint8_t type, Value a, Value b) {
    Value r = { .tag = (Types)type };

    switch (type) {
    case T_INT:
        r.data.i32 = (int32_t)bit_apply(op, (uint32_t)a.data.i32, (uint32_t)b.data.i32);
        break;
    case T_UINT:
        r.data.u32 = bit_apply(op, a.data.u32, b.data.u32);
        break;
    case T_BYTE:
        r.data.u8 = (uint8_t)bit_apply(op, a.data.u8, b.data.u8);
        break;
    }

    return r;
}

/**
 * @brief AND / OR / XOR: `TO = A op B`.
 *
 * Encoding: `[AND|OR|XOR][MODE][TYPE][TO][A][B]`. TYPE must be T_INT, T_UINT or T_BYTE, otherwise it panics.
 *
 * @param[in,out] s  The VM; `ip` points at the operands.
 * @param[in]     op Which operation.
 */
static void op_bitwise(iState* s, BitOp op) {
    /* [AND|OR|XOR][MODE][TYPE][TO][A][B] */
    uint8_t mode = fetch_byte(s);
    uint8_t type = fetch_byte(s);
    uint8_t to;
    Value a, b;

    if (!fetch_reg(s, &to)) return;

    if (!bit_type_ok(type)) {
        IlangVM_panic(s, bit_unsupported_msg[op]);
        return;
    }

    if (!fetch_operands(s, mode, type, &a, &b)) return;

    s->r[to] = bit_compute(op, type, a, b);
}

/* ------------------------------------------------------------------ */
/* SHL / SHR                                                          */
/* ------------------------------------------------------------------ */

/** @brief Which shift instruction is being computed. */
typedef enum ShiftOp {
    SHIFT_LEFT,  /**< SHL. */
    SHIFT_RIGHT  /**< SHR. */
} ShiftOp;

/** @brief Panic message for an unsupported operand type, indexed by ShiftOp. */
static const char* const shift_unsupported_msg[] = {
    "SHL received unsupported type!",
    "SHR received unsupported type!"
};

/** @brief Panic message for a negative T_INT shift count, indexed by ShiftOp. */
static const char* const shift_negative_msg[] = {
    "SHL received a negative shift count!",
    "SHR received a negative shift count!"
};

/**
 * @brief Logical shift left of a 32-bit value.
 *
 * Nothing in the shift helpers is undefined behaviour in C: a count at or above the width gives 0 (every bit is
 * shifted out), and the arithmetic right shift is spelled out instead of relying on how the compiler shifts a negative
 * number.
 *
 * @param x Value.
 * @param c Number of bits.
 */
ILANG_INLINE uint32_t shl_u32(uint32_t x, uint32_t c) { return c >= 32u ? 0u : x << c; }
/**
 * @brief Logical shift right of a 32-bit value (see shl_u32()).
 * @param x Value.
 * @param c Number of bits.
 */
ILANG_INLINE uint32_t shr_u32(uint32_t x, uint32_t c) { return c >= 32u ? 0u : x >> c; }

/**
 * @brief Arithmetic shift right: a count of 32 or more leaves only the sign (0 or -1).
 * @param x Value.
 * @param c Number of bits.
 */
ILANG_INLINE int32_t sar_i32(int32_t x, uint32_t c) {
    if (c >= 32u) c = 31u;
    return x >= 0 ? (int32_t)((uint32_t)x >> c) : ~(int32_t)(~(uint32_t)x >> c);
}

/**
 * @brief Computes a shift in `type`, which must satisfy bit_type_ok().
 *
 * Returns false (and writes nothing) for a negative T_INT count, which is a program error rather than a shift; the
 * caller panics.
 *
 * @param[in]  op   Which shift.
 * @param[in]  type Operand type tag.
 * @param[in]  a    The value to shift.
 * @param[in]  b    The count, in bits.
 * @param[out] out  Receives the tagged result.
 */
ILANG_INLINE bool shift_compute(ShiftOp op, uint8_t type, Value a, Value b, Value* out) {
    Value r = { .tag = (Types)type };

    switch (type) {
    case T_INT:
        if (b.data.i32 < 0) return false;
        r.data.i32 = op == SHIFT_LEFT ? (int32_t)shl_u32((uint32_t)a.data.i32, (uint32_t)b.data.i32)
                                      : sar_i32(a.data.i32, (uint32_t)b.data.i32);
        break;
    case T_UINT:
        r.data.u32 = op == SHIFT_LEFT ? shl_u32(a.data.u32, b.data.u32) : shr_u32(a.data.u32, b.data.u32);
        break;
    case T_BYTE:
        if (b.data.u8 >= 8u) r.data.u8 = 0;
        else r.data.u8 = op == SHIFT_LEFT ? (uint8_t)(a.data.u8 << b.data.u8) : (uint8_t)(a.data.u8 >> b.data.u8);
        break;
    }

    *out = r;
    return true;
}

/**
 * @brief SHL / SHR: `TO = A << COUNT` or `A >> COUNT`.
 *
 * Encoding: `[SHL|SHR][MODE][TYPE][TO][A][COUNT]`. TYPE must be T_INT, T_UINT or T_BYTE. A T_INT count must not be
 * negative. A count at or above the width of the type shifts every bit out (the result is 0, except SHR of a negative
 * T_INT, which gives -1); SHR of a T_INT is arithmetic, of the others logical.
 *
 * @param[in,out] s  The VM; `ip` points at the operands.
 * @param[in]     op Which shift.
 */
static void op_shift(iState* s, ShiftOp op) {
    /* [SHL|SHR][MODE][TYPE][TO][A][COUNT] */
    uint8_t mode = fetch_byte(s);
    uint8_t type = fetch_byte(s);
    uint8_t to;
    Value a, b, r;

    if (!fetch_reg(s, &to)) return;

    if (!bit_type_ok(type)) {
        IlangVM_panic(s, shift_unsupported_msg[op]);
        return;
    }

    if (!fetch_operands(s, mode, type, &a, &b)) return;

    if (!shift_compute(op, type, a, b, &r)) {
        IlangVM_panic(s, shift_negative_msg[op]);
        return;
    }

    s->r[to] = r;
}

/* ------------------------------------------------------------------ */
/* Stack                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief PUSH: pushes a register on the data stack. Panics on stack overflow.
 *
 * Encoding: `[PUSH][FROM_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
static void op_push(iState* s) {
    uint8_t from;

    if (!fetch_reg(s, &from)) return;

    if (s->top >= s->stack + MAX_STACK_SIZE) {
        IlangVM_panic(s, "Stack overflow on PUSH!");
        return;
    }

    *s->top++ = s->r[from];
}

/**
 * @brief POP: pops the top of the data stack into a register. Panics on stack underflow.
 *
 * Encoding: `[POP][TO_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
static void op_pop(iState* s) {
    uint8_t to;

    if (!fetch_reg(s, &to)) return;

    if (s->top <= s->stack) {
        IlangVM_panic(s, "Stack underflow on POP!");
        return;
    }

    s->r[to] = *--s->top;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Writes an integer to stdout without printf: the digits are produced into a small buffer and written with one
 *        fwrite.
 * @param v   The magnitude.
 * @param neg true to write a minus sign first.
 */
static void print_u32_fast(uint32_t v, bool neg) {
    char buf[12];
    char* e = buf + sizeof buf;
    char* p = e;

    do { *--p = (char)('0' + v % 10u); v /= 10u; } while (v != 0);
    if (neg) *--p = '-';
    ilang_fwrite(p, 1, (size_t)(e - p), stdout);
}

/**
 * @brief PRINT: writes the value in a register to stdout (no newline).
 *
 * Encoding: `[PRINT][REG]`. Integers print as decimal, floats with `%f`, booleans as `true` / `false`, addresses as
 * `0x%08X`, bytes as decimal, and a string address prints the bytes of the string. Panics for any other tag.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
ILANG_NOINLINE void op_print(iState* s) {
    uint8_t reg;
    Value v;

    if (!fetch_reg(s, &reg)) return;
    v = s->r[reg];

    switch (v.tag) {
    case T_INT:
        print_u32_fast(v.data.i32 < 0 ? 0u - (uint32_t)v.data.i32 : (uint32_t)v.data.i32, v.data.i32 < 0);
        break;
    case T_UINT:
        print_u32_fast(v.data.u32, false);
        break;
    case T_FLOAT:
        printf("%f", (double)v.data.f32);
        break;
    case T_BOOL:
        printf("%s", v.data.u8 ? "true" : "false");
        break;
    case T_ADDRESS:
        printf("0x%08" PRIX32, v.data.u32);
        break;
    case T_BYTE:
        printf("%u", (unsigned)v.data.u8);
        break;
    case T_STRINGADDRESS:
        print_string(s, "PRINT", v);
        break;
    default:
        IlangVM_panic(s, "PRINT received unsupported type!");
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Strings / byte arrays                                               */
/* ------------------------------------------------------------------ */

/*
 * A string block is [uint32 length][bytes]. The length is stored in host byte order, exactly like
 * the array lengths the compiler writes with STORE, so LOAD/len() and these instructions agree.
 */

/**
 * @brief Panics with an error text that has the instruction name in it ("<op> <what>").
 * @param[in,out] s    The VM.
 * @param[in]     op   Instruction name.
 * @param[in]     what The rest of the message.
 */
static void str_error(iState* s, const char* op, const char* what) {
    char msg[128];

    snprintf(msg, sizeof msg, "%s %s", op, what);
    IlangVM_panic(s, msg);
}

/** @brief Why a string operand could not be resolved (STR_OK = it could). */
enum {
    STR_OK = 0,        /**< The operand was resolved. */
    STR_NOT_ADDRESS,   /**< The operand is not an address. */
    STR_UNMAPPED,      /**< The address is not inside a live block. */
    STR_INVALID        /**< The block is too small for its length field. */
};

/**
 * @brief Resolves a string operand to (data, length) without reporting anything.
 *
 * On STR_UNMAPPED the caller reports the address, i.e. `v.data.u32`.
 *
 * @param[in]  memory The VM memory.
 * @param[in]  v      The operand.
 * @param[out] data   Receives the bytes (NULL when `len` is 0).
 * @param[out] len    Receives the length.
 * @return STR_OK, or the reason it could not be resolved.
 */
static int str_resolve(iMemory* memory, Value v, const uint8_t** data, uint32_t* len) {
    const uint8_t* header;
    const uint8_t* body;
    uint32_t n;

    if (!is_address(v)) {
        return STR_NOT_ADDRESS;
    }

    header = memory_ptr(memory, v.data.u32, 4);
    if (header == NULL) {
        return STR_UNMAPPED;
    }

    memcpy(&n, header, sizeof n);

    if (n == 0) {
        *data = NULL;
        *len = 0;
        return STR_OK;
    }

    body = (v.data.u32 <= UINT32_MAX - 4u) ? memory_ptr(memory, v.data.u32 + 4u, n) : NULL;
    if (body == NULL) {
        return STR_INVALID;
    }

    *data = body;
    *len = n;
    return STR_OK;
}

/**
 * @brief Resolves a string operand to (data, length); panics (naming the instruction) if it is not a valid string.
 * @param[in,out] s    The VM.
 * @param[in]     op   Instruction name, for the panic message.
 * @param[in]     v    The operand.
 * @param[out]    data Receives the bytes (NULL when `len` is 0).
 * @param[out]    len  Receives the length.
 * @return false after a panic.
 */
static bool str_view(iState* s, const char* op, Value v, const uint8_t** data, uint32_t* len) {
    switch (str_resolve(s->memory, v, data, len)) {
    case STR_OK:
        return true;
    case STR_NOT_ADDRESS:
        str_error(s, op, "requires a string!");
        return false;
    case STR_UNMAPPED: {
        char what[64];

        snprintf(what, sizeof what, "attempted to access unmapped memory at 0x%08" PRIX32 "!", v.data.u32);
        str_error(s, op, what);
        return false;
    }
    default:
        str_error(s, op, "received something that is not a valid string!");
        return false;
    }
}

/**
 * @brief STRCMP: bytewise comparison, then by length.
 * @return -1, 0 or 1.
 */
static int str_compare(const uint8_t* da, uint32_t la, const uint8_t* db, uint32_t lb) {
    uint32_t common = (la < lb) ? la : lb;
    int c = 0;

    if (common > 0) c = memcmp(da, db, common);

    if (c == 0) {
        c = (la < lb) ? -1 : (la > lb) ? 1 : 0;
    }
    else {
        c = (c < 0) ? -1 : 1;
    }

    return c;
}

/**
 * @brief STRFIND: index of the first occurrence of `needle` in `hay` (an empty needle is found at 0).
 * @return The index, or -1.
 */
static int32_t str_find(const uint8_t* hay, uint32_t lh, const uint8_t* needle, uint32_t ln) {
    int32_t found = -1;

    if (ln == 0) {
        found = 0;
    }
    else if (ln <= lh) {
        uint32_t last = lh - ln;
        uint32_t i;

        for (i = 0; i <= last; i++) {
            if (hay[i] == needle[0] && memcmp(hay + i, needle, ln) == 0) {
                found = (i <= (uint32_t)INT32_MAX) ? (int32_t)i : -1;
                break;
            }
        }
    }

    return found;
}

/**
 * @brief Allocates a string block of `n` bytes: `[uint32 length][bytes]` in VM memory, with the length already stored.
 * @param[in,out] s       The VM (panics if the memory cannot be reserved).
 * @param[in]     op      Instruction name, for the panic message.
 * @param[in]     n       Length of the string in bytes.
 * @param[out]    address Receives the VM address of the block.
 * @param[out]    data    Receives a host pointer to the bytes (NULL when `n` is 0).
 * @return false after a panic.
 */
static bool str_alloc(iState* s, const char* op, uint32_t n, uint32_t* address, uint8_t** data) {
    uint32_t a;
    uint8_t* header;

    a = (n <= UINT32_MAX - 4u) ? memory_reserve(s->memory, 4u + n) : 0;
    if (a == 0) {
        str_error(s, op, "failed to reserve VM memory for a string!");
        return false;
    }

    header = memory_ptr(s->memory, a, 4);
    memcpy(header, &n, sizeof n);

    *address = a;
    *data = (n > 0) ? header + 4 : NULL;
    return true;
}

/** @brief Integer-like register value -> int64 (T_INT, T_UINT, T_BYTE); false for any other tag. */
static bool value_as_i64(Value v, int64_t* out) {
    switch (v.tag) {
    case T_INT:  *out = v.data.i32; return true;
    case T_UINT: *out = v.data.u32; return true;
    case T_BYTE: *out = v.data.u8;  return true;
    default:     return false;
    }
}

/**
 * @brief STRLIT: `TO` = a new string block made from an inline literal.
 *
 * Encoding: `[STRLIT][TO_REG][LENGTH:uint32][BYTES]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_strlit(iState* s) {
    /* [STRLIT][TO][LENGTH:uint32][BYTES] */
    uint8_t to;
    uint32_t n, address;
    uint8_t* data;

    if (!fetch_reg(s, &to)) return;
    n = fetch_uint(s);
    if (s->halt) return;

    if (!fetch_available(s, n)) {
        str_error(s, "STRLIT", "string runs past the end of the bytecode!");
        return;
    }

    if (!str_alloc(s, "STRLIT", n, &address, &data)) return;

    if (n > 0) {
        memcpy(data, s->code + s->ip, n);
        s->ip += n;
    }

    s->r[to] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = address };
}

/**
 * @brief Writes a string block to stdout. Shared by STRPRINT and PRINT.
 * @param[in,out] s  The VM.
 * @param[in]     op Instruction name, for the panic message.
 * @param[in]     v  The string operand.
 */
static void print_string(iState* s, const char* op, Value v) {
    const uint8_t* data;
    uint32_t n;

    if (!str_view(s, op, v, &data, &n)) return;
    if (n > 0) ilang_fwrite(data, 1, n, stdout);
}

/**
 * @brief STRPRINT: writes the bytes of a string block to stdout.
 *
 * Encoding: `[STRPRINT][STR_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
ILANG_NOINLINE void op_strprint(iState* s) {
    uint8_t reg;

    if (!fetch_reg(s, &reg)) return;
    print_string(s, "STRPRINT", s->r[reg]);
}

/**
 * @brief STRCAT: `TO` = a new string, A followed by B.
 *
 * Encoding: `[STRCAT][TO_REG][A_REG][B_REG]`. Every operand is read before the destination is written, so `TO` may be
 * an operand register.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_strcat(iState* s) {
    uint8_t to, ra, rb;
    const uint8_t *da, *db;
    uint32_t la, lb, address;
    uint8_t* out;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &ra) || !fetch_reg(s, &rb)) return;

    if (!str_view(s, "STRCAT", s->r[ra], &da, &la)) return;
    if (!str_view(s, "STRCAT", s->r[rb], &db, &lb)) return;

    if (la > UINT32_MAX - lb) {
        str_error(s, "STRCAT", "result is too large!");
        return;
    }

    if (!str_alloc(s, "STRCAT", la + lb, &address, &out)) return;

    if (la > 0) memcpy(out, da, la);
    if (lb > 0) memcpy(out + la, db, lb);

    s->r[to] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = address };
}

/**
 * @brief STRCMP: `TO` = the int -1, 0 or 1 (bytewise, then by length).
 *
 * Encoding: `[STRCMP][TO_REG][A_REG][B_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_strcmp(iState* s) {
    uint8_t to, ra, rb;
    const uint8_t *da, *db;
    uint32_t la, lb;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &ra) || !fetch_reg(s, &rb)) return;

    if (!str_view(s, "STRCMP", s->r[ra], &da, &la)) return;
    if (!str_view(s, "STRCMP", s->r[rb], &db, &lb)) return;

    s->r[to] = (Value){ .tag = T_INT, .data.i32 = str_compare(da, la, db, lb) };
}

/**
 * @brief STRSLICE: `TO` = a new string holding `COUNT` bytes of a string from `START`. Panics if the range is outside
 *        the string.
 *
 * Encoding: `[STRSLICE][TO_REG][STR_REG][START_REG][COUNT_REG]`; START and COUNT are int / uint / byte registers.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_strslice(iState* s) {
    /* [STRSLICE][TO][STR][START][COUNT] */
    uint8_t to, rs, rstart, rcount;
    const uint8_t* data;
    uint32_t len, address;
    int64_t start, count;
    uint8_t* out;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rs) || !fetch_reg(s, &rstart) || !fetch_reg(s, &rcount)) return;

    if (!str_view(s, "STRSLICE", s->r[rs], &data, &len)) return;

    if (!value_as_i64(s->r[rstart], &start) || !value_as_i64(s->r[rcount], &count)) {
        str_error(s, "STRSLICE", "requires integer start and count!");
        return;
    }

    if (start < 0 || count < 0 || start > (int64_t)len || count > (int64_t)len - start) {
        str_error(s, "STRSLICE", "range is outside the string!");
        return;
    }

    if (!str_alloc(s, "STRSLICE", (uint32_t)count, &address, &out)) return;
    if (count > 0) memcpy(out, data + start, (size_t)count);

    s->r[to] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = address };
}

/**
 * @brief STRFIND: `TO` = the int index of the first match of NEEDLE in HAYSTACK, or -1.
 *
 * Encoding: `[STRFIND][TO_REG][HAYSTACK_REG][NEEDLE_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_strfind(iState* s) {
    uint8_t to, rh, rn;
    const uint8_t *hay, *needle;
    uint32_t lh, ln;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rh) || !fetch_reg(s, &rn)) return;

    if (!str_view(s, "STRFIND", s->r[rh], &hay, &lh)) return;
    if (!str_view(s, "STRFIND", s->r[rn], &needle, &ln)) return;

    s->r[to] = (Value){ .tag = T_INT, .data.i32 = str_find(hay, lh, needle, ln) };
}

/**
 * @brief STRCHR: `TO` = a new 1-byte string. The byte must be 0 to 255.
 *
 * Encoding: `[STRCHR][TO_REG][BYTE_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_strchr(iState* s) {
    uint8_t to, rb;
    int64_t b;
    uint32_t address;
    uint8_t* out;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rb)) return;

    if (!value_as_i64(s->r[rb], &b) || b < 0 || b > 255) {
        str_error(s, "STRCHR", "requires a value from 0 to 255!");
        return;
    }

    if (!str_alloc(s, "STRCHR", 1, &address, &out)) return;
    out[0] = (uint8_t)b;

    s->r[to] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = address };
}

/**
 * @brief STRFROM: `TO` = a new string with the value formatted exactly like PRINT does.
 *
 * Encoding: `[STRFROM][TO_REG][VALUE_REG]`. Panics for a tag PRINT cannot print as text.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_strfrom(iState* s) {
    uint8_t to, rv;
    Value v;
    char buf[64];
    int n;
    uint32_t address;
    uint8_t* out;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rv)) return;
    v = s->r[rv];

    /* The formats match op_print, so str(x) and print(x) always agree. */
    switch (v.tag) {
    case T_INT:     n = snprintf(buf, sizeof buf, "%" PRId32, v.data.i32); break;
    case T_UINT:    n = snprintf(buf, sizeof buf, "%" PRIu32, v.data.u32); break;
    case T_FLOAT:   n = snprintf(buf, sizeof buf, "%f", (double)v.data.f32); break;
    case T_BOOL:    n = snprintf(buf, sizeof buf, "%s", v.data.u8 ? "true" : "false"); break;
    case T_ADDRESS: n = snprintf(buf, sizeof buf, "0x%08" PRIX32, v.data.u32); break;
    case T_BYTE:    n = snprintf(buf, sizeof buf, "%u", (unsigned)v.data.u8); break;
    default:
        str_error(s, "STRFROM", "received an unsupported type!");
        return;
    }

    if (n < 0 || (size_t)n >= sizeof buf) {
        str_error(s, "STRFROM", "failed to format the value!");
        return;
    }

    if (!str_alloc(s, "STRFROM", (uint32_t)n, &address, &out)) return;
    if (n > 0) memcpy(out, buf, (size_t)n);

    s->r[to] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = address };
}

/* ---- number parsing: strict, but surrounding whitespace is allowed ---- */

/** @brief True for a whitespace byte (space, tab, line break, form feed, vertical tab). */
static bool is_space_byte(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/**
 * @brief Strips leading and trailing whitespace from a byte range.
 * @param[in,out] d Start of the range.
 * @param[in,out] n Length of the range.
 */
static void trim_bytes(const uint8_t** d, uint32_t* n) {
    while (*n > 0 && is_space_byte((*d)[0])) { (*d)++; (*n)--; }
    while (*n > 0 && is_space_byte((*d)[*n - 1])) { (*n)--; }
}

/**
 * @brief Parses `[+-]digits` (surrounding whitespace allowed); the value must fit the target type.
 * @param[in]  d           The text.
 * @param[in]  n           Its length.
 * @param[in]  is_unsigned true to parse a uint (no sign allowed), false for an int.
 * @param[out] out         Receives the value.
 * @return false if the text is not a number of that type.
 */
static bool parse_integer(const uint8_t* d, uint32_t n, bool is_unsigned, int64_t* out) {
    bool negative = false;
    uint64_t magnitude = 0;
    uint32_t i = 0;

    trim_bytes(&d, &n);
    if (n == 0) return false;

    if (d[0] == '+' || d[0] == '-') {
        negative = (d[0] == '-');
        i = 1;
    }
    if (i == n) return false;
    if (is_unsigned && negative) return false;

    for (; i < n; i++) {
        if (d[i] < '0' || d[i] > '9') return false;

        magnitude = magnitude * 10u + (uint64_t)(d[i] - '0');
        if (magnitude > 0x100000000ull) return false; /* far too big either way; also prevents wrapping */
    }

    if (is_unsigned) {
        if (magnitude > UINT32_MAX) return false;
        *out = (int64_t)magnitude;
    }
    else if (negative) {
        if (magnitude > 0x80000000ull) return false;
        *out = -(int64_t)magnitude;
    }
    else {
        if (magnitude > INT32_MAX) return false;
        *out = (int64_t)magnitude;
    }

    return true;
}

/**
 * @brief Parses `[+-](digits[.digits] | .digits)[(e|E)[+-]digits]`; no inf, nan, hex or locale tricks.
 * @param[in]  d   The text (surrounding whitespace allowed).
 * @param[in]  n   Its length.
 * @param[out] out Receives the value.
 * @return false if the text is not a finite float.
 */
static bool parse_float_text(const uint8_t* d, uint32_t n, float* out) {
    char buf[64];
    uint32_t i = 0, digits = 0;
    double value;
    float f;

    trim_bytes(&d, &n);
    if (n == 0 || n >= sizeof buf) return false;

    if (d[i] == '+' || d[i] == '-') i++;
    while (i < n && d[i] >= '0' && d[i] <= '9') { i++; digits++; }
    if (i < n && d[i] == '.') {
        i++;
        while (i < n && d[i] >= '0' && d[i] <= '9') { i++; digits++; }
    }
    if (digits == 0) return false;

    if (i < n && (d[i] == 'e' || d[i] == 'E')) {
        uint32_t exp_digits = 0;

        i++;
        if (i < n && (d[i] == '+' || d[i] == '-')) i++;
        while (i < n && d[i] >= '0' && d[i] <= '9') { i++; exp_digits++; }
        if (exp_digits == 0) return false;
    }

    if (i != n) return false;

    memcpy(buf, d, n);
    buf[n] = '\0';

    value = strtod(buf, NULL);
    f = (float)value;
    if (!isfinite(f)) return false;

    *out = f;
    return true;
}

/**
 * @brief STRPARSE: parses a string as an int, uint or float.
 *
 * Encoding: `[STRPARSE][TO_REG][OK_REG][STR_REG][TYPE:uint8]`; TYPE is T_INT, T_UINT or T_FLOAT. `TO` = the parsed value
 * (0 on failure) and `OK` = a bool. Never panics on bad text.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_strparse(iState* s) {
    /* [STRPARSE][TO][OK][STR][TYPE] */
    uint8_t to, ok, rs, type;
    const uint8_t* data;
    uint32_t len;
    Value result = { .tag = T_NONE };
    bool success = false;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &ok) || !fetch_reg(s, &rs)) return;
    type = fetch_byte(s);
    if (s->halt) return;

    if (!str_view(s, "STRPARSE", s->r[rs], &data, &len)) return;

    switch (type) {
    case T_INT: {
        int64_t v = 0;
        success = parse_integer(data, len, false, &v);
        result = (Value){ .tag = T_INT, .data.i32 = success ? (int32_t)v : 0 };
        break;
    }
    case T_UINT: {
        int64_t v = 0;
        success = parse_integer(data, len, true, &v);
        result = (Value){ .tag = T_UINT, .data.u32 = success ? (uint32_t)v : 0u };
        break;
    }
    case T_FLOAT: {
        float v = 0.0f;
        success = parse_float_text(data, len, &v);
        result = (Value){ .tag = T_FLOAT, .data.f32 = success ? v : 0.0f };
        break;
    }
    default:
        str_error(s, "STRPARSE", "can only parse int, uint or float!");
        return;
    }

    s->r[to] = result;
    s->r[ok] = (Value){ .tag = T_BOOL, .data.u8 = success };
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Reads one line from `fp` into a malloc'd buffer (`*buf_out` is NULL when the line is empty).
 *
 * The line ending is dropped, "\r\n" included, on every platform. Shared by READLINE (stdin) and FREADLINE.
 *
 * @param[in,out] s       The VM.
 * @param[in]     op      Instruction name, for the panic message.
 * @param[in]     fp      The stream.
 * @param[out]    buf_out Receives the buffer (the caller frees it).
 * @param[out]    len_out Receives its length.
 * @return false after panicking (line too long, out of memory, read error).
 */
static bool read_line(iState* s, const char* op, FILE* fp, uint8_t** buf_out, size_t* len_out) {
    uint8_t* buf = NULL;
    size_t cap = 0, len = 0;
    int c;

    while ((c = getc(fp)) != EOF) {
        if (c == '\n') break;

        if (len == cap) {
            size_t new_cap = cap ? cap * 2 : ILANG_VM_LINE_INITIAL_CAPACITY;
            uint8_t* grown;

            if (len >= ILANG_VM_MAX_LINE) {
                free(buf);
                str_error(s, op, "line is too long!");
                return false;
            }

            grown = (uint8_t*)realloc(buf, new_cap);
            if (grown == NULL) {
                free(buf);
                str_error(s, op, "ran out of host memory!");
                return false;
            }

            buf = grown;
            cap = new_cap;
        }

        buf[len++] = (uint8_t)c;
    }

    if (c == EOF && ferror(fp)) {
        free(buf);
        IlangVM_panicf(s, "%s failed to read: %s", op, strerror(errno));
        return false;
    }

    if (len > 0 && buf[len - 1] == '\r') len--;

    *buf_out = buf;
    *len_out = len;
    return true;
}

/**
 * @brief Moves `len` host bytes into a new string block, frees the host buffer and stores the string in register `to`.
 * @param[in,out] s   The VM.
 * @param[in]     op  Instruction name, for the panic message.
 * @param[in]     buf Host buffer (freed here; may be NULL when `len` is 0).
 * @param[in]     len Number of bytes.
 * @param[in]     to  Destination register.
 */
static void finish_string(iState* s, const char* op, uint8_t* buf, size_t len, uint8_t to) {
    uint32_t address;
    uint8_t* out;

    if (len > UINT32_MAX - 4u || !str_alloc(s, op, (uint32_t)len, &address, &out)) {
        if (len > UINT32_MAX - 4u) str_error(s, op, "result is too large!");
        free(buf);
        return;
    }

    if (len > 0) memcpy(out, buf, len);
    free(buf);

    s->r[to] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = address };
}

/**
 * @brief READLINE: flushes stdout, then reads one line from stdin into a new string (without the line ending).
 *
 * Encoding: `[READLINE][TO_REG]`. At end of input the string is empty.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
ILANG_NOINLINE void op_readline(iState* s) {
    uint8_t to;
    uint8_t* buf;
    size_t len;

    if (!fetch_reg(s, &to)) return;

    /* Make sure a prompt that was printed without a newline is visible before we wait. */
    fflush(stdout);

    if (!read_line(s, "READLINE", stdin, &buf, &len)) return;
    finish_string(s, "READLINE", buf, len, to);
}

/**
 * @brief ATEOF: `TO` = a bool, true if stdin has no more data (may block until it knows).
 *
 * Encoding: `[ATEOF][TO_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
static void op_ateof(iState* s) {
    uint8_t to;
    int c;

    if (!fetch_reg(s, &to)) return;

    fflush(stdout);

    c = getchar();
    if (c != EOF) ungetc(c, stdin);

    s->r[to] = (Value){ .tag = T_BOOL, .data.u8 = (c == EOF) };
}

/* ------------------------------------------------------------------ */
/* ABORT                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief ABORT: panics with the bytes of a string block as the message.
 *
 * Encoding: `[ABORT][STR_REG]`. The compiler uses it for its runtime checks (index out of bounds, bad number text), so
 * the report names the failing instruction. The message is cut to 512 bytes and trailing line breaks are dropped.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
ILANG_NOINLINE void op_abort(iState* s) {
    /* [ABORT][STR_REG] */
    uint8_t reg;
    const uint8_t* data;
    uint32_t n;

    if (!fetch_reg(s, &reg)) return;
    if (!str_view(s, "ABORT", s->r[reg], &data, &n)) return;

    /* The message is the program's own text: cut it to a sane size and drop trailing line breaks. */
    if (n > 512) n = 512;
    while (n > 0 && (data[n - 1] == '\n' || data[n - 1] == '\r')) n--;

    IlangVM_panicf(s, "%.*s", (int)n, n > 0 ? (const char*)data : "");
}

/* ------------------------------------------------------------------ */
/* File I/O                                                           */
/* ------------------------------------------------------------------ */

/** @brief Records the reason for the last file failure, for FERROR. */
static void io_set_error(iState* s, const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(s->io_error, sizeof s->io_error, fmt, args);
    va_end(args);
}

#ifndef ILANG_VM_NO_FILE_IO
/** @brief A path as it appears in messages: cut to a sane length (precision of `%.*s`). */
static int shown_len(uint32_t len) {
    return (int)(len > 120 ? 120 : len);
}
#endif

/**
 * @brief Resolves a handle operand to an open file that has all the `need` capabilities.
 * @param[in,out] s     The VM (panics on misuse).
 * @param[in]     op    Instruction name, for the panic message.
 * @param[in]     v     The handle operand.
 * @param[in]     need  FILE_CAN_READ / FILE_CAN_WRITE bits that are required (0 = any).
 * @param[out]    index Receives the handle number (may be NULL).
 * @return The file, or NULL after a panic.
 */
static FILE* file_from_handle(iState* s, const char* op, Value v, unsigned need, uint32_t* index) {
    int64_t h;

    if (!value_as_i64(v, &h)) {
        IlangVM_panicf(s, "%s requires an integer file handle!", op);
        return NULL;
    }

    if (h < 0 || h >= ILANG_VM_MAX_FILES || s->files[h] == NULL) {
        if (h == -1 && s->io_error[0] != '\0') {
            IlangVM_panicf(s, "%s received file handle -1: an earlier open failed (%s)", op, s->io_error);
        }
        else {
            IlangVM_panicf(s, "%s received file handle %lld, which is not open!", op, (long long)h);
        }
        return NULL;
    }

    if ((s->file_flags[h] & need) != need) {
        IlangVM_panicf(s, "%s on file handle %lld, which was not opened for %s!", op, (long long)h,
            need == FILE_CAN_READ ? "reading" : "writing");
        return NULL;
    }

    if (index != NULL) *index = (uint32_t)h;
    return s->files[h];
}

/**
 * @brief The most bytes a read may collect: one more than the VM can still hold as a string, so that a file which is
 *        too big is reported by the string allocation ("failed to reserve VM memory") instead of first filling the
 *        host's RAM.
 * @param s      The VM.
 * @param wanted The number of bytes the program asked for.
 */
static uint64_t read_cap(const iState* s, uint64_t wanted) {
    uint64_t avail = memory_available(s->memory);
    uint64_t cap = avail > 4u ? avail - 4u + 1u : 1u;

    return wanted < cap ? wanted : cap;
}

/**
 * @brief Reads up to `limit` bytes from `fp` into a malloc'd buffer (`*buf_out` is NULL when nothing was read).
 *
 * The buffer grows with what is really there, so a huge limit on a small file costs nothing.
 *
 * @param[in,out] s       The VM.
 * @param[in]     op      Instruction name, for the panic message.
 * @param[in]     fp      The stream.
 * @param[in]     limit   Most bytes to read.
 * @param[out]    buf_out Receives the buffer (the caller frees it).
 * @param[out]    len_out Receives the number of bytes read.
 * @return false after panicking (read error, or the host is out of memory).
 */
static bool read_stream(iState* s, const char* op, FILE* fp, uint64_t limit, uint8_t** buf_out, size_t* len_out) {
    uint8_t* buf = NULL;
    size_t cap = 0, len = 0;

    while (len < limit) {
        size_t room, got;

        if (len == cap) {
            uint64_t new_cap = cap ? (uint64_t)cap * 2u : ILANG_VM_READ_INITIAL_CAPACITY;
            uint8_t* grown;

            if (new_cap > limit) new_cap = limit;
            if (new_cap > (uint64_t)SIZE_MAX) new_cap = (uint64_t)SIZE_MAX;

            grown = (uint8_t*)realloc(buf, (size_t)new_cap);
            if (grown == NULL) {
                free(buf);
                str_error(s, op, "ran out of host memory!");
                return false;
            }

            buf = grown;
            cap = (size_t)new_cap;
        }

        room = cap - len;
        got = fread(buf + len, 1, room, fp);
        len += got;

        if (got < room) break; /* end of file, or an error (checked below) */
    }

    if (ferror(fp)) {
        int err = errno;

        free(buf);
        IlangVM_panicf(s, "%s failed to read: %s", op, strerror(err));
        return false;
    }

    *buf_out = buf;
    *len_out = len;
    return true;
}

/**
 * @brief Opens `path` and returns its handle, or -1 after recording the reason (see FERROR).
 * @param[in,out] s     The VM.
 * @param[in]     path  The path bytes (not terminated).
 * @param[in]     len   Their length.
 * @param[in]     fmode The `fopen` mode string.
 * @param[in]     flags FILE_CAN_READ / FILE_CAN_WRITE bits for the new handle.
 */
static int open_file(iState* s, const uint8_t* path, uint32_t len, const char* fmode, uint8_t flags) {
#ifdef ILANG_VM_NO_FILE_IO
    (void)path; (void)len; (void)fmode; (void)flags;
    io_set_error(s, "file I/O is disabled in this VM build");
    return -1;
#else
    char* cpath;
    FILE* fp;
    int slot, err;

    if (len == 0) {
        io_set_error(s, "cannot open an empty path");
        return -1;
    }

    if (memchr(path, 0, len) != NULL) {
        io_set_error(s, "cannot open '%.*s': the path contains a NUL byte", shown_len(len), (const char*)path);
        return -1;
    }

    for (slot = 3; slot < ILANG_VM_MAX_FILES; slot++) {
        if (s->files[slot] == NULL) break;
    }

    if (slot == ILANG_VM_MAX_FILES) {
        io_set_error(s, "cannot open '%.*s': too many open files (the limit is %d)",
            shown_len(len), (const char*)path, ILANG_VM_MAX_FILES - 3);
        return -1;
    }

    cpath = (char*)malloc((size_t)len + 1u);
    if (cpath == NULL) {
        io_set_error(s, "cannot open '%.*s': out of host memory", shown_len(len), (const char*)path);
        return -1;
    }

    memcpy(cpath, path, len);
    cpath[len] = '\0';

    errno = 0;
    fp = fopen(cpath, fmode);
    err = errno;
    free(cpath);

    if (fp == NULL) {
        io_set_error(s, "cannot open '%.*s': %s", shown_len(len), (const char*)path, err ? strerror(err) : "unknown error");
        return -1;
    }

    s->files[slot] = fp;
    s->file_flags[slot] = flags;
    return slot;
#endif
}

/**
 * @brief FOPEN: opens a file; `TO` = the int handle, or -1 on failure (FERROR says why).
 *
 * Encoding: `[FOPEN][TO_REG][PATH_REG][MODE_REG]`. MODE is the string `"r"`, `"w"` or `"a"` (optionally followed by
 * `"b"`; files are always binary). `"r"` reads an existing file, `"w"` creates / truncates, `"a"` appends. An invalid
 * mode panics.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_fopen(iState* s) {
    /* [FOPEN][TO][PATH][MODE] */
    uint8_t to, rp, rm;
    const uint8_t *path, *mode;
    uint32_t path_len, mode_len;
    const char* fmode = NULL;
    uint8_t flags = 0;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rp) || !fetch_reg(s, &rm)) return;

    if (!str_view(s, "FOPEN", s->r[rp], &path, &path_len)) return;
    if (!str_view(s, "FOPEN", s->r[rm], &mode, &mode_len)) return;

    if (mode_len == 1 || (mode_len == 2 && mode[1] == 'b')) {
        switch (mode[0]) {
        case 'r': fmode = "rb"; flags = FILE_CAN_READ;  break;
        case 'w': fmode = "wb"; flags = FILE_CAN_WRITE; break;
        case 'a': fmode = "ab"; flags = FILE_CAN_WRITE; break;
        default: break;
        }
    }

    if (fmode == NULL) {
        IlangVM_panicf(s, "FOPEN mode \"%.*s\" is not valid (use \"r\", \"w\" or \"a\")",
            (int)(mode_len > 16 ? 16 : mode_len), mode_len > 0 ? (const char*)mode : "");
        return;
    }

    s->r[to] = (Value){ .tag = T_INT, .data.i32 = open_file(s, path, path_len, fmode, flags) };
}

/**
 * @brief FCLOSE: flushes and closes a file; `TO` = a bool, false if the flush / close failed or the handle is a standard
 *        stream. The handle is free afterwards either way.
 *
 * Encoding: `[FCLOSE][TO_REG][HANDLE_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_fclose(iState* s) {
    /* [FCLOSE][TO][HANDLE] */
    uint8_t to, rh;
    uint32_t index;
    FILE* fp;
    bool ok = true;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rh)) return;

    fp = file_from_handle(s, "FCLOSE", s->r[rh], 0, &index);
    if (fp == NULL) return;

    if (index < 3) {
        io_set_error(s, "cannot close the standard stream %u", (unsigned)index);
        ok = false;
    }
    else {
        int rc = fclose(fp);
        int err = errno;

        s->files[index] = NULL; /* the handle is gone even if the final flush failed */
        s->file_flags[index] = 0;

        if (rc != 0) {
            io_set_error(s, "cannot close file handle %u: %s", (unsigned)index, strerror(err));
            ok = false;
        }
    }

    s->r[to] = (Value){ .tag = T_BOOL, .data.u8 = ok };
}

/**
 * @brief FREAD: `TO` = a new string of up to COUNT bytes read from a file. Shorter only at the end of the file; empty if
 *        nothing is left.
 *
 * Encoding: `[FREAD][TO_REG][HANDLE_REG][COUNT_REG]`; COUNT is an int / uint / byte register and must not be negative.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_fread(iState* s) {
    /* [FREAD][TO][HANDLE][COUNT] */
    uint8_t to, rh, rc;
    uint32_t index;
    int64_t count;
    FILE* fp;
    uint8_t* buf;
    size_t len;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rh) || !fetch_reg(s, &rc)) return;

    fp = file_from_handle(s, "FREAD", s->r[rh], FILE_CAN_READ, &index);
    if (fp == NULL) return;

    if (!value_as_i64(s->r[rc], &count) || count < 0) {
        IlangVM_panic(s, "FREAD requires a non-negative integer count!");
        return;
    }

    if (index == 0) fflush(stdout);

    if (count > (int64_t)(UINT32_MAX - 4u)) count = (int64_t)(UINT32_MAX - 4u);
    if (!read_stream(s, "FREAD", fp, read_cap(s, (uint64_t)count), &buf, &len)) return;

    finish_string(s, "FREAD", buf, len, to);
}

/**
 * @brief FREADLINE: like READLINE, but from a file: one line without its line ending (a "\r\n" ending is accepted too).
 *        At the end of the file the string is empty.
 *
 * Encoding: `[FREADLINE][TO_REG][HANDLE_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_freadline(iState* s) {
    /* [FREADLINE][TO][HANDLE] */
    uint8_t to, rh;
    uint32_t index;
    FILE* fp;
    uint8_t* buf;
    size_t len;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rh)) return;

    fp = file_from_handle(s, "FREADLINE", s->r[rh], FILE_CAN_READ, &index);
    if (fp == NULL) return;

    if (index == 0) fflush(stdout); /* a prompt without a newline must be visible before we wait */

    if (!read_line(s, "FREADLINE", fp, &buf, &len)) return;
    finish_string(s, "FREADLINE", buf, len, to);
}

/**
 * @brief FWRITE: writes the bytes of a string block to a file; `TO` = a bool, true if all of them were written.
 *
 * Encoding: `[FWRITE][TO_REG][HANDLE_REG][STR_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_fwrite(iState* s) {
    /* [FWRITE][TO][HANDLE][STR] */
    uint8_t to, rh, rs;
    uint32_t index, n;
    const uint8_t* data;
    FILE* fp;
    bool ok = true;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rh) || !fetch_reg(s, &rs)) return;

    fp = file_from_handle(s, "FWRITE", s->r[rh], FILE_CAN_WRITE, &index);
    if (fp == NULL) return;

    if (!str_view(s, "FWRITE", s->r[rs], &data, &n)) return;

    if (index == 2) fflush(stdout); /* keep stdout and stderr in the order the program wrote them */

    if (n > 0 && fwrite(data, 1, n, fp) != n) {
        io_set_error(s, "cannot write to file handle %u: %s", (unsigned)index, strerror(errno));
        clearerr(fp);
        ok = false;
    }

    s->r[to] = (Value){ .tag = T_BOOL, .data.u8 = ok };
}

/**
 * @brief FEOF: `TO` = a bool, true if the file has no more data (may block on stdin).
 *
 * Encoding: `[FEOF][TO_REG][HANDLE_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_feof(iState* s) {
    /* [FEOF][TO][HANDLE] */
    uint8_t to, rh;
    uint32_t index;
    FILE* fp;
    int c;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rh)) return;

    fp = file_from_handle(s, "FEOF", s->r[rh], FILE_CAN_READ, &index);
    if (fp == NULL) return;

    if (index == 0) fflush(stdout);

    c = getc(fp);
    if (c != EOF) ungetc(c, fp);

    if (c == EOF && ferror(fp)) {
        IlangVM_panicf(s, "FEOF failed to read: %s", strerror(errno));
        return;
    }

    s->r[to] = (Value){ .tag = T_BOOL, .data.u8 = (c == EOF) };
}

/**
 * @brief FERROR: `TO` = a new string describing the last failure of FOPEN / FCLOSE / FWRITE (empty if there has been none).
 *
 * Encoding: `[FERROR][TO_REG]`.
 *
 * @param[in,out] s The VM; `ip` points at the operand.
 */
ILANG_NOINLINE void op_ferror(iState* s) {
    /* [FERROR][TO] */
    uint8_t to;
    size_t n;
    uint32_t address;
    uint8_t* out;

    if (!fetch_reg(s, &to)) return;

    n = strlen(s->io_error);
    if (!str_alloc(s, "FERROR", (uint32_t)n, &address, &out)) return;
    if (n > 0) memcpy(out, s->io_error, n);

    s->r[to] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = address };
}

/**
 * @brief FREADFILE: `TO` = a new string holding the whole file.
 *
 * Encoding: `[FREADFILE][TO_REG][PATH_REG]`. Unlike FOPEN, a file that cannot be opened or read panics, with the reason
 * and the path in the message.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_freadfile(iState* s) {
    /* [FREADFILE][TO][PATH] */
    uint8_t to, rp;
    const uint8_t* path;
    uint32_t len;
    uint8_t* buf;
    size_t n;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &rp)) return;
    if (!str_view(s, "FREADFILE", s->r[rp], &path, &len)) return;

#ifdef ILANG_VM_NO_FILE_IO
    (void)to; (void)path; (void)len; (void)buf; (void)n;
    IlangVM_panic(s, "FREADFILE is disabled in this VM build (ILANG_VM_NO_FILE_IO)");
#else
    {
        char* cpath;
        FILE* fp;
        int err;
        bool ok;

        if (len == 0 || memchr(path, 0, len) != NULL) {
            IlangVM_panicf(s, "FREADFILE cannot open '%.*s': %s", shown_len(len), len > 0 ? (const char*)path : "",
                len == 0 ? "the path is empty" : "the path contains a NUL byte");
            return;
        }

        cpath = (char*)malloc((size_t)len + 1u);
        if (cpath == NULL) {
            str_error(s, "FREADFILE", "ran out of host memory!");
            return;
        }

        memcpy(cpath, path, len);
        cpath[len] = '\0';

        errno = 0;
        fp = fopen(cpath, "rb");
        err = errno;
        free(cpath);

        if (fp == NULL) {
            IlangVM_panicf(s, "FREADFILE cannot open '%.*s': %s", shown_len(len), (const char*)path,
                err ? strerror(err) : "unknown error");
            return;
        }

        ok = read_stream(s, "FREADFILE", fp, read_cap(s, (uint64_t)(UINT32_MAX - 4u)), &buf, &n);
        fclose(fp);
        if (!ok) return;

        finish_string(s, "FREADFILE", buf, n, to);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Memory fill                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief MEMSET: stores VALUE into COUNT consecutive elements starting at ADDRESS.
 *
 * Encoding: `[MEMSET][TYPE][ADDRESS_REG][VALUE_REG][COUNT_REG]`. The tag of VALUE must equal TYPE, exactly like STORE.
 * An element is 1 byte for T_BOOL / T_BYTE and 4 bytes for T_INT / T_UINT / T_FLOAT / T_ADDRESS. COUNT is an int / uint
 * / byte register and must not be negative; COUNT == 0 does nothing. The whole range must lie inside ONE live memory
 * block, otherwise the VM panics (nothing is written).
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_memset(iState* s) {
    /* [MEMSET][TYPE][ADDRESS_REG][VALUE_REG][COUNT_REG] */
    uint8_t type, addr_reg, val_reg, count_reg;
    uint8_t pattern[4];
    uint32_t width;
    uint64_t total, filled;
    int64_t count;
    Value value;
    uint8_t* dst;

    type = fetch_byte(s);
    if (!fetch_reg(s, &addr_reg) || !fetch_reg(s, &val_reg) || !fetch_reg(s, &count_reg)) return;

    value = s->r[val_reg];

    if (!is_address(s->r[addr_reg])) {
        IlangVM_panic(s, "MEMSET requires an address!");
        return;
    }

    if (value.tag != type) {
        IlangVM_panic(s, "MEMSET value type does not match requested type!");
        return;
    }

    width = mem_type_size(type);
    if (width == 0) {
        IlangVM_panic(s, "MEMSET received an unsupported type!");
        return;
    }

    if (!value_as_i64(s->r[count_reg], &count) || count < 0) {
        IlangVM_panic(s, "MEMSET requires a non-negative integer count!");
        return;
    }

    if (count == 0) return;

    total = (uint64_t)count * width;
    if (total > 0xFFFFFFFFull) {
        IlangVM_panic(s, "MEMSET range is too large!");
        return;
    }

    /* the whole range must be inside one live block: it is validated up front, so a failing MEMSET writes nothing */
    dst = memory_ptr(s->memory, s->r[addr_reg].data.u32, (uint32_t)total);
    if (dst == NULL) {
        IlangVM_panicf(s, "MEMSET attempted to access unmapped memory (%" PRIu32 " bytes at 0x%08" PRIX32 ")!",
            (uint32_t)total, s->r[addr_reg].data.u32);
        return;
    }

    if (type == T_BOOL) {
        value.data.u8 = (value.data.u8 != 0);
    }

    if (width == 1) {
        memset(dst, value.data.u8, (size_t)total);
        return;
    }

    memcpy(pattern, &value.data, 4);
    if (pattern[0] == pattern[1] && pattern[1] == pattern[2] && pattern[2] == pattern[3]) {
        /* 0, all ones, ...: a plain memset */
        memset(dst, pattern[0], (size_t)total);
        return;
    }

    /* any other 4-byte value: write one element, then keep doubling the filled prefix with memcpy */
    memcpy(dst, pattern, 4);
    filled = 4;
    while (filled < total) {
        uint64_t chunk = total - filled;
        if (chunk > filled) chunk = filled;
        memcpy(dst + filled, dst, (size_t)chunk);
        filled += chunk;
    }
}

/* ------------------------------------------------------------------ */
/* Dynamic arrays and arrays of arrays                                */
/* ------------------------------------------------------------------ */

/*
 * An array block is [uint32 length][elements...]. An array of arrays stores the 4-byte addresses of its rows as
 * its elements and owns them. The helpers below walk such a structure DEPTH levels deep; all of them panic
 * (through str_error) on anything that is not a well-formed array, and report that with their bool result.
 */

/**
 * @brief An int / uint / byte register -> a non-negative element count (panics otherwise).
 * @param[in,out] s   The VM.
 * @param[in]     op  Instruction name, for the panic message.
 * @param[in]     v   The register value.
 * @param[out]    out Receives the count.
 */
static bool array_count(iState* s, const char* op, Value v, uint32_t* out) {
    int64_t n;

    if (!value_as_i64(v, &n)) {
        str_error(s, op, "requires an integer array size!");
        return false;
    }

    if (n < 0) {
        str_error(s, op, "requires a non-negative array size!");
        return false;
    }

    *out = (uint32_t)n;
    return true;
}

/**
 * @brief Reserves a zeroed block for `count` elements of `elem_size` bytes and writes its length field.
 * @param[in,out] s         The VM (panics if the array is too large or memory runs out).
 * @param[in]     op        Instruction name, for the panic message.
 * @param[in]     count     Number of elements.
 * @param[in]     elem_size Bytes per element.
 * @param[out]    address   Receives the VM address of the block.
 * @param[out]    block     Receives a host pointer to the block.
 */
static bool arr_alloc(iState* s, const char* op, uint32_t count, uint32_t elem_size, uint32_t* address, uint8_t** block) {
    uint64_t bytes = 4ull + (uint64_t)count * elem_size;
    uint32_t a;
    uint8_t* p;

    if (bytes > 0xFFFFFFFFull) {
        str_error(s, op, "array is too large!");
        return false;
    }

    a = memory_reserve(s->memory, (uint32_t)bytes);
    if (a == 0) {
        str_error(s, op, "failed to reserve VM memory for an array!");
        return false;
    }

    p = memory_ptr(s->memory, a, (uint32_t)bytes);
    memcpy(p, &count, sizeof count);

    *address = a;
    *block = p;
    return true;
}

/**
 * @brief Resolves the array-of-arrays block at `address` to (block, row count).
 * @param[in,out] s       The VM (panics if it is not a well-formed array of arrays).
 * @param[in]     op      Instruction name, for the panic message.
 * @param[in]     address VM address of the outer block.
 * @param[out]    block   Receives a host pointer to the block.
 * @param[out]    count   Receives the number of rows.
 */
static bool arr_rows(iState* s, const char* op, uint32_t address, uint8_t** block, uint32_t* count) {
    uint32_t size = memory_block_size(s->memory, address);
    uint32_t n;
    uint8_t* p;

    if (size < 4) {
        str_error(s, op, "received something that is not a valid array!");
        return false;
    }

    p = memory_ptr(s->memory, address, size);
    memcpy(&n, p, sizeof n);

    if (n > (size - 4u) / 4u) {
        str_error(s, op, "received something that is not a valid array of arrays!");
        return false;
    }

    *block = p;
    *count = n;
    return true;
}

/**
 * @brief Frees the array at `address`, and first the rows it owns if `depth` > 1.
 * @param[in,out] s       The VM.
 * @param[in]     op      Instruction name, for the panic message.
 * @param[in]     address VM address of the array.
 * @param[in]     depth   Array levels.
 */
static bool arr_free_deep(iState* s, const char* op, uint32_t address, uint32_t depth) {
    if (depth > 1) {
        uint8_t* block;
        uint32_t n, i, row;

        if (!arr_rows(s, op, address, &block, &n)) return false;

        /* Last row first: rows are allocated in order, so each one is then the last entry of the block table and
           removing it moves nothing. Front to back, every free would shift the whole table (quadratic in the row count). */
        for (i = n; i-- > 0;) {
            memcpy(&row, block + 4u + 4u * i, sizeof row);
            if (row == 0) continue;
            if (!arr_free_deep(s, op, row, depth - 1u)) return false;
        }
    }

    if (!memory_free(s->memory, address)) {
        str_error(s, op, "on an invalid or already freed address!");
        return false;
    }

    return true;
}

/**
 * @brief Makes a new block that is a copy of the array at `src`, with a copy of every row below it if `depth` > 1.
 * @param[in,out] s     The VM.
 * @param[in]     op    Instruction name, for the panic message.
 * @param[in]     src   VM address of the array.
 * @param[in]     depth Array levels.
 * @param[out]    out   Receives the address of the copy.
 */
static bool arr_clone_deep(iState* s, const char* op, uint32_t src, uint32_t depth, uint32_t* out) {
    uint32_t size = memory_block_size(s->memory, src);
    uint32_t dst;
    uint8_t* from;
    uint8_t* to;

    if (size < 4) {
        str_error(s, op, "received something that is not a valid array!");
        return false;
    }

    dst = memory_reserve(s->memory, size);
    if (dst == 0) {
        str_error(s, op, "failed to reserve VM memory for an array!");
        return false;
    }

    from = memory_ptr(s->memory, src, size);
    to = memory_ptr(s->memory, dst, size);
    memcpy(to, from, size);

    if (depth > 1) {
        uint32_t n, i, row, copy;

        memcpy(&n, from, sizeof n);
        if (n > (size - 4u) / 4u) {
            str_error(s, op, "received something that is not a valid array of arrays!");
            return false;
        }

        for (i = 0; i < n; i++) {
            memcpy(&row, to + 4u + 4u * i, sizeof row);
            if (row == 0) continue;
            if (!arr_clone_deep(s, op, row, depth - 1u, &copy)) return false;
            memcpy(to + 4u + 4u * i, &copy, sizeof copy);
        }
    }

    *out = dst;
    return true;
}

/**
 * @brief NEWARRAY: `TO` = a new zeroed array block of COUNT elements of ELEM_SIZE bytes, its length field already set.
 *
 * Encoding: `[NEWARRAY][TO_REG][COUNT_REG][ELEM_SIZE:uint8]`. ELEM_SIZE is 1 or 4; COUNT is an int / uint / byte
 * register. A negative COUNT, a size that does not fit the address space or running out of VM memory panics.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_newarray(iState* s) {
    /* [NEWARRAY][TO][COUNT_REG][ELEM_SIZE:uint8] */
    uint8_t to, count_reg, elem_size;
    uint32_t count, address;
    uint8_t* block;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &count_reg)) return;
    elem_size = fetch_byte(s);
    if (s->halt) return;

    if (elem_size != 1 && elem_size != 4) {
        IlangVM_panic(s, "NEWARRAY element size must be 1 or 4!");
        return;
    }

    if (!array_count(s, "NEWARRAY", s->r[count_reg], &count)) return;
    if (!arr_alloc(s, "NEWARRAY", count, elem_size, &address, &block)) return;

    s->r[to] = (Value){ .tag = T_ADDRESS, .data.u32 = address };
}

/**
 * @brief ARRFREE: frees the array block, first freeing every row it owns when DEPTH > 1 (recursively).
 *
 * Encoding: `[ARRFREE][ADDRESS_REG][DEPTH:uint8]`. DEPTH == 1 is the same as FREEMEM.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_arrfree(iState* s) {
    /* [ARRFREE][ADDRESS_REG][DEPTH:uint8] */
    uint8_t reg, depth;

    if (!fetch_reg(s, &reg)) return;
    depth = fetch_byte(s);
    if (s->halt) return;

    if (!is_address(s->r[reg])) {
        IlangVM_panic(s, "ARRFREE requires an address!");
        return;
    }

    if (depth == 0) {
        IlangVM_panic(s, "ARRFREE depth must be at least 1!");
        return;
    }

    (void)arr_free_deep(s, "ARRFREE", s->r[reg].data.u32, depth);
}

/**
 * @brief ARRCLONE: `TO` = a deep copy of the array block in SRC.
 *
 * Encoding: `[ARRCLONE][TO_REG][SRC_REG][DEPTH:uint8]`. A new block is made for the array itself and, when DEPTH > 1, a
 * new copy of every row, so TO shares no memory with SRC. SRC is left alone.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_arrclone(iState* s) {
    /* [ARRCLONE][TO][SRC][DEPTH:uint8] */
    uint8_t to, src, depth;
    uint32_t copy;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &src)) return;
    depth = fetch_byte(s);
    if (s->halt) return;

    if (!is_address(s->r[src])) {
        IlangVM_panic(s, "ARRCLONE requires an address!");
        return;
    }

    if (depth == 0) {
        IlangVM_panic(s, "ARRCLONE depth must be at least 1!");
        return;
    }

    if (!arr_clone_deep(s, "ARRCLONE", s->r[src].data.u32, depth, &copy)) return;

    s->r[to] = (Value){ .tag = T_ADDRESS, .data.u32 = copy };
}

/**
 * @brief ARRREP: `TO` = a new array of COUNT rows (an array of arrays, DEPTH + 1 levels), each row an independent
 *        deep copy of the DEPTH-level array in SRC.
 *
 * Encoding: `[ARRREP][TO_REG][SRC_REG][COUNT_REG][DEPTH:uint8]`. SRC is left alone (the caller still owns and frees it).
 * COUNT is an int / uint / byte register and must not be negative.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_arrrep(iState* s) {
    /* [ARRREP][TO][SRC][COUNT_REG][DEPTH:uint8] */
    uint8_t to, src, count_reg, depth;
    uint32_t count, address, i, row;
    uint8_t* block;

    if (!fetch_reg(s, &to) || !fetch_reg(s, &src) || !fetch_reg(s, &count_reg)) return;
    depth = fetch_byte(s);
    if (s->halt) return;

    if (!is_address(s->r[src])) {
        IlangVM_panic(s, "ARRREP requires an address!");
        return;
    }

    if (depth == 0) {
        IlangVM_panic(s, "ARRREP depth must be at least 1!");
        return;
    }

    if (!array_count(s, "ARRREP", s->r[count_reg], &count)) return;
    if (!arr_alloc(s, "ARRREP", count, 4, &address, &block)) return;

    for (i = 0; i < count; i++) {
        if (!arr_clone_deep(s, "ARRREP", s->r[src].data.u32, depth, &row)) return;
        memcpy(block + 4u + 4u * i, &row, sizeof row);
    }

    s->r[to] = (Value){ .tag = T_ADDRESS, .data.u32 = address };
}

/* ------------------------------------------------------------------ */
/* Casting                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief CAST: converts the value in a register in place (see IlangVM_cast.c).
 *
 * Encoding: `[CAST][REG][TO_TYPE]`. If the conversion is invalid the VM panics and the register is left untouched.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
ILANG_NOINLINE void op_cast(iState* s) {
    uint8_t reg, to_type;
    Value result;

    if (!fetch_reg(s, &reg)) return;
    to_type = fetch_byte(s);
    if (s->halt) return;

    result = l_cast(s, s->r[reg], to_type);

    /* l_cast reports failure as T_NONE (and has already panicked):
       leave the register untouched instead of overwriting it. */
    if (result.tag == T_NONE) return;

    s->r[reg] = result;
}

/**
 * @brief REINTERPRET: changes a register's tag and leaves its bits alone (T_INT .. T_BYTE only).
 *
 * Encoding: `[REINTERPRET][REG][TO_TYPE]`. Panics for a tag outside T_INT .. T_BYTE.
 *
 * @param[in,out] s The VM; `ip` points at the operands.
 */
static void op_reinterpret(iState* s) {
    uint8_t reg, to_type;

    if (!fetch_reg(s, &reg)) return;
    to_type = fetch_byte(s);
    if (s->halt) return;

    if (to_type < T_INT || to_type > T_BYTE) {
        IlangVM_panic(s, "REINTERPRET to an unknown type!");
        return;
    }

    s->r[reg].tag = (Types)to_type;
}

/* ------------------------------------------------------------------ */
/* Fast paths                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief The instruction pointer of the running loop (the state of the fast paths).
 *
 * @section fast_contract Fast paths
 * The `op_xxx` functions above decode byte by byte with a bounds check for every field, which is what makes them safe
 * and what makes them slow. The `fast_xxx` functions below handle the COMMON, VALID case of the hot instructions: they
 * check once that the whole instruction fits in the code, decode it straight from the byte pointer, and execute it.
 *
 * Contract: a fast function returns true if it executed the instruction completely (ip advanced), and false if it did
 * NOTHING (no register, flag, stack, memory or ip change). The dispatcher then runs the original `op_xxx`, which
 * re-decodes the instruction and reports every problem (truncated code, bad register, bad type, division by zero, ...)
 * with exactly the same message as before. So the fast paths contain no error handling at all and cannot change
 * behaviour: anything unusual simply declines.
 *
 * On entry `s->ip` points at the first operand byte (the opcode has already been consumed).
 *
 * It is a local of the dispatch loop instead of a field of iState: a store through the VM-memory pointers (STORE,
 * MEMSET, ...) may alias any iState field as far as the compiler can tell, so with `state->ip` every instruction would
 * store ip and reload it at the top of the loop (a store-forwarding stall on the critical path). A local whose address
 * does not escape (every `fast_xxx` is always_inline) stays in a host register. Anything that is not a fast path still
 * works on `state->ip`: the loops copy it out before and back in after (see SLOW / GSLOW).
 *
 * Keep it that way: nothing called from a fast path may keep `f->ip` alive across a real function call (see
 * fast_mem()). If a profile makes such a call look hot, gcc spills ip to a stack slot that every instruction then
 * writes, and the whole VM runs ~20% slower.
 */
typedef struct FastCtx {
    uint32_t ip;  /**< Offset of the next byte to decode. */
} FastCtx;

/** @brief Most operand bytes any fast-pathed instruction can have: `[mode][type][to]` + two 4-byte immediates. */
#define FAST_BINARY_MAX 11u

/** @brief Little-endian uint32 at `p`, the byte order of the bytecode (same as fetch_uint()). */
ILANG_INLINE uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief Immediate of `type` at `*pp` (the caller guarantees enough bytes).
 * @param[in,out] pp   Position in the code; advanced past the immediate.
 * @param[in]     type A T_* tag.
 * @param[out]    out  Receives the tagged value.
 * @return false for a type fetch_imm() rejects.
 */
ILANG_INLINE bool fast_imm(const uint8_t** pp, uint8_t type, Value* out) {
    const uint8_t* p = *pp;
    Value v = { .tag = (Types)type };

    switch (type) {
    case T_INT:
    case T_UINT:
    case T_ADDRESS:
    case T_FLOAT:   /* same bit pattern, whichever member reads it */
        v.data.u32 = rd_u32(p);
        *pp = p + 4;
        break;
    case T_BOOL:
        v.data.u8 = (p[0] != 0);
        *pp = p + 1;
        break;
    case T_BYTE:
        v.data.u8 = p[0];
        *pp = p + 1;
        break;
    default:
        return false;
    }

    *out = v;
    return true;
}

/**
 * @brief A register operand (index checked) or an immediate.
 * @param[in]     s      The VM.
 * @param[in,out] pp     Position in the code; advanced past the operand.
 * @param[in]     is_reg true for a register index, false for an immediate.
 * @param[in]     type   Operand type.
 * @param[out]    out    Receives the operand value.
 * @return false if the operand cannot be decoded (the caller declines the instruction).
 */
ILANG_INLINE bool fast_operand(iState* s, const uint8_t** pp, bool is_reg, uint8_t type, Value* out) {
    const uint8_t* p = *pp;

    if (is_reg) {
        if (p[0] >= REG_COUNT) return false;
        *out = s->r[p[0]];
        *pp = p + 1;
        return true;
    }

    return fast_imm(pp, type, out);
}

/**
 * @brief Decodes `[mode][type][to][a][b]` (the layout shared by ADD..MOD, AND/OR/XOR and, without `to`, CMP).
 *
 * `mode`: bit 1 set = a is an immediate, bit 0 set = b is an immediate (REG_REG=0, REG_IMM=1, IMM_REG=2, IMM_IMM=3).
 * fast_binary() advances `ip` on success. A caller whose computation can still fail (fast_arith(), fast_shift(),
 * fast_cmp()) therefore saves `ip` first and restores it before declining.
 *
 * @param[in]     s      The VM.
 * @param[in,out] f      The fast context; `ip` is advanced past the operands on success.
 * @param[in]     has_to true if a destination register is encoded.
 * @param[out]    type   Receives the operand type.
 * @param[out]    to     Receives the destination register (if `has_to`).
 * @param[out]    a      Receives the first operand.
 * @param[out]    b      Receives the second operand.
 * @return false if the instruction cannot be decoded (the caller declines it).
 */
ILANG_INLINE bool fast_binary(iState* s, FastCtx* f, bool has_to, uint8_t* type, uint8_t* to, Value* a, Value* b) {
    const uint8_t* p;
    uint8_t mode;

    if (s->code_size - f->ip < FAST_BINARY_MAX) return false;

    p = s->code + f->ip;
    mode = p[0];
    *type = p[1];
    p += 2;

    if (mode > IMM_IMM) return false;

    if (has_to) {
        *to = p[0];
        if (*to >= REG_COUNT) return false;
        p++;
    }

    if (!fast_operand(s, &p, (mode & 2) == 0, *type, a)) return false;
    if (!fast_operand(s, &p, (mode & 1) == 0, *type, b)) return false;

    f->ip = (uint32_t)(p - s->code);
    return true;
}

/** @brief Fast path of ADD..MOD: executes the instruction completely, or declines (returns false) without any change. */
ILANG_INLINE bool fast_arith(iState* s, FastCtx* f, ArithOp op) {
#ifdef ILANG_VM_STRICT_TYPES
    (void)s; (void)f; (void)op;
    return false; /* strict mode checks register tags: leave it to the slow path */
#else
    uint32_t start = f->ip;
    uint8_t type, to = 0;
    Value a, b, r;

    if (!fast_binary(s, f, true, &type, &to, &a, &b)) return false;

    if (arith_compute(op, type, a, b, &r) != AR_OK) {
        f->ip = start; /* decline; the slow path reports the error */
        return false;
    }

    s->r[to] = r;
    return true;
#endif
}

/** @brief Fast path of AND / OR / XOR (see fast_arith()). */
ILANG_INLINE bool fast_bitwise(iState* s, FastCtx* f, BitOp op) {
#ifdef ILANG_VM_STRICT_TYPES
    (void)s; (void)f; (void)op;
    return false;
#else
    uint8_t type, to = 0;
    Value a, b;

    /* The type is checked before decoding (it is at offset 1), and it is the only way this can fail. */
    if (s->code_size - f->ip < 3u || !bit_type_ok(s->code[f->ip + 1])) return false;
    if (!fast_binary(s, f, true, &type, &to, &a, &b)) return false;

    s->r[to] = bit_compute(op, type, a, b);
    return true;
#endif
}

/** @brief Fast path of SHL / SHR (see fast_arith()). */
ILANG_INLINE bool fast_shift(iState* s, FastCtx* f, ShiftOp op) {
#ifdef ILANG_VM_STRICT_TYPES
    (void)s; (void)f; (void)op;
    return false;
#else
    uint32_t start = f->ip;
    uint8_t type, to = 0;
    Value a, b, r;

    if (s->code_size - f->ip < 3u || !bit_type_ok(s->code[f->ip + 1])) return false;
    if (!fast_binary(s, f, true, &type, &to, &a, &b)) return false;

    if (!shift_compute(op, type, a, b, &r)) {
        f->ip = start; /* a negative count: the slow path reports it */
        return false;
    }

    s->r[to] = r;
    return true;
#endif
}

/** @brief Fast path of CMP (see fast_arith()). */
ILANG_INLINE bool fast_cmp(iState* s, FastCtx* f) {
#ifdef ILANG_VM_STRICT_TYPES
    (void)s; (void)f;
    return false;
#else
    uint32_t start = f->ip;
    uint8_t type, to;
    Value a, b;
    CmpFlags fl;

    if (!fast_binary(s, f, false, &type, &to, &a, &b)) return false;

    if (cmp_compute(type, a, b, &fl) != CM_OK) {
        f->ip = start; /* the slow path clears the flags and reports the error */
        return false;
    }

    s->zF = fl.z;
    s->sF = fl.s;
    s->cF = fl.c;
    s->oF = fl.o;
    return true;
#endif
}

/** @brief Fast path of MOV: `[MOV][mode][imm_type][dst][src | imm]`. */
ILANG_INLINE bool fast_mov(iState* s, FastCtx* f) {
    const uint8_t* p;
    uint8_t dst;

    if (s->code_size - f->ip < 7u) return false;

    p = s->code + f->ip;
    dst = p[2];
    if (dst >= REG_COUNT) return false;

    if (p[0] == REG_REG) {
        if (p[3] >= REG_COUNT) return false;
        s->r[dst] = s->r[p[3]];
        f->ip += 4;
        return true;
    }

    if (p[0] == REG_IMM) {
        const uint8_t* q = p + 3;
        Value v;

        if (!fast_imm(&q, p[1], &v)) return false;
        s->r[dst] = v;
        f->ip = (uint32_t)(q - s->code);
        return true;
    }

    return false;
}

/** @brief Fast path of JMP / Jcc: `[Jxx][relative][register?][target]`, the immediate-target form only. */
ILANG_INLINE bool fast_jump(iState* s, FastCtx* f, bool condition) {
    const uint8_t* p;
    uint32_t next;

    if (s->code_size - f->ip < 6u) return false;

    p = s->code + f->ip;
    if (p[1] != 0) return false;

    next = f->ip + 6u;
    f->ip = condition ? (p[0] != 0 ? next + rd_u32(p + 2) : rd_u32(p + 2)) : next;
    return true;
}

/** @brief Fast path of CALL: `[CALL][relative][register?][target]`, the immediate-target form only (see op_call()). */
ILANG_INLINE bool fast_call(iState* s, FastCtx* f) {
    const uint8_t* p;
    uint32_t next;

    if (s->code_size - f->ip < 6u) return false;

    p = s->code + f->ip;
    if (p[1] != 0) return false;
    if ((size_t)(s->callTop - s->callStack) + 2 > MAX_CALL_DEPTH) return false; /* slow path panics */

    next = f->ip + 6u;
    *s->callTop++ = next;
    *s->callTop++ = (uint32_t)(s->bp - s->stack);
    s->bp = s->top;

    f->ip = (p[0] != 0) ? next + rd_u32(p + 2) : rd_u32(p + 2);
    return true;
}

/** @brief Fast path of RET. */
ILANG_INLINE bool fast_ret(iState* s, FastCtx* f) {
    if ((size_t)(s->callTop - s->callStack) < 2) return false;

    s->bp = s->stack + *--s->callTop;
    f->ip = *--s->callTop;
    return true;
}

/** @brief Fast path of PUSH: `[PUSH][from_reg]`. */
ILANG_INLINE bool fast_push(iState* s, FastCtx* f) {
    uint8_t from;

    if (s->code_size - f->ip < 1u) return false;
    from = s->code[f->ip];
    if (from >= REG_COUNT || s->top >= s->stack + MAX_STACK_SIZE) return false;

    *s->top++ = s->r[from];
    f->ip++;
    return true;
}

/** @brief Fast path of POP: `[POP][to_reg]`. */
ILANG_INLINE bool fast_pop(iState* s, FastCtx* f) {
    uint8_t to;

    if (s->code_size - f->ip < 1u) return false;
    to = s->code[f->ip];
    if (to >= REG_COUNT || s->top <= s->stack) return false;

    s->r[to] = *--s->top;
    f->ip++;
    return true;
}

/** @brief Fast path of ENTER: `[ENTER][locals:u32]`. */
ILANG_INLINE bool fast_enter(iState* s, FastCtx* f) {
    size_t used = (size_t)(s->top - s->stack);
    uint32_t locals, i;

    if (s->code_size - f->ip < 4u) return false;
    locals = rd_u32(s->code + f->ip);
    if (used > MAX_STACK_SIZE || (size_t)locals > MAX_STACK_SIZE - used) return false;

    for (i = 0; i < locals; i++) {
        *s->top++ = (Value){ .tag = T_INT, .data.i32 = 0 };
    }

    f->ip += 4;
    return true;
}

/** @brief Fast path of LEAVE: `[LEAVE][locals:u32]`. */
ILANG_INLINE bool fast_leave(iState* s, FastCtx* f) {
    uint32_t locals;

    if (s->code_size - f->ip < 4u) return false;
    locals = rd_u32(s->code + f->ip);
    if ((size_t)locals > (size_t)(s->top - s->stack)) return false;

    s->top -= locals;
    f->ip += 4;
    return true;
}

/** @brief Fast path of LOADLOCAL: `[LOADLOCAL][offset_reg][to]`. */
ILANG_INLINE bool fast_loadlocal(iState* s, FastCtx* f) {
    const uint8_t* p;
    uint32_t index;

    if (s->code_size - f->ip < 2u) return false;
    p = s->code + f->ip;
    if (p[0] >= REG_COUNT || p[1] >= REG_COUNT) return false;

    index = s->r[p[0]].data.u32;
    if ((size_t)index >= (size_t)(s->top - s->stack)) return false;

    s->r[p[1]] = s->stack[index];
    f->ip += 2;
    return true;
}

/** @brief Fast path of STORELOCAL: `[STORELOCAL][offset_reg][imm_type][is_reg][from | imm]`. */
ILANG_INLINE bool fast_storelocal(iState* s, FastCtx* f) {
    const uint8_t* p;
    const uint8_t* q;
    uint32_t index;
    Value v;

    if (s->code_size - f->ip < 7u) return false;
    p = s->code + f->ip;
    if (p[0] >= REG_COUNT) return false;

    index = s->r[p[0]].data.u32;
    if ((size_t)index >= (size_t)(s->top - s->stack)) return false;

    q = p + 3;
    if (p[2] != 0) {
        if (p[3] >= REG_COUNT) return false;
        v = s->r[p[3]];
        q = p + 4;
    }
    else if (!fast_imm(&q, p[1], &v)) {
        return false;
    }

    s->stack[index] = v;
    f->ip = (uint32_t)(q - s->code);
    return true;
}

/** @brief Fast path of LEA: `[LEA][to][mode][offset:u32]`. */
ILANG_INLINE bool fast_lea(iState* s, FastCtx* f) {
    const uint8_t* p;
    ptrdiff_t base, index, offset;

    if (s->code_size - f->ip < 6u) return false;
    p = s->code + f->ip;
    if (p[0] >= REG_COUNT) return false;

    offset = (ptrdiff_t)rd_u32(p + 2);
    base = s->bp - s->stack;
    index = (p[1] == 0) ? base + offset : base - (offset + 1);

    s->r[p[0]] = (Value){ .tag = T_ADDRESS, .data.u32 = (uint32_t)index };
    f->ip += 6;
    return true;
}

/**
 * @brief VM-memory window lookup for the fast paths.
 *
 * The one-entry block cache is checked inline; a miss is a real call (binary search), and the instruction pointer is
 * parked in the iState around it so that it does not have to be kept in a host register across the call (which, when
 * the register allocator gives up on it, turns into a stack slot that EVERY instruction then writes).
 *
 * @param[in,out] s       The VM.
 * @param[in,out] f       The fast context.
 * @param[in]     address First VM address.
 * @param[in]     size    Length in bytes.
 * @return Host pointer to the range, or NULL if it is not inside one live block.
 */
ILANG_INLINE uint8_t* fast_mem(iState* s, FastCtx* f, uint32_t address, uint32_t size) {
    iMemory* m = s->memory;
    uint8_t* r;

    if (m->last_block < m->block_count) {
        const iMemoryBlock* block = &m->blocks[m->last_block];
        uint32_t offset = address - block->address;

        if (offset < block->size && size <= block->size - offset) {
            return block->data + offset;
        }
    }

    s->ip = f->ip;
    r = memory_ptr(m, address, size);
    f->ip = s->ip;
    return r;
}

/** @brief Fast path of LOAD: `[LOAD][type][address_reg][to]`. */
ILANG_INLINE bool fast_load(iState* s, FastCtx* f) {
    const uint8_t* p;
    const uint8_t* src;
    Value addr, r = { 0 };

    if (s->code_size - f->ip < 3u) return false;
    p = s->code + f->ip;
    if (p[1] >= REG_COUNT || p[2] >= REG_COUNT) return false;

    addr = s->r[p[1]];
    if (!is_address(addr)) return false;

    r.tag = (Types)p[0];

    switch (p[0]) {
    case T_INT:
    case T_UINT:
    case T_FLOAT:
    case T_ADDRESS:
        src = fast_mem(s, f, addr.data.u32, 4);
        if (src == NULL) return false;
        memcpy(&r.data, src, 4);
        break;
    case T_BYTE:
    case T_BOOL:
        src = fast_mem(s, f, addr.data.u32, 1);
        if (src == NULL) return false;
        memcpy(&r.data, src, 1);
        if (p[0] == T_BOOL) r.data.u8 = (r.data.u8 != 0);
        break;
    default:
        return false;
    }

    s->r[p[2]] = r;
    f->ip += 3;
    return true;
}

/** @brief Fast path of STORE: `[STORE][type][address_reg][from_reg]`. */
ILANG_INLINE bool fast_store(iState* s, FastCtx* f) {
    const uint8_t* p;
    uint8_t* dst;
    Value addr, value;

    if (s->code_size - f->ip < 3u) return false;
    p = s->code + f->ip;
    if (p[1] >= REG_COUNT || p[2] >= REG_COUNT) return false;

    addr = s->r[p[1]];
    value = s->r[p[2]];
    if (!is_address(addr) || (uint8_t)value.tag != p[0]) return false;

    switch (p[0]) {
    case T_INT:
    case T_UINT:
    case T_FLOAT:
    case T_ADDRESS:
        dst = fast_mem(s, f, addr.data.u32, 4);
        if (dst == NULL) return false;
        memcpy(dst, &value.data, 4);
        break;
    case T_BYTE:
    case T_BOOL:
        dst = fast_mem(s, f, addr.data.u32, 1);
        if (dst == NULL) return false;
        if (p[0] == T_BOOL) value.data.u8 = (value.data.u8 != 0);
        memcpy(dst, &value.data, 1);
        break;
    default:
        return false;
    }

    f->ip += 3;
    return true;
}

/** @brief Fast path of REINTERPRET: `[REINTERPRET][reg][type]`. */
ILANG_INLINE bool fast_reinterpret(iState* s, FastCtx* f) {
    const uint8_t* p;

    if (s->code_size - f->ip < 2u) return false;
    p = s->code + f->ip;
    if (p[0] >= REG_COUNT || p[1] < T_INT || p[1] > T_BYTE) return false;

    s->r[p[0]].tag = (Types)p[1];
    f->ip += 2;
    return true;
}

/* ------------------------------------------------------------------ */
/* Opcode names (tracing and diagnostics)                              */
/* ------------------------------------------------------------------ */

/**
 * @def ILANG_OPNAME(x)
 * @brief One entry of the opcode name table: `[x] = "x"`.
 */
#define ILANG_OPNAME(x) [x] = #x
/** @brief Opcode mnemonics, indexed by opcode number (see IlangVM_opcode_name()). */
static const char* const opcode_names[] = {
    ILANG_OPNAME(MOV),
    ILANG_OPNAME(ADD), ILANG_OPNAME(SUB), ILANG_OPNAME(MUL), ILANG_OPNAME(DIV), ILANG_OPNAME(MOD),
    ILANG_OPNAME(RESERVEMEM), ILANG_OPNAME(FREEMEM),
    ILANG_OPNAME(LOAD), ILANG_OPNAME(STORE),
    ILANG_OPNAME(LOADLOCAL), ILANG_OPNAME(STORELOCAL),
    ILANG_OPNAME(LEA),
    ILANG_OPNAME(ENTER), ILANG_OPNAME(LEAVE),
    ILANG_OPNAME(CMP),
    ILANG_OPNAME(JMP), ILANG_OPNAME(JE), ILANG_OPNAME(JNE), ILANG_OPNAME(JGT), ILANG_OPNAME(JLT), ILANG_OPNAME(JGE),
    ILANG_OPNAME(JLE),
    ILANG_OPNAME(AND), ILANG_OPNAME(OR), ILANG_OPNAME(XOR),
    ILANG_OPNAME(CALL), ILANG_OPNAME(RET),
    ILANG_OPNAME(PUSH), ILANG_OPNAME(POP),
    ILANG_OPNAME(PRINT),
    ILANG_OPNAME(CAST), ILANG_OPNAME(REINTERPRET),
    ILANG_OPNAME(HALT),
    ILANG_OPNAME(STRLIT), ILANG_OPNAME(STRPRINT), ILANG_OPNAME(STRCAT), ILANG_OPNAME(STRCMP), ILANG_OPNAME(STRSLICE),
    ILANG_OPNAME(STRFIND), ILANG_OPNAME(STRCHR), ILANG_OPNAME(STRFROM), ILANG_OPNAME(STRPARSE),
    ILANG_OPNAME(READLINE), ILANG_OPNAME(ATEOF),
    ILANG_OPNAME(MEMSET),
    ILANG_OPNAME(NEWARRAY), ILANG_OPNAME(ARRFREE), ILANG_OPNAME(ARRCLONE), ILANG_OPNAME(ARRREP),
    ILANG_OPNAME(ABORT),
    ILANG_OPNAME(FOPEN), ILANG_OPNAME(FCLOSE), ILANG_OPNAME(FREAD), ILANG_OPNAME(FREADLINE), ILANG_OPNAME(FWRITE),
    ILANG_OPNAME(FEOF), ILANG_OPNAME(FERROR), ILANG_OPNAME(FREADFILE),
    ILANG_OPNAME(SHL), ILANG_OPNAME(SHR),
    ILANG_OPNAME(CALLC)
};
#undef ILANG_OPNAME

/** @brief Compile-time check: a compile error here means an opcode was added to the enum (IlangVM.h) without a name above. */
typedef char opcode_names_cover_the_enum[(sizeof opcode_names / sizeof opcode_names[0] == (size_t)OP__END) ? 1 : -1];

const char* IlangVM_opcode_name(uint8_t op) {
    if (op < OP__END && opcode_names[op] != NULL) {
        return opcode_names[op];
    }
    return "???";
}

/* ------------------------------------------------------------------ */
/* Dispatch and program lifecycle                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Slow path: runs every instruction that has no fast path, and every fast path that declined.
 *
 * It lives out of line so the dispatch loop stays small (a big IlangVM_execute() keeps less of its state in registers).
 * On entry `ip` is just behind the opcode byte, because a declined fast path changes nothing. An unknown opcode halts
 * the VM with a panic.
 *
 * @param[in,out] state The VM.
 * @param[in]     op    The opcode that was just fetched.
 */
ILANG_NOINLINE void slow_op(iState* state, OpCode op) {
    switch (op) {
    case MOV:          op_mov(state); break;
    case ADD:          op_arith(state, ARITH_ADD); break;
    case SUB:          op_arith(state, ARITH_SUB); break;
    case MUL:          op_arith(state, ARITH_MUL); break;
    case DIV:          op_arith(state, ARITH_DIV); break;
    case MOD:          op_arith(state, ARITH_MOD); break;
    case RESERVEMEM:   op_reservemem(state); break;
    case FREEMEM:      op_freemem(state); break;
    case LOAD:         op_load(state); break;
    case STORE:        op_store(state); break;
    case LOADLOCAL:    op_loadlocal(state); break;
    case STORELOCAL:   op_storelocal(state); break;
    case LEA:          op_lea(state); break;
    case ENTER:        op_enter(state); break;
    case LEAVE:        op_leave(state); break;
    case CMP:          op_cmp(state); break;
    case JMP:          op_jump(state, true); break;
    case JE:           op_jump(state, state->zF); break;
    case JNE:          op_jump(state, !state->zF); break;
    case JGT:          op_jump(state, !state->zF && state->sF == state->oF); break;
    case JLT:          op_jump(state, state->sF != state->oF); break;
    case JGE:          op_jump(state, state->sF == state->oF); break;
    case JLE:          op_jump(state, state->zF || state->sF != state->oF); break;
    case AND:          op_bitwise(state, BIT_AND); break;
    case OR:           op_bitwise(state, BIT_OR); break;
    case XOR:          op_bitwise(state, BIT_XOR); break;
    case SHL:          op_shift(state, SHIFT_LEFT); break;
    case SHR:          op_shift(state, SHIFT_RIGHT); break;
    case CALLC:        op_callc(state); break;
    case CALL:         op_call(state); break;
    case RET:          op_ret(state); break;
    case PUSH:         op_push(state); break;
    case POP:          op_pop(state); break;
    case PRINT:        op_print(state); break;
    case CAST:         op_cast(state); break;
    case REINTERPRET:  op_reinterpret(state); break;
    case STRLIT:       op_strlit(state); break;
    case STRPRINT:     op_strprint(state); break;
    case STRCAT:       op_strcat(state); break;
    case STRCMP:       op_strcmp(state); break;
    case STRSLICE:     op_strslice(state); break;
    case STRFIND:      op_strfind(state); break;
    case STRCHR:       op_strchr(state); break;
    case STRFROM:      op_strfrom(state); break;
    case STRPARSE:     op_strparse(state); break;
    case READLINE:     op_readline(state); break;
    case ATEOF:        op_ateof(state); break;
    case MEMSET:       op_memset(state); break;
    case NEWARRAY:     op_newarray(state); break;
    case ARRFREE:      op_arrfree(state); break;
    case ARRCLONE:     op_arrclone(state); break;
    case ARRREP:       op_arrrep(state); break;
    case ABORT:        op_abort(state); break;
    case FOPEN:        op_fopen(state); break;
    case FCLOSE:       op_fclose(state); break;
    case FREAD:        op_fread(state); break;
    case FREADLINE:    op_freadline(state); break;
    case FWRITE:       op_fwrite(state); break;
    case FEOF:         op_feof(state); break;
    case FERROR:       op_ferror(state); break;
    case FREADFILE:    op_freadfile(state); break;

    default:
        state->halt = true;
        IlangVM_panicf(state, "Unknown opcode 0x%02X!", (unsigned)op);
        break;
    }
}

/**
 * @brief Flushes and closes every file the program left open (handles 0..2 are the process's own streams: flushed only).
 * @param[in,out] state The VM.
 */
static void close_open_files(iState* state) {
    int i;

    for (i = 3; i < ILANG_VM_MAX_FILES; i++) {
        if (state->files[i] != NULL) {
            if (fclose(state->files[i]) != 0) {
                fflush(stdout);
                fprintf(stderr, "IlangVM warning: file handle %d could not be flushed and closed: %s\n", i, strerror(errno));
            }

            state->files[i] = NULL;
            state->file_flags[i] = 0;
        }
    }

    fflush(stdout);
}

/* Defined below; IlangVM_load() uses it to discard the decode tables of a previously loaded program. */
static void free_quick_tables(iState* state);

/**
 * @details Prepares `state` to run a bytecode image: validates the header, binds the import table to the host functions
 *          that were defined beforehand, reads the extension sections, resets registers, stacks and flags, sets up the
 *          file table (handles 0..2 are the standard streams) and creates a fresh VM memory. The bytecode is not copied:
 *          it must stay valid until the program has finished. A program loaded earlier into the same state is
 *          forgotten first.
 */
bool IlangVM_load(iState* state, const uint8_t* bytecode, size_t bytecode_size) {
    iHeader header;

    uint32_t code_start;

    if (!state || !bytecode) {
        return false;
    }

    state->load_error[0] = '\0';

    if (bytecode_size < sizeof(iHeader)) {
        snprintf(state->load_error, sizeof state->load_error, "the file is too small to be Ilang bytecode");
        return false;
    }

    /* Copy the header out instead of casting the buffer: the buffer may not
       be suitably aligned, and casting would violate strict aliasing. */
    memcpy(&header, bytecode, sizeof header);

    /* Validate the header */
    if (memcmp(header.Magic, "iVM\0", 4) != 0) {
        snprintf(state->load_error, sizeof state->load_error, "not Ilang bytecode (the file does not start with \"iVM\")");
        return false;
    }

    if (header.Version < MIN_SUPPORTED_VERSION || header.Version > SUPPORTED_VERSION) {
        snprintf(state->load_error, sizeof state->load_error,
            "the bytecode is version %" PRIu32 ", but this VM runs version %d: recompile the program with a matching Ilangc",
            header.Version, SUPPORTED_VERSION);
        return false;
    }

    /* code_size and every code offset are 32-bit values, so a larger image cannot be addressed. */
    if (bytecode_size > UINT32_MAX) {
        snprintf(state->load_error, sizeof state->load_error, "the file is too large");
        return false;
    }

    /* a program loaded before into this state: forget what belonged to it */
    free_quick_tables(state);
    host_end_program(state);
    state->panic_message[0] = '\0';

    /* the import table: every function the program calls with CALLC must be defined by now, with the same signature */
    if (!host_bind_imports(state, bytecode, (uint32_t)bytecode_size, &code_start)) {
        return false;
    }

    if (header.Entrypoint < code_start || header.Entrypoint >= bytecode_size) {
        snprintf(state->load_error, sizeof state->load_error, "the entry point lies outside the code");
        return false;
    }

    /* extension sections between the import table and the code (the exports) */
    if (!host_read_sections(state, bytecode, code_start, header.Entrypoint, (uint32_t)bytecode_size)) {
        return false;
    }

    /* Initialize the VM */
    memset(state->r, 0, sizeof state->r);

    state->halt = false;
    state->zF = false;
    state->sF = false;
    state->cF = false;
    state->oF = false;

    state->code = bytecode;
    state->code_size = (uint32_t)bytecode_size;

    state->top = state->stack;
    state->callTop = state->callStack;
    state->bp = state->stack;
    state->ip = header.Entrypoint;

    /* Files: the three standard streams are always open, everything else is free */
    memset(state->files, 0, sizeof state->files);
    memset(state->file_flags, 0, sizeof state->file_flags);
    state->io_error[0] = '\0';

    state->files[0] = stdin;
    state->file_flags[0] = FILE_CAN_READ;
    state->files[1] = stdout;
    state->file_flags[1] = FILE_CAN_WRITE;
    state->files[2] = stderr;
    state->file_flags[2] = FILE_CAN_WRITE;

    /* Create the memory object */
    if (state->memory) {
        memory_destroy(state->memory);
    }
    state->memory = memory_create();

    if (!state->memory) {
        snprintf(state->load_error, sizeof state->load_error, "out of memory");
        return false;
    }

    return true;
}

/**
 * @brief What both loops do after the last instruction: classify how the run ended.
 *
 * A HALT or running off the end of the code is a clean end; a panic, fetch error or limit is not; so is a jump past the
 * end of the code (that one is reported here).
 *
 * @param[in,out] state         The VM.
 * @param[in]     halt_executed true if the loop ended on a HALT instruction.
 * @return true if the program finished cleanly.
 */
static bool finish_run(iState* state, bool halt_executed) {
    bool finished_cleanly;

    if (halt_executed) {
        finished_cleanly = true;
    }
    else if (state->halt) {
        finished_cleanly = false;   /* stopped by a panic / fetch error / limit */
    }
    else if (state->ip == state->code_size) {
        finished_cleanly = true;    /* ran off the end of the code: normal end */
    }
    else {
        IlangVM_panicf(state, "Execution left the bytecode (instruction pointer 0x%08" PRIX32 ", the code ends at 0x%08" PRIX32 ")!",
            state->ip, state->code_size);
        finished_cleanly = false;   /* a jump went past the end */
    }

    return finished_cleanly;
}

/**
 * @brief Frees the quickening interpreter's decode tables (one per host-call nesting level).
 * @param[in,out] state The VM.
 */
static void free_quick_tables(iState* state) {
    size_t i;

    for (i = 0; i < sizeof state->quick_dec / sizeof state->quick_dec[0]; i++) {
        free(state->quick_dec[i]);
        state->quick_dec[i] = NULL;
    }
}

/** @details Everything that ends with the program: its files, its memory, the interpreter's decode tables, the exports. */
void vm_teardown(iState* state) {
    close_open_files(state);

    memory_destroy(state->memory);
    state->memory = NULL;

    free_quick_tables(state);

    host_end_program(state);
}

/**
 * @def ILANG_TRACE()
 * @brief Prints the instruction about to run when `ILANG_VM_TRACE` is defined; nothing otherwise.
 */
#ifdef ILANG_VM_TRACE
#define ILANG_TRACE() fprintf(stderr, "[%08" PRIX32 "] %s\n", state->ip, IlangVM_opcode_name(state->code[state->ip]))
#else
#define ILANG_TRACE() ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Loop 1: portable (switch)                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Loop 1: the portable `switch` interpreter.
 *
 * It keeps the instruction pointer in the local `f.ip`, tries the fast path of each hot instruction and falls back to
 * the slow path. It is still compiled everywhere (as an unused function when another loop is selected), so that it
 * cannot rot.
 *
 * @param[in,out] state The VM.
 * @return true if the program finished normally, false if it was stopped by an error.
 */
#if defined(ILANG_COMPUTED_GOTO) || defined(ILANG_VM_QUICK)
__attribute__((unused))   /* the portable loop is still compiled everywhere, so it cannot rot */
#endif
ILANG_DISPATCH static bool execute_switch(iState* state) {
    bool halt_executed = false;
    FastCtx f;

#ifdef ILANG_VM_MAX_INSTRUCTIONS
    uint64_t budget = ILANG_VM_MAX_INSTRUCTIONS;
#endif

        f.ip = state->ip;

/* SLOW(call): runs everything that is not a fast path on `state->ip`: hands it the instruction pointer, runs `call`,
   takes it back. A fast path never sets `state->halt`, so the halt flag is only looked at after these. */
#define SLOW(call) do { state->ip = f.ip; call; f.ip = state->ip; if (state->halt) goto done; } while (0)

    if (state->halt) goto done;

    while (f.ip < state->code_size) {
        OpCode op;

#ifdef ILANG_VM_MAX_INSTRUCTIONS
        if (budget-- == 0) {
            IlangVM_panic(state, "Instruction limit exceeded!");
            state->halt = true;
            break;
        }
#endif

#ifdef ILANG_VM_TRACE
        fprintf(stderr, "[%08" PRIX32 "] %s\n", f.ip, IlangVM_opcode_name(state->code[f.ip]));
#endif

        op = (OpCode)state->code[f.ip++];

        switch (op) {
        case MOV:          if (!fast_mov(state, &f)) SLOW(slow_op(state, op)); break;
        case ADD:          if (!fast_arith(state, &f, ARITH_ADD)) SLOW(slow_op(state, op)); break;
        case SUB:          if (!fast_arith(state, &f, ARITH_SUB)) SLOW(slow_op(state, op)); break;
        case MUL:          if (!fast_arith(state, &f, ARITH_MUL)) SLOW(slow_op(state, op)); break;
        case DIV:          if (!fast_arith(state, &f, ARITH_DIV)) SLOW(slow_op(state, op)); break;
        case MOD:          if (!fast_arith(state, &f, ARITH_MOD)) SLOW(slow_op(state, op)); break;
        case LOAD:         if (!fast_load(state, &f)) SLOW(slow_op(state, op)); break;
        case STORE:        if (!fast_store(state, &f)) SLOW(slow_op(state, op)); break;
        case LOADLOCAL:    if (!fast_loadlocal(state, &f)) SLOW(slow_op(state, op)); break;
        case STORELOCAL:   if (!fast_storelocal(state, &f)) SLOW(slow_op(state, op)); break;
        case LEA:          if (!fast_lea(state, &f)) SLOW(slow_op(state, op)); break;
        case ENTER:        if (!fast_enter(state, &f)) SLOW(slow_op(state, op)); break;
        case LEAVE:        if (!fast_leave(state, &f)) SLOW(slow_op(state, op)); break;
        case CMP:          if (!fast_cmp(state, &f)) SLOW(slow_op(state, op)); break;
        case JMP:          if (!fast_jump(state, &f, true)) SLOW(slow_op(state, op)); break;
        case JE:           if (!fast_jump(state, &f, state->zF)) SLOW(slow_op(state, op)); break;
        case JNE:          if (!fast_jump(state, &f, !state->zF)) SLOW(slow_op(state, op)); break;
        case JGT:          if (!fast_jump(state, &f, !state->zF && state->sF == state->oF)) SLOW(slow_op(state, op)); break;
        case JLT:          if (!fast_jump(state, &f, state->sF != state->oF)) SLOW(slow_op(state, op)); break;
        case JGE:          if (!fast_jump(state, &f, state->sF == state->oF)) SLOW(slow_op(state, op)); break;
        case JLE:          if (!fast_jump(state, &f, state->zF || state->sF != state->oF)) SLOW(slow_op(state, op)); break;
        case AND:          if (!fast_bitwise(state, &f, BIT_AND)) SLOW(slow_op(state, op)); break;
        case OR:           if (!fast_bitwise(state, &f, BIT_OR)) SLOW(slow_op(state, op)); break;
        case XOR:          if (!fast_bitwise(state, &f, BIT_XOR)) SLOW(slow_op(state, op)); break;
        case SHL:          if (!fast_shift(state, &f, SHIFT_LEFT)) SLOW(slow_op(state, op)); break;
        case SHR:          if (!fast_shift(state, &f, SHIFT_RIGHT)) SLOW(slow_op(state, op)); break;
        case CALL:         if (!fast_call(state, &f)) SLOW(slow_op(state, op)); break;
        case RET:          if (!fast_ret(state, &f)) SLOW(slow_op(state, op)); break;
        case PUSH:         if (!fast_push(state, &f)) SLOW(slow_op(state, op)); break;
        case POP:          if (!fast_pop(state, &f)) SLOW(slow_op(state, op)); break;
        case REINTERPRET:  if (!fast_reinterpret(state, &f)) SLOW(slow_op(state, op)); break;

        /* the common instructions without a fast path: called directly (one dispatch, not two), still out of line */
        case RESERVEMEM:   SLOW(op_reservemem(state)); break;
        case FREEMEM:      SLOW(op_freemem(state)); break;
        case STRLIT:       SLOW(op_strlit(state)); break;
        case STRPRINT:     SLOW(op_strprint(state)); break;
        case STRCAT:       SLOW(op_strcat(state)); break;
        case STRCMP:       SLOW(op_strcmp(state)); break;
        case STRSLICE:     SLOW(op_strslice(state)); break;
        case STRFIND:      SLOW(op_strfind(state)); break;
        case STRCHR:       SLOW(op_strchr(state)); break;
        case STRFROM:      SLOW(op_strfrom(state)); break;
        case PRINT:        SLOW(op_print(state)); break;
        case CAST:         SLOW(op_cast(state)); break;
        case NEWARRAY:     SLOW(op_newarray(state)); break;
        case ARRFREE:      SLOW(op_arrfree(state)); break;
        case MEMSET:       SLOW(op_memset(state)); break;

        case HALT:
            state->halt = true;
            halt_executed = true;
            goto done;

        default:
            SLOW(slow_op(state, op)); /* everything without a fast path, and the unknown opcodes */
            break;
        }
    }

done:
    state->ip = f.ip;
#undef SLOW
    return finish_run(state, halt_executed);
}

/* ------------------------------------------------------------------ */
/* Loop 2: computed gotos (gcc, clang)                                 */
/* ------------------------------------------------------------------ */

#ifdef ILANG_COMPUTED_GOTO

/**
 * @def ILANG_BUDGET()
 * @brief Instruction budget (`ILANG_VM_MAX_INSTRUCTIONS`), evaluated once per executed instruction in the goto loop.
 */
#ifdef ILANG_VM_MAX_INSTRUCTIONS
#define ILANG_BUDGET() do { if (budget-- == 0) { IlangVM_panic(state, "Instruction limit exceeded!"); state->halt = true; goto done; } } while (0)
#else
#define ILANG_BUDGET() ((void)0)
#endif

/**
 * @def GOTO_TRACE()
 * @brief The trace line (`ILANG_VM_TRACE`), evaluated once per executed instruction in the goto loop.
 */
#ifdef ILANG_VM_TRACE
#define GOTO_TRACE() fprintf(stderr, "[%08" PRIX32 "] %s\n", f.ip, IlangVM_opcode_name(state->code[f.ip]))
#else
#define GOTO_TRACE() ((void)0)
#endif

/**
 * @def NEXT()
 * @brief Fetches the next opcode and jumps to its label. Every use is its own indirect jump.
 */
#define NEXT() do { \
        if (f.ip >= state->code_size) goto done; \
        ILANG_BUDGET(); \
        GOTO_TRACE(); \
        op = state->code[f.ip++]; \
        goto *labels[op]; \
    } while (0)

/**
 * @def GSLOW(call)
 * @brief Like SLOW() for the goto loop: the instruction pointer lives in the local `f` (see FastCtx); everything that
 *        is not a fast path works on `state->ip`.
 */
#define GSLOW(call) do { state->ip = f.ip; call; f.ip = state->ip; if (state->halt) goto done; } while (0)

/**
 * @def FAST(name, call)
 * @brief An instruction with a fast path that may decline (then the slow path runs).
 */
#define FAST(name, call) L_##name: if (!(call)) GSLOW(slow_op(state, name)); NEXT()
/**
 * @def DIRECT(name, call)
 * @brief An instruction without a fast path that is called directly (one dispatch, not two).
 */
#define DIRECT(name, call) L_##name: GSLOW(call); NEXT()

/* The label table uses GNU extensions on purpose (label addresses, [a ... b] ranges, later entries overriding the
 * default), so the warnings that flag exactly that are switched off for this function. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Winitializer-overrides"
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#pragma clang diagnostic ignored "-Wgnu-designator"
#pragma clang diagnostic ignored "-Wgnu-statement-expression"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Woverride-init"
#endif

/**
 * @brief Loop 2: the computed-goto interpreter (gcc, clang; opt-in with `ILANG_VM_COMPUTED_GOTO`).
 *
 * Same behaviour as the switch loop, but every instruction ends in its own indirect jump through a table of label
 * addresses. Every byte value has an entry: the ones that are not opcodes go to the slow path, which reports them.
 *
 * @param[in,out] state The VM.
 * @return true if the program finished normally, false if it was stopped by an error.
 */
#ifdef ILANG_VM_QUICK
__attribute__((unused))   /* the quickening loop replaces it, but it is still compiled, so it cannot rot */
#endif
ILANG_GOTO_ATTR ILANG_DISPATCH static bool execute_goto(iState* state) {
    /* Every byte value has an entry: the ones that are not opcodes go to the slow path, which reports them. */
    static const void* const labels[256] = {
        [0 ... 255] = &&L_slow,
        [MOV] = &&L_MOV, [ADD] = &&L_ADD, [SUB] = &&L_SUB, [MUL] = &&L_MUL, [DIV] = &&L_DIV, [MOD] = &&L_MOD,
        [LOAD] = &&L_LOAD, [STORE] = &&L_STORE, [LOADLOCAL] = &&L_LOADLOCAL, [STORELOCAL] = &&L_STORELOCAL,
        [LEA] = &&L_LEA, [ENTER] = &&L_ENTER, [LEAVE] = &&L_LEAVE, [CMP] = &&L_CMP,
        [JMP] = &&L_JMP, [JE] = &&L_JE, [JNE] = &&L_JNE, [JGT] = &&L_JGT, [JLT] = &&L_JLT, [JGE] = &&L_JGE,
        [JLE] = &&L_JLE, [AND] = &&L_AND, [OR] = &&L_OR, [XOR] = &&L_XOR,
        [SHL] = &&L_SHL, [SHR] = &&L_SHR,
        [CALL] = &&L_CALL, [RET] = &&L_RET, [PUSH] = &&L_PUSH, [POP] = &&L_POP, [REINTERPRET] = &&L_REINTERPRET,
        [RESERVEMEM] = &&L_RESERVEMEM, [FREEMEM] = &&L_FREEMEM, [STRLIT] = &&L_STRLIT, [STRPRINT] = &&L_STRPRINT,
        [STRCAT] = &&L_STRCAT, [STRCMP] = &&L_STRCMP, [STRSLICE] = &&L_STRSLICE, [STRFIND] = &&L_STRFIND,
        [STRCHR] = &&L_STRCHR, [STRFROM] = &&L_STRFROM, [PRINT] = &&L_PRINT, [CAST] = &&L_CAST,
        [NEWARRAY] = &&L_NEWARRAY, [ARRFREE] = &&L_ARRFREE, [MEMSET] = &&L_MEMSET,
        [HALT] = &&L_HALT,
    };
    bool halt_executed = false;
    uint8_t op;
    FastCtx f;

#ifdef ILANG_VM_MAX_INSTRUCTIONS
    uint64_t budget = ILANG_VM_MAX_INSTRUCTIONS;
#endif

        f.ip = state->ip;

    if (state->halt) goto done;

    NEXT();

    FAST(MOV, fast_mov(state, &f));
    FAST(ADD, fast_arith(state, &f, ARITH_ADD));
    FAST(SUB, fast_arith(state, &f, ARITH_SUB));
    FAST(MUL, fast_arith(state, &f, ARITH_MUL));
    FAST(DIV, fast_arith(state, &f, ARITH_DIV));
    FAST(MOD, fast_arith(state, &f, ARITH_MOD));
    FAST(LOAD, fast_load(state, &f));
    FAST(STORE, fast_store(state, &f));
    FAST(LOADLOCAL, fast_loadlocal(state, &f));
    FAST(STORELOCAL, fast_storelocal(state, &f));
    FAST(LEA, fast_lea(state, &f));
    FAST(ENTER, fast_enter(state, &f));
    FAST(LEAVE, fast_leave(state, &f));
    FAST(CMP, fast_cmp(state, &f));
    FAST(JMP, fast_jump(state, &f, true));
    FAST(JE, fast_jump(state, &f, state->zF));
    FAST(JNE, fast_jump(state, &f, !state->zF));
    FAST(JGT, fast_jump(state, &f, !state->zF && state->sF == state->oF));
    FAST(JLT, fast_jump(state, &f, state->sF != state->oF));
    FAST(JGE, fast_jump(state, &f, state->sF == state->oF));
    FAST(JLE, fast_jump(state, &f, state->zF || state->sF != state->oF));
    FAST(AND, fast_bitwise(state, &f, BIT_AND));
    FAST(OR, fast_bitwise(state, &f, BIT_OR));
    FAST(XOR, fast_bitwise(state, &f, BIT_XOR));
    FAST(SHL, fast_shift(state, &f, SHIFT_LEFT));
    FAST(SHR, fast_shift(state, &f, SHIFT_RIGHT));
    FAST(CALL, fast_call(state, &f));
    FAST(RET, fast_ret(state, &f));
    FAST(PUSH, fast_push(state, &f));
    FAST(POP, fast_pop(state, &f));
    FAST(REINTERPRET, fast_reinterpret(state, &f));

    DIRECT(RESERVEMEM, op_reservemem(state));
    DIRECT(FREEMEM, op_freemem(state));
    DIRECT(STRLIT, op_strlit(state));
    DIRECT(STRPRINT, op_strprint(state));
    DIRECT(STRCAT, op_strcat(state));
    DIRECT(STRCMP, op_strcmp(state));
    DIRECT(STRSLICE, op_strslice(state));
    DIRECT(STRFIND, op_strfind(state));
    DIRECT(STRCHR, op_strchr(state));
    DIRECT(STRFROM, op_strfrom(state));
    DIRECT(PRINT, op_print(state));
    DIRECT(CAST, op_cast(state));
    DIRECT(NEWARRAY, op_newarray(state));
    DIRECT(ARRFREE, op_arrfree(state));
    DIRECT(MEMSET, op_memset(state));

L_HALT:
    state->halt = true;
    halt_executed = true;
    goto done;

L_slow:
    /* everything without a fast path, and the unknown opcodes */
    GSLOW(slow_op(state, (OpCode)op));
    NEXT();

done:
    state->ip = f.ip;
    return finish_run(state, halt_executed);
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#undef FAST
#undef DIRECT
#undef NEXT
#undef GSLOW
#undef GOTO_TRACE
#undef ILANG_BUDGET

#endif /* ILANG_COMPUTED_GOTO */

/* ------------------------------------------------------------------ */
/* Loop 3: lazily quickened (gcc, clang) - the default there          */
/* ------------------------------------------------------------------ */

#ifdef ILANG_VM_QUICK
#include "IlangVM_quick.inc"
#endif

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

/**
 * @details Runs the loaded program. Returns true if it finished normally (HALT, or execution reached the very end of
 *          the bytecode) and false if it was stopped by an error: a panic, a truncated instruction, an unknown opcode,
 *          the instruction limit, or a jump outside the bytecode.
 *
 *          gcc and clang run the quickening loop (IlangVM_quick.inc) unless `ILANG_VM_NO_QUICK`, `ILANG_VM_TRACE`,
 *          `ILANG_VM_MAX_INSTRUCTIONS` or `ILANG_VM_STRICT_TYPES` is defined. Everywhere else the switch loop is the
 *          default, and the computed-goto loop (gcc/clang) is selected with `ILANG_VM_COMPUTED_GOTO`.
 */
bool IlangVM_execute(iState* state) {
    bool ok;

    if (!state || !state->memory) {
        return false;
    }

    state->running = true;
    ok = vm_run(state);
    state->running = false;
    vm_teardown(state);
    return ok;
}

/** @details Leaves everything else (memory, files) alone. Used for the whole program by IlangVM_execute(), and - nested, from inside a host function - for every call from the host into the program. */
bool vm_run(iState* state) {
#ifdef ILANG_VM_QUICK
    {
        int r = execute_quick(state);   /* -1: could not allocate its tables, nothing has run yet */
        if (r >= 0) return r != 0;
    }
#endif

#ifdef ILANG_COMPUTED_GOTO
    return execute_goto(state);
#else
    return execute_switch(state);
#endif
}
