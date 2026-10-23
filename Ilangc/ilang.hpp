/**
 * @file ilang.hpp
 * @brief Definitions shared by every stage of the Ilang compiler: source locations, diagnostics, the VM's bytecode
 *        encoding and the description of the target VM.
 *
 * Nothing in this header depends on the lexer, the parser or the code generator, so all of them include it.
 * It contains four things:
 *  - source positions (il::Loc) and the table of source files they refer to;
 *  - the diagnostics machinery (il::CompileError, il::CompileErrors, il::SilentError) and the helpers that print
 *    errors with a source excerpt and "did you mean ...?" suggestions;
 *  - the bytecode constants (il::Tag, il::Mode, il::Op, the header size and version) that MUST mirror `IlangVM.h`;
 *  - il::Target, the properties of the VM the generated code is meant for.
 */
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

/**
 * @namespace il
 * @brief Everything in the Ilang compiler (`Ilangc`) lives in this namespace.
 */
namespace il {

    /**
     * @brief A position in a source file.
     *
     * Line and column are 1-based. A Loc with `line == 0` means "no location" (for example an error about the command
     * line); the printing helpers then leave out the file name and the source excerpt.
     */
    struct Loc {
        int file = 0,  ///< Index of the source file in fileNames().
            line = 0,  ///< 1-based line number; 0 means "no location".
            col = 0;   ///< 1-based column number.
        int len = 0;  ///< Length in bytes of the offending token; 0 = unknown (the error marker then underlines a single character).
    };

    /**
     * @brief The names of the source files read so far, for messages.
     *
     * The lexer appends one entry per file it reads (the main file and every `@include`d file); Loc::file indexes the
     * result.
     *
     * @return A reference to a process-wide list of file names.
     */
    inline std::vector<std::string>& fileNames() {
        static std::vector<std::string> names;
        return names;
    }

    /**
     * @brief The text of every file the lexer read (same index as fileNames()).
     *
     * Kept so that an error can show the source line it is about long after the lexer is gone.
     *
     * @return A reference to a process-wide list of file contents.
     */
    inline std::vector<std::string>& fileSources() {
        static std::vector<std::string> sources;
        return sources;
    }

    /**
     * @name Diagnostics
     * A CompileError is one problem: a message with a location, any number of notes (each pointing at another place,
     * e.g. "'x' is declared here"), and an optional one-line help (a suggested fix). The compiler collects several of
     * them (CompileErrors) instead of stopping at the first.
     * @{
     */

    /**
     * @brief A secondary message attached to a CompileError, usually pointing at another place in the source.
     */
    struct Note {
        Loc loc;  ///< Where the note points. `loc.line == 0` gives a plain "note: ..." line without a source excerpt.
        std::string msg;  ///< The note text.
    };

    /**
     * @brief One compile-time problem: where it is, what is wrong, related places, and how to fix it.
     *
     * It is thrown as an exception (see fail()) and caught either by the parser/code generator, which record it and carry
     * on, or by `main()`, which prints it with printError().
     *
     * Both setters return `*this`, so an error can be built fluently:
     * @code
     * fail(CompileError{ loc, "undefined variable 'x'", {}, {} }.note(other, "declared here").hint("did you mean 'y'?"));
     * @endcode
     */
    struct CompileError {
        Loc loc;  ///< Where the error is reported (the line is shown with a marker under the offending token).
        std::string msg;  ///< The message, printed after `error:`.
        std::vector<Note> notes;  ///< Additional places the reader should look at, printed after the main message.
        std::string help;  ///< An optional one-line suggested fix, printed as `help: ...`; empty if there is none.

        /**
         * @brief Adds a note to this error.
         * @param l Where the note points (`Loc{}` for a note without a source excerpt).
         * @param m The note text.
         * @return `*this`, for chaining.
         */
        CompileError& note(Loc l, std::string m) { notes.push_back({ l, std::move(m) }); return *this; }
        /**
         * @brief Sets the one-line suggested fix, replacing any earlier one.
         * @param h The suggestion, without the leading `help:`.
         * @return `*this`, for chaining.
         */
        CompileError& hint(std::string h) { help = std::move(h); return *this; }
    };

