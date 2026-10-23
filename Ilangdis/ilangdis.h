/**
 * @file ilangdis.h
 * @brief Ilangdis, the Ilang disassembler: the data model and the interface between its modules.
 *
 * Ilangdis reads a bytecode image (`.ic`, produced by `Ilangc`) and prints it as a readable listing. The work is split
 * in four steps, each in its own source file:
 *
 *  -# image.c: dis_image_parse() reads the header, the import table and the extension sections (`EXPT`), and checks
 *     them the way the VM's loader does. The result is a DisImage.
 *  -# decode.c: dis_decode() turns the bytes at one address into a DisInsn. The decoder mirrors the operand layouts of
 *     `IlangVM.h`; the VM's own `OpCode` and `Types` enumerations are used, so the two cannot drift apart unnoticed.
 *  -# analyze.c: dis_analyze() sweeps the whole code, then finds the functions, names them, finds the jump targets
 *     (labels) and records every cross reference. The result is a DisProgram.
 *  -# arrows.c, format.c and listing.c print it: instruction text, the arrow margin, the listing and the reports.
 *
 * The bytecode carries no symbols, so the function finder works from what the compiler leaves behind; see
 * dis_analyze() for the rules.
 */
#pragma once

/* Required */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The VM's definitions: OpCode, Types, OpType, iHostSig, SUPPORTED_VERSION. Nothing from the VM is linked. */
#include "../IlangVM/VM/IlangVM.h"

/** @brief Version of Ilangdis. */
#define ILANGDIS_VERSION "1.0.0"

/** @brief Highest register count a program can be compiled for (`ilangc --regs`), so a register operand must be below it. */
#define DIS_MAX_REGS 64

/* ------------------------------------------------------------------------------------------------------------------ */
/* Utilities (util.c)                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------------ */

/**
 * @brief malloc() that never returns NULL: when memory runs out it reports that and ends the program.
 * @param size Bytes wanted (0 is allowed).
 */
void* dis_xmalloc(size_t size);
/**
 * @brief calloc() that never returns NULL (see dis_xmalloc()).
 * @param count Number of elements.
 * @param size  Size of one element.
 */
void* dis_xcalloc(size_t count, size_t size);
/**
 * @brief realloc() that never returns NULL (see dis_xmalloc()).
 * @param ptr  The block to grow (or NULL).
 * @param size New size in bytes.
 */
void* dis_xrealloc(void* ptr, size_t size);
/**
 * @brief Copies at most @p length bytes of a string into new memory and terminates the copy.
 * @param text   The characters.
 * @param length Number of characters to copy.
 * @return A string to be released with free().
 */
char* dis_xstrndup(const char* text, size_t length);
/**
 * @brief Makes room for at least @p need elements in a growing array.
 *
 * The capacity doubles, so appending is amortised O(1).
 *
 * @param[in,out] array    Address of the array pointer.
 * @param[in,out] capacity Address of the current capacity, in elements.
 * @param[in]     element  Size of one element.
 * @param[in]     need     Number of elements the array has to hold.
 */
void dis_reserve(void* array, size_t* capacity, size_t element, size_t need);

/* ------------------------------------------------------------------------------------------------------------------ */
/* The image (image.c)                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------------ */

/** @brief One `extern fn` the program calls with CALLC (an entry of the import table). */
typedef struct DisImport {
    char* name;    /**< Name of the host function (non-printable bytes are shown as '?'). */
    iHostSig sig;  /**< Declared signature. */
} DisImport;

/** @brief One `export fn` the host may call (an entry of the EXPT section). */
typedef struct DisExport {
    char* name;        /**< Name of the function (non-printable bytes are shown as '?'). */
    uint32_t address;  /**< Absolute offset of its first instruction. */
    uint8_t nreg;      /**< How many arguments travel in registers (the rest go on the data stack). */
    iHostSig sig;      /**< Parameter and return types. */
} DisExport;

