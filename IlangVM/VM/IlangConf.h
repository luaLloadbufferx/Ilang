/**
 * @file IlangConf.h
 * @brief Compile-time configuration of the Ilang virtual machine.
 *
 * Every size and limit of the VM is a plain `#define` in this file: stacks, registers, files, memory (limit, block table,
 * small-buffer pool), buffer sizes and initial capacities. Optional features are switched on or off with macros that are
 * normally given on the compiler command line (`-DILANG_VM_NO_FILE_IO`, ...); they are described on the
 * @ref build_switches page. Values that the compiler must agree on (`REG_COUNT`, `MAX_STACK_SIZE`, the bytecode
 * version, `ILANG_HOST_MAX_ARGS`) are marked as such. Only #ILANG_VM_MEMORY_LIMIT can also be set with `-D`; to change
 * any other value, edit it here.
 */
#pragma once

/**
 * @brief Size of the call stack, in entries.
 *
 * A call uses two (return address and saved frame base), so programs can nest `MAX_CALL_DEPTH / 2` calls.
 */
#define MAX_CALL_DEPTH 1280

/**
 * @brief Size of the data stack, in values: globals at the bottom, then locals and pushed arguments.
 *
 * The compiler's `Target::stack` (`ilang.hpp`) mirrors it.
 */
#define MAX_STACK_SIZE 320

/**
 * @brief Register count.
 *
 * The compiler (`ilangc --regs N`, default 24) must be told the same number: it places arguments and variables in
 * registers, and the VM rejects code that names a register it does not have. Allowed: 8..64.
 */
#define REG_COUNT 24

/**
 * @brief Oldest bytecode version this VM runs (equal to #SUPPORTED_VERSION: only the newest is accepted).
 *
 * There is exactly one supported version: the opcode numbers are positional, so bytecode from a different instruction
 * set would silently run the wrong instructions. The loader rejects anything else. Version 1 is the format described
 * in `IlangVM.h` (the opcode list, the header and the import table). A change to the instruction set or the header
 * layout takes the next number, on both sides, and drops support for the one before it.
 */
#define MIN_SUPPORTED_VERSION 1
/** @brief The bytecode version this VM runs; must equal `kBytecodeVersion` in the compiler's `ilang.hpp`. */
#define SUPPORTED_VERSION 1

/** @brief Longest line `READLINE` (the language's `input()`) will accept, in bytes. */
#define ILANG_VM_MAX_LINE 0x1000000u

/** @brief Initial size, in bytes, of the buffer `READLINE` reads a line into. It doubles whenever the line is longer. */
#define ILANG_VM_LINE_INITIAL_CAPACITY 128u

/**
 * @brief Initial size, in bytes, of the buffer `FREAD` and `FREADFILE` read into.
 *
 * It doubles until the data fits, never going past the amount the call asked for.
 */
#define ILANG_VM_READ_INITIAL_CAPACITY 4096u

/** @brief Number of file handles, including the three standard streams (0 = stdin, 1 = stdout, 2 = stderr). */
#define ILANG_VM_MAX_FILES 32

/** @brief Size of the buffer holding the last file error message (`FERROR`); longer messages are cut. */
#define ILANG_VM_IO_ERROR_SIZE 256

/** @brief Size of `iState::load_error` (why IlangVM_load() failed), terminator included; longer messages are cut. */
#define ILANG_VM_LOAD_ERROR_SIZE 256

/**
 * @brief Size of `iState::panic_message` (the last panic's message), terminator included; longer messages are cut.
 *
 * Panics are formatted into a buffer of this size first, so the message printed to stderr is cut at the same length.
 */
#define ILANG_VM_PANIC_MESSAGE_SIZE 512

/** @brief Size of the buffer holding the first error found while a host function runs (see `IlangHost.h`); longer messages are cut. */
#define ILANG_VM_HOST_ERROR_SIZE 256