    /**
     * @brief Several CompileError objects collected in one pass; thrown when a stage has finished and found problems.
     */
    struct CompileErrors {
        std::vector<CompileError> list;  ///< The errors, in the order they were found (printErrors() sorts them back into source order).
        bool truncated = false;  ///< True if the stage gave up early because more than kMaxErrors problems were found.
    };

    /**
     * @brief Thrown to abandon a statement whose problem has already been reported.
     *
     * Used, for example, when a statement mentions a variable whose own declaration failed, so that one mistake does not
     * produce a pile of follow-up errors. It carries no information and is never printed.
     */
    struct SilentError {};

    /**
     * @brief The most errors reported in one run; after that the compiler stops with "stopped after 20 errors".
     */
    constexpr size_t kMaxErrors = 20;
    /** @} */

    /**
     * @brief Abandons the current construct with a compile error.
     * @param l   Where the problem is.
     * @param msg What is wrong.
     * @throws CompileError always.
     */
    [[noreturn]] inline void fail(Loc l, std::string msg) { throw CompileError{ l, std::move(msg), {}, {} }; }
    /**
     * @brief Abandons the current construct with an error that was built by hand (with notes and/or a hint).
     * @param e The complete error.
     * @throws CompileError always.
     */
    [[noreturn]] inline void fail(CompileError e) { throw std::move(e); }