/** @brief An extension section between the import table and the code (`char tag[4]`, `u32 length`, payload). */
typedef struct DisSection {
    char tag[5];      /**< The four tag characters, terminated (non-printable bytes are shown as '?'). */
    uint32_t offset;  /**< Offset of the payload in the image. */
    uint32_t length;  /**< Length of the payload in bytes. */
} DisSection;

/**
 * @brief A parsed `.ic` file: everything that comes before the code, plus the bytes themselves.
 *
 * The layout is described at iHeader in `IlangVM.h`. The code is the part of the file from #entry to the end.
 */
typedef struct DisImage {
    const uint8_t* data;      /**< The whole file. It is not copied and must stay alive as long as the DisImage is used. */
    uint32_t size;            /**< Size of the file in bytes. */
    uint32_t version;         /**< Bytecode version from the header. */
    uint32_t entry;           /**< Offset where execution starts; also where the code starts. */
    uint32_t table_end;       /**< Offset just past the import table. */
    DisImport* imports;       /**< The import table (index = operand of CALLC). */
    uint32_t import_count;    /**< Number of imports. */
    DisSection* sections;     /**< The extension sections, in file order. */
    uint32_t section_count;   /**< Number of sections. */
    bool has_expt;            /**< The file has an EXPT section (it offers `export fn`s to the host). */
    uint32_t init_address;    /**< EXPT only: offset of the stub that runs the global initializers. */
    DisExport* exports;       /**< EXPT only: the exported functions. */
    uint32_t export_count;    /**< Number of exports. */
} DisImage;

/**
 * @brief Reads the header, the import table and the extension sections of an image.
 *
 * The checks are the ones of the VM's loader, so a file that Ilangdis accepts is a file the VM accepts.
 *
 * @param[out] image Receives the parsed image (zeroed first). Release it with dis_image_free() after a success; after
 *                   a failure there is nothing to release.
 * @param[in]  data  The file contents.
 * @param[in]  size  Their size in bytes.
 * @param[out] error Receives a one-line explanation when the file is rejected.
 * @param[in]  error_size Size of @p error in bytes.
 * @return false if the file is not a valid image (@p error says why).
 */
bool dis_image_parse(DisImage* image, const uint8_t* data, size_t size, char* error, size_t error_size);
/**
 * @brief Releases what dis_image_parse() allocated. The file contents are not touched.
 * @param image The image (may be zeroed).
 */
void dis_image_free(DisImage* image);

/* ------------------------------------------------------------------------------------------------------------------ */
/* Instructions (decode.c)                                                                                             */
/* ------------------------------------------------------------------------------------------------------------------ */

/** @brief The family of an instruction; the listing colours mnemonics by it. */
typedef enum DisClass {
    DIS_CLASS_ALU,    /**< Moves, arithmetic, logic, comparisons and conversions. */
    DIS_CLASS_MEMORY, /**< Memory, arrays, stack slots. */
    DIS_CLASS_CONTROL,/**< Jumps, calls, returns, halting. */
    DIS_CLASS_IO,     /**< Strings, console and files. */
    DIS_CLASS_STACK   /**< Frame and data stack management. */
} DisClass;

/** @brief An operand of a binary instruction (ADD, CMP, ...) or the source of a MOV: a register or an inline immediate. */
typedef struct DisOperand {
    bool is_reg;      /**< true: the operand is register #reg; false: the immediate #bits. */
    uint8_t reg;      /**< Register index (if @c is_reg). */
    uint32_t bits;    /**< The immediate, as the bits of its type (a float is stored as its IEEE-754 bits). */
} DisOperand;

/** @brief Bit flags set by dis_analyze() in DisInsn::flags. */
typedef enum DisInsnFlag {
    DIS_F_TARGET_FWD  = 1,  /**< Some jump that lies before this instruction jumps to it. */
    DIS_F_TARGET_BACK = 2,  /**< Some jump at or after this instruction jumps back to it. */
    DIS_F_TAIL_CALL   = 4,  /**< This unconditional jump goes to the first instruction of a function: a tail call. */
    DIS_F_FUNC_START  = 8   /**< This instruction is the first of a function. */
} DisInsnFlag;

