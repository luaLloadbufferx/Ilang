/**
 * @file IlangVM.h
 * @brief Ilang virtual machine: value and instruction definitions, the iState structure and the public API.
 *
 * Typical use, for a program that embeds the VM:
 * @code
 * iState s = {0};                           // must start zeroed: host function definitions live in it
 * IlangVM_define(&s, "dot", "float(float[], float[])", dot_fn, NULL);   // optional host functions
 * if (!IlangVM_load(&s, bytecode, size)) fprintf(stderr, "%s\n", s.load_error);
 * else if (!IlangVM_execute(&s)) fprintf(stderr, "%s\n", s.panic_message);
 * @endcode
 * The build switches that shape the VM are documented in IlangConf.h.
 */
#pragma once

/* Required */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Configuration file */
#include "IlangConf.h"

/* Memory manager */
#include "IlangVM_Mem.h"

/* Host functions (C functions the program calls through CALLC) */
#include "IlangHost.h"

/**
 * @brief Value tags.
 *
 * The numbers are part of the bytecode; they mirror `enum Tag` in `Ilangc/ilang.hpp`.
 */
typedef enum Types {
    T_NONE = 0,  /**< No value (an unset register, or a void return type). */
    T_INT = 1,  /**< 32-bit signed integer. */
    T_UINT = 2,  /**< 32-bit unsigned integer. */
    T_FLOAT = 3,  /**< 32-bit float. */
    T_BOOL = 4,  /**< Boolean. */
    T_ADDRESS = 5,  /**< A raw VM address (what `unsafe` code and `address` values hold). */
    T_STRINGADDRESS = 6,  /**< Address of a string block (used in signatures; in registers a string is a plain address). */
    T_BYTE = 7  /**< Unsigned 8-bit integer. */
} Types;

/** @brief The payload of a Value; the tag says which member is valid. */
typedef union DataField {
    uint32_t u32;  /**< T_UINT, T_ADDRESS and T_STRINGADDRESS. */
    int32_t i32;  /**< T_INT. */

    float f32;  /**< T_FLOAT. */

    uint8_t u8;  /**< T_BOOL and T_BYTE. */
} DataField;

/** @brief A tagged value: what a register or a data stack slot holds. */
typedef struct Value {
    Types tag;  /**< Which member of `data` is valid. */

    DataField data;  /**< The payload. */
} Value;

/**
 * @brief Operand modes of the binary instructions (ADD, CMP, AND, ...): whether operands 1 and 2 are registers or
 *        immediates.
 */
typedef enum OpType {
    REG_REG = 0,  /**< Both operands are registers. */
    REG_IMM = 1,  /**< Operand 1 is a register, operand 2 an immediate. */
    IMM_REG = 2,  /**< Operand 1 is an immediate, operand 2 a register. */
    IMM_IMM = 3  /**< Both operands are immediates. */
} OpType;