/**
 * @page build_switches Build switches
 *
 * Macros that select optional behaviour. None of them is defined in the sources; define them on the compiler command
 * line (for example `gcc -DILANG_VM_NO_FILE_IO ...`).
 *
 * @section sw_no_file_io ILANG_VM_NO_FILE_IO
 * Build a VM whose programs cannot touch the file system. `FOPEN` then always fails and `FREADFILE` panics. The
 * standard streams (handles 0..2) keep working.
 *
 * @section sw_computed_goto ILANG_VM_COMPUTED_GOTO
 * Run programs with the computed-goto interpreter loop instead of the portable `switch` loop (gcc/clang only).
 *
 * Same behaviour. In a gcc 13 PGO build (`build.bat goto`) it was ~15% faster than the switch loop; the actual
 * difference depends on the CPU. MSVC always uses the switch loop.
 *
 * Note: on gcc and clang neither of those two loops is the default. `IlangVM_quick.inc` (decode each instruction once
 * into a specialised record, fuse compare+jump, array element access and used-once string literals) runs instead,
 * about 1.7x faster, unless `ILANG_VM_NO_QUICK`, `ILANG_VM_TRACE`, `ILANG_VM_MAX_INSTRUCTIONS` or
 * `ILANG_VM_STRICT_TYPES` is defined. `ILANG_VM_COMPUTED_GOTO` only matters in those builds. The bytecode format is
 * unchanged.
 *
 * @section sw_no_quick ILANG_VM_NO_QUICK
 * Never use the quickening interpreter loop (`IlangVM_quick.inc`), even on gcc and clang. The VM then runs the
 * computed-goto loop if `ILANG_VM_COMPUTED_GOTO` is defined and the portable `switch` loop otherwise.
 *
 * @section sw_trace ILANG_VM_TRACE
 * Print every instruction to stderr before it runs, as `[ip] OPCODE` (the ip in hex). Meant for debugging the VM or the
 * compiler. Implies the non-quickening loops, which are the ones that can trace.
 *
 * @section sw_max_instructions ILANG_VM_MAX_INSTRUCTIONS
 * Stop a program with the panic "Instruction limit exceeded!" after this many instructions. Define it as an integer
 * (for example `-DILANG_VM_MAX_INSTRUCTIONS=100000000`); useful for running untrusted or possibly endless programs.
 * Implies the non-quickening loops, which are the ones that can count.
 *
 * @section sw_strict_types ILANG_VM_STRICT_TYPES
 * Check that the tag of every register an instruction reads matches the type the instruction asks for. A mismatch
 * panics with "Register type does not match instruction type!". Without it the VM trusts the compiler. The fast paths
 * for arithmetic decline in this mode, so it is slower. Implies the non-quickening loops.
 *
 * @section sw_no_dynlib ILANG_VM_NO_DYNLIB
 * Build `main.c` without host library support (no `dlopen` / `LoadLibrary`). `--lib` then prints an error. Host
 * functions can still be defined by an embedding program through `IlangHost.h`.
 *
 * @section sw_memory_limit ILANG_VM_MEMORY_LIMIT
 * Maximum total size of all live memory blocks, in bytes (see #ILANG_VM_MEMORY_LIMIT).
 */

/**
 * @brief How deeply calls between the program and host functions may nest (Ilang -> C -> Ilang -> C ...).
 *
 * Each level uses about 2 KiB of the C stack.
 */
#define ILANG_VM_MAX_HOST_DEPTH 100

/**
 * @brief Most parameters a host function (`extern fn` / `export fn`) can have.
 *
 * The compiler enforces the same number (`kMaxExternParams` in `ilang.hpp`), so the two must agree. The argument count is
 * stored in a byte, so 255 is the ceiling, and every host call keeps arrays of this many values on the C stack.
 */
#define ILANG_HOST_MAX_ARGS 64

/** @brief Initial capacity of the memory block table, in blocks (it doubles when full). */
#define ILANG_VM_MEMORY_INITIAL_CAPACITY 16u

/**
 * @brief First virtual address the memory manager hands out.
 *
 * It is non-zero so that 0 can mean "no memory"; nothing below it can be read or written.
 */
#define ILANG_VM_MEMORY_START 0x1000u

/**
 * @def ILANG_VM_MEMORY_LIMIT
 * @brief Maximum total size of all live memory blocks, in bytes (default `0xFFFFFFFF`, the largest 32-bit value).
 *
 * The limit and all memory accounting are 32-bit, so the VM behaves the same on 32- and 64-bit hosts. The VM's addresses
 * are 32 bits wide and start at #ILANG_VM_MEMORY_START, so slightly less than 4 GiB (the start address and the 4-byte
 * alignment padding are lost) can be live at once, whatever the limit says.
 *
 * Unlike the other values here, this one is only a default: define it on the compiler command line
 * (`-DILANG_VM_MEMORY_LIMIT=268435456u`) to override it.
 */
#ifndef ILANG_VM_MEMORY_LIMIT
#define ILANG_VM_MEMORY_LIMIT 0xFFFFFFFFu
#endif

/**
 * @brief Number of size classes of the small-buffer pool in `IlangVM_Mem.c`; at least 1.
 *
 * Class `c` recycles buffers of exactly `(c + 1) * 16` bytes, so the pool serves blocks of up to
 * `ILANG_VM_POOL_CLASSES * 16` bytes (256 by default). Bigger blocks go straight to `calloc` / `free`.
 */
#define ILANG_VM_POOL_CLASSES 16u

/**
 * @brief Most free buffers the pool keeps in each size class; further ones go back to `free()`.
 *
 * With the defaults the pool never holds more than about 2 MiB (1024 buffers in each of the 16 classes).
 */
#define ILANG_VM_POOL_MAX_PER_CLASS 1024u