/**
 * @brief One decoded instruction.
 *
 * Which members are meaningful depends on the opcode; dis_decode() documents the mapping. The last three members are
 * filled in by dis_analyze().
 */
typedef struct DisInsn {
    uint32_t addr;       /**< Offset of the opcode byte in the image. */
    uint32_t len;        /**< Length in bytes (at least 1). */
    uint8_t op;          /**< An OpCode, or 0 for a byte that does not start a valid instruction. */
    uint8_t mode;        /**< MODE of a binary instruction (an OpType); MODE of LEA; for jumps and calls: 0 or 1 = relative. */
    uint8_t type;        /**< The type tag the instruction names (TYPE, IMM_TYPE, TO_TYPE); for op 0: the offending byte. */
    uint8_t nreg;        /**< Number of valid entries in #reg. */
    uint8_t reg[5];      /**< Register operands in encoding order (for a binary instruction: only the destination). */
    uint8_t small;       /**< A one-byte operand: element size, array depth, CALLC's register-argument count. */
    bool relative;       /**< Jumps and calls: the target is relative to the next instruction. */
    bool indirect;       /**< Jumps and calls: the target is in a register (reg[0]). */
    DisOperand a;        /**< First operand of a binary instruction; the value of MOV / STORELOCAL. */
    DisOperand b;        /**< Second operand of a binary instruction. */
    uint32_t value;      /**< A 32-bit operand: size, amount, slot offset, string length; the raw operand of a jump. */
    uint32_t target;     /**< Absolute destination of a jump or call whose target is not in a register. */
    bool has_target;     /**< #target is valid. */
    uint16_t import;     /**< CALLC: the import index. */
    uint32_t data_off;   /**< STRLIT: offset of the string bytes in the image (their count is #value). */
    const char* error;   /**< For op 0: why the bytes could not be decoded. */

    int32_t target_idx;  /**< (analysis) Index of the instruction #target points at, or -1 if there is none. */
    int32_t func;        /**< (analysis) Index of the function this instruction belongs to. */
    int32_t label;       /**< (analysis) Index of the label placed at this instruction, or -1. */
    uint8_t flags;       /**< (analysis) A combination of DisInsnFlag. */
} DisInsn;

/**
 * @brief Decodes the instruction at an address.
 *
 * The operand layouts are the ones documented at OpCode in `IlangVM.h` (and implemented by the VM's `op_*` functions).
 * A relative jump or call target is made absolute (relative to the end of the instruction, as the VM does).
 *
 * This never fails: a byte that does not start a valid instruction (unknown opcode, truncated operands, a register
 * number or type tag that cannot be right) is returned as an instruction with `op == 0`, length 1 and `error` set, so
 * the caller can carry on one byte later.
 *
 * @param[in]  image The image.
 * @param[in]  addr  Offset to decode at; must be below `image->size`.
 * @param[out] out   Receives the instruction.
 * @return The length of the instruction in bytes (at least 1).
 */
uint32_t dis_decode(const DisImage* image, uint32_t addr, DisInsn* out);
/**
 * @brief The mnemonic of an opcode.
 * @param op An opcode byte.
 * @return The name (`"MOV"`, ...), or `"???"` for a byte that is not an opcode.
 */
const char* dis_mnemonic(uint8_t op);
/**
 * @brief The family of an opcode (used to colour mnemonics).
 * @param op An opcode byte.
 */
DisClass dis_class(uint8_t op);
/**
 * @brief True for JMP, JE, JNE, JGT, JLT, JGE and JLE.
 * @param op An opcode byte.
 */
bool dis_is_jump(uint8_t op);
/**
 * @brief True for the conditional jumps (all of dis_is_jump() except JMP).
 * @param op An opcode byte.
 */