/**
 * @brief All opcodes.
 *
 * The numbers are positional (MOV = 1, then +1 each) and are what the bytecode contains, so this list must mirror enum
 * Op in Ilangc/ilang.hpp. Adding or reordering opcodes changes the numbering: bump SUPPORTED_VERSION (IlangConf.h) and
 * kBytecodeVersion (ilang.hpp) with it.
 *
 * In the layout comments, MODE is an OpType and TYPE is the operand type tag: an operand that MODE says is an immediate
 * is stored inline as a value of TYPE instead of as a register index.
 *
 * @par Strings / byte arrays and input
 * A "string block" is the same heap block the compiler uses for `byte[]`:
 * @code{.unparsed}
 * [uint32 length][length bytes]        (no terminator; embedded 0 bytes are fine)
 * @endcode
 * All of these ops only require an address operand, whatever its tag, and read every operand before writing the
 * destination, so TO may equal an operand register.
 *
 * @par Dynamic arrays and arrays of arrays
 * An array block is `[uint32 length][elements...]` (see the string block above). An "array of arrays" is an array whose
 * elements are 4-byte addresses of other array blocks (the rows); the outer block OWNS its rows. DEPTH below is the
 * number of array levels of the value: 1 = a flat array (`int[]`, `byte[]`, ...), 2 = an array of flat arrays
 * (`int[][]`), and so on. A null (0) row is tolerated by ARRFREE / ARRCLONE and skipped.
 *
 * @par File I/O
 * A file is named by a HANDLE: an int in 0 .. ILANG_VM_MAX_FILES-1. Handles 0, 1 and 2 are stdin, stdout and stderr and
 * are always open (they can be read / written but not closed). FOPEN hands out the others. Files are always binary (no
 * newline translation), and every handle still open when the program ends is flushed and closed by the VM.
 *
 * Failures that depend on the outside world (no such file, permission denied, disk full, too many open files) do NOT
 * panic: the op reports them through its result (-1 / false) and records a message that FERROR reads. Misuse (not a
 * handle, a handle that is not open, reading a file opened only for writing, a bad mode) panics. A string operand is a
 * string block, see above. TO may equal an operand register.
 *
 * @par Shifts
 * Same layout as AND / OR / XOR: both operands and the result have the one TYPE, which must be T_INT, T_UINT or T_BYTE.
 * The count is the second operand and is read as a number of bits (a T_INT count must not be negative; the others are
 * unsigned). A count at or above the width of the type (32, or 8 for T_BYTE) is not undefined: every bit is shifted out,
 * so the result is 0 - except SHR of a negative T_INT, which gives -1.
 *
 * @par Host functions
 * CALLC calls a C function. IMPORT indexes the import table that follows the header (see iHeader): the loader has
 * already bound every entry to a function the host defined with the same signature. The arguments are where a CALL would
 * put them: the first NREG in r0 .. r(NREG-1), the rest on the data stack (the last one on top; the caller drops them
 * afterwards with LEAVE, as for CALL). The result, if the function has one, goes to r0. No other register, flag or stack
 * slot changes. Any failure of the host function panics.
 */