    /**
     * @brief Optimal string alignment distance between two strings: Levenshtein distance plus adjacent transpositions.
     *
     * ASCII letter case is ignored, so `Count` and `count` are at distance 0. Used for "did you mean ...?" hints.
     *
     * @param a First string.
     * @param b Second string.
     * @return The minimum number of insertions, deletions, substitutions and swaps of two neighbouring characters that
     *         turn @p a into @p b.
     */
    inline int editDistance(const std::string& a, const std::string& b) {
        auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; };
        const size_t n = a.size(), m = b.size();
        std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1, 0));
        for (size_t i = 0; i <= n; ++i) d[i][0] = (int)i;
        for (size_t j = 0; j <= m; ++j) d[0][j] = (int)j;
        for (size_t i = 1; i <= n; ++i)
            for (size_t j = 1; j <= m; ++j) {
                const int cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1;
                d[i][j] = std::min({ d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost });
                if (i > 1 && j > 1 && lower(a[i - 1]) == lower(b[j - 2]) && lower(a[i - 2]) == lower(b[j - 1]))
                    d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        return d[n][m];
    }

    /**
     * @brief Finds the name that the author most plausibly meant.
     *
     * The allowed distance grows with the length of @p name. Very short names (fewer than three characters) are only matched
     * when they differ by letter case, because a one-letter variable has too many "close" neighbours. An abbreviation that
     * is a subsequence of a candidate (`cnt` -> `count`) counts as a second-best match.
     *
     * @param name       The unknown name as written.
     * @param candidates The names that are in scope. Empty entries and entries starting with `<` (compiler-internal names
     *                   such as `<globals>`) are ignored.
     * @return The closest candidate, or an empty string if nothing is close enough to be a plausible typo.
     */
    inline std::string suggestName(const std::string& name, const std::vector<std::string>& candidates) {
        std::string best, abbrev;
        const int limit = name.size() < 3 ? 0 : name.size() < 5 ? 1 : name.size() < 8 ? 2 : 3;
        int bestD = limit + 1;
        for (const std::string& c : candidates) {
            if (c == name || c.empty() || c[0] == '<') continue;
            const int dist = editDistance(name, c);
            if (dist < bestD) { bestD = dist; best = c; }
            if (name.size() >= 3 && c.size() > name.size() && c.size() <= name.size() * 2 + 1 && abbrev.empty()) {
                size_t k = 0;
                for (size_t i = 0; i < c.size() && k < name.size(); ++i) if (c[i] == name[k]) ++k;
                if (k == name.size()) abbrev = c;
            }
        }
        return best.empty() ? abbrev : best;
    }

    /**
     * @brief Fetches the text of the source line that a location points at.
     *
     * The line ending is removed and a leading UTF-8 byte order mark is skipped.
     *
     * @param[in]  l   The location; only `file` and `line` are used.
     * @param[out] out Receives the line text.
     * @return false if there is no such line (no location, unknown file, or line past the end of the file).
     */
    inline bool sourceLineOf(const Loc& l, std::string& out) {
        if (l.line <= 0 || l.file < 0 || l.file >= (int)fileSources().size()) return false;
        const std::string& s = fileSources()[l.file];
        size_t pos = 0;
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) pos = 3;
        for (int line = 1; line < l.line; ++line) {
            pos = s.find('\n', pos);
            if (pos == std::string::npos) return false;
            ++pos;
        }
        size_t end = s.find('\n', pos);
        if (end == std::string::npos) end = s.size();
        out = s.substr(pos, end - pos);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }

    /**
     * @brief Prints `file:line:col: kind: message`, or just `kind: message` when there is no location.
     * @param f    Output stream.
     * @param l    Location to print.
     * @param kind The severity word, e.g. `"error"` or `"note"`.
     * @param msg  The message.
     */
    inline void printLocHeader(std::FILE* f, const Loc& l, const char* kind, const std::string& msg) {
        const auto& names = fileNames();
        if (l.line > 0 && l.file >= 0 && l.file < (int)names.size())
            std::fprintf(f, "%s:%d:%d: %s: %s\n", names[l.file].c_str(), l.line, l.col, kind, msg.c_str());
        else
            std::fprintf(f, "%s: %s\n", kind, msg.c_str());
    }

    /**
     * @brief Prints the source line followed by a `^~~~` marker under the offending token.
     *
     * Tabs are shown as four spaces and UTF-8 continuation bytes take no room, so the marker lines up in an ordinary
     * terminal. Prints nothing if the line cannot be found.
     *
     * @param f Output stream.
     * @param l Location of the token; `l.len` gives the length of the underline.
     */
    inline void printSnippet(std::FILE* f, const Loc& l) {
        std::string text;
        if (!sourceLineOf(l, text)) return;
        auto width = [&](size_t from, size_t to) {
            size_t w = 0;
            for (size_t i = from; i < to && i < text.size(); ++i) {
                const unsigned char c = (unsigned char)text[i];
                if (c == '\t') w += 4;
                else if ((c & 0xC0) != 0x80) w += 1;
            }
            return w;
        };
        std::string shown;
        for (char c : text) { if (c == '\t') shown += "    "; else shown += c; }

        const size_t col0 = l.col > 0 ? (size_t)(l.col - 1) : 0;
        size_t pad = width(0, col0);
        if (col0 > text.size()) pad += col0 - text.size();
        size_t span = l.len > 1 ? width(col0, col0 + (size_t)l.len) : 1;
        if (span == 0) span = 1;

        const int digits = (int)std::to_string(l.line).size();
        const int w = std::max(4, digits);
        std::fprintf(f, "%*d | %s\n", w, l.line, shown.c_str());
        std::fprintf(f, "%*s | %s^%s\n", w, "", std::string(pad, ' ').c_str(), std::string(span - 1, '~').c_str());
    }

    /**
     * @brief Prints one error: its header and source excerpt, then each note, then the help line.
     * @param f Output stream.
     * @param e The error to print.
     */
    inline void printError(std::FILE* f, const CompileError& e) {
        printLocHeader(f, e.loc, "error", e.msg);
        printSnippet(f, e.loc);
        for (const Note& n : e.notes) {
            printLocHeader(f, n.loc, "note", n.msg);
            printSnippet(f, n.loc);
        }
        if (!e.help.empty()) std::fprintf(f, "  help: %s\n", e.help.c_str());
    }

    /**
     * @brief Prints all collected errors in source order, followed by how many there were.
     *
     * The final line is `N error(s) found`, or `stopped after N errors` if the list was truncated.
     *
     * @param f  Output stream.
     * @param es The errors to print.
     */
    inline void printErrors(std::FILE* f, const CompileErrors& es) {
        // functions are compiled callee-first, so sort what was found back into source order
        std::vector<const CompileError*> order;
        for (const CompileError& e : es.list) order.push_back(&e);
        std::stable_sort(order.begin(), order.end(), [](const CompileError* a, const CompileError* b) {
            if (a->loc.file != b->loc.file) return a->loc.file < b->loc.file;
            if (a->loc.line != b->loc.line) return a->loc.line < b->loc.line;
            return a->loc.col < b->loc.col;
        });
        for (const CompileError* e : order) {
            printError(f, *e);
            std::fputc('\n', f);
        }
        const size_t n = es.list.size();
        if (es.truncated) std::fprintf(f, "stopped after %zu errors\n", n);
        else std::fprintf(f, "%zu error%s found\n", n, n == 1 ? "" : "s");
    }

    // ---------------------------------------------------------------------------
    // Bytecode constants. These MUST mirror IlangVM.h (OpCode, the value tags and OpType).
    // Opcode numbers are positional (MOV = 1, then +1 each). Adding, removing or reordering an opcode renumbers the ones
    // after it, so it has to be done on both sides and be accompanied by a new kBytecodeVersion here and
    // SUPPORTED_VERSION in IlangConf.h.
    //
    // Arithmetic, logic and shift instructions are [OP][MODE][TYPE][TO][A][B]; CMP has the same layout without TO.
    // MODE says whether A and B are register indices or immediates. An immediate is 1 byte for bool and byte and 4 bytes
    // (little-endian) for everything else. The other instructions list their operands next to them below.
    // ---------------------------------------------------------------------------
    // The type tag of a value: carried by registers and named by instructions. T_STRINGADDRESS is the address of a string.
    /**
     * @brief The type tag of a value: carried by registers and named by instructions.
     *
     * `T_STRINGADDRESS` is the address of a string. The numbers are part of the bytecode format and must match the
     * `OpType` / value tags in `IlangVM.h`.
     */
    enum Tag : uint8_t {
        T_NONE = 0,           ///< No type: a `void` result or a register that holds nothing.
        T_INT = 1,            ///< 32-bit signed integer.
        T_UINT = 2,           ///< 32-bit unsigned integer.
        T_FLOAT = 3,          ///< 32-bit IEEE float.
        T_BOOL = 4,           ///< Boolean, one byte in memory.
        T_ADDRESS = 5,        ///< A raw VM address (a heap block, an array, a struct).
        T_STRINGADDRESS = 6,  ///< The address of a string block (an `[u32 length][bytes]` heap block).
        T_BYTE = 7            ///< Unsigned 8-bit integer.
    };

    /**
     * @brief Operand modes of a binary instruction: each of A and B is a register (REG) or an immediate (IMM).
     *
     * An immediate is 1 byte for `bool` and `byte` and 4 bytes (little-endian) for everything else.
     */
    enum Mode : uint8_t { REG_REG = 0, REG_IMM = 1, IMM_REG = 2, IMM_IMM = 3 };

    /**
     * @brief Opcodes of the VM, in bytecode order.
     *
     * The numbers are positional (`MOV = 1`, then +1 each) and are what the bytecode contains, so this list must mirror
     * `enum OpCode` in `IlangVM.h`. Adding or reordering opcodes changes the numbering: bump `kBytecodeVersion` and
     * `SUPPORTED_VERSION` (`IlangConf.h`) with it. The VM header documents the semantics of every instruction, the layout
     * of string blocks and arrays, the file I/O rules and the host function call sequence.
     *
     * In the encodings, MODE says whether an operand is a register or an immediate, and TYPE is the operand type tag.
     */
    enum Op : uint8_t {
        MOV = 1,  ///< Copies a register or an immediate into a register. Encoding: `[MOV][MODE][IMM_TYPE][0 (TO)][1 (register or immediate)]`
        ADD,  ///< `TO = A + B` (integers wrap). Encoding: `[ADD][MODE][TYPE][0 (TO)][1][2]`
        SUB,  ///< `TO = A - B` (integers wrap). Encoding: `[SUB][MODE][TYPE][0 (TO)][1][2]`
        MUL,  ///< `TO = A * B` (integers wrap). Encoding: `[MUL][MODE][TYPE][0 (TO)][1][2]`
        DIV,  ///< `TO = A / B`; division by zero panics. Encoding: `[DIV][MODE][TYPE][0 (TO)][1][2]`
        MOD,  ///< `TO = A % B`; modulo by zero panics, and modulo is not defined for floats. Encoding: `[MOD][MODE][TYPE][0 (TO)][1][2]`
        RESERVEMEM,  ///< `TO` = address of a new zeroed memory block of SIZE bytes. Encoding: `[RESERVEMEM][TO_REG][SIZE:uint32]`
        FREEMEM,  ///< Frees the memory block that starts at the address in a register. Encoding: `[FREEMEM][ADDRESS_REG]`
        LOAD,  ///< Loads one element of TYPE from VM memory into a register. Encoding: `[LOAD][TYPE][ADDRESS_REG][TO_REG]`
        STORE,  ///< Stores a register into VM memory as TYPE (the register must hold that type). Encoding: `[STORE][TYPE][ADDRESS_REG][FROM_REG]`
        LOADLOCAL,  ///< `TO` = the data stack slot whose absolute index is in a register. Encoding: `[LOADLOCAL][STACK OFFSET HOLDING REGISTER][0 (TO)]`
        STORELOCAL,  ///< Stores a register or an immediate into the data stack slot whose absolute index is in a register. Encoding: `[STORELOCAL][STACK OFFSET HOLDING REGISTER][IMMTYPE][REGISTER (bool)][0 (from) OR value]`
        LEA,  ///< `TO` = the absolute stack index of a local or a parameter of the current frame (for LOADLOCAL / STORELOCAL). Encoding: `[LEA][TO_REG][MODE (0 = local, 1 = parameter)][OFFSET]`
        ENTER,  ///< Reserves AMOUNT zeroed local slots at the top of the data stack. Encoding: `[ENTER][AMOUNT OF LOCALS]`
        LEAVE,  ///< Releases the top AMOUNT slots of the data stack. Encoding: `[LEAVE][AMOUNT OF LOCALS]`
        CMP,  ///< Compares A and B and sets the zero, sign, carry and overflow flags that the conditional jumps read. Encoding: `[CMP][MODE][TYPE][0][1]`
        JMP,  ///< Jumps unconditionally. Encoding: `[JMP][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
        JE,  ///< Jumps if the last CMP found its operands equal. Encoding: `[JE][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
        JNE,  ///< Jumps if the last CMP found its operands different. Encoding: `[JNE][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
        JGT,  ///< Jumps if the last CMP found A greater than B. Encoding: `[JGT][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
        JLT,  ///< Jumps if the last CMP found A less than B. Encoding: `[JLT][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
        JGE,  ///< Jumps if the last CMP found A greater than or equal to B. Encoding: `[JGE][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
        JLE,  ///< Jumps if the last CMP found A less than or equal to B. Encoding: `[JLE][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t) OR register]`
        AND,  ///< `TO = A & B` (int, uint or byte). Encoding: `[AND][MODE][TYPE][0 (TO)][1][2]`
        OR,  ///< `TO = A | B` (int, uint or byte). Encoding: `[OR][MODE][TYPE][0 (TO)][1][2]`
        XOR,  ///< `TO = A ^ B` (int, uint or byte). Encoding: `[XOR][MODE][TYPE][0 (TO)][1][2]`
        CALL,  ///< Calls a function: pushes the return address and the caller's frame base on the call stack and starts a new frame. Encoding: `[CALL][RELATIVE (bool)][REGISTER (bool)][ADDRESS (uint32_t)]`
        RET,  ///< Returns from the current function: restores the caller's frame base and resumes after the CALL. Encoding: `[RET]`
        PUSH,  ///< Pushes a register on the data stack. Encoding: `[PUSH][0 (TO PUSH)]`
        POP,  ///< Pops the top of the data stack into a register. Encoding: `[POP][0 (POPPED FROM STACK)]`
        PRINT,  ///< Writes the value in a register to stdout. Encoding: `[PRINT][VALUE HOLDING REGISTER]`
        CAST,  ///< Converts the value in a register to another type, in place. Encoding: `[CAST][0 (REG)][TO_TYPE]`
        REINTERPRET,  ///< Changes a register's type tag and leaves its bits alone. Encoding: `[REINTERPRET][0 (REG)][TO_TYPE]`
        HALT,  ///< Stops the program normally. Encoding: `[HALT]`
        STRLIT,  ///< `TO` = a new string block made from an inline literal. Encoding: `[STRLIT][TO_REG][LENGTH:uint32][BYTES]`
        STRPRINT,  ///< Writes the bytes of a string block to stdout. Encoding: `[STRPRINT][STR_REG]`
        STRCAT,  ///< `TO` = a new string, A followed by B. Encoding: `[STRCAT][TO_REG][A_REG][B_REG]`
        STRCMP,  ///< `TO` = the int -1, 0 or 1 (bytewise, then by length). Encoding: `[STRCMP][TO_REG][A_REG][B_REG]`
        STRSLICE,  ///< `TO` = a new string holding a range of another; panics if the range is outside the string. Encoding: `[STRSLICE][TO_REG][STR_REG][START_REG][COUNT_REG]`
        STRFIND,  ///< `TO` = the int index of the first match of NEEDLE in HAYSTACK, or -1. Encoding: `[STRFIND][TO_REG][HAYSTACK_REG][NEEDLE_REG]`
        STRCHR,  ///< `TO` = a new 1-byte string. Encoding: `[STRCHR][TO_REG][BYTE_REG]`
        STRFROM,  ///< `TO` = a new string with a value formatted the way PRINT does. Encoding: `[STRFROM][TO_REG][VALUE_REG]`
        STRPARSE,  ///< Parses a string as an int, uint or float. Encoding: `[STRPARSE][TO_REG][OK_REG][STR_REG][TYPE]`
        READLINE,  ///< Flushes stdout, then reads one line from stdin into a new string. Encoding: `[READLINE][TO_REG]`
        ATEOF,  ///< `TO` = true if stdin has no more data. Encoding: `[ATEOF][TO_REG]`
        MEMSET,  ///< Stores a value into COUNT consecutive elements of VM memory. Encoding: `[MEMSET][TYPE][ADDRESS_REG][VALUE_REG][COUNT_REG]`
        NEWARRAY,  ///< `TO` = a new zeroed array block of COUNT elements. Encoding: `[NEWARRAY][TO_REG][COUNT_REG][ELEM_SIZE:uint8]`
        ARRFREE,  ///< Frees an array block and, when DEPTH > 1, the rows it owns. Encoding: `[ARRFREE][ADDRESS_REG][DEPTH:uint8]`
        ARRCLONE,  ///< `TO` = a deep copy of an array block. Encoding: `[ARRCLONE][TO_REG][SRC_REG][DEPTH:uint8]`
        ARRREP,  ///< `TO` = a new array of COUNT independent deep copies of an array (an array of arrays). Encoding: `[ARRREP][TO_REG][SRC_REG][COUNT_REG][DEPTH:uint8]`
        ABORT,  ///< Panics with the bytes of a string block as the message. Encoding: `[ABORT][STR_REG]`
        FOPEN,  ///< Opens a file; `TO` = an int handle, or -1 on failure. Encoding: `[FOPEN][TO_REG][PATH_REG][MODE_REG]`
        FCLOSE,  ///< Flushes and closes a file; `TO` = a bool. Encoding: `[FCLOSE][TO_REG][HANDLE_REG]`
        FREAD,  ///< `TO` = a new string of up to COUNT bytes read from a file. Encoding: `[FREAD][TO_REG][HANDLE_REG][COUNT_REG]`
        FREADLINE,  ///< `TO` = a new string holding one line read from a file, without its line ending. Encoding: `[FREADLINE][TO_REG][HANDLE_REG]`
        FWRITE,  ///< Writes the bytes of a string block to a file; `TO` = a bool. Encoding: `[FWRITE][TO_REG][HANDLE_REG][STR_REG]`
        FEOF,  ///< `TO` = true if the file has no more data. Encoding: `[FEOF][TO_REG][HANDLE_REG]`
        FERROR,  ///< `TO` = a new string describing the last file failure. Encoding: `[FERROR][TO_REG]`
        FREADFILE,  ///< `TO` = a new string holding a whole file; panics on failure. Encoding: `[FREADFILE][TO_REG][PATH_REG]`
        SHL,  ///< `TO = A << COUNT`. Encoding: `[SHL][MODE][TYPE][0 (TO)][1][2]`
        SHR,  ///< `TO = A >> COUNT` (arithmetic for int, logical for uint and byte). Encoding: `[SHR][MODE][TYPE][0 (TO)][1][2]`
        CALLC  ///< Calls a C host function. Encoding: `[CALLC][IMPORT:uint16][NREG:uint8]`
    };

    // The fixed header is char[4] "iVM\0", u32 version and u32 entry point. It is followed by the import
    // table (the C functions the program calls, see Compiler::compile and IlangVM.h), then the code.
    /**
     * @name Executable format
     * The fixed header is `char[4] "iVM\0"`, `u32` version and `u32` entry point. It is followed by the
     * import table (the C functions the program calls, see Compiler::compile and `IlangVM.h`), then the code.
     * @{
     */

    /** @brief Size in bytes of the fixed executable header: magic (4), version (4), entry point (4). */
    constexpr uint32_t kHeaderSize = 12;
    /** @brief Most parameters an `extern fn` or `export fn` may have (`ILANG_HOST_MAX_ARGS` in the VM). */
    constexpr size_t kMaxExternParams = 64;  // ILANG_HOST_MAX_ARGS in the VM
    /** @brief Most different `extern fn`s a program may call: the import index of `CALLC` is a `u16`. */
    constexpr size_t kMaxImports = 65535;    // CALLC's import index is a u16

    /**
     * @brief The one bytecode version the VM runs (`MIN_SUPPORTED_VERSION == SUPPORTED_VERSION` in `IlangConf.h`).
     *
     * Opcode numbers are positional, so any change to the opcode list must bump this on both sides.
     */
    constexpr uint32_t kBytecodeVersion = 1;
    /** @} */

    /**
     * @brief The properties of the target VM that the generated code depends on; most are set from the command line.
     *
     * These correspond to settings in `IlangConf.h` on the VM side and must agree with the VM that will run the program.
     */
    struct Target {
        int regs = 24;           ///< Number of VM registers (`REG_COUNT` in `IlangConf.h`); 8..64.
        int temps = 8;           ///< Registers kept free for expression temporaries (`main()` defaults this to `max(4, regs / 3)`). The remaining `regs - temps` registers hold variables and parameters.
        int stack = 320;         ///< Data stack slots (`MAX_STACK_SIZE` in `IlangConf.h`); the globals live in the bottom half of it.
        bool zeroInit = false;   ///< Explicitly zero struct fields in constructors (redundant: `memory_reserve()` already returns zeroed memory).
        bool strictTags = false; ///< Retag int array indices as uint (MOV + REINTERPRET), so a VM built with
                                 ///< `ILANG_VM_STRICT_TYPES` still accepts them (costs two instructions per checked access).
    };

}  // namespace il