bool dis_is_cond_jump(uint8_t op);
/**
 * @brief The Ilang name of a type tag.
 * @param tag A T_* value.
 * @return `"int"`, `"uint"`, `"float"`, `"bool"`, `"address"`, `"string"`, `"byte"` or `"void"`; `"?"` for any other tag.
 */
const char* dis_type_name(uint8_t tag);
/**
 * @brief Writes a type as it appears in Ilang source: its name followed by `[]` once per array level.
 * @param[out] buf   Output buffer.
 * @param[in]  size  Its size.
 * @param[in]  tag   Type tag (T_STRINGADDRESS is a string).
 * @param[in]  depth Number of array levels.
 */
void dis_format_type(char* buf, size_t size, uint8_t tag, uint8_t depth);
/**
 * @brief Writes a function signature such as `fib(int, string) -> int` (`-> void` is left out).
 * @param[out] buf  Output buffer.
 * @param[in]  size Its size.
 * @param[in]  name Function name.
 * @param[in]  sig  The signature.
 */
void dis_format_sig(char* buf, size_t size, const char* name, const iHostSig* sig);

/* ------------------------------------------------------------------------------------------------------------------ */
/* The analysed program (analyze.c)                                                                                    */
/* ------------------------------------------------------------------------------------------------------------------ */

/** @brief Where the name of a function came from, from least to most reliable. */
typedef enum DisNameSource {
    DIS_NAME_GENERATED, /**< Made up from the address (`func_0048`). */
    DIS_NAME_FAILSTUB,  /**< Read from the function's own runtime-error message ("... in 'fib'"). */
    DIS_NAME_STARTUP,   /**< Deduced from the startup stub: `main` and `_globals`. */
    DIS_NAME_EXPORT,    /**< The name in the EXPT section; the entry and init stubs (`_start`, `_init`) count as this too. */
    DIS_NAME_USER       /**< Chosen by the user (`--name`, `--names`); it beats everything else. */
} DisNameSource;

/** @brief A function found in the code. */
typedef struct DisFunc {
    char* name;              /**< Its name (never empty, unique). */
    DisNameSource source;    /**< How the name was found. */
    uint32_t first;          /**< Index of its first instruction. */
    uint32_t count;          /**< Number of instructions. */
    uint32_t start;          /**< Offset of the first byte. */
    uint32_t end;            /**< Offset just past the last byte. */
    bool has_frame;          /**< It starts with ENTER, so #frame is valid. */
    uint32_t frame;          /**< Number of stack slots ENTER reserves for locals and temporaries. */
    uint32_t body;           /**< Index of the first instruction after the ENTER (a self tail call jumps here). */
    int32_t export_index;    /**< Index in DisImage::exports if the host can call it, else -1. */
    bool is_entry;           /**< It is the startup stub at the entry point (`_start`). */
    bool is_init;            /**< It is the stub IlangVM_start() runs (`_init`). */
} DisFunc;

/** @brief The role a label plays; it decides its name. */
typedef enum DisLabelKind {
    DIS_LABEL_PLAIN,    /**< A branch target: `.L1`. */
    DIS_LABEL_LOOP,     /**< The target of a jump that goes backwards: `.loop1`. */
    DIS_LABEL_BODY,     /**< The start of the body, past the ENTER: `.body` (where a self tail call jumps). */
    DIS_LABEL_EPILOGUE, /**< The common exit that leaves the frame and returns: `.epilogue`. */
    DIS_LABEL_OOB,      /**< The compiler's array bounds failure stub: `.oob_fail`. */
    DIS_LABEL_PARSE,    /**< The compiler's number conversion failure stub: `.parse_fail`. */
    DIS_LABEL_FAIL      /**< Some other runtime error stub: `.fail`. */
} DisLabelKind;