typedef enum OpCode {
    /**
     * @brief Copies a register or an immediate into a register.
     *
     * Encoding: `[MOV][MODE][IMM_TYPE][0 (TO)][1 (register or immediate)]`
     */
    MOV = 1,

    /**
     * @brief `TO = A + B` (integers wrap).
     *
     * Encoding: `[ADD][MODE][TYPE][0 (TO)][1][2]`
     */
    ADD,

    /**
     * @brief `TO = A - B` (integers wrap).
     *
     * Encoding: `[SUB][MODE][TYPE][0 (TO)][1][2]`
     */
    SUB,

    /**
     * @brief `TO = A * B` (integers wrap).
     *
     * Encoding: `[MUL][MODE][TYPE][0 (TO)][1][2]`
     */
    MUL,

    /**
     * @brief `TO = A / B`; division by zero panics.
     *
     * Encoding: `[DIV][MODE][TYPE][0 (TO)][1][2]`
     */
    DIV,

    /**
     * @brief `TO = A % B`; modulo by zero panics, and modulo is not defined for floats.
     *
     * Encoding: `[MOD][MODE][TYPE][0 (TO)][1][2]`
     */
    MOD,

    /**
     * @brief `TO` = address of a new zeroed memory block of SIZE bytes.
     *
     * Encoding: `[RESERVEMEM][TO_REG][SIZE:uint32]`
     */
    RESERVEMEM,

    /**
     * @brief Frees the memory block that starts at the address in a register.
     *
     * Encoding: `[FREEMEM][ADDRESS_REG]`
     */
    FREEMEM,

    /**
     * @brief Loads one element of TYPE from VM memory into a register.
     *
     * Encoding: `[LOAD][TYPE][ADDRESS_REG][TO_REG]`
     */
    LOAD,

    /**
     * @brief Stores a register into VM memory as TYPE (the register must hold that type).
     *
     * Encoding: `[STORE][TYPE][ADDRESS_REG][FROM_REG]`
     */
    STORE,

    /**
     * @brief `TO` = the data stack slot whose absolute index is in a register.
     *
     * Encoding: `[LOADLOCAL][STACK OFFSET HOLDING REGISTER][0 (TO)]`
     */
    LOADLOCAL,

    /**
     * @brief Stores a register or an immediate into the data stack slot whose absolute index is in a register.
     *
     * Encoding: `[STORELOCAL][STACK OFFSET HOLDING REGISTER][IMMTYPE][REGISTER (bool)][0 (from) OR value]`
     */
    STORELOCAL,

    /**
     * @brief `TO` = the absolute stack index of a local or a parameter of the current frame (for LOADLOCAL / STORELOCAL).
     *
     * Encoding: `[LEA][TO_REG][MODE (0 = local, 1 = parameter)][OFFSET]`
     */
    LEA,

    /**
     * @brief Reserves AMOUNT zeroed local slots at the top of the data stack.
     *
     * Encoding: `[ENTER][AMOUNT OF LOCALS]`
     */
    ENTER,

    /**
     * @brief Releases the top AMOUNT slots of the data stack.
     *
     * Encoding: `[LEAVE][AMOUNT OF LOCALS]`
     */
    LEAVE,

    /**
     * @brief Compares A and B and sets the zero, sign, carry and overflow flags that the conditional jumps read.
     *
     * Encoding: `[CMP][MODE][TYPE][0][1]`
     */
    CMP,

    /**
     * @brief Jumps unconditionally.
     *
     * Encoding: `[JMP][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
     */
    JMP,

    /**
     * @brief Jumps if the last CMP found its operands equal.
     *
     * Encoding: `[JE][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
     */
    JE,

    /**
     * @brief Jumps if the last CMP found its operands different.
     *
     * Encoding: `[JNE][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
     */
    JNE,

    /**
     * @brief Jumps if the last CMP found A greater than B.
     *
     * Encoding: `[JGT][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
     */
    JGT,

    /**
     * @brief Jumps if the last CMP found A less than B.
     *
     * Encoding: `[JLT][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
     */
    JLT,

    /**
     * @brief Jumps if the last CMP found A greater than or equal to B.
     *
     * Encoding: `[JGE][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
     */
    JGE,

    /**
     * @brief Jumps if the last CMP found A less than or equal to B.
     *
     * Encoding: `[JLE][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
     */
    JLE,

    /**
     * @brief `TO = A & B` (int, uint or byte).
     *
     * Encoding: `[AND][MODE][TYPE][0 (TO)][1][2]`
     */
    AND,

    /**
     * @brief `TO = A | B` (int, uint or byte).
     *
     * Encoding: `[OR][MODE][TYPE][0 (TO)][1][2]`
     */
    OR,

    /**
     * @brief `TO = A ^ B` (int, uint or byte).
     *
     * Encoding: `[XOR][MODE][TYPE][0 (TO)][1][2]`
     */
    XOR,

    /**
     * @brief Calls a function: pushes the return address and the caller's frame base on the call stack and starts a new frame.
     *
     * Encoding: `[CALL][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t)]`
     */
    CALL,

    /**
     * @brief Returns from the current function: restores the caller's frame base and resumes after the CALL.
     *
     * Encoding: `[RET]`
     */
    RET,

    /**
     * @brief Pushes a register on the data stack.
     *
     * Encoding: `[PUSH][0 (TO PUSH)]`
     */
    PUSH,

    /**
     * @brief Pops the top of the data stack into a register.
     *
     * Encoding: `[POP][0 (POPPED FROM STACK)]`
     */
    POP,

    /**
     * @brief Writes the value in a register to stdout.
     *
     * Encoding: `[PRINT][VALUE HOLDING REGISTER]`
     */
    PRINT,

    /**
     * @brief Converts the value in a register to another type, in place.
     *
     * Encoding: `[CAST][0 (REG)][TO_TYPE]`
     */
    CAST,

    /**
     * @brief Changes a register's type tag and leaves its bits alone.
     *
     * Encoding: `[REINTERPRET][0 (REG)][TO_TYPE]`
     */
    REINTERPRET,

    /**
     * @brief Stops the program normally.
     *
     * Encoding: `[HALT]`
     */
    HALT,

    /**
     * @brief `TO` = a new string block made from an inline literal.
     *
     * Encoding: `[STRLIT][TO_REG][LENGTH:uint32][BYTES]`
     */
    STRLIT,

    /**
     * @brief Writes the bytes of a string block to stdout.
     *
     * Encoding: `[STRPRINT][STR_REG]`
     */
    STRPRINT,

    /**
     * @brief `TO` = a new string, A followed by B.
     *
     * Encoding: `[STRCAT][TO_REG][A_REG][B_REG]`
     */
    STRCAT,

    /**
     * @brief `TO` = the int -1, 0 or 1 (bytewise, then by length).
     *
     * Encoding: `[STRCMP][TO_REG][A_REG][B_REG]`
     */
    STRCMP,

    /**
     * @brief `TO` = a new string holding a range of another; panics if the range is outside the string.
     *
     * Encoding: `[STRSLICE][TO_REG][STR_REG][START_REG][COUNT_REG]`
     */
    STRSLICE,

    /**
     * @brief `TO` = the int index of the first match of NEEDLE in HAYSTACK, or -1.
     *
     * Encoding: `[STRFIND][TO_REG][HAYSTACK_REG][NEEDLE_REG]`
     */
    STRFIND,

    /**
     * @brief `TO` = a new 1-byte string.
     *
     * Encoding: `[STRCHR][TO_REG][BYTE_REG]`
     */
    STRCHR,

    /**
     * @brief `TO` = a new string with a value formatted the way PRINT does.
     *
     * Encoding: `[STRFROM][TO_REG][VALUE_REG]`
     */
    STRFROM,

    /**
     * @brief Parses a string as an int, uint or float.
     *
     * Encoding: `[STRPARSE][TO_REG][OK_REG][STR_REG][TYPE]`
     *
     * TYPE = T_INT / T_UINT / T_FLOAT. TO = parsed value (0 on failure), OK = bool. Never panics on bad text.
     */
    STRPARSE,

    /**
     * @brief Flushes stdout, then reads one line from stdin into a new string.
     *
     * Encoding: `[READLINE][TO_REG]`
     *
     * flush stdout, read one line from stdin into a new string (without the line ending). At end of input the string
     * is empty.
     */
    READLINE,

    /**
     * @brief `TO` = true if stdin has no more data.
     *
     * Encoding: `[ATEOF][TO_REG]`
     */
    ATEOF,

    /**
     * @brief Stores a value into COUNT consecutive elements of VM memory.
     *
     * Encoding: `[MEMSET][TYPE][ADDRESS_REG][VALUE_REG][COUNT_REG]`
     *
     * Stores VALUE (its tag must equal TYPE, exactly like STORE) into COUNT consecutive elements starting at ADDRESS.
     * An element is 1 byte for T_BOOL / T_BYTE and 4 bytes for T_INT / T_UINT / T_FLOAT / T_ADDRESS. COUNT is an int /
     * uint / byte register and must not be negative; COUNT == 0 does nothing. The whole range must lie inside ONE live
     * memory block, otherwise the VM panics (nothing is written).
     */
    MEMSET,

    /**
     * @brief `TO` = a new zeroed array block of COUNT elements.
     *
     * Encoding: `[NEWARRAY][TO_REG][COUNT_REG][ELEM_SIZE:uint8]`
     *
     * TO = new zeroed array block holding COUNT elements of ELEM_SIZE bytes (1 or 4), its length field already set to
     * COUNT. COUNT is an int / uint / byte register; a negative COUNT, a size that does not fit the address space or
     * running out of VM memory panics.
     */
    NEWARRAY,

    /**
     * @brief Frees an array block and, when DEPTH > 1, the rows it owns.
     *
     * Encoding: `[ARRFREE][ADDRESS_REG][DEPTH:uint8]`
     *
     * Frees the array block, first freeing every row it owns when DEPTH > 1 (recursively). DEPTH == 1 is the same as
     * FREEMEM.
     */
    ARRFREE,

    /**
     * @brief `TO` = a deep copy of an array block.
     *
     * Encoding: `[ARRCLONE][TO_REG][SRC_REG][DEPTH:uint8]`
     *
     * TO = deep copy of the array block in SRC: a new block for the array itself and, when DEPTH > 1, a new copy of
     * every row, so TO shares no memory with SRC. SRC is left alone.
     */
    ARRCLONE,

    /**
     * @brief `TO` = a new array of COUNT independent deep copies of an array (an array of arrays).
     *
     * Encoding: `[ARRREP][TO_REG][SRC_REG][COUNT_REG][DEPTH:uint8]`
     *
     * TO = new array of COUNT rows (an array of arrays, DEPTH + 1 levels), each row an independent deep copy of the
     * DEPTH-level array in SRC. SRC is left alone (the caller still owns and frees it). COUNT is an int / uint / byte
     * register and must not be negative.
     */
    ARRREP,

    /**
     * @brief Panics with the bytes of a string block as the message.
     *
     * Encoding: `[ABORT][STR_REG]`
     *
     * panic with the bytes of a string block as the message. Used by the compiler for its runtime checks (index out of
     * bounds, bad number text), so the report names the failing instruction.
     */
    ABORT,

    /**
     * @brief Opens a file; `TO` = an int handle, or -1 on failure.
     *
     * Encoding: `[FOPEN][TO_REG][PATH_REG][MODE_REG]`
     *
     * MODE is the string "r", "w" or "a" (optionally followed by "b"). TO = int handle, or -1 on failure. "r" reads an
     * existing file, "w" creates / truncates, "a" appends.
     */
    FOPEN,

    /**
     * @brief Flushes and closes a file; `TO` = a bool.
     *
     * Encoding: `[FCLOSE][TO_REG][HANDLE_REG]`
     *
     * flush and close. TO = bool, false if the flush / close failed or the handle is a standard stream. The handle is
     * free afterwards either way.
     */
    FCLOSE,

    /**
     * @brief `TO` = a new string of up to COUNT bytes read from a file.
     *
     * Encoding: `[FREAD][TO_REG][HANDLE_REG][COUNT_REG]`
     *
     * TO = new string of up to COUNT bytes (COUNT is an int / uint / byte register and must not be negative). Shorter
     * only at the end of the file; empty if nothing is left.
     */
    FREAD,

    /**
     * @brief `TO` = a new string holding one line read from a file, without its line ending.
     *
     * Encoding: `[FREADLINE][TO_REG][HANDLE_REG]`
     *
     * like READLINE, but from a file: one line without its line ending (a "\r\n" ending is accepted too). At the end
     * of the file the string is empty.
     */
    FREADLINE,

    /**
     * @brief Writes the bytes of a string block to a file; `TO` = a bool.
     *
     * Encoding: `[FWRITE][TO_REG][HANDLE_REG][STR_REG]`
     *
     * write the bytes of a string block. TO = bool, true if all of them were written.
     */
    FWRITE,

    /**
     * @brief `TO` = true if the file has no more data.
     *
     * Encoding: `[FEOF][TO_REG][HANDLE_REG]`
     */
    FEOF,

    /**
     * @brief `TO` = a new string describing the last file failure.
     *
     * Encoding: `[FERROR][TO_REG]`
     *
     * TO = new string describing the last failure of FOPEN / FCLOSE / FWRITE (empty if there has been none).
     */
    FERROR,

    /**
     * @brief `TO` = a new string holding a whole file; panics on failure.
     *
     * Encoding: `[FREADFILE][TO_REG][PATH_REG]`
     *
     * TO = new string holding the whole file. Unlike FOPEN, a file that cannot be opened or read panics, with the
     * reason and the path in the message.
     */
    FREADFILE,

    /**
     * @brief `TO = A << COUNT`.
     *
     * Encoding: `[SHL][MODE][TYPE][0 (TO)][1][2]`
     *
     * TO = 1 << 2 (bits shifted out on the left are lost). A negative T_INT count panics.
     */
    SHL,

    /**
     * @brief `TO = A >> COUNT` (arithmetic for int, logical for uint and byte).
     *
     * Encoding: `[SHR][MODE][TYPE][0 (TO)][1][2]`
     *
     * TO = 1 >> 2. T_INT: arithmetic (the sign bit is copied in); T_UINT / T_BYTE: logical (zeros come in). A negative
     * T_INT count panics.
     */
    SHR,

    /**
     * @brief Calls a C host function.
     *
     * Encoding: `[CALLC][IMPORT:uint16][NREG:uint8]`
     */
    CALLC,

    /**
     * @brief Not an opcode: one past the last one (sizes the name table in IlangVM.c).
     */
    OP__END
} OpCode;