/** @brief A jump target inside a function. */
typedef struct DisLabel {
    uint32_t insn;        /**< Index of the instruction it is placed at. */
    int32_t func;         /**< Index of the function it belongs to. */
    DisLabelKind kind;    /**< Its role. */
    char name[24];        /**< Its name, with the leading dot (unique inside the function). */
} DisLabel;

/** @brief The kind of a cross reference. */
typedef enum DisXrefKind {
    DIS_XREF_JUMP,  /**< An unconditional jump. */
    DIS_XREF_COND,  /**< A conditional jump. */
    DIS_XREF_CALL,  /**< A CALL. */
    DIS_XREF_TAIL   /**< An unconditional jump to the start of a function: a tail call. */
} DisXrefKind;

/** @brief "Instruction #from refers to instruction #to". */
typedef struct DisXref {
    uint32_t to;        /**< Index of the instruction that is referred to. */
    uint32_t from;      /**< Index of the jump or call. */
    DisXrefKind kind;   /**< What kind of reference it is. */
} DisXref;

/** @brief The result of the analysis: the decoded code with its functions, labels and cross references. */
typedef struct DisProgram {
    const DisImage* image;   /**< The image the program was read from. */
    DisInsn* insns;          /**< Every instruction of the code, in address order. */
    size_t insn_count;       /**< Number of instructions. */
    DisFunc* funcs;          /**< The functions, in address order. */
    size_t func_count;       /**< Number of functions. */
    DisLabel* labels;        /**< The labels, in address order. */
    size_t label_count;      /**< Number of labels. */
    DisXref* xrefs;          /**< Every jump and call with a known target, sorted by DisXref::to, then DisXref::from. */
    size_t xref_count;       /**< Number of cross references. */
    size_t bad_bytes;        /**< Bytes that could not be decoded as instructions. */
    size_t bad_targets;      /**< Jumps and calls whose target is not the start of an instruction. */
} DisProgram;

/**
 * @brief Decodes the whole code and finds its structure.
 *
 * The bytecode has no symbol table and no function boundaries, so the analysis relies on how `Ilangc` lays code out:
 *  - the code is one run of instructions without data, so a sweep from the entry point decodes it exactly (inline
 *    string bytes belong to their STRLIT);
 *  - every function ends with exactly one RET, possibly followed by the compiler's failure stubs (`STRLIT "runtime
 *    error: ..."`, `ABORT`); the startup stubs end with HALT. The instruction after that starts the next function;
 *  - every static CALL target, every EXPT address and the init address also start a function (this matters only for
 *    code that was not produced by Ilangc);
 *  - a function is named, in order of preference, by the EXPT section, by its place in the startup stub (`main`,
 *    `_globals`), by the name inside its own failure-stub message, or after its address;
 *  - a jump target inside a function becomes a label; an unconditional jump to the start of a function is a tail call.
 *
 * @param[out] program Receives the result. Release it with dis_program_free().
 * @param[in]  image   The parsed image; it must outlive @p program.
 */
void dis_analyze(DisProgram* program, const DisImage* image);
/**
 * @brief Releases everything dis_analyze() allocated.
 * @param program The program (may be zeroed).
 */
void dis_program_free(DisProgram* program);
/**
 * @brief Finds the instruction that starts at an offset.
 * @param program The program.
 * @param addr    An offset in the image.
 * @return Its index, or -1 if no instruction starts there.
 */
int32_t dis_find_insn(const DisProgram* program, uint32_t addr);
/**
 * @brief Finds the references that point at an instruction.
 * @param[in]  program The program.
 * @param[in]  insn    Index of the instruction.
 * @param[out] count   Receives the number of references (they follow each other, sorted by source).
 * @return The first reference, or NULL if there is none.
 */
const DisXref* dis_xrefs_to(const DisProgram* program, uint32_t insn, size_t* count);
/**
 * @brief Finds a function by name or by address.
 * @param program The program.
 * @param text    A function name, or a number (decimal or `0x` hexadecimal) that is any offset inside the function.
 * @return The function index, or -1 if there is no such function.
 */