/**
 * @brief Bytecode header.
 *
 * It is followed by the import table, then the code (`Entrypoint` points into it):
 * @code{.unparsed}
 * uint32 count
 * count x { uint8 name_length, name bytes, uint8 ret_type, uint8 ret_depth, uint8 argc, argc x { uint8 type, uint8 depth } }
 * @endcode
 * A type is an operand tag (T_INT .. T_BYTE, T_STRINGADDRESS for string, T_NONE for a void return) and depth is the
 * number of array levels (0 for a scalar or string, 1 for `int[]`, 2 for `int[][]`, ...).
 *
 * @par Extension sections
 * Anything between the end of the import table and `Entrypoint` is a sequence of
 * @code{.unparsed}
 * { char tag[4], uint32 length, length bytes }
 * @endcode
 * A VM skips the tags it does not know (and a VM that predates them never looks there: it starts at `Entrypoint`).
 * The tag `"EXPT"` lists the functions the host may call (`export fn`):
 * @code{.unparsed}
 * uint32 init_address   a stub that runs the global initializers and HALTs
 * uint32 count
 * count x { uint8 name_length, name bytes, uint32 address, uint8 nreg, uint8 ret_type, uint8 ret_depth,
 *           uint8 argc, argc x { uint8 type, uint8 depth } }
 * @endcode
 */
typedef struct iHeader {
    char Magic[4];  /**< Bytecode magic ("iVM\0"). */
    uint32_t Version;  /**< Bytecode version. */
    uint32_t Entrypoint;  /**< Absolute code offset where execution starts. */
} iHeader;

/** @brief A parsed signature: `ret` / `args` are (type, depth) pairs as in the import table. */
typedef struct iHostSig {
    uint8_t ret_type,    /**< Return type tag (T_NONE for void). */
            ret_depth;   /**< Return array levels (0 for a scalar or string). */
    uint8_t argc;  /**< Number of parameters. */
    uint8_t arg_type[ILANG_HOST_MAX_ARGS];  /**< Type tag of each parameter. */
    uint8_t arg_depth[ILANG_HOST_MAX_ARGS];  /**< Array levels of each parameter. */
} iHostSig;

/** @brief A function the program offers to the host (`export fn`): from the EXPT section, see iHeader. */
typedef struct iExport {
    char* name;  /**< Name of the function. */
    uint32_t address;  /**< Absolute code offset. */
    uint8_t nreg;     /**< How many arguments travel in registers (the rest go on the data stack, last on top). */
    iHostSig sig;  /**< Parameter and return types. */
} iExport;

/** @brief A function the host defined (IlangVM_define() / a library's `ilang_open`). */
typedef struct iHostFunc {
    char* name;  /**< Name of the function (what an `extern fn` declaration uses). */
    iHostSig sig;  /**< Parameter and return types. */
    IlangHostFn fn;  /**< The C function. */
    void* user;  /**< Pointer passed back to the function by `api->user()`. */
} iHostFunc;