int32_t dis_find_function(const DisProgram* program, const char* text);
/**
 * @brief Gives a function a name chosen by the user.
 *
 * The new name replaces whatever Ilangdis found. It must be a plain identifier (letters, digits and underscores, not
 * starting with a digit) that no other function has.
 *
 * @param[in,out] program    The program.
 * @param[in]     which      The function: its current name, or an offset inside it (see dis_find_function()).
 * @param[in]     name       The new name.
 * @param[out]    error      Receives the reason if the function returns false.
 * @param[in]     error_size Size of @p error.
 * @return true if the function was renamed.
 */
bool dis_rename_function(DisProgram* program, const char* which, const char* name, char* error, size_t error_size);
/**
 * @brief Names the position of an instruction as `function+0xOFFSET` (just the function name at its first instruction).
 * @param[in]  program The program.
 * @param[in]  insn    Index of the instruction.
 * @param[out] buf     Output buffer.
 * @param[in]  size    Its size.
 */
void dis_describe_position(const DisProgram* program, uint32_t insn, char* buf, size_t size);

/* ------------------------------------------------------------------------------------------------------------------ */
/* Output (listing.c)                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------------ */

/** @brief Which arrows the margin of the listing shows. */
typedef enum DisArrowMode {
    DIS_ARROWS_NONE,   /**< No margin. */
    DIS_ARROWS_JUMPS,  /**< Jumps (single lines). */
    DIS_ARROWS_CALLS,  /**< Calls and tail calls (double lines). */
    DIS_ARROWS_ALL     /**< Both. */
} DisArrowMode;

/** @brief Everything the user can ask for on the command line that changes the output. */
typedef struct DisOptions {
    const char* file_name;   /**< Name of the disassembled file, shown in the header (may be NULL). */
    bool bytes;              /**< Show the raw bytes of every instruction (the byte view). */
    int bytes_per_line;      /**< Bytes per row in the byte view. */
    DisArrowMode arrows;     /**< Which arrows to draw. */
    int arrow_width;         /**< Most lanes the arrow margin may have. */
    bool labels;             /**< Name functions and jump targets (otherwise: raw addresses, no banners). */
    bool comments;           /**< Add the explanatory comments after instructions. */
    bool header;             /**< Start with the summary of the file. */
    int string_limit;        /**< Longest string literal shown, in bytes; 0 = unlimited. */
    bool color;              /**< Colour the output with ANSI escape sequences. */
    bool unicode;            /**< Use box-drawing characters (otherwise plain ASCII). */
    bool has_range;          /**< Only show instructions in [#range_from, #range_to). */
    uint32_t range_from;     /**< First offset of the range. */
    uint32_t range_to;       /**< Offset just past the range. */
    const int32_t* only;     /**< Only show these functions (indices); NULL = all. */
    size_t only_count;       /**< Number of entries in #only. */
} DisOptions;

/**
 * @brief Prints the disassembly listing.
 * @param out     Destination.
 * @param program The analysed program.
 * @param options What to show and how.
 */
void dis_print_listing(FILE* out, const DisProgram* program, const DisOptions* options);
/**
 * @brief Prints the summary of the file only: header, imports, exports and sections.
 * @param out     Destination.
 * @param program The analysed program.
 * @param options Only the colour and character set options are used.
 */
void dis_print_info(FILE* out, const DisProgram* program, const DisOptions* options);
/**
 * @brief Prints the table of functions: address, size, instruction count, number of callers, name and notes.
 * @param out     Destination.
 * @param program The analysed program.
 * @param options Only the colour option is used.
 */
void dis_print_functions(FILE* out, const DisProgram* program, const DisOptions* options);
/**
 * @brief Prints the call graph as a tree that starts at the entry point, the init stub and the exports.
 * @param out     Destination.
 * @param program The analysed program.
 * @param options Only the colour and character set options are used.
 */
void dis_print_callgraph(FILE* out, const DisProgram* program, const DisOptions* options);