/**
 * @brief Main VM state.
 *
 * Must start zeroed (a static one, or `iState s = {0};`): host function definitions live in it. All memory the program
 * uses is owned by `memory`; see IlangVM_Mem.h.
 */
typedef struct iState {
    Value r[REG_COUNT];  /**< Virtual registers. */

    const uint8_t* code;  /**< Pointer to bytecode in memory. */
    uint32_t code_size;  /**< Bytecode size in bytes. */

    Value stack[MAX_STACK_SIZE];  /**< Virtual (data) stack. */
    Value* top;  /**< Pointer to the top value on the stack. */
    Value* bp;  /**< Base pointer (current frame). */

    uint32_t callStack[MAX_CALL_DEPTH];  /**< Call stack (return addresses). */
    uint32_t* callTop;  /**< Pointer to the top value on the call stack. */

    uint32_t ip;  /**< Instruction pointer (code offset). */

    bool halt;  /**< Halt flag. */

    /* Condition flags, set by CMP and read by the conditional jumps (as on x86) */
    bool zF;  /**< Zero flag. */
    bool sF;  /**< Sign flag. */
    bool cF;  /**< Carry flag. */
    bool oF;  /**< Overflow flag. */

    /* Memory */
    iMemory* memory;  /**< The program's virtual memory. */

    /* Files (handles 0..2 are stdin / stdout / stderr) */
    FILE* files[ILANG_VM_MAX_FILES];  /**< Open files by handle; NULL = free slot (handles 0..2 are stdin / stdout / stderr). */
    uint8_t file_flags[ILANG_VM_MAX_FILES];  /**< FILE_CAN_READ / FILE_CAN_WRITE for each handle. */
    char io_error[ILANG_VM_IO_ERROR_SIZE];  /**< Last file failure, read by FERROR. */

    /* Host functions: what the host defined (kept across loads) and what the loaded program imports */
    iHostFunc* host_funcs;  /**< What the host defined (kept across loads). */
    uint32_t host_count,      /**< Number of entries in `host_funcs`. */
             host_capacity;   /**< Allocated length of `host_funcs`, in entries. */
    const iHostFunc** imports;  /**< Import index -> bound host function (what the loaded program imports). */
    uint32_t import_count;  /**< Number of imports of the loaded program. */

    /* Calls from the host into the program (`export fn`) */
    iExport* exports;  /**< The `export fn` functions of the loaded program. */
    uint32_t export_count;  /**< Number of entries in `exports`. */
    uint32_t init_address;  /**< Code offset of the global-initializer stub; valid if `has_init`. */
    bool has_init;  /**< The program has an EXPT section (so `init_address` and `exports` are valid). */
    bool started;          /**< IlangVM_start() ran the global initializers. */
    bool running;          /**< IlangVM_execute() is running the program. */
    uint32_t host_depth;   /**< Host calls currently running inside each other (Ilang -> C -> Ilang -> C ...). */
    struct IlangCall* root_call;  /**< The context IlangVM_context() hands out (outside any host function). */

    /**
     * @brief The quickening interpreter's decode tables (IlangVM_quick.inc), kept for the whole run: one per nesting level
     *        of host calls (`host_depth`), so that a loop running nested inside another never touches the outer loop's table.
     */
    void* quick_dec[ILANG_VM_MAX_HOST_DEPTH + 1];

    char load_error[ILANG_VM_LOAD_ERROR_SIZE];    /**< Why IlangVM_load() failed ("" if it did not say). */
    char panic_message[ILANG_VM_PANIC_MESSAGE_SIZE];  /**< The last panic's message ("" if none). */
} iState;

/** @brief Flag in iState::file_flags: the handle may be read. */
#define FILE_CAN_READ 1u
/** @brief Flag in iState::file_flags: the handle may be written. */
#define FILE_CAN_WRITE 2u

/**
 * @brief Name of an opcode.
 * @param op An opcode byte.
 * @return The mnemonic, or "???" for a byte that is not an opcode.
 */
const char* IlangVM_opcode_name(uint8_t op);

/**
 * @brief Loads a program (header, import table, extension sections) and binds its `extern fn` imports.
 *
 * Host functions must be defined BEFORE this call.
 *
 * @param[in,out] state         The VM (zeroed before its first use).
 * @param[in]     bytecode      The `.ic` image; it must stay alive while the program runs.
 * @param[in]     bytecode_size Its size in bytes.
 * @return false on failure; `state->load_error` says why.
 */
bool IlangVM_load(iState* state, const uint8_t* bytecode, size_t bytecode_size);
/**
 * @brief Runs the loaded program from its entry point until it halts or panics, then closes its files and frees its
 *        memory.
 * @param[in,out] state The VM, after a successful IlangVM_load().
 * @return true if the program ended normally; false on a panic (`state->panic_message` holds the message).
 */
bool IlangVM_execute(iState* state);

/**
 * @brief Runs the global initializers (not `main`); the program then stays loaded.
 *
 * Calling the program from C (functions it declares with `export fn`), after IlangVM_load():
 *  -# IlangVM_start() runs the global initializers (not main); the program then stays loaded.
 *  -# IlangVM_call() calls an exported function by name, any number of times.
 *  -# IlangVM_finish() closes the program's files and frees its memory.
 *
 * IlangVM_context() is the IlangCall to use with the IlangHostAPI functions (`new_array`, `view`, `release`,
 * `last_error`, `call`) outside a host function. A host function calls the program with `api->call()` on its own
 * IlangCall instead.
 *
 * Does nothing (and returns true) if it already ran.
 *
 * @param[in,out] state The VM, after a successful IlangVM_load().
 * @return false if no program is loaded, the program has no `export fn` (so there is nothing to call), or the
 *         initializers panicked (see `state->panic_message`).
 */
bool IlangVM_start(iState* state);
/**
 * @brief Calls an exported function (`export fn`) by name.
 * @param[in,out] state  The VM, after IlangVM_start().
 * @param[in]     name   Name of the exported function.
 * @param[in]     args   The arguments (see the `ilang_*` constructors in IlangHost.h).
 * @param[in]     argc   Number of arguments.
 * @param[out]    result Receives the return value (type 0 for a void function); may be NULL.
 * @return false if the call was wrong or the program panicked.
 */
bool IlangVM_call(iState* state, const char* name, const IlangValue* args, uint32_t argc, IlangValue* result);
/**
 * @brief The IlangCall to pass to IlangHostAPI functions outside any host function.
 * @param[in,out] state The VM.
 */
IlangCall* IlangVM_context(iState* state);
/**
 * @brief Closes the program's files and frees its memory (the end of a start / call / finish session).
 * @param[in,out] state The VM.
 */
void IlangVM_finish(iState* state);

/**
 * @brief Defines a host function. Define them BEFORE IlangVM_load(), which binds the program's imports to them.
 *
 * The iState must start zeroed (a static one, or `iState s = {0};`), because the definitions live in it.
 *
 * @param[in,out] s         The VM.
 * @param[in]     name      Name used by the program's `extern fn` declaration.
 * @param[in]     signature Signature string such as `"float(float[], float[])"` (see IlangHost.h).
 * @param[in]     fn        The C function.
 * @param[in]     user      Pointer passed back by `api->user()`.
 * @return false if the signature does not parse or the name is already defined.
 */
bool IlangVM_define(iState* state, const char* name, const char* signature, IlangHostFn fn, void* user);
/**
 * @brief Runs a library's `ilang_open()` against this state.
 * @param[in,out] state The VM.
 * @param[in]     open  The library's entry point.
 * @return The result of `open`.
 */
bool IlangVM_open_library(iState* state, IlangOpenFn open);
/** @brief The function table that host functions and libraries get (IlangHost.h). */
const IlangHostAPI* IlangVM_host_api(void);
/**
 * @brief Forgets every defined function and import (frees their memory).
 * @param[in,out] state The VM.
 */
void IlangVM_host_reset(iState* state);

/**
 * @brief Internal (IlangVM_host.c): reads the import table after the header and binds every entry to a host function.
 * @param[in,out] s          The VM.
 * @param[in]     code       The `.ic` image.
 * @param[in]     size       Its size in bytes.
 * @param[out]    code_start Receives the offset just past the import table.
 * @return false on failure (`state->load_error` says why).
 */
bool host_bind_imports(iState* state, const uint8_t* bytecode, uint32_t size, uint32_t* code_start);
/**
 * @brief Internal (IlangVM_host.c): reads the extension sections between `from` and `to` (the EXPT table).
 * @param[in,out] s        The VM.
 * @param[in]     code     The `.ic` image.
 * @param[in]     from     Offset of the first section.
 * @param[in]     to       Offset where the sections end (the entry point).
 * @param[in]     size     Size of the image in bytes.
 * @return false on a malformed section (`state->load_error` says why).
 */
bool host_read_sections(iState* state, const uint8_t* bytecode, uint32_t from, uint32_t to, uint32_t size);
/**
 * @brief Internal (IlangVM_host.c): frees what belongs to the loaded program (exports, the root context).
 * @param[in,out] s     The VM.
 */
void host_end_program(iState* state);
/**
 * @brief Internal (IlangVM_host.c): executes the CALLC instruction.
 * @param[in,out] s     The VM; `ip` points at the instruction's operands.
 */
void op_callc(iState* state);
/**
 * @brief Internal (IlangVM.c): runs from `state->ip` until HALT, a panic or `ip == code_size`.
 * @param[in,out] state The VM.
 * @return false on a panic.
 */
bool vm_run(iState* state);
/**
 * @brief Internal (IlangVM.c): closes the program's files and frees its memory.
 * @param[in,out] state The VM.
 */
void vm_teardown(iState* state);
