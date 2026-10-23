/**
 * @file codegen.hpp
 * @brief Semantic checking and bytecode generation for Ilang, done together in a single walk over the AST.
 *
 * Two classes do the work:
 *  - il::Compiler holds what is global to a program (structs, functions, globals, the error list), registers the
 *    declarations, decides the order in which functions are compiled and assembles the final executable image;
 *  - il::FuncGen compiles one function: it allocates registers, type-checks the body, emits its bytecode and records
 *    which other functions it calls.
 *
 * The data types in between (il::Type, il::StructInfo, il::FuncInfo, il::GlobalInfo, il::Val) describe what the
 * compiler knows about the program and about the value of the expression being compiled.
 *
 * The executable format and the opcodes are described in `ilang.hpp` and, from the VM's side, in `IlangVM.h`.
 *
 * @section codegen_abi ABI and code generation notes
 *
 * ABI (all of it lives in this file):
 *   * the first V args (V = regs - temps) are passed in r0..r(V-1), any further args are PUSHed
 *     by the caller (last arg on top) and read by the callee at bp-(n+1) via LEA mode 1; the
 *     caller drops them again with LEAVE. The result comes back in r0
 *   * every register is caller-saved: a call site PUSHes the live registers the
 *     callee may clobber (known exactly, since callees are compiled first) and
 *     POPs them afterwards
 *   * scalar variables live in registers; if a function runs out, the extra
 *     ones are spilled to frame slots (ENTER / LOADLOCAL / STORELOCAL)
 *   * jumps and calls are RELATIVE, so function bodies are position independent
 *   * structs/strings/arrays are heap blocks; the declaring variable owns them and
 *     FREEMEM is emitted on every scope exit; returning a struct/array/string transfers it.
 *     A temporary passed straight to a call (f("a" + b)) is kept on the stack across the call
 *     and freed afterwards
 *   * an array block is [u32 length][elements...] (bool/byte elements take 1 byte, the rest 4).
 *     A constant size ('[0; 16]') is reserved with RESERVEMEM; any other size ('[0; n]', n computed at run time) uses
 *     NEWARRAY, which takes the count from a register and panics on a negative one. Every index is
 *     bounds-checked against the stored length; a failure jumps to a per-function stub that
 *     prints a message and then makes the VM panic
 *   * an array of arrays ('int[][]') is an array whose elements are the 4-byte addresses of its rows, and it OWNS
 *     them: freeing it frees every row first (ARRFREE with the nesting depth). Rows are independent blocks, so they
 *     may have different lengths. '[row; n]' makes n deep copies of row (ARRREP); an array literal of arrays moves
 *     its fresh rows in; a row that already has an owner must be copied with copy(...) first. 'g[i]' reads a row as a
 *     borrowed alias (like a struct's string field): it is valid until the row is replaced or 'g' is freed.
 *     'g[i] = row' frees the row it replaces. copy(x) deep-copies an array (or a string) with ARRCLONE
 *   * 'unsafe { ... }' is a lexically scoped block inside which array/string indexing is NOT bounds-checked: no
 *     length load, no compare, no branch (and, for int indices, no retagging). It is meant for hot loops whose
 *     indices are known to be valid. An out-of-range index is the program's bug: it reads/writes whatever sits
 *     at that address. The VM still refuses addresses that are not inside a live heap block (it panics), so the
 *     host stays safe, but a neighbouring block can be corrupted silently. Functions called from the block are
 *     compiled as usual (their own indexing stays checked)
 *   * '[value; n]' (a non-zero value) is filled with a single MEMSET instruction; a zero value needs no fill at all,
 *     because the VM hands out zeroed memory
 *   * a string IS a byte array: same [u32 length][bytes] block (no terminator), so it indexes,
 *     has len(), can be passed/returned like any array and converts to/from byte[] for free.
 *     The string-only operations (+, comparison, substr, find, str, input, ...) are single
 *     instructions (STRCAT, STRCMP, ... see IlangVM.h)
 *   * files are int handles: fopen(path, "r" | "w" | "a") gives one (-1 if the file cannot be opened; file_error() says
 *     why), then fread / freadline / fwrite / feof / fclose work on it. Handles 0, 1 and 2 are stdin, stdout and
 *     stderr (fwrite(2, "oops\n")). read_file(path) returns a whole file as a string and, unlike fopen, panics if it
 *     cannot be read. A file the program forgets to close is flushed and closed when it ends
 *   * 'void' functions return nothing (r0 is not written, the call is not a value)
 *   * tail calls are jumps, so tail recursion runs in constant stack space. 'return f(...);' and, in a '-> void'
 *     function, a call that is the last thing the function does ('f(...);' at the end, before a bare 'return;', or at the
 *     end of an if / else / block that is itself in that position) release everything the frame owns, move the
 *     arguments into place and jump instead of CALL + RET. A call to the function itself overwrites its own
 *     parameters and jumps to the start of the body, past the ENTER; a call to another function (mutual recursion)
 *     does LEAVE and jumps to its entry, and its RET returns straight to our caller. The call stays an ordinary
 *     one when the callee is an 'extern fn' or returns another type, when a different function would need stack
 *     arguments (those belong to our caller's frame), or when a heap argument would not survive the release of the
 *     frame (a fresh temporary, or anything not reached from a parameter or a global); see FuncGen::canTailCall()
 *   * 'set const x = ...' declares an immutable variable (local or global): it cannot be reassigned, and a const
 *     struct / array / string cannot be modified through its own name either (field or element assignment).
 *     Const is a property of the name: a const array passed to a function is received by a plain parameter
 *   * a struct may have 'string' fields. The struct OWNS them: the constructor fills each with an empty string
 *     (so a string field is never null), assigning a field releases the old string, and freeing the struct
 *     releases every string it holds first. A field has no 'string' memory type in the VM, so it is stored as an
 *     address and retagged on load; reading 'p.name' gives a borrowed alias (not fresh, so it cannot be returned
 *     directly), assigning a non-fresh string to a field copies it
 *   * globals (top-level 'set') live in the bottom slots of the VM data stack: the startup stub reserves
 *     them with ENTER, then calls a synthetic function that runs the initializers in source order, then
 *     calls main. Any function reaches a global by its ABSOLUTE slot index (MOV + LOADLOCAL / STORELOCAL),
 *     so no frame is needed. Globals of heap type (string/array/struct) are created once and live until
 *     the program ends: they can be read, indexed and modified in place, but not reassigned or returned
 *   * loops keep a stack of (break label, continue label, first variable of the body): break/continue free
 *     the heap values owned by the scopes they leave, exactly like 'return' does, then jump
 *   * expression temporaries live in registers; when an expression needs more than are free, a temporary that is only
 *     waiting for the rest of the expression (the left operand while the right one is computed, the arguments already
 *     evaluated, ...) is SPILLED to a frame slot and reloaded when it is needed (see "spilling" below). So an
 *     expression does not fail for being too big; it just gets a few more instructions (only a register file too small
 *     for a single step, e.g. --temps 3 with a call of three heap-temporary arguments, still reports an error)
 *   * 'extern fn f(...) -> T;' declares a C function of the host. A call places its arguments exactly like a CALL and
 *     then emits CALLC with an index into the import table (filled in by Compiler::compile, which lists only the
 *     extern functions reachable from main); the VM binds each entry to a host function with the same signature when
 *     it loads the program. A C function writes only r0, so its FuncInfo::mask is r0
 *   * 'export fn f(...) { ... }' is an ordinary function that C may also call: it is a root of the call graph (always
 *     emitted), and the EXPT extension section (between the import table and the entry point) lists it with its address
 *     and signature, plus an init stub (the startup stub without CALL main) that IlangVM_start runs. Without exports
 *     there is no EXPT section; with exports, main may be left out
 *   * 'c ? a : b' evaluates only the branch it takes. 'x += v' / 'x++' are statements, rewritten by the parser to
 *     'x = x + v'; for an element ('a[i] += v') the element's address is computed once
 */
#pragma once
#include "parser.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace il {

    // ------------------------------------------------------------------ types
    /**
     * @brief Kinds of types.
     *
     * The scalars map to one VM tag each; String, Struct and Array are heap blocks (see Type::heap()).
     */
    enum class K : uint8_t {
        None,     ///< No type: the result of a `void` function, or an expression that has no value.
        Int,      ///< 32-bit signed integer.
        UInt,     ///< 32-bit unsigned integer.
        Float,    ///< 32-bit float.
        Bool,     ///< Boolean (one byte in memory).
        Byte,     ///< Unsigned 8-bit integer.
        Address,  ///< A raw VM address.
        String,   ///< A string: a heap block `[u32 length][bytes]`.
        Struct,   ///< A user struct: a heap block holding the fields.
        Array     ///< An array: a heap block `[u32 length][elements]`; see Type::depth for arrays of arrays.
    };

    /**
     * @brief A resolved type.
     *
     * `sid` indexes Compiler::structs (K::Struct); `ek` and `depth` describe an array (K::Array). Two Type values are
     * equal if all four fields are equal.
     */
    struct Type {
        K k = K::None;  ///< The kind of type.
        int sid = -1;  ///< K::Struct only: index of the struct in Compiler::structs.
        K ek = K::None;  ///< K::Array only: the innermost element kind; always a scalar.
        int depth = 0;   ///< K::Array only: number of array levels; `int[]` has 1, `int[][]` has 2.
        /** @brief Two types are the same type if kind, struct, element kind and depth all match. */
        bool operator==(const Type& o) const { return k == o.k && sid == o.sid && ek == o.ek && depth == o.depth; }
        /** @brief Negation of operator==(). */
        bool operator!=(const Type& o) const { return !(*this == o); }
        /** @brief True for the types whose values are heap blocks that someone has to own and free: structs, strings, arrays. */
        bool heap() const { return k == K::Struct || k == K::String || k == K::Array; }

        /**
         * @brief Builds an array type.
         * @param elem   The innermost element kind (a scalar).
         * @param levels Number of array levels (1 for `T[]`, 2 for `T[][]`).
         */
        static Type arrayOf(K elem, int levels) { Type t; t.k = K::Array; t.ek = elem; t.depth = levels; return t; }
        /**
         * @brief The type of one element of this array: `int` for `int[]`, `int[]` for `int[][]`.
         * @pre This is a K::Array type.
         */
        Type elem() const { return depth > 1 ? arrayOf(ek, depth - 1) : Type{ ek }; }
    };

    /** @brief One field of a struct, and its byte offset inside the struct's heap block. */
    struct StructField {
        std::string name;  ///< The field name.
        Type t;  ///< The field type (a scalar or a string).
        uint32_t off = 0;  ///< Byte offset of the field inside the struct block (natural alignment, declaration order).
        Loc loc;  ///< Where the field is declared.
    };

    /** @brief A resolved struct: its fields and the size of its heap block in bytes. */
    struct StructInfo {
        std::string name;  ///< The struct's name.
        std::vector<StructField> fields;  ///< The fields in declaration order, with their offsets.
        uint32_t size = 4;  ///< Size of the heap block in bytes: the fields at natural alignment, rounded up to a multiple of 4 (at least 4).
        bool hasStrings = false;  ///< Some field is a string: the constructor must fill it, freeing the struct must release it.
    };

    /**
     * @brief Everything the compiler knows about one function: its signature, the generated code and how it calls others.
     *
     * `state` and `mask` are maintained by Compiler::ensureCompiled and FuncGen::run. An `extern fn` has a FuncInfo
     * too (with `isExtern` set); it has no code.
     */
    struct FuncInfo {
        std::string name;  ///< The function's name (`<globals>` for the synthetic initializer function).
        Loc loc;  ///< Where the function's name is declared.
        const FuncDef* def = nullptr;  ///< The parsed declaration (owned by the Module).
        std::vector<Type> ptypes;  ///< Resolved parameter types.
        Type ret;  ///< Resolved return type (K::None for `void`).
        int state = 0;                                   ///< Compilation state: 0 = new, 1 = compiling, 2 = done.
        uint64_t mask = 0;                               ///< Bit set of the registers this function (or its callees) may clobber; call sites save only those that are live.
        std::vector<uint8_t> code;  ///< The generated bytecode, with relative jumps and calls; `calls` and `hostCalls` list the operands still to be patched.
        std::vector<std::pair<uint32_t, int>> calls;     ///< Pairs (offset of the `rel32` operand of a `CALL` in `code`, callee index in Compiler::funcs).
        std::vector<std::pair<uint32_t, int>> hostCalls;  ///< Pairs (offset of the `u16` import operand of a `CALLC` in `code`, index of the extern callee in Compiler::funcs).
        std::vector<int> callees;  ///< Indices of the user functions this function calls (used for reachability and the compile order).
        bool isExtern = false;                           ///< An `extern fn`: a C function of the host, called with `CALLC`; it has no code.
    };

    /** @brief A top-level `set` variable. It lives in an absolute slot of the VM data stack. */
    struct GlobalInfo {
        std::string name;  ///< The global's name.
        Type t;  ///< The global's type.
        int slot = 0;  ///< Absolute index in the VM data stack (globals occupy the bottom slots, in declaration order).
        Loc loc;  ///< Where the global is declared.
        bool isConst = false;  ///< Declared as `set const`.
        bool bad = false;  ///< Its initializer failed to compile and its type is unknown: uses are skipped silently.
    };

    /**
     * @brief Compiles a whole parsed program (a Module) into a bytecode image.
     *
     * Usage: construct with the module and the target description, then call compile(), which returns the executable
     * image or throws CompileErrors.
     * @code
     * Compiler comp(mod, target);
     * std::vector<uint8_t> image = comp.compile();
     * @endcode
     *
     * The compiler registers all declarations first (collect()), then compiles the functions callee-first (so every call
     * site knows exactly which registers the callee clobbers), and finally lays out the image: header, import table,
     * optional EXPT section, startup stub, then the reachable functions.
     */
    class Compiler {
    public:
        /**
         * @brief Creates a compiler.
         * @param m The parsed program; it must outlive the compiler (FuncInfo refers into it).
         * @param t The target VM description.
         */
        Compiler(const Module& m, const Target& t) : mod(m), tg(t) {}

        std::vector<uint8_t> compile();

        const Module& mod;  ///< The program being compiled.
        Target tg;  ///< The target VM description (register counts, stack size, code generation options).
        std::vector<StructInfo> structs;  ///< The resolved structs, in declaration order.
        std::unordered_map<std::string, int> structIdx;  ///< Struct name to index in #structs.
        std::vector<FuncInfo> funcs;  ///< Every function, in declaration order; the synthetic global initializer function comes last (see #initIdx).
        std::unordered_map<std::string, int> funcIdx;  ///< Function name to index in #funcs.
        std::vector<GlobalInfo> globals;                 ///< The globals; filled in order while the initializer function compiles.
        int initIdx = -1;                                ///< Index in #funcs of the synthetic function that initializes the globals; -1 if the program has no globals.
        std::vector<CompileError> errors;                ///< Everything reported so far (see report()).
        int spills = 0;                                  ///< Number of temporaries moved to frame slots (statistics, printed by `-v`).

        /**
         * @brief Records an error and carries on.
         *
         * Exact repeats (same place and message) are ignored.
         *
         * @param e The error.
         * @throws CompileErrors once kMaxErrors errors have been recorded: the whole compile is abandoned.
         */
        void report(CompileError e) {
            for (const CompileError& o : errors)
                if (o.loc.file == e.loc.file && o.loc.line == e.loc.line && o.loc.col == e.loc.col && o.msg == e.msg) return;
            errors.push_back(std::move(e));
            if (errors.size() >= kMaxErrors) throw CompileErrors{ std::move(errors), true };
        }

        /**
         * @brief Registers available to variables and parameters (V); the rest are expression temporaries.
         * @return `tg.regs - tg.temps`.
         */
        int varRegLimit() const { return tg.regs - tg.temps; }
        /** @brief A mask with one bit for each register of the target. */
        uint64_t fullMask() const { return tg.regs >= 64 ? ~0ull : ((1ull << tg.regs) - 1); }

        /**
         * @brief Tells whether @p n is one of the reserved built-in names (`print len int uint float bool byte`).
         *
         * These cannot be redefined as a function or a struct.
         */
        static bool isBuiltinName(const std::string& n) {
            return n == "print" || n == "len" || n == "int" || n == "uint" || n == "float" || n == "bool" || n == "byte";
        }

        /**
         * @brief Tells whether @p n is a library function (`input`, `str`, `substr`, `fopen`, ...).
         *
         * Unlike the names tested by isBuiltinName() these are NOT reserved: a program may define its own function or struct
         * with the same name, and that one wins.
         */
        static bool isLibBuiltin(const std::string& n) {
            return n == "input" || n == "eof" || n == "str" || n == "chr" || n == "substr" || n == "find" ||
                n == "is_int" || n == "is_uint" || n == "is_float" || n == "copy" ||
                n == "fopen" || n == "fclose" || n == "fread" || n == "freadline" || n == "fwrite" || n == "feof" ||
                n == "file_error" || n == "read_file";
        }

        /** @brief The names of the built-in types, for "did you mean" hints. */
        static const std::vector<std::string>& typeWords() {
            static const std::vector<std::string> w = { "int", "uint", "float", "bool", "byte", "address", "string" };
            return w;
        }

        /**
         * @brief Maps a type name to a Type, ignoring array dimensions.
         * @param r The type as written.
         * @return The scalar, string or struct type named by `r.name`.
         * @throws CompileError "unknown type", with a suggestion when one is close (or the usual spelling from another
         *         language, such as `double` or `char`).
         */
        Type resolveBase(const TypeRef& r) const {
            const std::string& n = r.name;
            if (n == "int") return Type{ K::Int };
            if (n == "uint") return Type{ K::UInt };
            if (n == "float") return Type{ K::Float };
            if (n == "bool") return Type{ K::Bool };
            if (n == "byte") return Type{ K::Byte };
            if (n == "address") return Type{ K::Address };
            if (n == "string") return Type{ K::String };
            auto it = structIdx.find(n);
            if (it != structIdx.end()) return Type{ K::Struct, it->second };

            // an unknown name: offer the closest real type, or the usual spelling from another language
            CompileError e{ r.loc, "unknown type '" + n + "'", {}, {} };
            std::vector<std::string> names = typeWords();
            for (const auto& st : structs) names.push_back(st.name);
            static const std::pair<const char*, const char*> aliases[] = {
                {"str", "string"}, {"char", "byte"}, {"double", "float"}, {"integer", "int"}, {"boolean", "bool"},
                {"long", "int"}, {"short", "int"}, {"i32", "int"}, {"u32", "uint"}, {"f32", "float"}, {"u8", "byte"},
                {"size_t", "uint"}, {"number", "int"}, {"text", "string"} };
            std::string guess;
            for (const auto& a : aliases) if (n == a.first) guess = a.second;
            if (guess.empty()) guess = suggestName(n, names);
            if (!guess.empty()) e.hint("did you mean '" + guess + "'?");
            else e.hint("the types are int, uint, float, bool, byte, address, string" + std::string(structs.empty() ? "" : ", and the declared structs") +
                "; append [] for an array (int[], int[][])");
            throw e;
        }

        /**
         * @brief Resolves a full type, array dimensions included.
         * @param r The type as written.
         * @return The resolved type.
         * @throws CompileError if the type is `void` (allowed only as a return type) or an array whose elements are not
         *         scalars (arrays of strings or structs are not supported).
         */
        Type resolveType(const TypeRef& r) const {
            if (r.name == "void")
                fail(CompileError{ r.loc, "'void' is only allowed as a function's return type", {}, {} }
                    .hint("a variable, parameter or field needs a real type such as int or string"));
            Type base = resolveBase(r);
            if (r.dims == 0) return base;
            switch (base.k) {  // T[] (and T[][], ...): only scalar element types
            case K::Int: case K::UInt: case K::Float: case K::Bool: case K::Byte: case K::Address: break;
            default:
                fail(CompileError{ r.loc, "arrays of '" + typeName(base) + "' are not supported", {}, {} }
                    .hint("array elements must be int, uint, float, bool, byte or address (or another array of those)"));
            }
            return Type::arrayOf(base.k, r.dims);
        }

        /** @brief The source spelling of a type, for messages: `int`, `float[][]`, a struct name; `void` for K::None. */
        std::string typeName(Type t) const {
            switch (t.k) {
            case K::None: return "void";
            case K::Int: return "int";
            case K::UInt: return "uint";
            case K::Float: return "float";
            case K::Bool: return "bool";
            case K::Byte: return "byte";
            case K::Address: return "address";
            case K::String: return "string";
            case K::Struct: return structs[t.sid].name;
            case K::Array: {
                std::string n = typeName(Type{ t.ek });
                for (int d = 0; d < t.depth; ++d) n += "[]";
                return n;
            }
            }
            return "?";
        }

        /** @brief The declaration of a function as source text, `fn name(a: int, b: float) -> int`, for notes that point at it. */
        std::string signatureOf(const FuncInfo& f) const {
            std::string sg = "fn " + f.name + "(";
            for (size_t i = 0; i < f.ptypes.size(); ++i) {
                if (i) sg += ", ";
                sg += (f.def && i < f.def->params.size() ? f.def->params[i].name : std::string("_")) + ": " + typeName(f.ptypes[i]);
            }
            sg += ")";
            if (f.ret.k != K::None) sg += " -> " + typeName(f.ret);
            return sg;
        }

        /**
         * @brief Validates an `extern fn` declaration: what a C function can take and return.
         *
         * It has no body to compile, so it is marked "done" (`state = 2`) and its register mask is r0 only: `CALLC` writes
         * nothing else (the argument registers are counted by callUser()).
         *
         * @param fd The declaration.
         * @param fi Its FuncInfo, updated.
         * @throws CompileError for structs in the signature, too many parameters, or duplicate parameter names.
         */
        void checkExtern(const FuncDef& fd, FuncInfo& fi) const {
            checkBoundary(fd, fi, "an extern");
            for (size_t i = 0; i < fd.params.size(); ++i)  // (an ordinary function's parameters are checked by FuncGen)
                for (size_t j = 0; j < i; ++j)
                    if (fd.params[j].name == fd.params[i].name)
                        fail(CompileError{ fd.params[i].loc, "duplicate parameter '" + fd.params[i].name + "'", {}, {} }
                            .note(fd.params[j].loc, "first declared here"));
            fi.isExtern = true;
            fi.state = 2;
            fi.mask = 1ull;
        }

        /**
         * @brief Checks what can cross between Ilang and C.
         *
         * The same rules apply to an `extern fn` (C called by Ilang) and an `export fn` (Ilang called by C): no structs, at
         * most kMaxExternParams parameters, and a name that fits the `u8` length of the import/export tables.
         *
         * @param fd   The declaration.
         * @param fi   Its FuncInfo (resolved types).
         * @param kind Used in messages: `"an extern"` or `"an exported"`.
         * @throws CompileError if a rule is broken.
         */
        void checkBoundary(const FuncDef& fd, const FuncInfo& fi, const char* kind) const {
            if (fd.name.size() > 255) fail(fd.loc, std::string("the name of ") + kind + " function can be at most 255 characters long");
            if (fd.params.size() > kMaxExternParams)
                fail(CompileError{ fd.loc, std::string(kind) + " function can take at most " + std::to_string(kMaxExternParams) + " parameters", {}, {} }
                    .hint("pass the values in an array instead"));
            auto check = [&](const Type& t, Loc l, const char* what) {
                if (t.k == K::Struct)
                    fail(CompileError{ l, std::string("a struct cannot be ") + what + " " + kind + " function", {}, {} }
                        .hint("pass its fields one by one, or put them in an array"));
            };
            for (size_t i = 0; i < fd.params.size(); ++i)
                check(fi.ptypes[i], fd.params[i].ty.set ? fd.params[i].ty.loc : fd.params[i].loc, "passed to");
            check(fi.ret, fd.ret.loc, "returned from");
        }

        /**
         * @brief Resolves a return type; `void` (the only place it is accepted) gives K::None, everything else goes through
         *        resolveType().
         */
        Type resolveReturn(const TypeRef& r) const {
            if (r.name == "void" && r.dims == 0) return Type{ K::None };
            return resolveType(r);
        }

        /** @brief Compiles function @p fi once; see the definition below. */
        void ensureCompiled(int fi);

    private:
        /** @brief Registers all declarations before any body is compiled; see the definition below. */
        void collect();
        /** @brief Collects the functions called inside an expression; see the definition below. */
        void scanExpr(const Expr& e, std::vector<int>& out) const;
        /** @brief Collects the functions called inside a statement list; see the definition below. */
        void scanStmts(const std::vector<StmtP>& v, std::vector<int>& out) const;
        /** @brief Collects the functions called inside the expressions of one statement; see the definition below. */
        void scanStmt(const Stmt& s, std::vector<int>& out) const;
    };

    // ------------------------------------------------------------ expression value
    /**
     * @brief The result of compiling an expression.
     *
     * A Val is either a compile-time constant (`imm`), or a value held in a register, together with the flags that say who
     * has to release it (`temp`, `fresh`).
     */
    struct Val {
        Type t;  ///< The type of the value (K::None for an expression that has no value, e.g. a `void` call).
        bool imm = false;       ///< Compile-time constant (value in `i` / `d`).
        bool temp = false;      ///< `reg` is a temporary owned by this value; it must be freed with FuncGen::freeVal() when done. False for a variable's own register.
        bool fresh = false;     ///< Heap value created by this expression: the receiver must own it or free it.
        bool untyped = false;   ///< Integer literal that has not picked a type yet; it takes the type of the other operand or its context (see FuncGen::coerce()).
        bool inv = false;       ///< Untyped literal produced by `~`: `i` holds `~literal`, and coerce() wraps it to the
                                ///< target width so that `x & ~1` / `~0` work for uint and byte too.
        int reg = -1;  ///< The register holding the value (-1 if none, e.g. for a constant).
        int64_t i = 0;  ///< The value of an integer, bool or byte constant (also the shifted/folded value of an untyped literal).
        double d = 0;  ///< The value of a float constant.
        bool keepSpilled = false;  ///< callUser(): when released, leave this value in its frame slot (it is loaded straight into its argument register).
        int spillSlot = -1;        ///< >= 0: the value is in temporary frame slot `nextSlot_ + spillSlot`, not in a register.
    };

    // ---------------------------------------------------------------- per-function
    /**
     * @brief Compiles one function: it allocates registers for the variables and expression temporaries, checks the body
     *        and emits its bytecode, and records which functions it calls.
     *
     * A FuncGen is created by Compiler::ensureCompiled() for one function, run() is called once, and the result is left in
     * FuncInfo::code. The `gen*` methods compile expressions and return a Val; the `genStmt` family compiles statements.
     * Compile errors are thrown as CompileError exceptions; genGuarded() catches them per statement, so one run reports
     * every independent mistake.
     *
     * The code is emitted with the `u8` / `u16` / `u32` helpers, instruction by instruction, so the operand order in the
     * generator is the encoding documented in `ilang.hpp` and `IlangVM.h`.
     */
    class FuncGen {
    public:
        /**
         * @brief Creates the generator for one function.
         * @param c The owning compiler (types, other functions, globals, error list).
         * @param f The function to compile; its code is written to `f.code` by run().
         */
        FuncGen(Compiler& c, FuncInfo& f) : C(c), F(f), N(c.tg.regs), V(c.varRegLimit()) {}

        /**
         * @brief Compiles the whole function into `F.code`.
         *
         * Declares the globals (the outermost scope) and the parameters, compiles the body, then emits the shared epilogue
         * (`LEAVE` for the frame slots, `RET`) and the error stubs that the body used (index out of bounds, failed string to
         * number conversion). Finally an `ENTER` prologue sized for the frame slots that were needed is put in front, the
         * call fix-ups are shifted by its length, and the set of registers the function may clobber is stored in `F.mask`.
         *
         * @throws CompileError for a problem outside any statement, e.g. a duplicate parameter name.
         */
        void run() {
            const FuncDef& d = *F.def;
            // Globals are the outermost scope: always visible, shadowed by parameters and locals of the same name.
            // (The initializer function starts with none and gains them as their `set` statements are compiled.)
            for (const GlobalInfo& g : C.globals) vars_.push_back(globalVar(g));
            pushScope();
            selfIdx_ = (int)(&F - C.funcs.data());
            if (F.ret.k == K::None) markTail(d.body, true);
            for (size_t i = 0; i < d.params.size(); ++i) {
                for (size_t j = scopes_.back(); j < vars_.size(); ++j)  // earlier parameters only, not globals
                    if (vars_[j].name == d.params[i].name)
                        fail(CompileError{ d.params[i].loc, "duplicate parameter '" + vars_[j].name + "'", {}, {} }
                            .note(vars_[j].loc, "first declared here"));
                const size_t k = d.params.size(), nreg = std::min<size_t>(k, (size_t)V);
                Var v;
                v.name = d.params[i].name;
                v.loc = d.params[i].loc;
                v.t = F.ptypes[i];
                v.isParam = true;
                if (i < nreg) {
                    v.inReg = true;
                    v.reg = allocReg(d.params[i].loc);  // lowest free => reg == i
                    ++varRegs_;
                }
                else {                                // pushed by the caller: last arg is at offset 0
                    v.inReg = false;
                    v.param = true;
                    v.slot = (int)(k - 1 - i);
                }
                vars_.push_back(v);
            }

            bind(start_);  // nothing has been emitted yet: a self tail call jumps here, past the ENTER that run() puts in front
            bool terminated = genStmts(d.body, true);
            if (!terminated) {
                cleanupAll(nullptr);
                if (F.ret.k != K::None) emitMovImm(0, zeroOf(F.ret));
            }
            bind(epilogue_);
            if (maxSlot_ > 0) { u8(LEAVE); u32((uint32_t)maxSlot_); }
            u8(RET);
            if (oobUsed_) emitFailStub(oob_, "index out of bounds");
            if (parseUsed_) emitFailStub(parse_, "cannot convert the string to a number");
            // the frame size is only known now: fill in the LEAVE of every tail call into another function
            for (uint32_t at : leavePatches_)
                for (int b = 0; b < 4; ++b) code[at + b] = (uint8_t)((uint32_t)maxSlot_ >> (8 * b));

            // assemble: [ENTER n] body
            uint32_t shift = 0;
            std::vector<uint8_t> out;
            if (maxSlot_ > 0) {
                out.push_back(ENTER);
                for (int b = 0; b < 4; ++b) out.push_back((uint8_t)((uint32_t)maxSlot_ >> (8 * b)));
                shift = 5;
            }
            out.insert(out.end(), code.begin(), code.end());
            F.code = std::move(out);
            for (auto& c : calls_) F.calls.push_back({ c.first + shift, c.second });
            for (auto& c : hostCalls_) F.hostCalls.push_back({ c.first + shift, c.second });
            F.callees = std::move(callees_);
            F.mask = touched_ | calleeUnion_ | 1ull;
        }

        /**
         * @brief The VM operand tag of a type (also used for the import table and the EXPT section).
         * @param t A type.
         * @return `T_INT`, `T_UINT`, `T_FLOAT`, `T_BOOL`, `T_BYTE`, `T_STRINGADDRESS` for a string, `T_ADDRESS` otherwise.
         */
        static uint8_t tagFor(Type t) { return tagOf(t); }

        /**
         * @brief Startup stub support: appends code that stores a typed zero into global slot @p slot.
         *
         * Without this a global read before its initializer ran (a function called from an earlier initializer) would see an
         * int-tagged zero, which e.g. a float `STORE` rejects. Uses r1 as scratch.
         *
         * @param[in,out] o    The stub being built.
         * @param[in]     t    The global's type (a heap type is stored as an address).
         * @param[in]     slot Absolute data-stack index of the global.
         */
        static void stubZeroGlobal(std::vector<uint8_t>& o, Type t, int slot) {
            auto le32 = [&](uint32_t x) { for (int b = 0; b < 4; ++b) o.push_back((uint8_t)(x >> (8 * b))); };
            const Type zt = t.heap() ? Type{ K::Address } : t;
            o.push_back(MOV); o.push_back(REG_IMM); o.push_back(T_UINT); o.push_back(1); le32((uint32_t)slot);
            o.push_back(STORELOCAL); o.push_back(1); o.push_back(tagOf(zt)); o.push_back(0);
            if (zt.k == K::Bool || zt.k == K::Byte) o.push_back(0); else le32(0);
        }

    private:
        /** @brief A variable that is in scope: a parameter, a local or a global. */
        struct Var {
            std::string name;  ///< The variable's name.
            Type t;  ///< The variable's type.
            bool inReg = true;  ///< The variable lives in the register `reg` (otherwise in a frame, stack-parameter or global slot).
            int reg = -1;  ///< The register holding the variable, if `inReg`.
            int slot = -1;         ///< Local slot (`bp + slot`) or, if `param`, stack parameter offset; for a global, the absolute data-stack index.
            bool param = false;    ///< Lives in a caller-pushed stack parameter, not a register.
            bool global = false;   ///< A global: `slot` is an ABSOLUTE data-stack index (never freed by any function).
            bool owned = false;  ///< The variable owns the heap block it holds, so scope exit must free it (only for `set` of a fresh heap value).
            bool isConst = false;  ///< Declared with `set const`.
            bool isParam = false;  ///< A parameter of the function being compiled (in a register or on the stack): what it holds belongs to the caller.
            Loc loc;               ///< Where the name was declared (for "declared here" notes).
            bool bad = false;      ///< Its declaration failed and its type is unknown; any use is dropped silently.
        };
        /**
         * @brief A jump target.
         *
         * Jumps emitted before the label is bound are remembered in `fix` and patched by bind().
         */
        struct Label {
            int pos = -1;  ///< Offset in `code` once bound, -1 before that.
            std::vector<uint32_t> fix;  ///< Offsets of the `rel32` operands of the jumps to this label that were emitted before it was bound.
        };
        /** @brief The innermost enclosing loop, for `break` and `continue`. */
        struct Loop {
            Label* brk;        ///< Where `break` goes (just after the loop).
            Label* cont;       ///< Where `continue` goes (the condition, or the step of a `for` loop).
            size_t varStart;   ///< Index in `vars_` where the loop body begins: `break` / `continue` free everything from here.
        };
        std::vector<Loop> loops_;  ///< The enclosing loops, innermost last.

        Compiler& C;  ///< The compiler that owns this generator.
        FuncInfo& F;  ///< The function being compiled.
        const int N,  ///< Number of registers in all.
                  V;  ///< Number of registers for variables and parameters (the rest are temporaries).
        std::vector<uint8_t> code;  ///< The bytecode of the body (without the `ENTER` prologue, which run() adds at the end).
        uint64_t used_ = 0,         ///< Register set (bit mask): registers in use now.
                 touched_ = 0,      ///< Register set: registers ever written in this function.
                 calleeUnion_ = 0;  ///< Register set: registers possibly clobbered by callees.
        int varRegs_ = 0,   ///< Number of variables currently in registers.
            nextSlot_ = 0,  ///< Next free frame slot.
            maxSlot_ = 0;   ///< Most frame slots ever needed (the size of `ENTER`).
        std::vector<Var> vars_;         ///< The variables in scope, globals first.
        std::vector<size_t> scopes_;    ///< For each open scope, the index in `vars_` where it begins.
        Label epilogue_;               ///< Where `return` jumps to.
        Label oob_;            ///< Index bounds failure stub (see emitFailStub()).
        bool oobUsed_ = false;  ///< Some bounds check jumps to `oob_`, so the stub must be emitted.
        int unsafe_ = 0;       ///< Greater than 0 while generating the body of an `unsafe` block (nesting depth).
        Label parse_;          ///< Failure stub for `int("abc")`: string to number conversion failed.
        bool parseUsed_ = false;  ///< Some conversion jumps to `parse_`, so the stub must be emitted.
        std::vector<std::pair<uint32_t, int>> calls_;  ///< Pairs (offset of the `rel32` operand of a `CALL`, callee index); copied to `F.calls` by run().
        std::vector<std::pair<uint32_t, int>> hostCalls_;  ///< `CALLC` sites (offset of the `u16` operand, extern callee index); the import index is filled in by Compiler::compile().
        std::vector<int> callees_;  ///< Indices of the functions called from this one; copied to `F.callees` by run().

        // ---- tail calls (see callUser(), emitTailCall() and genReturn()) ----
        Label start_;                          ///< The start of the body, after the `ENTER`: where a self tail call jumps.
        int selfIdx_ = -1;                     ///< Index of the function being compiled in Compiler::funcs.
        const Expr* tailExpr_ = nullptr;       ///< The call expression that is in tail position right now (the next callUser() may turn it into a jump).
        bool didTail_ = false;                 ///< Set by callUser() when it compiled `tailExpr_` as a jump: control never comes back.
        std::vector<uint32_t> leavePatches_;   ///< Offsets of the `LEAVE` operands of tail calls into other functions, filled in with the frame size at the end of run().
        std::unordered_set<const Stmt*> tailStmts_;  ///< Call statements of a `void` function after which the function returns (see markTail()).

        // ------------------------------------------------------------ emission
        /** @brief Emits one byte. */
        void u8(unsigned x) { code.push_back((uint8_t)x); }
        /** @brief Emits a 16-bit value, little-endian. */
        void u16(uint32_t x) { u8(x); u8(x >> 8); }
        /** @brief Emits a 32-bit value, little-endian. */
        void u32(uint32_t x) { u8(x); u8(x >> 8); u8(x >> 16); u8(x >> 24); }
        /** @brief Releases a heap temporary after the instruction that consumed it has been emitted (does nothing if @p v is not fresh). */
        void dropFresh(const Val& v) { if (v.fresh) emitFree(v.reg, v.t); }

        /**
         * @brief Emits the release of the heap block whose address is in @p reg.
         *
         * A struct first releases the strings its fields own (using at most two scratch registers, both given back). An array
         * of arrays is freed with `ARRFREE`, which frees its rows too; everything else with `FREEMEM`.
         *
         * @param reg Register holding the block's address.
         * @param t   The block's type.
         */
        void emitFree(int reg, Type t) {
            if (t.k == K::Struct && C.structs[t.sid].hasStrings) {
                const StructInfo& si = C.structs[t.sid];
                int tmp = allocReg(Loc{}), ad = -1;
                for (const StructField& f : si.fields) {
                    if (f.t.k != K::String) continue;
                    int addr = reg;
                    if (f.off) {
                        if (ad < 0) ad = allocReg(Loc{});
                        u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(ad); u8(reg);
                        u32(f.off);
                        addr = ad;
                    }
                    u8(LOAD); u8(T_ADDRESS); u8(addr); u8(tmp);
                    u8(FREEMEM); u8(tmp);
                }
                freeReg(tmp);
                if (ad >= 0) freeReg(ad);
            }
            if (t.k == K::Array && t.depth > 1) { u8(ARRFREE); u8(reg); u8(t.depth); return; }  // frees the rows too
            u8(FREEMEM); u8(reg);
        }
        /** @brief A fresh string temporary held in register @p reg (the receiver owns it). */
        static Val strVal(int reg) {
            Val v;
            v.t = Type{ K::String };
            v.reg = reg;
            v.temp = v.fresh = true;
            return v;
        }
        /** @brief True if one type is `string` and the other `byte[]`: they share one layout, so they convert implicitly (strings are mutable). */
        static bool sameLayout(Type a, Type b) {
            return (a.k == K::String && b.k == K::Array && b.depth == 1 && b.ek == K::Byte) ||
                (b.k == K::String && a.k == K::Array && a.depth == 1 && a.ek == K::Byte);
        }
        /** @brief True for arrays and strings, the types that can be indexed and have a length. */
        static bool isSeq(Type t) { return t.k == K::Array || t.k == K::String; }
        /** @brief The type of one element of an array or string (a string holds bytes). */
        static Type elemOf(Type t) { return t.k == K::String ? Type{ K::Byte } : t.elem(); }

        /** @brief The VM operand tag for a type: heap types other than strings are plain addresses. */
        static uint8_t tagOf(Type t) {
            switch (t.k) {
            case K::Int: return T_INT;
            case K::UInt: return T_UINT;
            case K::Float: return T_FLOAT;
            case K::Bool: return T_BOOL;
            case K::Byte: return T_BYTE;
            case K::String: return T_STRINGADDRESS;
            default: return T_ADDRESS;
            }
        }

        /**
         * @brief Appends an immediate in the encoding the VM expects for the value's type.
         *
         * 4 bytes little-endian for `int`, `uint`, `address` and `float`; 1 byte for `bool` and `byte`.
         */
        void writeImm(const Val& v) {
            switch (v.t.k) {
            case K::Int: u32((uint32_t)(int32_t)v.i); break;
            case K::UInt: case K::Address: u32((uint32_t)v.i); break;
            case K::Float: { float f = (float)v.d; uint32_t b; std::memcpy(&b, &f, 4); u32(b); break; }
            case K::Bool: case K::Byte: u8((uint8_t)v.i); break;
            default: fail(Loc{}, "internal error: bad immediate type");
            }
        }

        /** @brief Emits `MOV rd, imm`: loads the constant @p v into register @p rd. */
        void emitMovImm(int rd, const Val& v) {
            u8(MOV); u8(REG_IMM); u8(tagOf(v.t)); u8(rd); writeImm(v);
        }
        /** @brief Emits `MOV rd, rs`; nothing if they are the same register. */
        void emitMov(int rd, int rs) {
            if (rd == rs) return;
            u8(MOV); u8(REG_REG); u8(0); u8(rd); u8(rs);
        }

        /** @brief A compile-time constant of type @p t and value @p i. */
        static Val immOf(Type t, int64_t i) { Val v; v.t = t; v.imm = true; v.i = i; return v; }
        /** @brief The zero of type @p t (a heap type gets a null address). */
        static Val zeroOf(Type t) {
            if (t.heap()) t = Type{ K::Address };
            Val v; v.t = t; v.imm = true; return v;
        }

        /** @brief Appends an operand: the immediate's bytes, or the register index. */
        void operand(const Val& v) { if (v.imm) writeImm(v); else u8(v.reg); }
        /** @brief The MODE byte of a binary instruction for operands @p a and @p b (REG_REG, REG_IMM, IMM_REG or IMM_IMM). */
        static uint8_t modeOf(const Val& a, const Val& b) {
            return a.imm ? (b.imm ? IMM_IMM : IMM_REG) : (b.imm ? REG_IMM : REG_REG);
        }

        // ------------------------------------------------------------ labels / jumps
        /**
         * @brief Writes the relative jump distance into a previously emitted `rel32` operand.
         * @param at     Offset of the operand in `code`.
         * @param target Offset of the jump target; the distance is measured from the end of the instruction (`at + 4`).
         */
        void patch(uint32_t at, int target) {
            uint32_t rel = (uint32_t)(target - (int)(at + 4));  // relative to the end of the instruction
            for (int b = 0; b < 4; ++b) code[at + b] = (uint8_t)(rel >> (8 * b));
        }
        /**
         * @brief Emits a jump instruction to a label: `[op][mode 1][type 0][rel32]`.
         * @param o The jump opcode (`JMP`, `JE`, `JNE`, `JGT`, `JLT`, `JGE`, `JLE`).
         * @param L The target; if it is not bound yet the operand is patched when bind() is called.
         */
        void jump(Op o, Label& L) {
            u8(o); u8(1); u8(0);
            uint32_t at = (uint32_t)code.size();
            u32(0);
            if (L.pos >= 0) patch(at, L.pos); else L.fix.push_back(at);
        }
        /** @brief Binds a label to the current position and patches all jumps that were waiting for it. */
        void bind(Label& L) {
            L.pos = (int)code.size();
            for (uint32_t at : L.fix) patch(at, L.pos);
            L.fix.clear();
        }

        // ------------------------------------------------------------ registers
        /**
         * @brief Counts the free registers.
         * @param exclude Registers that are not to be counted even though nothing lives in them.
         */
        int freeCount(uint64_t exclude) const {
            int n = 0;
            for (int r = 0; r < N; ++r)
                if (!(used_ >> r & 1) && !(exclude >> r & 1)) ++n;
            return n;
        }

        /**
         * @brief Allocates the lowest free register.
         *
         * Taking the last free register would leave none for the next step, so first the oldest held temporary is moved to a
         * frame slot (see spillOne()).
         *
         * @param l       Location for the error message.
         * @param exclude Registers that must not be handed out even though nothing lives in them (the destination registers
         *                of a pending parallel move).
         * @return The register number.
         * @throws CompileError "expression too complex" if no register is left and nothing can be spilled.
         */
        int allocReg(Loc l, uint64_t exclude = 0) {
            // Taking the last free register would leave none for the next step, so first move the oldest held
            // temporary to a frame slot (needs one free register for the slot address; see spillOne).
            if (freeCount(exclude) <= 1) spillOne(exclude);
            for (int r = 0; r < N; ++r) {
                uint64_t bit = 1ull << r;
                if (!(used_ & bit) && !(exclude & bit)) {
                    used_ |= bit;
                    touched_ |= bit;
                    return r;
                }
            }
            fail(CompileError{ l, "expression too complex: it needs more registers than the VM has", {}, {} }
                .hint("split it into steps with 'set', e.g. set t = a + b; then use t (or compile with --regs / --temps for a larger register file)"));
        }
        /** @brief Returns register @p r to the pool. */
        void freeReg(int r) { used_ &= ~(1ull << r); }
        /** @brief Releases the temporary register of a value (does nothing for constants and for a variable's own register). */
        void freeVal(Val& v) {
            if (v.temp && v.reg >= 0) { freeReg(v.reg); v.temp = false; }
        }

        // ------------------------------------------------------------ spilling
        /**
         * @name Spilling
         * @brief How temporaries are moved out of registers when registers run low.
         *
         * An expression that has computed one operand and is now computing the next one is HOLDING the first: its register
         * is tied up, but nothing reads it until the next operand is done. A routine that does this says so:
         * @code
         *     Val a = gen(left);
         *     const size_t m = holdMark();
         *     hold(a);                    // a may be moved to a frame slot from here on ...
         *     Val b = gen(right);
         *     release(m);                 // ... and is back in a register (a.reg is valid again) from here on
         * @endcode
         *
         * Between hold() and release() the held Val's register must not be read: it may be gone. allocReg() spills the oldest
         * held temporary when the registers run low; release() reloads whatever was spilled. The slots are ordinary frame
         * slots (above the variables'), lent out by lowest free index, so reloading in any order works.
         *
         * Two rules keep the generated code right:
         *  - A spill is a store that must happen on EVERY path that reaches the matching reload. Code that is only sometimes
         *    executed (the right side of `&&` and `||`, the arms of `?:`) therefore may not spill anything that was held
         *    before it started: enterCond() pins those holders until exitCond().
         *  - A call sequence pushes registers and pops them again; a spill in between would store a value that is about to be
         *    overwritten. `noSpill_` is raised for the length of it.
         * @{
         */

        /** @brief A temporary that is being held while another operand is computed; it may be spilled. */
        struct Hold {
            Val* v;  ///< The held value; it lives in a caller's stack frame. `reg` is -1 while it is spilled.
            bool spilled = false;  ///< The value is currently stored in a frame slot.
            size_t slot = 0;  ///< Index into `tslots_` of the slot, while spilled.
        };
        std::vector<Hold> holds_;   ///< The held temporaries, oldest first. The pointers are to Vals that live in the callers' stack frames.
        std::vector<char> tslots_;  ///< `tslots_[k] != 0`: frame slot `nextSlot_ + k` holds a spilled temporary.
        size_t pinFloor_ = 0;       ///< `holds_[0 .. pinFloor_)` may not be spilled right now (see enterCond()).
        int noSpill_ = 0;  ///< Greater than 0 while spilling is forbidden (during a call sequence).

        /** @brief Returns a mark to pass to release() later, to end every hold made after this point. */
        size_t holdMark() const { return holds_.size(); }

        /**
         * @brief Declares that @p v is waiting for another operand and may be spilled until release() is called.
         *
         * Constants, a variable's own register, and everything while spilling is forbidden are ignored. If no register is free
         * for the slot address the value could never be spilled, so it is not recorded either.
         *
         * @param v The value to hold. It must stay at this address until release().
         */
        void hold(Val& v) {
            if (noSpill_ || v.imm || !v.temp || v.reg < 0) return;  // a variable's own register or a constant: nothing to free
            if (freeCount(0) < 1) return;                           // no register for the slot address: it could never be spilled
            holds_.push_back(Hold{ &v });
        }

        /**
         * @brief Moves the oldest spillable held temporary to a frame slot.
         *
         * Emits `LEA` for the slot address and `STORELOCAL`, frees the register, and counts the spill in `Compiler::spills`.
         *
         * @param exclude Registers that must not be used as the scratch register for the slot address.
         * @return false if there is nothing to spill (or nowhere to put the slot address).
         */
        bool spillOne(uint64_t exclude) {
            if (noSpill_) return false;
            size_t vi = holds_.size();
            for (size_t i = pinFloor_; i < holds_.size(); ++i)
                if (!holds_[i].spilled) { vi = i; break; }
            if (vi == holds_.size()) return false;
            int scratch = -1;
            for (int r = 0; r < N && scratch < 0; ++r)
                if (!(used_ >> r & 1) && !(exclude >> r & 1)) scratch = r;
            if (scratch < 0) return false;
            Hold& h = holds_[vi];
            Val& v = *h.v;
            size_t k = 0;
            while (k < tslots_.size() && tslots_[k]) ++k;
            if (k == tslots_.size()) tslots_.push_back(0);
            tslots_[k] = 1;
            const int slot = nextSlot_ + (int)k;
            maxSlot_ = std::max(maxSlot_, slot + 1);
            touched_ |= 1ull << scratch;  // callers must know that this function writes it
            emitSlotAddr(scratch, slot, false);
            u8(STORELOCAL); u8(scratch); u8(0); u8(1); u8(v.reg);
            freeReg(v.reg);
            v.reg = -1;
            v.temp = false;
            h.spilled = true;
            h.slot = k;
            ++C.spills;
            return true;
        }

        /**
         * @brief Ends every hold made since @p mark (newest first), reloading the ones that were spilled.
         *
         * A value marked `keepSpilled` stays in its slot (`spillSlot` is set); its consumer loads it from there.
         *
         * @param mark A value returned by holdMark().
         */
        void release(size_t mark) {
            while (holds_.size() > mark) {
                const Hold h = holds_.back();
                holds_.pop_back();
                if (!h.spilled) continue;
                Val& v = *h.v;
                if (v.keepSpilled) { v.spillSlot = (int)h.slot; continue; }  // its consumer loads it from the slot
                const int r = allocReg(Loc{});
                emitSlotAddr(r, nextSlot_ + (int)h.slot, false);
                u8(LOADLOCAL); u8(r); u8(r);
                tslots_[h.slot] = 0;
                v.reg = r;
                v.temp = true;
            }
        }

        /**
         * @brief Called BEFORE the first jump of a construct with conditionally executed parts.
         *
         * If registers are scarce, what is held around the construct is moved out of the way now (on the one path every
         * execution takes), because inside the conditional parts nothing held from outside may be spilled (see enterCond()).
         */
        void makeRoomForCond() {
            while (freeCount(0) < 6 && spillOne(0)) {}
        }

        /**
         * @brief Marks the start of code that runs on only some paths: whatever is held now stays where it is until exitCond().
         * @return The old pin floor; pass it to exitCond().
         */
        size_t enterCond() {
            const size_t old = pinFloor_;
            pinFloor_ = holds_.size();
            return old;
        }
        /**
         * @brief Ends the region started by enterCond().
         * @param old The value returned by enterCond().
         */
        void exitCond(size_t old) { pinFloor_ = old; }
        /** @} */

        // ------------------------------------------------------------ spill slots
        /**
         * @brief Emits `LEA rd, slot`: the address of a frame slot or stack parameter.
         *
         * `LOADLOCAL` / `STORELOCAL` take an absolute stack index; `LEA` mode 0 produces `bp + offset`, mode 1 a stack
         * parameter at `bp - (offset + 1)`.
         *
         * @param rd    Destination register.
         * @param slot  Slot number or parameter offset.
         * @param param true for a caller-pushed stack parameter, false for a local frame slot.
         */
        void emitSlotAddr(int rd, int slot, bool param) { u8(LEA); u8(rd); u8(param ? 1 : 0); u32((uint32_t)slot); }
        /** @brief Loads the value of a frame slot (or stack parameter) into register @p rd. */
        void loadSlot(int rd, int slot, bool param = false) {
            emitSlotAddr(rd, slot, param);
            u8(LOADLOCAL); u8(rd); u8(rd);
        }
        /** @brief Loads a global (absolute data-stack slot @p slot) into @p rd. The index goes into the register as a plain integer first. */
        void loadGlobal(int rd, int slot) {
            u8(MOV); u8(REG_IMM); u8(T_UINT); u8(rd); u32((uint32_t)slot);
            u8(LOADLOCAL); u8(rd); u8(rd);
        }
        /** @brief Stores @p v (constant or register) into the global at absolute data-stack slot @p slot. */
        void storeGlobal(int slot, const Val& v) {
            int t = allocReg(Loc{});
            u8(MOV); u8(REG_IMM); u8(T_UINT); u8(t); u32((uint32_t)slot);
            u8(STORELOCAL); u8(t);
            if (v.imm) { u8(tagOf(v.t)); u8(0); writeImm(v); }
            else { u8(0); u8(1); u8(v.reg); }
            freeReg(t);
        }
        /** @brief The variable-table entry for a global. It is never `owned`: no function ever frees a global. */
        static Var globalVar(const GlobalInfo& g) {
            Var v;
            v.name = g.name;
            v.t = g.t;
            v.inReg = false;
            v.global = true;
            v.slot = g.slot;
            v.isConst = g.isConst;
            v.loc = g.loc;
            v.bad = g.bad;
            return v;  // owned stays false: no function ever frees a global
        }

        /** @brief Stores @p v (constant or register) into frame slot @p slot (or stack parameter, if @p param). */
        void storeSlot(int slot, const Val& v, bool param = false) {
            int t = allocReg(Loc{});
            emitSlotAddr(t, slot, param);
            u8(STORELOCAL); u8(t);
            if (v.imm) { u8(tagOf(v.t)); u8(0); writeImm(v); }
            else { u8(0); u8(1); u8(v.reg); }
            freeReg(t);
        }

        // ------------------------------------------------------------ scopes / variables
        /** @brief Opens a scope: variables declared from now on are removed again by popScope(). */
        void pushScope() { scopes_.push_back(vars_.size()); }

        /** @brief Emits the release of the heap block a variable owns (nothing if it owns none). */
        void freeOwned(const Var& v) {
            if (!v.owned) return;
            int r = v.reg;
            bool tmp = false;
            if (!v.inReg) { r = allocReg(Loc{}); loadSlot(r, v.slot, v.param); tmp = true; }
            emitFree(r, v.t);
            if (tmp) freeReg(r);
        }
        /**
         * @brief Releases everything owned by every variable in scope, as `return` must.
         * @param except The variable whose value is being returned (not released), or null.
         */
        void cleanupAll(const Var* except) {
            for (size_t i = vars_.size(); i-- > 0;)
                if (&vars_[i] != except) freeOwned(vars_[i]);
        }
        /**
         * @brief Closes the innermost scope: releases what its variables own (if @p cleanup) and gives back their registers and slots.
         * @param cleanup false when the code before the scope end already left the scope (e.g. after a `return`).
         */
        void popScope(bool cleanup) {
            size_t st = scopes_.back();
            scopes_.pop_back();
            if (cleanup)
                for (size_t i = vars_.size(); i-- > st;) freeOwned(vars_[i]);
            for (size_t i = st; i < vars_.size(); ++i) {
                if (vars_[i].inReg) { freeReg(vars_[i].reg); --varRegs_; }
                else if (!vars_[i].param) --nextSlot_;
            }
            vars_.resize(st);
        }
        /** @brief Index in `vars_` of the innermost variable called @p n, or -1. */
        int findVar(const std::string& n) const {
            for (size_t i = vars_.size(); i-- > 0;)
                if (vars_[i].name == n) return (int)i;
            return -1;
        }

        /**
         * @brief Reports `undefined variable 'x'`, with a "did you mean" when a visible name is close, or a pointer when the
         *        name belongs to a function or struct instead.
         * @throws CompileError always.
         */
        [[noreturn]] void undefinedVar(const std::string& name, Loc l) const {
            CompileError e{ l, "undefined variable '" + name + "'", {}, {} };
            if (C.funcIdx.count(name))
                e.hint("'" + name + "' is a function; call it with " + name + "(...)");
            else if (C.structIdx.count(name))
                e.hint("'" + name + "' is a struct; create one with " + name + "()");
            else {
                std::vector<std::string> names;
                for (const Var& v : vars_) if (!v.bad) names.push_back(v.name);
                const std::string g = suggestName(name, names);
                if (!g.empty()) e.hint("did you mean '" + g + "'?");
                else e.hint("declare it first with 'set " + name + " = ...;'");
            }
            throw e;
        }

        /**
         * @brief Looks a variable up for use.
         * @param n The name.
         * @param l Where it is used.
         * @return Its index in `vars_`.
         * @throws CompileError if it is not defined.
         * @throws SilentError if the name's own declaration failed (the real problem was already reported).
         */
        int useVar(const std::string& n, Loc l) const {
            int vi = findVar(n);
            if (vi < 0) undefinedVar(n, l);
            if (vars_[vi].bad) throw SilentError{};
            return vi;
        }

        /** @brief The value of a variable: its own register, or a temporary loaded from its frame or global slot. */
        Val loadVar(const Var& v) {
            Val r;
            r.t = v.t;
            if (v.inReg) { r.reg = v.reg; }
            else if (v.global) { r.reg = allocReg(Loc{}); r.temp = true; loadGlobal(r.reg, v.slot); }
            else { r.reg = allocReg(Loc{}); r.temp = true; loadSlot(r.reg, v.slot, v.param); }
            return r;
        }

        /**
         * @brief Introduces a local variable.
         *
         * It gets a register while one is free (adopting @p v's temporary when it can), otherwise a frame slot. A fresh heap
         * value becomes owned by the variable.
         *
         * @param name    The variable name.
         * @param ty      Its type.
         * @param v       The initial value.
         * @param isConst Declared with `set const`.
         * @param loc     Where it is declared.
         */
        void declare(const std::string& name, Type ty, Val v, bool isConst = false, Loc loc = Loc{}) {
            Var var;
            var.name = name;
            var.t = ty;
            var.owned = ty.heap() && v.fresh;
            var.isConst = isConst;
            var.loc = loc;
            if (varRegs_ < V) {
                if (!v.imm && v.temp) {            // adopt the temporary: no copy needed
                    var.reg = v.reg;
                    v.temp = false;
                }
                else {
                    int r = allocReg(Loc{});
                    if (v.imm) emitMovImm(r, v); else emitMov(r, v.reg);
                    freeVal(v);
                    var.reg = r;
                }
                var.inReg = true;
                ++varRegs_;
            }
            else {
                var.inReg = false;
                var.slot = nextSlot_++;
                maxSlot_ = std::max(maxSlot_, nextSlot_);
                storeSlot(var.slot, v);
                freeVal(v);
            }
            vars_.push_back(var);
        }

        /** @brief Assigns @p v to an existing variable, wherever it lives (register, global slot or frame slot), then releases @p v's register. */
        void storeVar(const Var& var, Val v) {
            if (var.inReg) {
                if (v.imm) emitMovImm(var.reg, v); else emitMov(var.reg, v.reg);
            }
            else if (var.global) {
                storeGlobal(var.slot, v);
            }
            else {
                storeSlot(var.slot, v, var.param);
            }
            freeVal(v);
        }

        // ------------------------------------------------------------ typing helpers
        /** @brief Shorthand for Compiler::typeName(), used in messages. */
        std::string tn(Type t) const { return C.typeName(t); }

        /**
         * @brief Reports that an expression has no value (it calls a `void` function or `print`).
         * @throws CompileError always.
         */
        [[noreturn]] void noValue(Loc l) const {
            fail(CompileError{ l, "this expression has no value", {}, {} }
                .hint("it calls a 'void' function (or print), which returns nothing"));
        }

        /** @brief Extra context for a type mismatch message: what was being checked, and a declaration to point at. */
        struct Ctx {
            std::string what;  ///< What was being checked, as a phrase that follows "type mismatch", e.g. `in argument 2 of 'f'`.
            Loc noteLoc;  ///< Where the relevant declaration is (`line == 0`: no note).
            std::string note;  ///< The text of the note that points at `noteLoc`.
        };

        /**
         * @brief A suggestion for fixing a type mismatch.
         * @param from    The type found.
         * @param literal The value is an untyped integer literal.
         * @param to      The type that was expected.
         * @return For example "convert explicitly with float(...)", or "" if there is no useful suggestion.
         */
        std::string conversionHint(Type from, bool literal, Type to) const {
            auto numeric = [](K k) { return k == K::Int || k == K::UInt || k == K::Float || k == K::Byte; };
            if (literal) {
                if (to.k == K::Bool) return "bool values are written true and false";
                if (to.k == K::String) return "a string is written in double quotes, e.g. \"5\"";
                return "";
            }
            if (numeric(from.k) && numeric(to.k)) return "convert explicitly with " + tn(to) + "(...)";
            if (to.k == K::String && (numeric(from.k) || from.k == K::Bool)) return "use str(...) to turn a value into a string";
            if (from.k == K::String && numeric(to.k)) return "use " + tn(to) + "(...) to parse the string as a number";
            if (to.k == K::Bool && numeric(from.k)) return "compare explicitly (x != 0) or use bool(...)";
            if (from.k == K::Array && to.k == K::Array && from.ek == to.ek && from.depth == to.depth + 1)
                return "'" + tn(from) + "' is an array of '" + tn(to) + "' rows: pick one with g[i], or copy(g[i]) to own it";
            return "";
        }

        /**
         * @brief Reports `type mismatch: expected 'T', found 'U'`, with the context note and a conversion hint.
         * @param want    The expected type.
         * @param got     The type found.
         * @param literal The value found is an untyped integer literal.
         * @param l       Where to report.
         * @param cx      Optional context (may be null).
         * @throws CompileError always.
         */
        [[noreturn]] void mismatch(Type want, Type got, bool literal, Loc l, const Ctx* cx) const {
            CompileError e{ l, std::string("type mismatch") + (cx ? " " + cx->what : std::string()) + ": expected '" + tn(want) + "', found " +
                (literal ? std::string("an integer literal") : "'" + tn(got) + "'"), {}, {} };
            if (cx && cx->noteLoc.line > 0) e.note(cx->noteLoc, cx->note);
            const std::string h = conversionHint(got, literal, want);
            if (!h.empty()) e.hint(h);
            throw e;
        }

        /**
         * @brief Converts @p v to the wanted type if the language allows it implicitly.
         *
         * An untyped integer literal takes the type (and is range-checked: `byte` 0..255, `uint` 0..4294967295, `int`
         * -2147483648..2147483647), and `string` and `byte[]` are interchangeable. Anything else is a type mismatch.
         *
         * @param v    The value.
         * @param want The type the context expects.
         * @param l    Where to report problems.
         * @param cx   Optional context for the message.
         * @return The value, now of type @p want.
         * @throws CompileError if the value does not fit or the types differ, or if @p v has no value (K::None).
         */
        Val coerce(Val v, Type want, Loc l, const Ctx* cx = nullptr) {
            if (v.t.k == K::None) noValue(l);
            if (v.untyped) {
                v.untyped = false;
                const bool inv = v.inv;
                v.inv = false;
                const int64_t lit = inv ? ~v.i : v.i;  // the literal as written, before any '~'
                auto range = [&](const char* ty, const char* span, const char* extra) {
                    CompileError e{ l, "integer literal " + std::to_string(lit) + " does not fit in '" + ty + "'", {}, {} };
                    e.hint(std::string("'") + ty + "' holds " + span + extra);
                    throw e;
                };
                switch (want.k) {
                case K::Int:
                    if (lit < INT32_MIN || lit > INT32_MAX) range("int", "-2147483648 to 2147483647", lit > 0 ? "; use 'uint' for values up to 4294967295" : "");
                    break;
                case K::UInt:
                    if (lit < 0 || lit >(int64_t)UINT32_MAX) range("uint", "0 to 4294967295", lit < 0 ? "; a uint cannot be negative" : "");
                    break;
                case K::Byte:
                    if (lit < 0 || lit > 255) range("byte", "0 to 255", "");
                    break;
                case K::Float:
                    if (inv) fail(l, "'~' is not supported for float");
                    v.d = (double)v.i;
                    break;
                default:
                    mismatch(want, v.t, true, l, cx);
                }
                if (inv) v.i = wrap(want.k, v.i);  // ~0 is 0xFFFFFFFF as a uint, 0xFF as a byte, -1 as an int
                v.t = want;
                return v;
            }
            if (v.t != want) {
                if (sameLayout(v.t, want)) { v.t = want; return v; }
                mismatch(want, v.t, false, l, cx);
            }
            return v;
        }

        /**
         * @brief Gives two operands a common type (an untyped literal adopts the other side's) or reports that they differ.
         *
         * `byte[]` against `string` is compared as strings.
         *
         * @param[in,out] a Left operand.
         * @param[in,out] b Right operand.
         * @param[in]     l Where to report.
         * @throws CompileError "operand types differ" if the types cannot be reconciled.
         */
        void unify(Val& a, Val& b, Loc l) {
            if (a.t.k == K::None) noValue(l);
            if (b.t.k == K::None) noValue(l);
            if (a.untyped && !b.untyped) a = coerce(a, b.t, l);
            else if (b.untyped && !a.untyped) b = coerce(b, a.t, l);
            else if (!a.untyped && sameLayout(a.t, b.t)) a.t = b.t = Type{ K::String };  // byte[] vs string: compare as strings
            else if (!a.untyped && a.t != b.t) {
                CompileError e{ l, "operand types differ: '" + tn(a.t) + "' and '" + tn(b.t) + "'", {}, {} };
                auto numeric = [](K k) { return k == K::Int || k == K::UInt || k == K::Float || k == K::Byte; };
                if (numeric(a.t.k) && numeric(b.t.k))
                    e.hint("both sides must have the same type; convert one explicitly, e.g. " + tn(a.t) + "(...) or " + tn(b.t) + "(...)");
                else
                    e.hint("both sides of an operator must have the same type");
                throw e;
            }
        }

        /** @brief Makes sure @p v is in a register, loading a constant into a new temporary if necessary (an untyped literal becomes `int`). */
        Val toReg(Val v, Loc l) {
            if (!v.imm) return v;
            if (v.untyped) v = coerce(v, Type{ K::Int }, l);
            int r = allocReg(l);
            emitMovImm(r, v);
            v.imm = false;
            v.reg = r;
            v.temp = true;
            return v;
        }

        /** @brief Truncates @p r to the width of kind @p k (int: 32 bits signed, uint: 32 bits, byte: 8 bits; others unchanged). */
        static int64_t wrap(K k, int64_t r) {
            switch (k) {
            case K::Int: return (int32_t)(uint32_t)(uint64_t)r;
            case K::UInt: return (uint32_t)(uint64_t)r;
            case K::Byte: return (uint8_t)(uint64_t)r;
            default: return r;
            }
        }

        /**
         * @brief Converts a constant to another type at compile time, by the same rules as the VM's `CAST` (floats saturate,
         *        NaN is 0).
         * @param v  The constant.
         * @param to The target type (`int`, `uint`, `byte`, `bool` or `float`).
         */
        Val convertImm(const Val& v, Type to) {
            bool ff = v.t.k == K::Float;
            float f = (float)v.d;
            Val r;
            r.t = to;
            r.imm = true;
            switch (to.k) {
            case K::Int:
                if (!ff) r.i = (int32_t)(uint32_t)(uint64_t)v.i;
                else r.i = f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
                break;
            case K::UInt:
                if (!ff) r.i = (uint32_t)(uint64_t)v.i;
                else r.i = (f != f || f <= 0.0f) ? 0 : f >= 4294967296.0f ? (int64_t)UINT32_MAX : (int64_t)(uint32_t)f;
                break;
            case K::Byte:
                if (!ff) r.i = (uint8_t)(uint64_t)v.i;
                else r.i = (f != f || f <= 0.0f) ? 0 : f >= 255.0f ? 255 : (uint8_t)f;
                break;
            case K::Bool: r.i = ff ? (f != 0.0f) : (v.i != 0); break;
            case K::Float: r.d = ff ? (double)f : (double)(float)v.i; break;
            default: break;
            }
            return r;
        }

        /**
         * @brief Constant folding for `+ - * / % & | ^` (the shifts have their own, see foldShift()).
         * @param op  The operator token.
         * @param a   Left constant.
         * @param b   Right constant.
         * @param out Receives the result.
         * @param l   Unused.
         * @return false if the operation is left to run time (division or remainder by zero, or an operator that does not apply).
         */
        bool fold(T op, const Val& a, const Val& b, Val& out, Loc l) {
            out = a;
            out.imm = true;
            out.temp = out.fresh = false;
            out.inv = false;  // two untyped literals fold as plain 64-bit integers
            if (a.t.k == K::Float) {
                float x = (float)a.d, y = (float)b.d, r;
                switch (op) {
                case T::Plus: r = x + y; break;
                case T::Minus: r = x - y; break;
                case T::Star: r = x * y; break;
                case T::Slash: if (y == 0.0f) return false; r = x / y; break;
                default: return false;
                }
                out.d = r;
                return true;
            }
            uint64_t x = (uint64_t)a.i, y = (uint64_t)b.i, r;
            switch (op) {
            case T::Plus: r = x + y; break;
            case T::Minus: r = x - y; break;
            case T::Star: r = x * y; break;
            case T::Slash: if (b.i == 0) return false; r = (uint64_t)(a.i / b.i); break;
            case T::Percent: if (b.i == 0) return false; r = (uint64_t)(a.i % b.i); break;
            case T::Amp: r = x & y; break;
            case T::Pipe: r = x | y; break;
            case T::Caret: r = x ^ y; break;
            default: (void)l; return false;
            }
            out.i = a.untyped ? (int64_t)r : wrap(a.t.k, (int64_t)r);
            return true;
        }

        /** @brief Evaluates a comparison of two constants. */
        static bool cmpConst(T op, const Val& a, const Val& b) {
            if (a.t.k == K::Float) {
                switch (op) {
                case T::Eq: return a.d == b.d; case T::Ne: return a.d != b.d;
                case T::Lt: return a.d < b.d;  case T::Gt: return a.d > b.d;
                case T::Le: return a.d <= b.d; default: return a.d >= b.d;
                }
            }
            switch (op) {
            case T::Eq: return a.i == b.i; case T::Ne: return a.i != b.i;
            case T::Lt: return a.i < b.i;  case T::Gt: return a.i > b.i;
            case T::Le: return a.i <= b.i; default: return a.i >= b.i;
            }
        }

        /** @brief The source text of a binary operator token, for messages. */
        static const char* opText(T k) {
            switch (k) {
            case T::Plus: return "+"; case T::Minus: return "-"; case T::Star: return "*"; case T::Slash: return "/";
            case T::Percent: return "%"; case T::Amp: return "&"; case T::Pipe: return "|"; case T::Caret: return "^";
            case T::Shl: return "<<"; case T::Shr: return ">>";
            case T::Eq: return "=="; case T::Ne: return "!="; case T::Lt: return "<"; case T::Gt: return ">";
            case T::Le: return "<="; case T::Ge: return ">="; default: return "?";
            }
        }

        /** @brief True for `==  !=  <  >  <=  >=`. */
        static bool isCompare(T k) {
            return k == T::Eq || k == T::Ne || k == T::Lt || k == T::Gt || k == T::Le || k == T::Ge;
        }
        /** @brief The comparison that is true exactly when @p op is false. */
        static T invert(T op) {
            switch (op) {
            case T::Eq: return T::Ne; case T::Ne: return T::Eq;
            case T::Lt: return T::Ge; case T::Ge: return T::Lt;
            case T::Gt: return T::Le; default: return T::Gt;
            }
        }
        /**
         * @brief The conditional jump that is taken when comparison @p op is true (@p jumpIf) or false (`!jumpIf`).
         * @return One of `JE`, `JNE`, `JLT`, `JGT`, `JLE`, `JGE`.
         */
        static Op jccFor(T op, bool jumpIf) {
            if (!jumpIf) op = invert(op);
            switch (op) {
            case T::Eq: return JE; case T::Ne: return JNE;
            case T::Lt: return JLT; case T::Gt: return JGT;
            case T::Le: return JLE; default: return JGE;
            }
        }

        // ------------------------------------------------------------ expressions
        /**
         * @brief Compiles an expression.
         *
         * This is the dispatcher for all expression kinds. The result says where the value is (a constant, or a register)
         * and who has to free it (see Val).
         *
         * @param e    The expression.
         * @param dst  A register the result should be placed in, or -1 for "any" (the result is then a new temporary). Only
         *             honoured by the operators that can write straight into it.
         * @param want The type the context expects (used by array literals and `?:` to pick their element type).
         * @return The value of the expression; a Val of type K::None for a call of a `void` function.
         */
        Val gen(const Expr& e, int dst = -1, Type want = Type{}) {
            switch (e.k) {
            case Expr::Int: { Val v = immOf(Type{ K::Int }, e.iv); v.untyped = true; return v; }
            case Expr::Float: { Val v; v.t = Type{ K::Float }; v.imm = true; v.d = e.dv; return v; }
            case Expr::Bool: return immOf(Type{ K::Bool }, e.iv);
            case Expr::Str: return genStr(e);
            case Expr::Char: return immOf(Type{ K::Byte }, e.iv);
            case Expr::Ident: return loadVar(vars_[useVar(e.s, e.loc)]);
            case Expr::Unary: return genUnary(e, dst);
            case Expr::Binary:
                if (e.op == T::AndAnd || e.op == T::OrOr) return genBoolGeneral(e, dst);
                if (isCompare(e.op)) return genCompareValue(e, dst);
                return genArith(e, dst);
            case Expr::Call: return genCall(e);
            case Expr::Member: return genMember(e, dst);
            case Expr::Index: return genIndex(e, dst);
            case Expr::ArrayLit: return genArrayLit(e, want);
            case Expr::ArrayRep: return genArrayRep(e, want);
            case Expr::Ternary: return genTernary(e, dst, want);
            }
            fail(e.loc, "internal error: bad expression");
        }

        /** @brief Compiles a string literal: a fresh heap block every time it is evaluated (strings are mutable); emits `STRLIT`. */
        Val genStr(const Expr& e) {
            int r = allocReg(e.loc);
            u8(STRLIT); u8(r); u32((uint32_t)e.s.size());
            for (char c : e.s) u8((uint8_t)c);
            return strVal(r);
        }

        /**
         * @brief Compiles unary `-`, `!` and `~`.
         *
         * Constants are folded. Negation is `0 - x` (`SUB` with an immediate on the left); `!` goes through the condition
         * machinery (genBoolGeneral()); `~` is genBitNot().
         */
        Val genUnary(const Expr& e, int dst) {
            if (e.op == T::Bang) {
                // constant fold, otherwise go through the condition machinery
                if (e.a->k == Expr::Bool) return immOf(Type{ K::Bool }, !e.a->iv);
                return genBoolGeneral(e, dst);
            }
            if (e.op == T::Tilde) return genBitNot(e, dst);
            Val a = gen(*e.a);
            if (a.t.k == K::None) noValue(e.loc);
            if (a.imm) {
                if (a.untyped) { a.i = -a.i; a.inv = false; return a; }
                switch (a.t.k) {
                case K::Int: a.i = wrap(K::Int, -a.i); return a;
                case K::Float: a.d = -a.d; return a;
                default: fail(e.loc, "cannot negate a value of type '" + tn(a.t) + "'");
                }
            }
            if (a.t.k != K::Int && a.t.k != K::Float) fail(e.loc, "cannot negate a value of type '" + tn(a.t) + "'");
            Type t = a.t;
            freeVal(a);
            int rd = dst >= 0 ? dst : allocReg(e.loc);
            Val zero = zeroOf(t);
            u8(SUB); u8(IMM_REG); u8(tagOf(t)); u8(rd);
            writeImm(zero);
            u8(a.reg);
            Val r;
            r.t = t;
            r.reg = rd;
            r.temp = dst < 0;
            return r;
        }

        /** @brief Compiles `~x` for `int`, `uint` and `byte`. The VM has no NOT instruction: `x ^ all-ones` is the same thing (one instruction). */
        Val genBitNot(const Expr& e, int dst) {
            Val a = gen(*e.a);
            if (a.t.k == K::None) noValue(e.loc);
            if (a.imm) {
                if (a.untyped) { a.i = ~a.i; a.inv = !a.inv; return a; }  // see Val::inv
                switch (a.t.k) {
                case K::Int: case K::UInt: case K::Byte: a.i = wrap(a.t.k, ~a.i); return a;
                default: break;
                }
            }
            if (a.t.k != K::Int && a.t.k != K::UInt && a.t.k != K::Byte)
                fail(e.loc, "cannot apply '~' to a value of type '" + tn(a.t) + "'" +
                    (a.t.k == K::Bool ? " (use '!' for bool)" : ""));
            Type t = a.t;
            freeVal(a);
            int rd = dst >= 0 ? dst : allocReg(e.loc);
            u8(XOR); u8(REG_IMM); u8(tagOf(t)); u8(rd);
            u8(a.reg);
            writeImm(immOf(t, t.k == K::Byte ? 0xFF : t.k == K::UInt ? (int64_t)0xFFFFFFFFll : -1));
            Val r;
            r.t = t;
            r.reg = rd;
            r.temp = dst < 0;
            return r;
        }

        /**
         * @brief Compiles string concatenation (`STRCAT`); the operands that were fresh temporaries are freed afterwards.
         * @throws CompileError if @p op is not `+` or an operand is not a string.
         */
        Val genConcat(T op, Loc loc, Val a, Val b) {
            if (op != T::Plus)
                fail(CompileError{ loc, "operator '" + std::string(opText(op)) + "' is not supported for strings", {}, {} }
                    .hint("strings can be joined with '+' and compared with == != < > <= >="));
            if (sameLayout(a.t, b.t)) a.t = b.t = Type{ K::String };  // byte[] + string
            if (a.t.k != K::String || b.t.k != K::String)
                fail(CompileError{ loc, "cannot concatenate '" + tn(a.t) + "' and '" + tn(b.t) + "'", {}, {} }
                    .hint("both sides of '+' must be strings; use str(x) to turn a number, bool or byte into one"));
            int rd = allocReg(loc);  // distinct from the operands: they may still need FREEMEM
            u8(STRCAT); u8(rd); u8(a.reg); u8(b.reg);
            dropFresh(a);
            dropFresh(b);
            freeVal(a);
            freeVal(b);
            return strVal(rd);
        }

        /** @brief Compiles a binary arithmetic, bitwise or shift expression: both operands, then arithValues(). */
        Val genArith(const Expr& e, int dst) {
            Val a = gen(*e.a);
            const size_t m = holdMark();
            hold(a);
            Val b = gen(*e.b);
            release(m);
            return arithValues(e.op, e.loc, a, b, dst);
        }

        // ---- shifts ----------------------------------------------------------------------------------------------
        /** @brief The width in bits of the integer kind @p k for shifts: 8 for `byte`, 32 otherwise. */
        static int shiftWidth(K k) { return k == K::Byte ? 8 : 32; }

        /**
         * @brief Checks a constant shift count at compile time.
         *
         * `a << n` and `a >> n` take int, uint or byte and give the type of `a`; the count has the same type (a literal takes
         * it). `>>` copies the sign in for int and shifts zeros in for uint and byte. A count the VM sees at run time that is
         * at or above the width gives 0 (-1 for a negative int), and a negative int count panics. A count that is a constant is
         * checked here instead, because it is nearly always a mistake.
         *
         * @param loc         Where to report.
         * @param t           The type of the value being shifted.
         * @param count       The constant count.
         * @param bothUntyped Both operands are literals (`a << 3`): no width yet, so only 62 is the limit.
         * @throws CompileError for a negative count or one at or above the width.
         */
        void checkShiftCount(Loc loc, Type t, const Val& count, bool bothUntyped) {
            if (count.i < 0)
                fail(CompileError{ loc, "negative shift count " + std::to_string(count.i), {}, {} }
                    .hint("a shift count must be 0 or more"));
            if (bothUntyped) {
                if (count.i > 62)
                    fail(CompileError{ loc, "shift count " + std::to_string(count.i) + " is too large for a constant", {}, {} }
                        .hint("a constant is at most 63 bits wide, so the count can be at most 62"));
                return;
            }
            const int w = shiftWidth(t.k);
            if (count.i >= w)
                fail(CompileError{ loc, "shift count " + std::to_string(count.i) + " is too large for '" + tn(t) + "'", {}, {} }
                    .hint("'" + tn(t) + "' has " + std::to_string(w) + " bits, so a constant count must be 0 to " +
                          std::to_string(w - 1) + (t.k == K::Byte ? "; convert to int or uint to shift further" : "")));
        }

        /**
         * @brief Folds a shift of two constants (the count already passed checkShiftCount()).
         *
         * Untyped literals are exact 64-bit integers until they meet a type.
         *
         * @throws CompileError "the result of this constant shift is too large" if a left shift overflows 64 bits.
         */
        Val foldShift(T op, Loc loc, const Val& a, const Val& b) {
            Val r = a;
            r.imm = true;
            r.temp = r.fresh = false;
            const int64_t c = b.i;
            if (a.untyped) {
                // untyped literals are exact 64-bit integers until they meet a type (see coerce)
                if (op == T::Shl) {
                    const int64_t v = (int64_t)((uint64_t)a.i << c);
                    if ((v >> c) != a.i) fail(loc, "the result of this constant shift is too large");
                    r.i = v;  // r.inv stays as it was: '~0 << 4' is still "all ones below bit 4", which wraps to any width
                }
                else {
                    r.i = a.i >> c;  // a.i >= 0 here (arithValues turns a negative literal into an int first)
                    r.inv = false;
                }
                return r;
            }
            r.i = op == T::Shl ? wrap(a.t.k, (int64_t)((uint64_t)a.i << c)) : wrap(a.t.k, a.i >> c);
            return r;
        }

        /**
         * @brief Computes `a <op> b` for two values that are already computed (also used by `a[i] += v`).
         *
         * Checks the operand types, folds constants, and otherwise emits `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `AND`, `OR`, `XOR`,
         * `SHL` or `SHR` in the form `[op][mode][type][to][a][b]`.
         *
         * @param op  The binary operator token.
         * @param loc Where to report problems.
         * @param a   Left operand.
         * @param b   Right operand.
         * @param dst Destination register, or -1 for a new temporary.
         * @return The result.
         * @throws CompileError if the operator does not apply to the operand type or a constant shift count is invalid.
         */
        Val arithValues(T op, Loc loc, Val a, Val b, int dst) {
            if (a.t.k == K::String || b.t.k == K::String) return genConcat(op, loc, a, b);
            const bool shift = op == T::Shl || op == T::Shr;
            auto integral = [](K k) { return k == K::Int || k == K::UInt || k == K::Byte; };
            if (shift && !a.untyped && !b.untyped && a.t != b.t && integral(a.t.k) && integral(b.t.k))
                fail(CompileError{ loc, "operand types differ: '" + tn(a.t) + "' and '" + tn(b.t) + "'", {}, {} }
                    .hint("the shift count must have the same type as the value being shifted; convert it, e.g. " +
                          tn(a.t) + "(...)"));
            unify(a, b, loc);
            bool bit = op == T::Amp || op == T::Pipe || op == T::Caret;
            Type t = a.untyped ? Type{ K::Int } : a.t;
            switch (t.k) {
            case K::Int: case K::UInt: case K::Byte: break;
            case K::Float:
                if (shift) fail(loc, std::string("'") + opText(op) + "' is not supported for float");
                if (bit) fail(loc, "bitwise operators are not supported for float");
                if (op == T::Percent) fail(loc, "'%' is not supported for float");
                break;
            default: {
                CompileError err{ loc, "operator '" + std::string(opText(op)) + "' is not supported for type '" + tn(t) + "'", {}, {} };
                if (t.k == K::Bool) err.hint("for bool logic use '&&', '||' and '!'");
                else if (t.k == K::Array || t.k == K::Struct) err.hint("operate on its elements or fields instead");
                throw err;
            }
            }
            if (shift) {
                // a negative literal shifted right has no width to be sign-extended to: it is an int
                if (op == T::Shr && a.untyped && b.untyped && a.i < 0) {
                    t = Type{ K::Int };
                    a = coerce(a, t, loc);
                    b = coerce(b, t, loc);
                }
                if (b.imm) checkShiftCount(loc, t, b, a.untyped && b.untyped);
                if (a.imm && b.imm) return foldShift(op, loc, a, b);
            }
            if (a.imm && b.imm) {
                Val r;
                if (fold(op, a, b, r, loc)) return r;
            }
            if (a.untyped) a = coerce(a, t, loc);
            if (b.untyped) b = coerce(b, t, loc);
            Op o = ADD;
            switch (op) {
            case T::Plus: o = ADD; break;
            case T::Minus: o = SUB; break;
            case T::Star: o = MUL; break;
            case T::Slash: o = DIV; break;
            case T::Percent: o = MOD; break;
            case T::Amp: o = AND; break;
            case T::Pipe: o = OR; break;
            case T::Shl: o = SHL; break;
            case T::Shr: o = SHR; break;
            default: o = XOR; break;
            }
            freeVal(a);
            freeVal(b);
            int rd = dst >= 0 ? dst : allocReg(loc);
            u8(o); u8(modeOf(a, b)); u8(tagOf(t)); u8(rd);
            operand(a);
            operand(b);
            Val r;
            r.t = t;
            r.reg = rd;
            r.temp = dst < 0;
            return r;
        }

        /** @brief Outcome of emitting a comparison: either a known constant, or the conditional jump that follows the `CMP`. */
        struct CmpRes {
            bool isConst = false;  ///< The comparison was decided at compile time; `value` holds the answer.
            bool value = false;  ///< The constant outcome, if `isConst`.
            Op jcc = JE;  ///< Otherwise: the conditional jump to emit after the `CMP`.
        };

        /**
         * @brief Compiles a string comparison.
         *
         * Strings compare bytewise, then by length (`"ab" < "abc"`). `STRCMP` yields -1/0/1, which is then compared against 0
         * with the requested operator.
         */
        CmpRes emitStrCompare(const Expr& e, Val a, Val b, bool jumpIf) {
            int t = allocReg(e.loc);
            u8(STRCMP); u8(t); u8(a.reg); u8(b.reg);
            dropFresh(a);
            dropFresh(b);
            freeVal(a);
            freeVal(b);
            u8(CMP); u8(REG_IMM); u8(T_INT); u8(t); u32(0);
            freeReg(t);
            CmpRes r;
            r.jcc = jccFor(e.op, jumpIf);
            return r;
        }

        /**
         * @brief Emits the `CMP` (if needed) for a comparison expression.
         * @param e      A comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`).
         * @param jumpIf The truth value on which the returned jump is taken (`jcc` is taken when `expr == jumpIf`).
         * @return A constant, or the conditional jump to emit.
         * @throws CompileError if the operand types cannot be compared (structs, arrays; `<` on bool).
         */
        CmpRes emitCompare(const Expr& e, bool jumpIf) {
            Val a = gen(*e.a);
            const size_t m = holdMark();
            hold(a);
            Val b = gen(*e.b);
            release(m);
            unify(a, b, e.loc);
            Type t = a.untyped ? Type{ K::Int } : a.t;
            bool eqOnly = e.op == T::Eq || e.op == T::Ne;
            if (t.k == K::String) return emitStrCompare(e, a, b, jumpIf);
            switch (t.k) {
            case K::Int: case K::UInt: case K::Byte: case K::Float: break;
            case K::Bool: case K::Address:
                if (!eqOnly)
                    fail(CompileError{ e.loc, "only '==' and '!=' are supported for '" + tn(t) + "'", {}, {} });
                break;
            default:
                fail(CompileError{ e.loc, "cannot compare values of type '" + tn(t) + "'", {}, {} }
                    .hint(t.k == K::Struct ? "compare the fields one by one" : "compare the elements one by one (and the lengths with len)"));
            }
            CmpRes r;
            if (a.imm && b.imm) {
                r.isConst = true;
                r.value = cmpConst(e.op, a, b);
                return r;
            }
            if (a.untyped) a = coerce(a, t, e.loc);
            if (b.untyped) b = coerce(b, t, e.loc);
            u8(CMP); u8(modeOf(a, b)); u8(tagOf(t));
            operand(a);
            operand(b);
            freeVal(a);
            freeVal(b);
            r.jcc = jccFor(e.op, jumpIf);
            return r;
        }

        /** @brief Compiles a comparison used as a value: leaves `true` or `false` in a register (`MOV` does not touch the flags, so `CMP / MOV / Jcc / MOV` is safe). */
        Val genCompareValue(const Expr& e, int dst) {
            CmpRes c = emitCompare(e, true);
            if (c.isConst) return immOf(Type{ K::Bool }, c.value);
            int rd = dst >= 0 ? dst : allocReg(e.loc);
            Label end;
            // MOV does not touch the flags, so CMP / MOV / Jcc / MOV is safe.
            emitMovImm(rd, immOf(Type{ K::Bool }, 1));
            jump(c.jcc, end);
            emitMovImm(rd, immOf(Type{ K::Bool }, 0));
            bind(end);
            Val r;
            r.t = Type{ K::Bool };
            r.reg = rd;
            r.temp = dst < 0;
            return r;
        }

        /** @brief Compiles a condition with `&&` or `||` (or anything else genCond() handles) used as a value. */
        Val genBoolGeneral(const Expr& e, int dst) {
            Label lf, end;
            makeRoomForCond();
            genCond(e, lf, false);
            // Past the jumps only some executions are here, so choosing the result register must not spill anything
            // that is held around this expression (the spill would run on the "true" path only).
            const size_t pf = enterCond();
            int rd = dst >= 0 ? dst : allocReg(e.loc);
            exitCond(pf);
            emitMovImm(rd, immOf(Type{ K::Bool }, 1));
            jump(JMP, end);
            bind(lf);
            emitMovImm(rd, immOf(Type{ K::Bool }, 0));
            bind(end);
            Val r;
            r.t = Type{ K::Bool };
            r.reg = rd;
            r.temp = dst < 0;
            return r;
        }

        /**
         * @brief Compiles a condition: jumps to @p L if `e == jumpIf`, otherwise falls through.
         *
         * Short-circuits `&&` and `||`, folds constant conditions, and treats any other bool or numeric expression as true
         * when it is non-zero.
         *
         * @param e      The condition.
         * @param L      The jump target.
         * @param jumpIf Jump when the condition has this truth value.
         * @throws CompileError if the condition is not a bool or a number.
         */
        void genCond(const Expr& e, Label& L, bool jumpIf) {
            if (e.k == Expr::Bool) {
                if ((e.iv != 0) == jumpIf) jump(JMP, L);
                return;
            }
            if (e.k == Expr::Unary && e.op == T::Bang) {
                genCond(*e.a, L, !jumpIf);
                return;
            }
            if (e.k == Expr::Binary && e.op == T::AndAnd) {
                makeRoomForCond();
                if (!jumpIf) {
                    genCond(*e.a, L, false);
                    const size_t pf = enterCond();
                    genCond(*e.b, L, false);
                    exitCond(pf);
                }
                else {
                    Label skip;
                    genCond(*e.a, skip, false);
                    const size_t pf = enterCond();
                    genCond(*e.b, L, true);
                    exitCond(pf);
                    bind(skip);
                }
                return;
            }
            if (e.k == Expr::Binary && e.op == T::OrOr) {
                makeRoomForCond();
                if (jumpIf) {
                    genCond(*e.a, L, true);
                    const size_t pf = enterCond();
                    genCond(*e.b, L, true);
                    exitCond(pf);
                }
                else {
                    Label skip;
                    genCond(*e.a, skip, true);
                    const size_t pf = enterCond();
                    genCond(*e.b, L, false);
                    exitCond(pf);
                    bind(skip);
                }
                return;
            }
            if (e.k == Expr::Binary && isCompare(e.op)) {
                CmpRes c = emitCompare(e, jumpIf);
                if (c.isConst) { if (c.value == jumpIf) jump(JMP, L); }
                else jump(c.jcc, L);
                return;
            }
            // any other expression: bool or numeric truthiness
            Val v = gen(e);
            if (v.t.k == K::None) noValue(e.loc);
            if (v.untyped) v = coerce(v, Type{ K::Int }, e.loc);
            if (v.imm) {
                bool truth = v.t.k == K::Float ? v.d != 0 : v.i != 0;
                if (truth == jumpIf) jump(JMP, L);
                return;
            }
            Val rhs;
            rhs.imm = true;
            rhs.t = v.t;
            bool isBool = v.t.k == K::Bool;
            switch (v.t.k) {
            case K::Bool: rhs.i = 1; break;
            case K::Int: case K::UInt: case K::Byte: case K::Float: break;
            default:
                fail(CompileError{ e.loc, "a condition must be bool or numeric, found '" + tn(v.t) + "'", {}, {} }
                    .hint(v.t.k == K::String ? "to test a string, compare it: s != \"\" or len(s) > 0" : "write the comparison out, e.g. x == y"));
            }
            u8(CMP); u8(REG_IMM); u8(tagOf(v.t)); u8(v.reg);
            writeImm(rhs);
            freeVal(v);
            // bool: equal to true => taken-if-true.  numeric: nonzero => taken-if-true
            if (isBool) jump(jumpIf ? JE : JNE, L);
            else jump(jumpIf ? JNE : JE, L);
        }

        // ------------------------------------------------------------ c ? a : b
        /**
         * @brief Moves @p v into register @p rd, releasing the register it came from.
         *
         * Afterwards @p rd holds the value and stays allocated to the caller.
         */
        void placeVal(int rd, Val& v) {
            if (v.imm) emitMovImm(rd, v);
            else if (v.reg != rd) emitMov(rd, v.reg);
            if (v.reg == rd) v.temp = false;  // it already is where it should be; rd stays allocated
            freeVal(v);
        }

        /**
         * @brief Compiles `c ? a : b`. Only the branch that is taken runs.
         *
         * Both must have the same type (an integer literal adopts the other branch's type, or the type the context expects).
         * A heap value must be either newly created in both branches or an existing one in both: the result is then fresh
         * (the receiver owns it) or borrowed accordingly.
         *
         * Layout: `cond; jump-if-false else; <then> -> rd; jump end; else: <else> -> rd; end:`
         * When the "then" value is a bare integer literal its type is not known until the "else" branch has been seen, so its
         * `MOV` is emitted after the else branch instead: `...; jump then_stub  ...  then_stub: MOV rd, lit; end:`
         *
         * The result register is not reserved while the condition and the branches are computed (that would waste a register
         * exactly when they need many): the "then" branch ends in some register, which becomes the result register, and the
         * "else" branch moves its value there. Nothing is live in it on the else path before that.
         *
         * @param e    The ternary expression.
         * @param dst  Destination register or -1.
         * @param want The type the context expects, or an empty Type.
         * @throws CompileError if the branches have different types or disagree about ownership.
         */
        Val genTernary(const Expr& e, int dst, Type want) {
            Label lelse, lend, lthen;
            makeRoomForCond();
            genCond(*e.a, lelse, false);
            const uint64_t usedAtStart = used_;
            const size_t pf = enterCond();  // the branches run on one path each: no spilling of what is held around us

            Ctx cxThen{ "in the 'then' part of the '?' expression", Loc{}, "" };
            Ctx cxElse{ "in the 'else' part of the '?' expression (it must have the same type as the 'then' part)", Loc{}, "" };

            int rd = dst;
            Val a = gen(*e.b, dst, want);
            if (a.t.k == K::None) noValue(e.b->loc);
            const bool deferred = a.untyped;  // a bare integer literal: its type comes from the other branch
            Type ft = a.t;
            if (deferred) {
                jump(JMP, lthen);
            }
            else {
                if (rd < 0) rd = (!a.imm && a.temp) ? a.reg : allocReg(e.loc);
                placeVal(rd, a);
                jump(JMP, lend);
            }

            bind(lelse);
            used_ = usedAtStart;  // the else branch starts from the same registers the then branch did
            Val b = gen(*e.c, dst, want);
            if (b.t.k == K::None) noValue(e.c->loc);
            if (deferred) {
                if (b.untyped) ft = (want.k == K::Int || want.k == K::UInt || want.k == K::Byte || want.k == K::Float) ? want : Type{ K::Int };
                else ft = b.t;
                a = coerce(a, ft, e.b->loc, &cxThen);
                b = coerce(b, ft, e.c->loc, &cxElse);
            }
            else {
                b = coerce(b, ft, e.c->loc, &cxElse);
            }
            if (ft.heap() && a.fresh != b.fresh)
                fail(CompileError{ e.loc, "the two parts of this '?' expression disagree about ownership", {}, {} }
                    .hint("one creates a new " + tn(ft) + " and the other reuses an existing one; wrap the existing one in copy(...) "
                        "(or make both reuse existing values)"));
            const bool fresh = ft.heap() && b.fresh;
            if (rd < 0) rd = (!b.imm && b.temp) ? b.reg : allocReg(e.loc);  // (only when the 'then' value was a bare literal)
            placeVal(rd, b);
            if (deferred) {
                jump(JMP, lend);
                bind(lthen);
                placeVal(rd, a);
            }
            bind(lend);
            exitCond(pf);
            used_ = usedAtStart;  // nothing but the result is live after the join
            if (dst < 0) { used_ |= 1ull << rd; touched_ |= 1ull << rd; }

            Val r;
            r.t = ft;
            r.reg = rd;
            r.temp = dst < 0;
            r.fresh = fresh;
            return r;
        }

        // ------------------------------------------------------------ structs
        /**
         * @brief Finds the field called @p name in struct @p sid.
         * @throws CompileError "has no field", with a suggestion when one is close.
         */
        const StructField& fieldOf(int sid, const std::string& name, Loc l) {
            for (const StructField& f : C.structs[sid].fields)
                if (f.name == name) return f;
            CompileError e{ l, "struct '" + C.structs[sid].name + "' has no field '" + name + "'", {}, {} };
            std::vector<std::string> names;
            std::string list;
            for (const StructField& f : C.structs[sid].fields) { names.push_back(f.name); list += (list.empty() ? "" : ", ") + f.name; }
            const std::string g = suggestName(name, names);
            if (!g.empty()) e.hint("did you mean '" + g + "'?");
            else if (!list.empty()) e.hint("its fields are: " + list);
            else e.hint("the struct has no fields");
            throw e;
        }

        /**
         * @brief Compiles `p.field`: loads the field from the struct's heap block.
         *
         * A string field yields a borrowed alias of the string (the struct keeps owning it). Field access on a temporary
         * struct is rejected.
         */
        Val genMember(const Expr& e, int dst) {
            Val b = gen(*e.a);
            if (b.t.k != K::Struct) {
                CompileError err{ e.loc, "'.' requires a struct value, found '" + tn(b.t) + "'", {}, {} };
                if (isSeq(b.t)) err.hint("the length of an array or string is len(x); there are no methods or properties");
                throw err;
            }
            if (b.fresh)
                fail(CompileError{ e.loc, "field access on a temporary struct is not supported", {}, {} }
                    .hint("store it in a variable first: set p = make(); p.x"));
            const StructField& f = fieldOf(b.t.sid, e.s, e.loc);
            freeVal(b);
            int rd = dst >= 0 ? dst : allocReg(e.loc);
            int addr = b.reg;
            if (f.off) {
                u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(rd); u8(b.reg);
                u32(f.off);
                addr = rd;
            }
            if (f.t.k == K::String) {  // stored as an address; the loaded value is a borrowed alias of the string
                u8(LOAD); u8(T_ADDRESS); u8(addr); u8(rd);
                u8(REINTERPRET); u8(rd); u8(T_STRINGADDRESS);
            }
            else {
                u8(LOAD); u8(tagOf(f.t)); u8(addr); u8(rd);
            }
            Val r;
            r.t = f.t;
            r.reg = rd;
            r.temp = dst < 0;
            return r;
        }

        /**
         * @brief Compiles `S()`: reserves the heap block for a struct (`RESERVEMEM`).
         *
         * A string field starts as an empty string that the struct owns, and the other fields are zeroed explicitly only when
         * the target asks for it (Target::zeroInit; the VM's memory is already zero).
         *
         * @param e   The call (must have no arguments).
         * @param sid The struct's index.
         * @return A fresh struct.
         */
        Val genCtor(const Expr& e, int sid) {
            const StructInfo& si = C.structs[sid];
            if (!e.args.empty())
                fail(CompileError{ e.loc, "struct constructor '" + si.name + "()' takes no arguments", {}, {} }
                    .hint("create it empty, then set the fields: set p = " + si.name + "(); p.field = value;"));
            int rd = allocReg(e.loc);
            u8(RESERVEMEM); u8(rd); u32(si.size);
            if (C.tg.zeroInit && !si.fields.empty()) {
                int z = allocReg(e.loc), a = -1;
                for (const StructField& f : si.fields) {
                    if (f.t.k == K::String) continue;  // filled with an empty string below
                    emitMovImm(z, zeroOf(f.t));
                    int addr = rd;
                    if (f.off) {
                        if (a < 0) a = allocReg(e.loc);
                        u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(a); u8(rd);
                        u32(f.off);
                        addr = a;
                    }
                    u8(STORE); u8(tagOf(f.t)); u8(addr); u8(z);
                }
                freeReg(z);
                if (a >= 0) freeReg(a);
            }
            if (si.hasStrings) {  // a string field is never null: it starts as an empty string that the struct owns
                int sr = allocReg(e.loc), a = -1;
                for (const StructField& f : si.fields) {
                    if (f.t.k != K::String) continue;
                    u8(STRLIT); u8(sr); u32(0);
                    u8(REINTERPRET); u8(sr); u8(T_ADDRESS);  // STORE writes exactly the tag it is told to
                    int addr = rd;
                    if (f.off) {
                        if (a < 0) a = allocReg(e.loc);
                        u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(a); u8(rd);
                        u32(f.off);
                        addr = a;
                    }
                    u8(STORE); u8(T_ADDRESS); u8(addr); u8(sr);
                }
                freeReg(sr);
                if (a >= 0) freeReg(a);
            }
            Val r;
            r.t = Type{ K::Struct, sid };
            r.reg = rd;
            r.temp = r.fresh = true;
            return r;
        }

        // ------------------------------------------------------------ arrays
        /**
         * @brief Size in bytes of the length word at the start of every array and string block.
         *
         * Heap layout: `[u32 length][elements...]`. The array value is the address of the block, so `FREEMEM` works on it
         * directly. A constant size goes straight into `RESERVEMEM`; a size only known at run time is handed to `NEWARRAY` in a
         * register (which panics on a negative one). An array of arrays stores the addresses of its rows as its elements and
         * owns them.
         */
        static constexpr uint32_t kArrayHeader = 4;

        /** @brief Size in bytes of one array element: 1 for `bool` and `byte`, 4 for everything else (an array row is an address). */
        static uint32_t elemSize(Type t) { return (t.k == K::Bool || t.k == K::Byte) ? 1 : 4; }

        /**
         * @brief Rejects an element type that an array cannot hold.
         * @throws CompileError for strings and structs (arrays of scalars and of arrays are fine).
         */
        void requireElem(Type t, Loc l) {
            switch (t.k) {
            case K::Int: case K::UInt: case K::Float: case K::Bool: case K::Byte: case K::Address: case K::Array: return;
            default:
                fail(CompileError{ l, "arrays of '" + tn(t) + "' are not supported", {}, {} }
                    .hint("array elements must be int, uint, float, bool, byte, address or another array"));
            }
        }

        /**
         * @brief Checks that a row stored into an array of arrays is a brand-new value.
         *
         * A row is taken over by the outer array (and freed with it). One that somebody else owns would be freed twice.
         *
         * @throws CompileError if @p v is not fresh.
         */
        void requireFreshRow(const Val& v, Loc l) {
            if (!v.fresh)
                fail(CompileError{ l, "this array already has an owner, so it cannot become a row of an array of arrays", {}, {} }
                    .hint("an array of arrays owns its rows; store an independent copy with copy(...), "
                        "or write the row in place, e.g. [[0; 3], [1, 2]]"));
        }

        /** @brief True for an immediate whose bits are all zero: it needs no store, because VM memory is zero-initialised. */
        static bool isZeroImm(const Val& v) {
            if (!v.imm) return false;
            if (v.t.k == K::Float) { float f = (float)v.d; uint32_t b; std::memcpy(&b, &f, 4); return b == 0; }
            return v.i == 0;
        }

        /**
         * @brief Reserves an array block of a constant length and stores the length.
         * @param n  Number of elements.
         * @param sz Element size in bytes.
         * @param l  Where to report problems.
         * @return The register holding the array address (the caller owns it).
         * @throws CompileError "array is too large" if the block would not fit in 32 bits.
         */
        int newArray(uint64_t n, uint32_t sz, Loc l) {
            uint64_t bytes = kArrayHeader + n * sz;
            if (bytes > 0xFFFFFFFFull) fail(l, "array is too large");
            int base = allocReg(l);
            u8(RESERVEMEM); u8(base); u32((uint32_t)bytes);
            if (n > 0) {
                int t = allocReg(l);
                emitMovImm(t, immOf(Type{ K::UInt }, (int64_t)n));
                u8(STORE); u8(T_UINT); u8(base); u8(t);
                freeReg(t);
            }
            return base;
        }

        /** @brief The array type whose elements are @p et: `int` gives `int[]`, `int[]` gives `int[][]`. */
        static Type arrayOfElem(Type et) {
            return et.k == K::Array ? Type::arrayOf(et.ek, et.depth + 1) : Type::arrayOf(et.k, 1);
        }

        /** @brief A newly created array held in register @p base; the receiver owns it. */
        static Val arrayVal(int base, Type t) {
            Val v;
            v.t = t;
            v.reg = base;
            v.temp = v.fresh = true;
            return v;
        }

        /**
         * @brief Compiles an array literal `[a, b, c]`.
         *
         * The element type comes from the context (@p want) if known, else from the type of the first element. Zero elements are
         * not stored (the block is already zeroed). Rows of an array of arrays move in and must be fresh.
         *
         * @throws CompileError for an empty literal without a type context, or an element of the wrong type.
         */
        Val genArrayLit(const Expr& e, Type want) {
            Type et;
            Val first;
            bool haveFirst = false;
            if (want.k == K::Array) {
                et = want.elem();
            }
            else {
                if (e.args.empty())
                    fail(CompileError{ e.loc, "cannot infer the element type of an empty array", {}, {} }
                        .hint("give it a type, e.g. 'set a: int[] = [];'"));
                first = gen(*e.args[0]);
                if (first.t.k == K::None) noValue(e.args[0]->loc);
                if (first.untyped) first = coerce(first, Type{ K::Int }, e.args[0]->loc);
                et = first.t;
                haveFirst = true;
            }
            requireElem(et, haveFirst ? e.args[0]->loc : e.loc);
            const uint32_t sz = elemSize(et);
            const uint64_t n = e.args.size();
            Val arr = arrayVal(newArray(n, sz, e.loc), arrayOfElem(et));
            for (size_t i = 0; i < n; ++i) {
                const Expr& ae = *e.args[i];
                const size_t m = holdMark();
                hold(arr);  // the block's address waits in a register while an element is computed
                Val v = (haveFirst && i == 0) ? first : gen(ae, -1, et);
                Ctx cx{ "in element " + std::to_string(i + 1) + " of the array literal", Loc{}, "" };
                v = coerce(v, et, ae.loc, &cx);
                if (et.k == K::Array) requireFreshRow(v, ae.loc);
                release(m);
                if (isZeroImm(v)) continue;
                v = toReg(v, ae.loc);
                int a = allocReg(ae.loc);
                u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(a); u8(arr.reg);
                u32((uint32_t)(kArrayHeader + i * sz));
                u8(STORE); u8(tagOf(et)); u8(a); u8(v.reg);  // a row moves in: the outer array owns it from here on
                freeReg(a);
                freeVal(v);
            }
            return arr;
        }

        /**
         * @brief Compiles `[value; count]`.
         *
         * The count may be any int / uint / byte expression; a constant one is reserved at compile time (`RESERVEMEM`), any
         * other with `NEWARRAY`. A non-zero fill value is written with a single `MEMSET` instruction; a zero value needs no
         * fill at all. For an array of arrays, `value` is a pattern: the result holds `count` independent deep copies of it
         * (`ARRREP`).
         */
        Val genArrayRep(const Expr& e, Type want) {
            Val nv = gen(*e.b);
            if (nv.t.k == K::None) noValue(e.b->loc);
            const bool constant = nv.imm;
            uint64_t n = 0;
            if (constant) {
                if (nv.t.k == K::Float || nv.t.k == K::Bool)
                    fail(CompileError{ e.b->loc, "array size must be an integer, found '" + tn(nv.t) + "'", {}, {} });
                if (nv.i < 0) fail(e.b->loc, "array size cannot be negative");
                n = (uint64_t)nv.i;
            }
            else if (nv.t.k != K::Int && nv.t.k != K::UInt && nv.t.k != K::Byte) {
                fail(CompileError{ e.b->loc, "array size must be an integer, found '" + tn(nv.t) + "'", {}, {} }
                    .hint("convert it with int(...)"));
            }

            const size_t m = holdMark();
            hold(nv);  // the count waits while the value is computed
            Val v = gen(*e.a, -1, want.k == K::Array ? want.elem() : Type{});
            release(m);
            if (v.t.k == K::None) noValue(e.a->loc);
            Type et;
            if (want.k == K::Array) {
                et = want.elem();
            }
            else {
                if (v.untyped) v = coerce(v, Type{ K::Int }, e.a->loc);
                et = v.t;
            }
            requireElem(et, e.loc);
            v = coerce(v, et, e.a->loc);

            if (et.k == K::Array) {
                if (n > 0xFFFFFFFFull) fail(e.b->loc, "array is too large");
                Val cnt = constant ? toReg(immOf(Type{ K::UInt }, (int64_t)n), e.loc) : nv;
                int rd = allocReg(e.loc);
                u8(ARRREP); u8(rd); u8(v.reg); u8(cnt.reg); u8(et.depth);
                dropFresh(v);  // the pattern was only a template: ARRREP made its own copies (a borrowed one is left alone)
                freeVal(v);
                freeVal(cnt);
                return arrayVal(rd, arrayOfElem(et));
            }

            const uint32_t sz = elemSize(et);
            if (!constant) {
                int rd = allocReg(e.loc);
                u8(NEWARRAY); u8(rd); u8(nv.reg); u8(sz);
                if (!isZeroImm(v)) {
                    // MEMSET works on the elements, so rd points at them while it runs, then is rewound
                    v = toReg(v, e.loc);
                    u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(rd); u8(rd); u32(kArrayHeader);
                    u8(MEMSET); u8(tagOf(et)); u8(rd); u8(v.reg); u8(nv.reg);
                    u8(SUB); u8(REG_IMM); u8(T_ADDRESS); u8(rd); u8(rd); u32(kArrayHeader);
                }
                freeVal(v);
                freeVal(nv);
                return arrayVal(rd, arrayOfElem(et));
            }

            int base = newArray(n, sz, e.loc);
            if (n > 0 && !isZeroImm(v)) {
                // one MEMSET instead of a fill loop. The array register itself points at the elements while it runs
                // (saves a register), then is rewound:  base += 4; count = n; MEMSET(base, value, count); base -= 4;
                v = toReg(v, e.loc);
                int cnt = allocReg(e.loc);
                u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(base); u8(base); u32(kArrayHeader);
                emitMovImm(cnt, immOf(Type{ K::UInt }, (int64_t)n));
                u8(MEMSET); u8(tagOf(et)); u8(base); u8(v.reg); u8(cnt);
                u8(SUB); u8(REG_IMM); u8(T_ADDRESS); u8(base); u8(base); u32(kArrayHeader);  // rewind
                freeReg(cnt);
            }
            freeVal(v);
            return arrayVal(base, arrayOfElem(et));
        }

        /**
         * @brief Emits the bounds check for `base[idx]` and returns a temporary register holding the element's address.
         *
         * The check loads the stored length and compares it, unsigned, with the index, jumping to the function's out-of-bounds
         * stub on failure. A negative `int` index becomes huge when compared as unsigned and so fails the check. Inside an
         * `unsafe` block the check is left out entirely. A constant index is checked against the length directly; a constant
         * negative index is a compile error.
         *
         * @param base A non-fresh array or string value.
         * @param idx  The index (consumed; its register is freed).
         * @param sz   Element size in bytes (1 or 4).
         * @param l    Where to report problems.
         * @return A temporary register with the address of the element.
         * @throws CompileError if the index is not an int, uint or byte, or is a negative constant.
         */
        int elemAddr(const Val& base, Val& idx, uint32_t sz, Loc l) {
            if (idx.t.k == K::None) noValue(l);
            const Type it = idx.untyped ? Type{ K::Int } : idx.t;
            if (it.k != K::Int && it.k != K::UInt && it.k != K::Byte)
                fail(CompileError{ l, "an array index must be int, uint or byte, found '" + tn(it) + "'", {}, {} }
                    .hint(it.k == K::Float ? "convert it with int(...)" : "indices are whole numbers"));

            const bool checked = unsafe_ == 0;  // inside 'unsafe' the bounds check is left out entirely
            int t = allocReg(l);
            if (checked) { u8(LOAD); u8(T_UINT); u8(base.reg); u8(t); }  // t = length
            if (idx.imm) {
                if (idx.i < 0) fail(l, "negative array index");
                if (checked) {
                    u8(CMP); u8(REG_IMM); u8(T_UINT); u8(t);
                    u32((uint32_t)idx.i);
                    jump(JLE, oob_);                     // length <= index  =>  out of bounds
                    oobUsed_ = true;
                }
                uint64_t off = kArrayHeader + (uint64_t)idx.i * sz;
                if (off > 0xFFFFFFFFull) fail(l, "array index too large");
                u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(t); u8(base.reg);
                u32((uint32_t)off);
            }
            else {
                // The bounds check compares as unsigned, so a negative int index becomes huge and fails it. A VM that does
                // not check register tags compares the bits and does not care that the register is tagged int, so no
                // retag (MOV + REINTERPRET, two extra instructions on every checked access) is emitted: the check is then
                // the plain LOAD len; CMP.uint; JGE sequence that the VM's array superinstruction recognises. The tag is
                // only needed by a strict-types VM (--strict-tags). A byte index must always be widened, because only its
                // low 8 bits are defined.
                int ri = idx.reg;
                const bool retag = idx.t.k != K::UInt && (idx.t.k == K::Byte || C.tg.strictTags);
                if (retag) {
                    if (!idx.temp) {                     // never retag a variable's own register
                        ri = allocReg(l);
                        emitMov(ri, idx.reg);
                        idx.reg = ri;
                        idx.temp = true;
                    }
                    if (idx.t.k == K::Int) { u8(REINTERPRET); u8(ri); u8(T_UINT); }
                    else { u8(CAST); u8(ri); u8(T_UINT); }
                }
                if (checked) {
                    u8(CMP); u8(REG_REG); u8(T_UINT); u8(ri); u8(t);
                    jump(JGE, oob_);                     // index >= length  =>  out of bounds
                    oobUsed_ = true;
                }
                if (sz == 1) {
                    u8(ADD); u8(REG_REG); u8(T_ADDRESS); u8(t); u8(base.reg); u8(ri);
                }
                else {
                    u8(MUL); u8(REG_IMM); u8(T_UINT); u8(t); u8(ri); u32(sz);
                    u8(ADD); u8(REG_REG); u8(T_ADDRESS); u8(t); u8(t); u8(base.reg);
                }
                u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(t); u8(t); u32(kArrayHeader);
            }
            freeVal(idx);
            return t;
        }

        /**
         * @brief Emits a run-time failure handler, placed after the function's `RET`.
         *
         * It builds the message "runtime error: (what) in '(function)'" with `STRLIT`, then `ABORT` makes the VM panic with it.
         *
         * @param L    The stub's label (bound here).
         * @param what Text of the error, e.g. "index out of bounds".
         */
        void emitFailStub(Label& L, const std::string& what) {
            bind(L);
            const std::string msg = "runtime error: " + what + " in '" + F.name.substr(0, 200) + "'";
            u8(STRLIT); u8(0); u32((uint32_t)msg.size());
            for (char c : msg) u8((uint8_t)c);
            u8(ABORT); u8(0);
        }

        /**
         * @brief Compiles `a[i]` / `s[i]`: a bounds-checked load of one element.
         *
         * An element that is an array is yielded as a borrowed alias of the row. Indexing a temporary releases it after the
         * element is read (but a temporary array of arrays cannot be indexed).
         */
        Val genIndex(const Expr& e, int dst) {
            Val b = gen(*e.a);
            if (!isSeq(b.t)) {
                CompileError err{ e.loc, "'[]' requires an array or a string, found '" + tn(b.t) + "'", {}, {} };
                if (b.t.k == K::Struct) err.hint("a struct's fields are read with '.', e.g. p.x");
                throw err;
            }
            const size_t m = holdMark();
            hold(b);
            Val ix = gen(*e.b);
            release(m);
            const Type et = elemOf(b.t);
            if (b.fresh && et.k == K::Array)
                fail(CompileError{ e.loc, "cannot index a temporary array of arrays", {}, {} }
                    .hint("it would be freed, rows included, as soon as the element is read; store it in a variable first"));
            int t = elemAddr(b, ix, elemSize(et), e.loc);
            int rd = dst >= 0 ? dst : t;
            u8(LOAD); u8(tagOf(et)); u8(t); u8(rd);  // an element that is an array is read as a borrowed alias of the row
            dropFresh(b);  // indexing a temporary ("abc"[1], f()[0]): release it once the element is read
            freeVal(b);
            if (rd != t) freeReg(t);
            Val r;
            r.t = et;
            r.reg = rd;
            r.temp = dst < 0;
            return r;
        }

        /**
         * @brief The type of an assignment target such as `a`, `g[i]` or `g[i][j]`, worked out without generating any code.
         *
         * The value on the right needs to know it before it is compiled.
         *
         * @throws CompileError if the target is not a variable or an element of one.
         */
        Type targetType(const Expr& e) {
            if (e.k == Expr::Ident) return vars_[useVar(e.s, e.loc)].t;
            if (e.k == Expr::Index) {
                const Type bt = targetType(*e.a);
                if (!isSeq(bt))
                    fail(CompileError{ e.loc, "'[]' requires an array or a string, found '" + tn(bt) + "'", {}, {} });
                return elemOf(bt);
            }
            fail(CompileError{ e.loc, "cannot assign to an element of a temporary value", {}, {} }
                .hint("store the array in a variable first"));
        }

        /**
         * @brief Compiles `target[idx] = value`, where the target is a variable or itself an element (`grid[i][j] = v`).
         *
         * The value is computed first and parked (it may be spilled); then the index and the array are computed, the bounds are
         * checked, and the value is stored with `STORE`. Replacing a row of an array of arrays frees the old row first.
         * Compound forms (`+=`, `++`, ...) go to genIndexCompound().
         *
         * @throws CompileError if the root variable is `const`, is not an array or string, or the value has the wrong type.
         */
        void genIndexAssign(const Stmt& s) {
            const Expr* root = s.target.get();  // the variable at the root of the target, for the const check
            while (root->k == Expr::Index) root = root->a.get();
            if (root->k == Expr::Ident) {
                const Var& rv = vars_[useVar(root->s, root->loc)];
                if (rv.isConst)
                    fail(CompileError{ s.loc, "cannot modify an element of '" + root->s + "': it is declared const", {}, {} }
                        .note(rv.loc, "'" + root->s + "' is declared const here")
                        .hint("remove 'const' from its declaration if its contents need to change"));
            }
            const Type bt = targetType(*s.target);
            if (!isSeq(bt))
                fail(CompileError{ s.loc, "'" + (s.name.empty() ? std::string("this") : s.name) + "' is not an array or a string (it is '" + tn(bt) + "')", {}, {} }
                    .hint("only arrays and strings can be indexed"));
            const Type et = elemOf(bt);
            if (s.op != T::Eof) { genIndexCompound(s, et); return; }
            Ctx cx{ "in assignment to an element", Loc{}, "" };
            Val v = coerce(gen(*s.e, -1, et), et, s.e->loc, &cx);
            if (et.k == K::Array) requireFreshRow(v, s.e->loc);
            v = toReg(v, s.loc);
            const size_t mv = holdMark();
            hold(v);  // the value is only needed again by the final STORE, so it stays out of the way while the address is worked out
            Val ix = gen(*s.idx);
            const size_t mi = holdMark();
            hold(ix);
            Val base = gen(*s.target);
            release(mi);
            int t = elemAddr(base, ix, elemSize(et), s.loc);
            freeVal(base);  // only the element's address is needed from here on
            if (et.k == K::Array) {  // the row being replaced is owned by this array: release it first
                int old = allocReg(s.loc);
                u8(LOAD); u8(T_ADDRESS); u8(t); u8(old);
                emitFree(old, et);
                freeReg(old);
            }
            release(mv);
            u8(STORE); u8(tagOf(et)); u8(t); u8(v.reg);
            freeReg(t);
            freeVal(base);
            freeVal(v);
        }

        /**
         * @brief Compiles `target[idx] op= value` (`a[i] += 1`, `a[i]++`, `grid[i][j] *= 2`).
         *
         * Like a plain assignment the value is computed first; then the element's address is worked out ONCE, the element is
         * loaded, combined with the value (arithValues()) and stored back through the same address, so `a[f()] += 1` calls
         * `f` once and the bounds are checked once.
         *
         * @param s  The IndexAssign statement (`s.op` is the operator).
         * @param et The element type.
         */
        void genIndexCompound(const Stmt& s, Type et) {
            Val v = gen(*s.e, -1, et);
            if (v.t.k == K::None) noValue(s.e->loc);
            const size_t mv = holdMark();
            hold(v);
            Val ix = gen(*s.idx);
            const size_t mi = holdMark();
            hold(ix);
            Val base = gen(*s.target);
            release(mi);
            int t = elemAddr(base, ix, elemSize(et), s.loc);
            freeVal(base);  // only the element's address is needed from here on
            const int cr = allocReg(s.loc);
            u8(LOAD); u8(tagOf(et)); u8(t); u8(cr);
            release(mv);
            Val cur;
            cur.t = et;
            cur.reg = cr;
            cur.temp = true;
            Val res = arithValues(s.op, s.opLoc, cur, v, cr);  // writes the result into cr
            if (res.imm || res.reg != cr) fail(s.opLoc, "internal error: compound assignment");
            u8(STORE); u8(tagOf(et)); u8(t); u8(cr);
            freeReg(t);
        }

        /** @brief Compiles `len(array or string)`: loads the length word of the block; a temporary argument is released. */
        Val genLen(const Expr& e) {
            if (e.args.size() != 1) fail(e.loc, "'len' expects exactly 1 argument");
            Val a = gen(*e.args[0]);
            if (!isSeq(a.t))
                fail(CompileError{ e.args[0]->loc, "'len' expects an array or a string, found '" + tn(a.t) + "'", {}, {} }
                    .hint(a.t.k == K::Struct ? "a struct has a fixed set of fields; there is nothing to count" : "only arrays and strings have a length"));
            int rd = allocReg(e.loc);
            u8(LOAD); u8(T_INT); u8(a.reg); u8(rd);
            dropFresh(a);  // len() of a temporary: release it
            freeVal(a);
            Val r;
            r.t = Type{ K::Int };
            r.reg = rd;
            r.temp = true;
            return r;
        }

        /** @brief Compiles an expression that must be a string (a `byte[]` is accepted as well). */
        Val stringArg(const Expr& a) {
            Val v = gen(a);
            if (v.t.k == K::None) noValue(a.loc);
            return coerce(v, Type{ K::String }, a.loc);  // a byte[] is accepted too
        }

        /** @brief Compiles an expression that must be an int, uint or byte, and makes sure it is in a register. */
        Val intArg(const Expr& a) {
            Val v = gen(a);
            if (v.t.k == K::None) noValue(a.loc);
            const Type t = v.untyped ? Type{ K::Int } : v.t;
            if (t.k != K::Int && t.k != K::UInt && t.k != K::Byte)
                fail(a.loc, "expected an int, uint or byte, found '" + tn(t) + "'");
            return toReg(v, a.loc);
        }

        /** @brief Checks the argument count of a built-in. @throws CompileError if `e` has not exactly @p n arguments. */
        void needArgs(const Expr& e, size_t n) {
            if (e.args.size() != n)
                fail(e.loc, "'" + e.s + "' expects " + std::to_string(n) + " argument" + (n == 1 ? "" : "s") +
                    ", found " + std::to_string(e.args.size()));
        }

        /**
         * @brief Compiles `input()` / `input("prompt")`: one line from stdin without its line ending.
         *
         * At the end of input it returns an empty string; use `eof()` to tell that apart from an empty line. The prompt is
         * printed first (`READLINE` flushes stdout, so a prompt without a newline shows up).
         */
        Val genInput(const Expr& e) {
            if (e.args.size() > 1) fail(e.loc, "'input' expects an optional prompt string");
            if (e.args.size() == 1) {
                Val p = stringArg(*e.args[0]);
                u8(STRPRINT); u8(p.reg);
                dropFresh(p);
                freeVal(p);
            }
            int rd = allocReg(e.loc);
            u8(READLINE); u8(rd);  // flushes stdout first, so a prompt without a newline shows up
            return strVal(rd);
        }

        /** @brief Compiles `eof()`: true if standard input has no more data (`ATEOF`). */
        Val genEof(const Expr& e) {
            needArgs(e, 0);
            int rd = allocReg(e.loc);
            u8(ATEOF); u8(rd);
            Val r;
            r.t = Type{ K::Bool };
            r.reg = rd;
            r.temp = true;
            return r;
        }

        /** @brief Compiles `str(x)`: a number, bool or byte as a string, formatted exactly like `print(x)` (`STRFROM`). */
        Val genToStr(const Expr& e) {
            needArgs(e, 1);
            Val v = gen(*e.args[0]);
            if (v.t.k == K::None) noValue(e.args[0]->loc);
            if (v.t.k == K::String)
                fail(e.args[0]->loc, "'str' converts numbers, bools and bytes; a string is already one (s + \"\" makes a copy)");
            if (v.t.k == K::Struct || v.t.k == K::Array)
                fail(e.args[0]->loc, "cannot convert a value of type '" + tn(v.t) + "' to a string");
            v = toReg(v, e.loc);
            freeVal(v);
            int rd = allocReg(e.loc);
            u8(STRFROM); u8(rd); u8(v.reg);
            return strVal(rd);
        }

        /** @brief Compiles `chr(byte)`: a string of length 1 (`STRCHR`). A constant outside 0..255 is a compile error. */
        Val genChr(const Expr& e) {
            needArgs(e, 1);
            Val v = intArg(*e.args[0]);
            freeVal(v);
            if (e.args[0]->k == Expr::Int && (e.args[0]->iv < 0 || e.args[0]->iv > 255))
                fail(e.args[0]->loc, "'chr' needs a value from 0 to 255");
            int rd = allocReg(e.loc);
            u8(STRCHR); u8(rd); u8(v.reg);
            return strVal(rd);
        }

        /** @brief Compiles `substr(s, start, count)` (`STRSLICE`); a range outside the string is a run-time error. */
        Val genSubstr(const Expr& e) {
            needArgs(e, 3);
            Val s = stringArg(*e.args[0]);
            const size_t m = holdMark();
            hold(s);
            Val st = intArg(*e.args[1]);
            hold(st);
            Val ct = intArg(*e.args[2]);
            release(m);
            int rd = allocReg(e.loc);
            u8(STRSLICE); u8(rd); u8(s.reg); u8(st.reg); u8(ct.reg);
            dropFresh(s);
            freeVal(s);
            freeVal(st);
            freeVal(ct);
            return strVal(rd);
        }

        /** @brief Compiles `find(haystack, needle)`: the index of the first match, or -1 (`STRFIND`). */
        Val genFind(const Expr& e) {
            needArgs(e, 2);
            Val h = stringArg(*e.args[0]);
            const size_t m = holdMark();
            hold(h);
            Val n = stringArg(*e.args[1]);
            release(m);
            int rd = allocReg(e.loc);
            u8(STRFIND); u8(rd); u8(h.reg); u8(n.reg);
            dropFresh(h);
            dropFresh(n);
            freeVal(h);
            freeVal(n);
            Val r;
            r.t = Type{ K::Int };
            r.reg = rd;
            r.temp = true;
            return r;
        }

        /**
         * @brief Compiles `is_int(s)` / `is_uint(s)` / `is_float(s)`: would parsing @p s succeed (`STRPARSE`).
         * @param e   The call.
         * @param tag The number type to test for (`T_INT`, `T_UINT` or `T_FLOAT`).
         */
        Val genIsNumber(const Expr& e, uint8_t tag) {
            needArgs(e, 1);
            Val s = stringArg(*e.args[0]);
            int rv = allocReg(e.loc), rok = allocReg(e.loc);
            u8(STRPARSE); u8(rv); u8(rok); u8(s.reg); u8(tag);
            dropFresh(s);
            freeVal(s);
            freeReg(rv);
            Val r;
            r.t = Type{ K::Bool };
            r.reg = rok;
            r.temp = true;
            return r;
        }

        /**
         * @brief Compiles `int(s)` / `uint(s)` / `float(s)` for a string @p s.
         *
         * A string that is not a valid number jumps to the function's `parse_` stub, which panics with "cannot convert the
         * string to a number".
         */
        Val genParse(const Expr& e, Val s, Type to) {
            int rv = allocReg(e.loc), rok = allocReg(e.loc);
            u8(STRPARSE); u8(rv); u8(rok); u8(s.reg); u8(tagOf(to));
            dropFresh(s);
            freeVal(s);
            u8(CMP); u8(REG_IMM); u8(T_BOOL); u8(rok); u8(0);
            jump(JE, parse_);
            parseUsed_ = true;
            freeReg(rok);
            Val r;
            r.t = to;
            r.reg = rv;
            r.temp = true;
            return r;
        }

        // ------------------------------------------------------------ files
        /**
         * @name File built-ins
         * A file is an `int` handle (see `IlangVM.h`): 0, 1 and 2 are stdin, stdout and stderr, the rest come from `fopen`.
         * @{
         */

        /** @brief A value of kind @p k in a fresh temporary register @p reg. */
        static Val tempVal(int reg, K k) {
            Val r;
            r.t = Type{ k };
            r.reg = reg;
            r.temp = true;
            return r;
        }

        // The modes fopen accepts.
        /** @brief The modes `fopen` accepts: `"r"`, `"w"`, `"a"`, optionally followed by `b`. */
        static bool validFileMode(const std::string& m) {
            return m == "r" || m == "w" || m == "a" || m == "rb" || m == "wb" || m == "ab";
        }

        /**
         * @brief Compiles `fopen(path, mode)`: an `int` handle, or -1 if the file cannot be opened (`file_error()` says why).
         *
         * Mode `"r"` reads an existing file, `"w"` creates it or empties it, `"a"` appends. A mode that is a string literal is
         * checked here; a computed one is checked by the VM.
         */
        Val genFopen(const Expr& e) {
            needArgs(e, 2);
            const Expr& m = *e.args[1];
            if (m.k == Expr::Str && !validFileMode(m.s))
                fail(CompileError{ m.loc, "invalid file mode \"" + m.s + "\"", {}, {} }
                    .hint("use \"r\" to read, \"w\" to write (the file is emptied first) or \"a\" to append"));
            Val p = stringArg(*e.args[0]);
            const size_t hm = holdMark();
            hold(p);
            Val md = stringArg(m);
            release(hm);
            int rd = allocReg(e.loc);
            u8(FOPEN); u8(rd); u8(p.reg); u8(md.reg);
            dropFresh(p);
            dropFresh(md);
            freeVal(p);
            freeVal(md);
            return tempVal(rd, K::Int);
        }

        /** @brief Compiles `fclose(handle)`: false if the final flush failed. */
        Val genFclose(const Expr& e) {
            needArgs(e, 1);
            Val h = intArg(*e.args[0]);
            int rd = allocReg(e.loc);
            u8(FCLOSE); u8(rd); u8(h.reg);
            freeVal(h);
            return tempVal(rd, K::Bool);
        }

        /** @brief Compiles `fread(handle, count)`: a string of up to `count` bytes, shorter only at the end of the file. */
        Val genFread(const Expr& e) {
            needArgs(e, 2);
            Val h = intArg(*e.args[0]);
            const size_t m = holdMark();
            hold(h);
            Val n = intArg(*e.args[1]);
            release(m);
            int rd = allocReg(e.loc);
            u8(FREAD); u8(rd); u8(h.reg); u8(n.reg);
            freeVal(h);
            freeVal(n);
            return strVal(rd);
        }

        /** @brief Compiles `freadline(handle)`: one line without its line ending; empty at the end of the file (see `feof`). */
        Val genFreadline(const Expr& e) {
            needArgs(e, 1);
            Val h = intArg(*e.args[0]);
            int rd = allocReg(e.loc);
            u8(FREADLINE); u8(rd); u8(h.reg);
            freeVal(h);
            return strVal(rd);
        }

        /** @brief Compiles `fwrite(handle, text)`: true if all of it was written. */
        Val genFwrite(const Expr& e) {
            needArgs(e, 2);
            Val h = intArg(*e.args[0]);
            const size_t m = holdMark();
            hold(h);
            Val s = stringArg(*e.args[1]);
            release(m);
            int rd = allocReg(e.loc);
            u8(FWRITE); u8(rd); u8(h.reg); u8(s.reg);
            dropFresh(s);
            freeVal(h);
            freeVal(s);
            return tempVal(rd, K::Bool);
        }

        /** @brief Compiles `feof(handle)`: true if no data is left to read. */
        Val genFeof(const Expr& e) {
            needArgs(e, 1);
            Val h = intArg(*e.args[0]);
            int rd = allocReg(e.loc);
            u8(FEOF); u8(rd); u8(h.reg);
            freeVal(h);
            return tempVal(rd, K::Bool);
        }

        /** @brief Compiles `file_error()`: why the last `fopen` / `fclose` / `fwrite` failed (`""` if none has). */
        Val genFileError(const Expr& e) {
            needArgs(e, 0);
            int rd = allocReg(e.loc);
            u8(FERROR); u8(rd);
            return strVal(rd);
        }

        /** @brief Compiles `read_file(path)`: the whole file as a string. A file that cannot be read is a run-time error. */
        Val genReadFile(const Expr& e) {
            needArgs(e, 1);
            Val p = stringArg(*e.args[0]);
            int rd = allocReg(e.loc);
            u8(FREADFILE); u8(rd); u8(p.reg);
            dropFresh(p);
            freeVal(p);
            return strVal(rd);
        }

        /** @} */
        // ------------------------------------------------------------ calls
        /**
         * @brief Compiles a call: a built-in, a struct constructor, or a user (or extern) function.
         *
         * The name is looked up in this order: the reserved built-ins (`print`, `len`, the casts), struct constructors, the
         * program's own functions (including `extern fn`s; they win over the library functions), then the library functions
         * (`input`, `str`, `substr`, `fopen`, ...).
         *
         * @throws CompileError "unknown function" otherwise.
         */
        Val genCall(const Expr& e) {
            const std::string& n = e.s;
            if (n == "print") return genPrint(e);
            if (n == "len") return genLen(e);
            if (n == "int") return genCast(e, Type{ K::Int });
            if (n == "uint") return genCast(e, Type{ K::UInt });
            if (n == "float") return genCast(e, Type{ K::Float });
            if (n == "bool") return genCast(e, Type{ K::Bool });
            if (n == "byte") return genCast(e, Type{ K::Byte });
            auto si = C.structIdx.find(n);
            if (si != C.structIdx.end()) return genCtor(e, si->second);
            auto fi = C.funcIdx.find(n);
            if (fi != C.funcIdx.end()) return callUser(e, fi->second);
            if (n == "input") return genInput(e);
            if (n == "eof") return genEof(e);
            if (n == "str") return genToStr(e);
            if (n == "chr") return genChr(e);
            if (n == "substr") return genSubstr(e);
            if (n == "find") return genFind(e);
            if (n == "is_int") return genIsNumber(e, T_INT);
            if (n == "is_uint") return genIsNumber(e, T_UINT);
            if (n == "is_float") return genIsNumber(e, T_FLOAT);
            if (n == "copy") return genCopy(e);
            if (n == "fopen") return genFopen(e);
            if (n == "fclose") return genFclose(e);
            if (n == "fread") return genFread(e);
            if (n == "freadline") return genFreadline(e);
            if (n == "fwrite") return genFwrite(e);
            if (n == "feof") return genFeof(e);
            if (n == "file_error") return genFileError(e);
            if (n == "read_file") return genReadFile(e);
            unknownFunction(e);
        }

        /** @brief Reports a call to an undefined function, suggesting the likely intended name (including those other languages use, such as `println`). */
        [[noreturn]] void unknownFunction(const Expr& e) const {
            const std::string& n = e.s;
            CompileError err{ e.loc, "unknown function '" + n + "'", {}, {} };
            static const std::pair<const char*, const char*> aliases[] = {
                {"println", "print"}, {"printf", "print"}, {"puts", "print"}, {"echo", "print"}, {"length", "len"}, {"size", "len"},
                {"strlen", "len"}, {"to_string", "str"}, {"tostring", "str"}, {"toString", "str"}, {"itoa", "str"}, {"string", "str"},
                {"atoi", "int"}, {"parseInt", "int"}, {"parse_int", "int"}, {"substring", "substr"}, {"readline", "input"},
                {"read_line", "input"}, {"gets", "input"}, {"scanf", "input"}, {"clone", "copy"}, {"strcpy", "copy"},
                {"char", "chr"}, {"double", "float"}, {"boolean", "bool"},
                {"open", "fopen"}, {"close", "fclose"}, {"fgets", "freadline"}, {"readfile", "read_file"},
                {"readFile", "read_file"}, {"file_get_contents", "read_file"}, {"fputs", "fwrite"}, {"fprintf", "fwrite"},
                {"write", "fwrite"}, {"strerror", "file_error"}, {"ferror", "file_error"}, {"fileerror", "file_error"} };
            std::string guess;
            for (const auto& a : aliases) if (n == a.first) guess = a.second;
            if (guess.empty()) {
                std::vector<std::string> names;
                for (const auto& kv : C.funcIdx) names.push_back(kv.first);
                for (const auto& kv : C.structIdx) names.push_back(kv.first);
                for (const char* b : { "print", "len", "int", "uint", "float", "bool", "byte", "input", "eof", "str", "chr", "substr",
                    "find", "is_int", "is_uint", "is_float", "copy", "fopen", "fclose", "fread", "freadline", "fwrite", "feof",
                    "file_error", "read_file" }) names.push_back(b);
                guess = suggestName(n, names);
            }
            if (findVar(n) >= 0) err.hint("'" + n + "' is a variable, not a function");
            else if (!guess.empty()) {
                err.hint("did you mean '" + guess + "'?");
                if (n == "println" || n == "printf" || n == "puts" || n == "echo")
                    err.hint("did you mean 'print'? (it does not add a newline: print(\"text\\n\"))");
            }
            else err.hint("declare it with 'fn " + n + "(...) { ... }'");
            throw err;
        }

        /** @brief Compiles `copy(x)`: a new, independent copy of an array (every row included) or of a string (`ARRCLONE`). A temporary argument is released. */
        Val genCopy(const Expr& e) {
            needArgs(e, 1);
            Val a = gen(*e.args[0]);
            if (a.t.k == K::None) noValue(e.args[0]->loc);
            if (a.t.k != K::Array && a.t.k != K::String)
                fail(CompileError{ e.args[0]->loc, "copy() needs an array or a string, found '" + tn(a.t) + "'", {}, {} }
                    .hint("a struct is copied field by field"));
            int rd = allocReg(e.loc);
            u8(ARRCLONE); u8(rd); u8(a.reg); u8(a.t.k == K::Array ? a.t.depth : 1);
            if (a.t.k == K::String) { u8(REINTERPRET); u8(rd); u8(T_STRINGADDRESS); }
            const Type t = a.t;
            dropFresh(a);  // copying a temporary: the original is released
            freeVal(a);
            if (t.k == K::String) return strVal(rd);
            return arrayVal(rd, t);
        }

        /**
         * @brief Compiles `int(x)`, `uint(x)`, `float(x)`, `bool(x)` and `byte(x)`.
         *
         * A number is converted (constants at compile time, otherwise with `CAST`); a string is parsed instead (see genParse()).
         *
         * @param e  The call expression (exactly one argument).
         * @param to The target type.
         */
        Val genCast(const Expr& e, Type to) {
            if (e.args.size() != 1) fail(e.loc, "'" + e.s + "' expects exactly 1 argument");
            Val v = gen(*e.args[0]);
            if (v.t.k == K::None) noValue(e.loc);
            if (v.t.k == K::String) {
                if (to.k == K::Int || to.k == K::UInt || to.k == K::Float) return genParse(e, v, to);
                fail(e.loc, "cannot convert a string to '" + tn(to) + "'; only int(), uint() and float() can parse one");
            }
            if (v.t.k == K::Struct || v.t.k == K::Address || v.t.k == K::Array)
                fail(e.loc, "cannot cast a value of type '" + tn(v.t) + "'");
            if (v.untyped) { v.untyped = false; v.t = Type{ K::Int }; }
            if (v.imm) return convertImm(v, to);
            if (v.t == to) return v;
            Val r = v;
            if (!v.temp) {
                r.reg = allocReg(e.loc);
                r.temp = true;
                emitMov(r.reg, v.reg);
            }
            u8(CAST); u8(r.reg); u8(tagOf(to));
            r.t = to;
            return r;
        }

        /** @brief Compiles `print(x)`: writes a number, bool, byte or string to standard output, without a trailing newline. Returns a value-less Val. */
        Val genPrint(const Expr& e) {
            if (e.args.size() != 1) fail(e.loc, "'print' expects exactly 1 argument");
            Val v = gen(*e.args[0]);
            if (v.t.k == K::None) noValue(e.args[0]->loc);
            if (v.t.k == K::Struct) fail(e.args[0]->loc, "cannot print a struct; print its fields instead");
            if (v.t.k == K::Array) fail(e.args[0]->loc, "cannot print an array; print its elements instead");
            if (v.t.k == K::String) {
                u8(STRPRINT); u8(v.reg);
                dropFresh(v);
            }
            else {
                v = toReg(v, e.loc);
                u8(PRINT); u8(v.reg);
            }
            freeVal(v);
            return Val{};
        }

        /** @brief True for an argument that has no source register: a constant, or a value waiting in a frame slot. It never blocks another move. */
        static bool sourceless(const Val& v) { return v.imm || v.spillSlot >= 0; }

        /**
         * @brief Performs register moves (destination register, source) as if they all happened at once.
         *
         * A move waits while a pending move still has to read its destination, and a cycle is broken by parking one register
         * in a scratch register that is not in @p dstMask.
         *
         * @param[in,out] mv      The moves: destination register and source value (register, constant, or spill slot).
         * @param[in]     dstMask Bit set of all the destination registers.
         * @param[in]     l       Where to report a failure to find a scratch register.
         */
        void parallelMove(std::vector<std::pair<int, Val>>& mv, uint64_t dstMask, Loc l) {
            mv.erase(std::remove_if(mv.begin(), mv.end(),
                [](const std::pair<int, Val>& m) { return !sourceless(m.second) && m.second.reg == m.first; }),
                mv.end());
            std::vector<int> tmps;
            while (!mv.empty()) {
                bool progress = false;
                for (size_t i = 0; i < mv.size();) {
                    int d = mv[i].first;
                    bool blocked = false;
                    for (size_t j = 0; j < mv.size(); ++j)
                        if (j != i && !sourceless(mv[j].second) && mv[j].second.reg == d) { blocked = true; break; }
                    if (blocked) { ++i; continue; }
                    const Val& src = mv[i].second;
                    if (src.imm) emitMovImm(d, src);
                    else if (src.spillSlot >= 0) {
                        emitSlotAddr(d, nextSlot_ + src.spillSlot, false);
                        u8(LOADLOCAL); u8(d); u8(d);
                        tslots_[src.spillSlot] = 0;
                    }
                    else emitMov(d, src.reg);
                    mv.erase(mv.begin() + (long)i);
                    progress = true;
                }
                if (!progress) {  // cycle: park one destination in a scratch register
                    int d = mv[0].first;
                    int t = allocReg(l, dstMask);
                    emitMov(t, d);
                    for (auto& m : mv)
                        if (!sourceless(m.second) && m.second.reg == d) m.second.reg = t;
                    tmps.push_back(t);
                }
            }
            for (int t : tmps) freeReg(t);
        }

        /**
         * @brief Compiles a call to a user function or an `extern fn`.
         *
         * Arguments go in `r0..` (up to V of them) and the rest are pushed on the stack; the result comes back in `r0`.
         * Caller-saved registers are pushed around the call (only those the callee may clobber), and a fresh heap value passed
         * as an argument is released by the caller afterwards.
         *
         * The sequence is: evaluate and type-check the arguments (each is "held" and may be spilled meanwhile); make room;
         * `PUSH` the live registers the callee may clobber; `PUSH` the addresses of fresh heap arguments (to free them later);
         * `PUSH` the stack arguments; move the register arguments into `r0..` with parallelMove(); emit `CALL` (or `CALLC` with
         * the import index for an `extern fn`); copy `r0` to a new temporary; drop the stack arguments with `LEAVE`; `POP`
         * everything back; free the fresh arguments. Nothing may be spilled between the first `PUSH` and the last `POP`.
         *
         * @param e  The call expression.
         * @param fi Index of the callee in Compiler::funcs.
         * @return The result (fresh if the callee returns a heap value), or a value-less Val for a `void` function.
         * @throws CompileError for a wrong argument count or argument type.
         */
        Val callUser(const Expr& e, int fi) {
            FuncInfo& cf = C.funcs[fi];
            if (e.args.size() != cf.ptypes.size()) {
                const size_t want = cf.ptypes.size();
                fail(CompileError{ e.loc, "'" + cf.name + "' expects " + std::to_string(want) + " argument" + (want == 1 ? "" : "s") +
                    ", found " + std::to_string(e.args.size()), {}, {} }
                    .note(cf.loc, "'" + cf.name + "' is declared here: " + C.signatureOf(cf)));
            }
            const bool wantTail = (&e == tailExpr_);  // genReturn() / genStmt() name the call that is in tail position
            tailExpr_ = nullptr;                      // ... and only that one: calls inside its arguments are not
            callees_.push_back(fi);
            // callees are compiled first (see Compiler::compile); a callee that is not finished yet
            // is part of a cycle (recursion), so assume it clobbers every register
            uint64_t cmask = cf.state == 2 ? cf.mask : C.fullMask();
            calleeUnion_ |= cmask;

            std::vector<Val> av;
            av.reserve(e.args.size());  // the holds below point into it
            const size_t hm = holdMark();
            for (size_t i = 0; i < e.args.size(); ++i) {
                const Param& pd = cf.def->params[i];
                Ctx cx{ "in argument " + std::to_string(i + 1) + " of '" + cf.name + "'", pd.loc,
                    "parameter '" + pd.name + "' is declared here as '" + tn(cf.ptypes[i]) + "'" };
                Val v = coerce(gen(*e.args[i], -1, cf.ptypes[i]), cf.ptypes[i], e.args[i]->loc, &cx);
                av.push_back(v);
                hold(av.back());  // an argument already computed waits (and may be spilled) while the next one is
            }
            const size_t k = av.size(), nreg = std::min<size_t>(k, (size_t)V), nstack = k - nreg;
            // A register argument that had to be spilled is not brought back into a temporary only to be moved again:
            // it is loaded from its slot into its argument register by the parallel move below.
            for (size_t i = 0; i < k; ++i) av[i].keepSpilled = !av[i].fresh;
            release(hm);
            uint64_t argMask = nreg ? ((1ull << nreg) - 1) : 0;
            cmask |= argMask | 1ull;

            // A call in tail position needs no frame of its own: when nothing here has to outlive it, it becomes a jump.
            if (wantTail) {
                while (freeCount(0) < 4 && spillOne(0)) {}
                if (canTailCall(e, fi, av, nreg)) {
                    emitTailCall(e, fi, av, nreg, argMask);
                    didTail_ = true;
                    return Val{};
                }
            }

            // From the first PUSH to the last POP below nothing may be spilled: a register that is pushed here and
            // popped after the call would get its old value back, and a spill would have stored the wrong one. So
            // make sure now that the few registers the sequence needs are free.
            while (freeCount(0) < 3 && spillOne(0)) {}
            ++noSpill_;

            uint64_t argTemps = 0;
            for (const Val& v : av)
                if (v.temp) argTemps |= 1ull << v.reg;
            uint64_t save = used_ & ~argTemps & cmask;
            std::vector<int> saved;
            for (int r = 0; r < N; ++r)
                if (save >> r & 1) { saved.push_back(r); u8(PUSH); u8(r); }

            // A heap temporary passed as an argument (f("a" + b)) belongs to nobody once the callee
            // returns, so the caller frees it. The callee may clobber any register, so its address
            // is parked on the stack for the duration of the call.
            std::vector<int> fresh;
            std::vector<Type> freshT;
            for (const Val& v : av)
                if (v.fresh) { fresh.push_back(v.reg); freshT.push_back(v.t); u8(PUSH); u8(v.reg); }

            // stack arguments first (they read registers that the moves below may overwrite)
            for (size_t i = nreg; i < k; ++i) {
                Val& v = av[i];
                if (v.imm || v.spillSlot >= 0) {
                    int t = allocReg(e.loc);
                    if (v.imm) {
                        emitMovImm(t, v);
                    }
                    else {  // it was spilled while the later arguments were computed: bring it back just for the push
                        emitSlotAddr(t, nextSlot_ + v.spillSlot, false);
                        u8(LOADLOCAL); u8(t); u8(t);
                        tslots_[v.spillSlot] = 0;
                    }
                    u8(PUSH); u8(t);
                    freeReg(t);
                }
                else {
                    u8(PUSH); u8(v.reg);
                    if (!v.fresh) freeVal(v);
                }
            }

            std::vector<std::pair<int, Val>> mv;
            for (size_t i = 0; i < nreg; ++i) mv.push_back({ (int)i, av[i] });
            parallelMove(mv, argMask, e.loc);

            if (cf.isExtern) {
                // a C function: same argument placement as CALL, the VM finds the rest in the import table
                u8(CALLC);
                hostCalls_.push_back({ (uint32_t)code.size(), fi });
                u16(0);
                u8((unsigned)nreg);
            }
            else {
                u8(CALL); u8(1); u8(0);
                calls_.push_back({ (uint32_t)code.size(), fi });
                u32(0);
            }

            for (Val& v : av)
                if (!v.fresh) freeVal(v);  // fresh ones stay allocated until they are freed below
            const bool isVoid = cf.ret.k == K::None;
            int rt = -1;
            if (!isVoid) {
                rt = allocReg(e.loc);
                emitMov(rt, 0);
            }
            if (nstack) { u8(LEAVE); u32((uint32_t)nstack); }  // drop the pushed arguments
            for (size_t i = fresh.size(); i-- > 0;) { u8(POP); u8(fresh[i]); }
            for (size_t i = saved.size(); i-- > 0;) { u8(POP); u8(saved[i]); }
            --noSpill_;
            for (size_t i = 0; i < fresh.size(); ++i) { emitFree(fresh[i], freshT[i]); freeReg(fresh[i]); }

            Val r;
            if (isVoid) return r;  // no value: Val{} has type none, so any use of it reports "this expression has no value"
            r.t = cf.ret;
            r.reg = rt;
            r.temp = true;
            r.fresh = cf.ret.heap();
            return r;
        }

        // ------------------------------------------------------------ tail calls
        /**
         * @name Tail calls
         * @brief A call whose result is returned as it is (`return f(x);`, or `f(x);` at the end of a `void` function)
         *        does not need the caller's frame, so it is compiled as a jump and deep recursion no longer grows the stack.
         *
         * The sequence replaces `CALL ... RET`: the arguments are evaluated as for any call; then everything the frame owns
         * is released (nothing runs after the jump), the arguments are moved into place, and
         *  - a call to the function itself overwrites its own parameters and jumps back to the start of the body (the frame
         *    is reused as it is);
         *  - a call to another function drops the frame (`LEAVE`) and jumps to its entry; its `RET` then returns straight to
         *    our caller. That is only possible if the callee takes its arguments in registers, because the arguments on
         *    the stack belong to our caller, who removes them after the return.
         *
         * The call stays an ordinary call when
         *  - the callee is an `extern fn` or returns a different type;
         *  - an argument is a heap value that does not outlive this frame (a fresh temporary, which the caller would free
         *    after the call, or something that belongs to a local variable, which is released before the jump). Only values
         *    reached from a parameter or a global are passed on.
         * @{
         */

        /**
         * @brief Records the statements of a `void` function body that are the last thing the function does.
         *
         * Those are the statement before a bare `return;`, and the last statement of the body, including the last statements
         * of the branches of an `if` and the contents of a block in that position. Loop bodies are never in tail position.
         *
         * @param body    A statement list.
         * @param tailPos The list is in tail position: control leaves the function when it ends.
         */
        void markTail(const std::vector<StmtP>& body, bool tailPos) {
            for (size_t i = 0; i < body.size(); ++i) {
                const Stmt& s = *body[i];
                const bool beforeReturn = i + 1 < body.size() && body[i + 1]->k == Stmt::Return && !body[i + 1]->e;
                const bool tail = beforeReturn || (tailPos && i + 1 == body.size());
                switch (s.k) {
                case Stmt::ExprS:
                    if (tail && s.e && s.e->k == Expr::Call) tailStmts_.insert(&s);
                    break;
                case Stmt::If: markTail(s.body, tail); markTail(s.els, tail); break;
                case Stmt::Block: case Stmt::Unsafe: markTail(s.body, tail); break;
                case Stmt::While: case Stmt::For: markTail(s.body, false); break;
                default: break;
                }
            }
        }

        /** @brief True if the heap value of @p a is reached from a parameter or a global, so it outlives this frame and the caller's own references to it. */
        bool outlivesFrame(const Expr& a) const {
            switch (a.k) {
            case Expr::Ident: {
                const int vi = findVar(a.s);
                return vi >= 0 && !vars_[vi].bad && (vars_[vi].isParam || vars_[vi].global);
            }
            case Expr::Member: case Expr::Index: return a.a && outlivesFrame(*a.a);
            default: return false;
            }
        }

        /**
         * @brief Whether the call @p e (whose arguments @p av have just been evaluated) may be compiled as a jump.
         * @param e    The call.
         * @param fi   The callee.
         * @param av   The evaluated arguments.
         * @param nreg How many of them go in registers.
         */
        bool canTailCall(const Expr& e, int fi, const std::vector<Val>& av, size_t nreg) const {
            const FuncInfo& cf = C.funcs[fi];
            if (cf.isExtern || cf.ret != F.ret) return false;
            if (fi != selfIdx_ && av.size() != nreg) return false;  // stack arguments belong to our caller's frame
            // emitTailCall() needs scratch registers, and running out of them there would be a compile error that the
            // ordinary call does not have, so count them first, phase by phase:
            //  - storing a stack argument takes one (the slot's address), and two for one that was spilled (it is loaded
            //    first); the register of a stored argument is free again;
            //  - releasing the frame takes up to three (the address of a local's slot and two for a struct's strings);
            //  - the moves into the argument registers take one, to break a cycle.
            const bool self = fi == selfIdx_;
            int fr = freeCount(0);
            if (self) {
                for (int pass = 0; pass < 2; ++pass)  // the same order as emitTailCall(): values in registers first, then spilled ones
                    for (size_t i = nreg; i < av.size(); ++i) {
                        const bool spilled = av[i].spillSlot >= 0;
                        if (spilled != (pass == 1)) continue;
                        if (fr < (spilled ? 2 : 1)) return false;
                        if (av[i].temp && av[i].reg >= 0) ++fr;
                    }
            }
            int release = 0;  // the most scratch registers freeOwned() takes for one variable
            for (const Var& v : vars_) {
                if (!v.owned) continue;
                const int need = (v.inReg ? 0 : 1) + (v.t.k == K::Struct && C.structs[v.t.sid].hasStrings ? 2 : 0);
                release = std::max(release, need);
            }
            if (fr < std::max(release, 1)) return false;
            for (size_t i = 0; i < av.size(); ++i) {
                if (!cf.ptypes[i].heap()) continue;
                if (av[i].fresh || !outlivesFrame(*e.args[i])) return false;
            }
            return true;
        }

        /**
         * @brief Emits the jump that replaces `CALL` + `RET` (see the description above); canTailCall() must have said yes.
         * @param e       The call.
         * @param fi      The callee.
         * @param av      The evaluated arguments.
         * @param nreg    How many of them go in registers.
         * @param argMask The set of argument registers.
         */
        void emitTailCall(const Expr& e, int fi, std::vector<Val>& av, size_t nreg, uint64_t argMask) {
            const bool self = fi == selfIdx_;
            const size_t k = av.size();

            if (self) {  // stack parameters: overwrite this frame's own (the last one is at offset 0)
                for (int pass = 0; pass < 2; ++pass)  // values in registers first: storing them frees registers for the spilled ones
                for (size_t i = nreg; i < k; ++i) {
                    Val& v = av[i];
                    if ((v.spillSlot >= 0) != (pass == 1)) continue;
                    const int slot = (int)(k - 1 - i);
                    if (v.spillSlot >= 0) {
                        Val t;
                        t.t = v.t;
                        t.reg = allocReg(e.loc);
                        emitSlotAddr(t.reg, nextSlot_ + v.spillSlot, false);
                        u8(LOADLOCAL); u8(t.reg); u8(t.reg);
                        tslots_[v.spillSlot] = 0;
                        storeSlot(slot, t, true);
                        freeReg(t.reg);
                    }
                    else {
                        storeSlot(slot, v, true);
                        freeVal(v);
                    }
                }
            }

            // As `return` does: release what the frame owns. The arguments are computed, and none of them refers to it.
            // (The parameters are not owned, so neither the stores above nor the moves below are in its way.)
            cleanupAll(nullptr);

            std::vector<std::pair<int, Val>> mv;
            for (size_t i = 0; i < nreg; ++i) mv.push_back({ (int)i, av[i] });
            parallelMove(mv, argMask, e.loc);
            for (Val& v : av) freeVal(v);

            if (self) {
                jump(JMP, start_);
            }
            else {
                u8(LEAVE);                                // drop the frame: the callee starts where it began
                leavePatches_.push_back((uint32_t)code.size());
                u32(0);
                u8(JMP); u8(1); u8(0);
                calls_.push_back({ (uint32_t)code.size(), fi });
                u32(0);
            }
        }
        /** @} */

        // ------------------------------------------------------------ statements
        /**
         * @brief Compiles a statement list.
         * @param body    The statements.
         * @param funcTop Marks the function's outermost list, whose last statement may fall through to the epilogue instead of jumping to it.
         * @return true if control can never fall out of its end.
         */
        bool genStmts(const std::vector<StmtP>& body, bool funcTop) {
            for (size_t i = 0; i < body.size(); ++i) {
                bool tail = funcTop && i + 1 == body.size();
                if (genGuarded(*body[i], tail)) return true;
            }
            return false;
        }

        // ---- error recovery ---------------------------------------------------------------------------------
        /**
         * @brief The generator's bookkeeping at the start of a statement, used for error recovery.
         *
         * A statement that fails is reported and abandoned, and compilation carries on with the next one, so a single run
         * lists every independent mistake in a function. The generator's bookkeeping is put back to what it was before the
         * statement (the code emitted so far is junk, but it is never used: an erroneous program produces no output). A
         * failed `set` still declares its name - with the annotated type if there is one, otherwise as "bad" - so later uses
         * neither cascade into "undefined variable" nor are checked against a guessed type.
         */
        struct Snapshot {
            size_t vars,    ///< Size of `vars_`.
                   scopes,  ///< Size of `scopes_`.
                   loops;   ///< Size of `loops_`.
            int unsafe,     ///< Saved `unsafe_`.
                varRegs,    ///< Saved `varRegs_`.
                nextSlot;   ///< Saved `nextSlot_`.
            uint64_t used;  ///< Saved set of registers in use.
        };
        /** @brief Captures the current bookkeeping. */
        Snapshot snapshot() const {
            return { vars_.size(), scopes_.size(), loops_.size(), unsafe_, varRegs_, nextSlot_, used_ };
        }

        /**
         * @brief Compiles one statement; a compile error is reported and the statement abandoned, so that the next one is still checked.
         * @param s    The statement.
         * @param tail The statement is the last of the function's outermost list.
         * @return true if control can never continue past the statement.
         */
        bool genGuarded(const Stmt& s, bool tail) {
            const Snapshot sn = snapshot();
            try {
                return genStmt(s, tail);
            }
            catch (CompileError& e) {
                C.report(std::move(e));
            }
            catch (SilentError&) {
            }
            restore(sn, s);
            return false;
        }

        /** @brief Puts the generator back into the state saved before statement @p s failed, and re-declares what @p s would have declared. */
        void restore(const Snapshot& sn, const Stmt& s) {
            holds_.clear();  // the statement was abandoned half way: whatever it held is gone
            tslots_.clear();
            pinFloor_ = 0;
            noSpill_ = 0;
            vars_.resize(sn.vars);
            scopes_.resize(sn.scopes);
            loops_.resize(sn.loops);
            unsafe_ = sn.unsafe;
            varRegs_ = sn.varRegs;
            nextSlot_ = sn.nextSlot;
            used_ = sn.used;
            if (s.k != Stmt::Set && s.k != Stmt::Global) return;

            Type ty;
            bool typed = false;
            if (s.ty.set) {
                try { ty = C.resolveType(s.ty); typed = true; }
                catch (CompileError&) {}  // already reported when the statement was compiled
            }
            if (s.k == Stmt::Global) {
                for (const GlobalInfo& g : C.globals) if (g.name == s.name) return;
                GlobalInfo g;
                g.name = s.name;
                g.t = ty;
                g.slot = (int)C.globals.size();
                g.loc = s.nameLoc;
                g.isConst = s.isConst;
                g.bad = !typed;
                C.globals.push_back(g);
                vars_.push_back(globalVar(g));
                return;
            }
            for (size_t i = scopes_.back(); i < vars_.size(); ++i)
                if (vars_[i].name == s.name) return;  // a redeclaration: the first one stays
            Var v;
            v.name = s.name;
            v.t = ty;
            v.inReg = false;
            v.param = true;  // "lives in a caller-pushed slot": scope exit then touches no registers or frame slots
            v.isConst = s.isConst;
            v.loc = s.nameLoc;
            v.bad = !typed;
            vars_.push_back(v);
        }

        /** @brief Compiles a block in a scope of its own. @return true if the block can never complete normally. */
        bool genScoped(const std::vector<StmtP>& body) {
            pushScope();
            bool term = genStmts(body, false);
            popScope(!term);
            return term;
        }

        /**
         * @brief Compiles one statement.
         *
         * `while` loops are inverted so that there is one conditional jump per iteration. `for (init; cond; step) body` is
         * `{ init; while (cond) { body; step } }` where `continue` runs `step`; the initializer's variable lives in a scope of
         * its own that covers the whole loop, and `break` lands before that scope's cleanup so the variable is released on every
         * way out. `break` and `continue` free what the scopes they leave own, exactly as `return` does, then jump.
         *
         * @param s    The statement.
         * @param tail The statement is the last of the function's outermost list.
         * @return true if control can never continue past it (`return`, `break`, `continue`, or an `if`/`else` whose
         *         branches all end that way).
         */
        bool genStmt(const Stmt& s, bool tail) {
            switch (s.k) {
            case Stmt::Set: genSet(s); return false;
            case Stmt::Assign: genAssign(s); return false;
            case Stmt::FieldAssign: genFieldAssign(s); return false;
            case Stmt::IndexAssign: genIndexAssign(s); return false;
            case Stmt::ExprS: {
                if (tailStmts_.count(&s)) tailExpr_ = s.e.get();  // the last thing a void function does: may become a jump
                Val v = gen(*s.e);
                tailExpr_ = nullptr;
                if (didTail_) { didTail_ = false; return true; }  // it jumped: nothing after it can run
                if (v.fresh) emitFree(v.reg, v.t);  // discarded heap result
                freeVal(v);
                return false;
            }
            case Stmt::Block: return genScoped(s.body);
            case Stmt::Unsafe: {
                ++unsafe_;
                bool term = genScoped(s.body);
                --unsafe_;
                return term;
            }
            case Stmt::If: {
                Label lelse, lend;
                genCond(*s.e, lelse, false);
                bool t1 = genScoped(s.body);
                if (s.els.empty()) {
                    bind(lelse);
                    return false;
                }
                if (!t1) jump(JMP, lend);
                bind(lelse);
                bool t2 = genScoped(s.els);
                bind(lend);
                return t1 && t2;
            }
            case Stmt::While: {
                // loop inversion: one conditional jump per iteration
                Label lbody, lcond, lbrk;
                jump(JMP, lcond);
                bind(lbody);
                loops_.push_back({ &lbrk, &lcond, vars_.size() });
                genScoped(s.body);
                loops_.pop_back();
                bind(lcond);
                genCond(*s.e, lbody, true);
                bind(lbrk);
                return false;
            }
            case Stmt::For: {
                // for (init; cond; step) body  ==  { init; while (cond) { body; step } }  but 'continue' runs step.
                // The initializer's variable lives in a scope of its own that covers the whole loop; 'break' lands
                // before that scope's cleanup so the variable is released on every way out.
                Label lbody, lcont, lcheck, lbrk;
                pushScope();
                if (s.init) genGuarded(*s.init, false);
                jump(JMP, lcheck);
                bind(lbody);
                loops_.push_back({ &lbrk, &lcont, vars_.size() });
                genScoped(s.body);
                loops_.pop_back();
                bind(lcont);
                if (s.step) genGuarded(*s.step, false);
                bind(lcheck);
                if (s.e) genCond(*s.e, lbody, true);
                else jump(JMP, lbody);
                bind(lbrk);
                popScope(true);
                return false;
            }
            case Stmt::Break:
            case Stmt::Continue: {
                const bool isBreak = s.k == Stmt::Break;
                if (loops_.empty())
                    fail(CompileError{ s.loc, std::string("'") + (isBreak ? "break" : "continue") + "' outside of a loop", {}, {} }
                        .hint("it only makes sense inside a 'while' or 'for' body"));
                const Loop& L = loops_.back();
                // like 'return': free what the scopes we are leaving own (innermost first), then jump
                for (size_t i = vars_.size(); i-- > L.varStart;) freeOwned(vars_[i]);
                jump(JMP, isBreak ? *L.brk : *L.cont);
                return true;  // nothing after it in this block can run
            }
            case Stmt::Global: genGlobal(s); return false;
            case Stmt::Return: genReturn(s, tail); return true;
            }
            return false;
        }

        /**
         * @brief Compiles `set [const] name [: type] = value;` (a local variable).
         * @throws CompileError if the name is already declared in this scope or the initializer does not match the annotation.
         */
        void genSet(const Stmt& s) {
            for (size_t i = scopes_.back(); i < vars_.size(); ++i)
                if (vars_[i].name == s.name) {
                    CompileError e{ s.nameLoc, "variable '" + s.name + "' is already declared in this scope", {}, {} };
                    if (vars_[i].loc.line > 0) e.note(vars_[i].loc, "previous declaration of '" + s.name + "' is here");
                    e.hint("to change its value, assign without 'set': " + s.name + " = ...;");
                    throw e;
                }
            Val v;
            Type ty;
            if (s.ty.set) {
                ty = C.resolveType(s.ty);
                Ctx cx{ "in the initializer of '" + s.name + "'", s.ty.loc, "'" + s.name + "' is declared as '" + tn(ty) + "' here" };
                v = coerce(gen(*s.e, -1, ty), ty, s.e->loc, &cx);
            }
            else {
                v = gen(*s.e);
                if (v.untyped) v = coerce(v, Type{ K::Int }, s.e->loc);
                if (v.t.k == K::None) noValue(s.e->loc);
                ty = v.t;
            }
            declare(s.name, ty, v, s.isConst, s.nameLoc);
        }

        /**
         * @brief Compiles a top-level `set`.
         *
         * Only ever compiled as part of the synthetic initializer function, which the startup stub calls before `main`; the
         * value is stored into the global's absolute data-stack slot, and the global becomes visible to the initializers after it.
         *
         * @throws CompileError for a duplicate global or too many globals.
         */
        void genGlobal(const Stmt& s) {
            for (const GlobalInfo& g : C.globals)
                if (g.name == s.name) {
                    CompileError e{ s.nameLoc, "global '" + s.name + "' is already declared", {}, {} };
                    if (g.loc.line > 0) e.note(g.loc, "previous declaration of '" + s.name + "' is here");
                    throw e;
                }
            if ((int)C.globals.size() >= C.tg.stack / 2)
                fail(s.loc, "too many globals (at most " + std::to_string(C.tg.stack / 2) + ")");
            Val v;
            Type ty;
            if (s.ty.set) {
                ty = C.resolveType(s.ty);
                Ctx cx{ "in the initializer of '" + s.name + "'", s.ty.loc, "'" + s.name + "' is declared as '" + tn(ty) + "' here" };
                v = coerce(gen(*s.e, -1, ty), ty, s.e->loc, &cx);
            }
            else {
                v = gen(*s.e);
                if (v.untyped) v = coerce(v, Type{ K::Int }, s.e->loc);
                if (v.t.k == K::None) noValue(s.e->loc);
                ty = v.t;
            }
            GlobalInfo g;
            g.name = s.name;
            g.t = ty;
            g.slot = (int)C.globals.size();
            g.loc = s.nameLoc;
            g.isConst = s.isConst;
            storeGlobal(g.slot, v);  // a fresh heap value is now owned by the global, i.e. by the whole program
            freeVal(v);
            C.globals.push_back(g);
            vars_.push_back(globalVar(g));  // visible to the initializers after this one
        }

        /**
         * @brief Compiles `name = value;`.
         *
         * A variable that owns a heap value releases the old one first, so the new value must be a fresh one. A parameter,
         * alias or global of heap type cannot be reassigned.
         *
         * @throws CompileError if the variable is `const`, or ownership rules are broken.
         */
        void genAssign(const Stmt& s) {
            int vi = useVar(s.name, s.nameLoc);
            if (vars_[vi].isConst) {
                CompileError e{ s.nameLoc, "cannot assign to '" + s.name + "': it is declared const", {}, {} };
                if (vars_[vi].loc.line > 0) e.note(vars_[vi].loc, "'" + s.name + "' is declared const here");
                throw e;
            }
            Type vt = vars_[vi].t;
            int hint = (vars_[vi].inReg && !vt.heap()) ? vars_[vi].reg : -1;
            Ctx cx{ "in assignment to '" + s.name + "'", vars_[vi].loc, "'" + s.name + "' has type '" + tn(vt) + "' (declared here)" };
            Val v = coerce(gen(*s.e, hint, vt), vt, s.e->loc, &cx);
            const Var& var = vars_[vi];
            if (var.global && vt.heap())
                fail(CompileError{ s.loc, "global '" + s.name + "' cannot be reassigned", {}, {} }
                    .hint("a global string, array or struct is created once and lives for the whole program; "
                        "its contents can still be changed in place (" + s.name + "[i] = ..., " + s.name + ".field = ...)"));
            if (vt.heap()) {
                if (var.owned) {
                    if (!v.fresh)
                        fail(CompileError{ s.e->loc, "'" + s.name + "' owns its value, so it can only be assigned a newly created one", {}, {} }
                            .hint(vt.k == K::Struct ? std::string("assigning an existing value would share it, and it would be freed twice")
                                : "assign a copy instead: " + s.name + " = copy(...)"));
                    freeOwned(var);
                }
                else if (v.fresh) {
                    fail(CompileError{ s.e->loc, "'" + s.name + "' does not own its value, so a newly created one would leak", {}, {} }
                        .hint("'" + s.name + "' is a parameter or an alias of something else; declare a new variable with 'set' to hold it"));
                }
            }
            storeVar(var, v);
        }

        /** @brief Compiles `name.field = value;` (a string field goes through genStringFieldAssign()). */
        void genFieldAssign(const Stmt& s) {
            int vi = useVar(s.name, s.nameLoc);
            if (vars_[vi].isConst) {
                CompileError e{ s.nameLoc, "cannot modify '" + s.name + "." + s.field + "': '" + s.name + "' is declared const", {}, {} };
                if (vars_[vi].loc.line > 0) e.note(vars_[vi].loc, "'" + s.name + "' is declared const here");
                throw e;
            }
            Type bt = vars_[vi].t;
            if (bt.k != K::Struct)
                fail(CompileError{ s.nameLoc, "'" + s.name + "' is not a struct (it is '" + tn(bt) + "')", {}, {} }
                    .hint("only struct values have fields"));
            const StructField& f = fieldOf(bt.sid, s.field, s.fieldLoc);
            if (f.t.k == K::String) { genStringFieldAssign(s, vi, f); return; }
            Ctx cx{ "in assignment to field '" + s.name + "." + s.field + "'", f.loc, "field '" + f.name + "' is declared as '" + tn(f.t) + "' here" };
            Val v = toReg(coerce(gen(*s.e), f.t, s.e->loc, &cx), s.loc);
            Val base = loadVar(vars_[vi]);
            int addr = base.reg, scratch = -1;
            if (f.off) {
                scratch = allocReg(s.loc);
                u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(scratch); u8(base.reg);
                u32(f.off);
                addr = scratch;
            }
            u8(STORE); u8(tagOf(f.t)); u8(addr); u8(v.reg);
            if (scratch >= 0) freeReg(scratch);
            freeVal(base);
            freeVal(v);
        }

        /**
         * @brief Compiles `p.name = <string>;`.
         *
         * The struct owns the string in the field, so: the new value is evaluated first (which keeps `p.name = p.name + "x"`
         * working), then the old string is released and the new one stored. A value that is not a fresh temporary (a variable,
         * a parameter, another field) is copied first, so the struct never holds an alias of a string somebody else will free.
         *
         * @param s  The statement.
         * @param vi Index of the struct variable in `vars_`.
         * @param f  The string field.
         */
        void genStringFieldAssign(const Stmt& s, int vi, const StructField& f) {
            Ctx cx{ "in assignment to field '" + s.name + "." + s.field + "'", f.loc, "field '" + f.name + "' is declared as 'string' here" };
            Val v = coerce(gen(*s.e, -1, f.t), f.t, s.e->loc, &cx);
            if (!v.fresh) {  // copy it: v + ""
                int empty = allocReg(s.loc), copy = allocReg(s.loc);
                u8(STRLIT); u8(empty); u32(0);
                u8(STRCAT); u8(copy); u8(v.reg); u8(empty);
                u8(FREEMEM); u8(empty);
                freeReg(empty);
                freeVal(v);
                v = strVal(copy);
            }
            const size_t m = holdMark();
            hold(v);  // the new string waits (and may be spilled) while the old one is looked up and released
            Val base = loadVar(vars_[vi]);
            int addr = base.reg, scratch = -1;
            if (f.off) {
                scratch = allocReg(s.loc);
                u8(ADD); u8(REG_IMM); u8(T_ADDRESS); u8(scratch); u8(base.reg);
                u32(f.off);
                addr = scratch;
            }
            int old = allocReg(s.loc);
            u8(LOAD); u8(T_ADDRESS); u8(addr); u8(old);
            u8(FREEMEM); u8(old);
            freeReg(old);
            release(m);
            u8(REINTERPRET); u8(v.reg); u8(T_ADDRESS);  // STORE writes exactly the tag it is told to; v is ours to retag
            u8(STORE); u8(T_ADDRESS); u8(addr); u8(v.reg);
            if (scratch >= 0) freeReg(scratch);
            freeVal(base);
            freeVal(v);
        }

        /**
         * @brief Compiles `return [value];`.
         *
         * Releases everything the function owns except the returned variable, puts the result in `r0` and jumps to the epilogue
         * (unless this is the last statement of the function). A heap value can only be returned if the function owns it.
         *
         * @param s    The statement.
         * @param tail The statement is the last of the function: no jump is needed.
         * @throws CompileError for a wrong type, a value in a `void` function, or returning something not owned.
         */
        void genReturn(const Stmt& s, bool tail) {
            Type rt = F.ret;
            if (rt.k == K::None) {  // void: nothing to compute, r0 is left alone
                if (s.e)
                    fail(CompileError{ s.e->loc, "a void function cannot return a value", {}, {} }
                        .note(F.loc, "'" + F.name + "' is declared '-> void'")
                        .hint("remove the value, or change the function's return type"));
                cleanupAll(nullptr);
                if (!tail) jump(JMP, epilogue_);
                return;
            }
            Val v;
            const Var* except = nullptr;
            if (s.e) {
                Ctx cx{ "in the return value of '" + F.name + "'", Loc{}, "" };
                if (F.def && F.def->ret.set) { cx.noteLoc = F.def->ret.loc; cx.note = "'" + F.name + "' is declared to return '" + tn(rt) + "' here"; }
                else { cx.noteLoc = F.loc; cx.note = "'" + F.name + "' has no '->', so it returns 'int'"; }
                if (s.e->k == Expr::Call) tailExpr_ = s.e.get();  // `return f(...)`: may become a jump
                Val rv = gen(*s.e, -1, rt);
                tailExpr_ = nullptr;
                if (didTail_) { didTail_ = false; return; }       // it jumped: the callee's result goes straight to our caller
                v = coerce(rv, rt, s.e->loc, &cx);
                if (rt.heap()) {
                    if (s.e->k == Expr::Ident) {
                        int vi = findVar(s.e->s);
                        if (vi >= 0 && vars_[vi].owned) { except = &vars_[vi]; v.fresh = true; }
                    }
                    if (!v.fresh)
                        fail(CompileError{ s.e->loc, "cannot return " + std::string(rt.k == K::Struct ? "a struct" : rt.k == K::String ? "a string" : "an array") +
                            " that the function does not own (a parameter, a global or an alias)", {}, {} }
                            .hint(rt.k == K::Struct ? "build a new struct and copy the fields into it"
                                : "return a copy instead: return copy(...)"));
                }
            }
            else {
                v = zeroOf(rt);
            }
            cleanupAll(except);
            if (v.imm) emitMovImm(0, v); else emitMov(0, v.reg);
            freeVal(v);
            if (!tail) jump(JMP, epilogue_);
        }
    };

    // ------------------------------------------------------------------ Compiler
    /**
     * @brief Compiles function @p fi once.
     *
     * `state`: 0 = not started, 1 = in progress (a recursive call), 2 = done. Errors outside any statement are reported.
     */
    inline void Compiler::ensureCompiled(int fi) {
        FuncInfo& f = funcs[fi];
        if (f.state != 0) return;
        f.state = 1;
        try {
            FuncGen g(*this, f);
            g.run();
        }
        catch (CompileError& e) {  // outside any statement (e.g. a duplicate parameter name)
            report(std::move(e));
        }
        catch (SilentError&) {}
        f.state = 2;
    }

    /** @brief Appends the index of every user function called inside expression @p e to @p out. */
    inline void Compiler::scanExpr(const Expr& e, std::vector<int>& out) const {
        if (e.k == Expr::Call) {
            auto it = funcIdx.find(e.s);
            if (it != funcIdx.end()) out.push_back(it->second);
        }
        if (e.a) scanExpr(*e.a, out);
        if (e.b) scanExpr(*e.b, out);
        if (e.c) scanExpr(*e.c, out);
        for (const ExprP& a : e.args) scanExpr(*a, out);
    }

    /** @brief The same for the expressions of a single statement (not its nested statements). */
    inline void Compiler::scanStmt(const Stmt& s, std::vector<int>& out) const {
        if (s.e) scanExpr(*s.e, out);
        if (s.target) scanExpr(*s.target, out);
        if (s.idx) scanExpr(*s.idx, out);
    }

    /** @brief The same for a statement list, nested statements included. */
    inline void Compiler::scanStmts(const std::vector<StmtP>& v, std::vector<int>& out) const {
        for (const StmtP& s : v) {
            if (s->e) scanExpr(*s->e, out);
            if (s->target) scanExpr(*s->target, out);
            if (s->idx) scanExpr(*s->idx, out);
            if (s->init) scanStmt(*s->init, out);
            if (s->step) scanStmt(*s->step, out);
            scanStmts(s->body, out);
            scanStmts(s->els, out);
        }
    }

    /**
     * @brief Registers the program's declarations.
     *
     * Struct names first, then their fields and layout (natural alignment, in declaration order; the block is rounded up
     * to a multiple of 4), then function signatures (and the synthetic function that runs the global initializers). Every
     * declaration error is collected before any body is compiled, because a bad declaration makes all uses of its name
     * unreliable.
     *
     * @throws CompileErrors if any declaration is wrong.
     */
    inline void Compiler::collect() {
        // Problems with declarations are all collected before anything is compiled: they make every use of the broken
        // name unreliable, so the bodies are only checked once the declarations are right.
        auto guarded = [&](auto&& fn) {
            try { fn(); }
            catch (CompileError& e) { report(std::move(e)); }
        };
        std::unordered_map<std::string, Loc> declaredAt;
        for (const StructDef& sd : mod.structs) {
            guarded([&] {
                if (isBuiltinName(sd.name) || sd.name == "string" || sd.name == "address" || sd.name == "void")
                    fail(CompileError{ sd.loc, "'" + sd.name + "' is a reserved name and cannot be used for a struct", {}, {} });
                if (structIdx.count(sd.name)) {
                    CompileError e{ sd.loc, "struct '" + sd.name + "' is declared twice", {}, {} };
                    throw e.note(declaredAt[sd.name], "previous declaration of '" + sd.name + "' is here");
                }
                declaredAt[sd.name] = sd.loc;
                structIdx[sd.name] = (int)structs.size();
                StructInfo si;
                si.name = sd.name;
                structs.push_back(std::move(si));
            });
        }
        for (const StructDef& sd : mod.structs) {
            auto it = structIdx.find(sd.name);
            if (it == structIdx.end() || declaredAt[sd.name].line != sd.loc.line || declaredAt[sd.name].col != sd.loc.col) continue;
            StructInfo& si = structs[it->second];
            uint32_t off = 0;
            for (const Param& f : sd.fields) {
                guarded([&] {
                    for (const StructField& o : si.fields)
                        if (o.name == f.name)
                            throw CompileError{ f.loc, "duplicate field '" + f.name + "' in struct '" + sd.name + "'", {}, {} }
                                .note(o.loc, "first declared here");
                    Type t = resolveType(f.ty);
                    if (t.k == K::Struct || t.k == K::Array)
                        fail(CompileError{ f.ty.loc, "a struct field cannot be of type '" + typeName(t) + "'", {}, {} }
                            .hint("struct fields must be int, uint, float, bool, byte, address or string"));
                    if (t.k == K::String) si.hasStrings = true;
                    uint32_t sz = (t.k == K::Bool || t.k == K::Byte) ? 1 : 4;
                    off = (off + sz - 1) / sz * sz;
                    si.fields.push_back({ f.name, t, off, f.loc });
                    off += sz;
                });
            }
            si.size = std::max<uint32_t>(4, (off + 3) / 4 * 4);
        }
        for (const FuncDef& fd : mod.funcs) {
            guarded([&] {
                if (isBuiltinName(fd.name))
                    fail(CompileError{ fd.loc, "'" + fd.name + "' is a built-in and cannot be redefined", {}, {} });
                if (structIdx.count(fd.name))
                    throw CompileError{ fd.loc, "'" + fd.name + "' is already the name of a struct", {}, {} }
                        .note(declaredAt[fd.name], "the struct is declared here");
                if (funcIdx.count(fd.name)) {
                    CompileError e{ fd.loc, "function '" + fd.name + "' is defined twice", {}, {} };
                    throw e.note(funcs[funcIdx[fd.name]].loc, "previous definition of '" + fd.name + "' is here");
                }
                FuncInfo fi;
                fi.name = fd.name;
                fi.loc = fd.loc;
                fi.def = &fd;
                for (const Param& p : fd.params) fi.ptypes.push_back(p.ty.set ? resolveType(p.ty) : Type{ K::Int });
                fi.ret = fd.ret.set ? resolveReturn(fd.ret) : Type{ K::Int };  // no '->' means int; '-> void' is explicit
                if (fd.isExtern) checkExtern(fd, fi);
                if (fd.isExport) checkBoundary(fd, fi, "an exported");
                funcIdx[fd.name] = (int)funcs.size();
                funcs.push_back(std::move(fi));
            });
        }
        if (!errors.empty()) throw CompileErrors{ std::move(errors), false };
        if (!mod.init.body.empty()) {  // the global initializers: a function nobody can call by name
            FuncInfo fi;
            fi.name = mod.init.name;
            fi.loc = mod.init.loc;
            fi.def = &mod.init;
            fi.ret = Type{ K::None };
            initIdx = (int)funcs.size();
            funcs.push_back(std::move(fi));
        }
    }

    /**
     * @brief Compiles the whole program into a bytecode image.
     *
     * The image is: header | import table | [EXPT section] | stub | [init stub] | the reachable functions, callee first.
     *  - The global initializers compile first, because that is when each global's type becomes known.
     *  - Functions are compiled callee-before-caller (iterative post-order depth-first search over the call graph, so deep
     *    call chains cannot overflow the stack), which lets every call site know exactly which registers the callee clobbers.
     *    Functions unreachable from `main` are still compiled, to report errors, but not emitted.
     *  - The startup stub is `ENTER nGlobals`, a typed zero for each non-int global, `CALL <globals>`, `CALL main`, `HALT`.
     *    With `export fn`s an init stub (the same without `CALL main`) follows, which `IlangVM_start()` runs.
     *  - The import table lists every `extern fn` reachable from `main`; the EXPT section lists the exports. See
     *    iHeader in `IlangVM.h` for the byte layout.
     *
     * @return The executable image, ready to be written to a `.ic` file.
     * @throws CompileErrors if anything was reported while compiling.
     * @throws CompileError for an invalid register setup (`--regs`, `--temps`), a missing or malformed `main`, or too many
     *         extern functions.
     */
    inline std::vector<uint8_t> Compiler::compile() {
        if (tg.regs < 8 || tg.regs > 64) fail(Loc{}, "--regs must be between 8 and 64");
        if (tg.temps < 3 || tg.regs - tg.temps < 2) fail(Loc{}, "invalid --temps for this register count");
        collect();

        // `export fn`s, in source order: the host may call them (so they are roots of the call graph, like main)
        std::vector<int> exported;
        for (size_t i = 0; i < funcs.size(); ++i)
            if (funcs[i].def && funcs[i].def->isExport) exported.push_back((int)i);

        auto mi = funcIdx.find("main");
        const bool hasMain = mi != funcIdx.end();
        if (!hasMain && exported.empty())
            fail(CompileError{ Loc{}, "no 'main' function defined", {}, {} }
                .hint("every program starts at 'fn main() { ... }' (a program that only offers 'export fn's to C may leave it out)"));
        if (hasMain && funcs[mi->second].isExtern)
            fail(CompileError{ funcs[mi->second].loc, "'main' cannot be an extern function", {}, {} }
                .hint("every program starts at its own 'fn main() { ... }'"));
        if (hasMain && !funcs[mi->second].ptypes.empty())
            fail(CompileError{ funcs[mi->second].loc, "'main' must take no parameters", {}, {} }
                .hint("write it as: fn main() { ... }"));

        // The global initializers compile FIRST: that is when each global's type becomes known (`set x = f();` has
        // no annotation), and every other function needs them all. A function the initializers call has not been
        // compiled yet at that point; callUser treats such a callee as clobbering every register, which is safe.
        if (initIdx >= 0) ensureCompiled(initIdx);

        // Compile callees before callers (iterative post-order DFS over the call graph, so deep call
        // chains cannot overflow the stack). This lets every call site know exactly which registers
        // the callee clobbers. Functions unreachable from main are still compiled, to report errors.
        {
            std::vector<std::vector<int>> adj(funcs.size());
            for (size_t i = 0; i < funcs.size(); ++i) scanStmts(funcs[i].def->body, adj[i]);
            std::vector<char> seen(funcs.size(), 0);
            std::vector<size_t> next(funcs.size(), 0);
            std::vector<int> stk;
            auto visit = [&](int root) {
                if (seen[root]) return;
                seen[root] = 1;
                stk.push_back(root);
                while (!stk.empty()) {
                    int f = stk.back();
                    if (next[f] < adj[f].size()) {
                        int c = adj[f][next[f]++];
                        if (!seen[c]) { seen[c] = 1; stk.push_back(c); }
                    }
                    else {
                        stk.pop_back();
                        ensureCompiled(f);
                    }
                }
                };
            if (hasMain) visit(mi->second);
            for (size_t i = 0; i < funcs.size(); ++i) visit((int)i);
        }

        // Everything that went wrong was reported along the way and compilation carried on; none of it produces output.
        if (!errors.empty()) throw CompileErrors{ std::move(errors), false };

        // reachability from main (and the exports): unused functions are not emitted
        std::vector<char> reach(funcs.size(), 0);
        std::vector<int> order, work;
        if (hasMain) { reach[mi->second] = 1; work.push_back(mi->second); }
        if (initIdx >= 0) { reach[initIdx] = 1; work.push_back(initIdx); }
        for (size_t k = exported.size(); k-- > 0;)  // (popped from the back: the first export is reached first)
            if (!reach[exported[k]]) { reach[exported[k]] = 1; work.push_back(exported[k]); }
        while (!work.empty()) {
            int f = work.back();
            work.pop_back();
            order.push_back(f);
            for (int c : funcs[f].callees)
                if (!reach[c]) { reach[c] = 1; work.push_back(c); }
        }

        // image = header | import table | [EXPT section] | stub | [init stub] | functions
        //   stub      = [ENTER nGlobals; typed zero for each non-int global; CALL <globals>]  CALL main  HALT
        //   init stub = the same without CALL main: what IlangVM_start() runs for a host that calls the exports
        // The globals sit at the very bottom of the data stack (bp is 0 here, so slot i is absolute index i). The
        // functions' frames start above them because ENTER raised the stack top before the first CALL.
        std::vector<uint8_t> stub;
        std::vector<std::pair<uint32_t, int>> stubCalls;  // (operand offset within the stub, callee)
        auto stubCall = [&](int callee) {
            stub.push_back(CALL); stub.push_back(1); stub.push_back(0);
            stubCalls.push_back({ (uint32_t)stub.size(), callee });
            for (int b = 0; b < 4; ++b) stub.push_back(0);
        };
        auto stubGlobals = [&]() {
            if (initIdx < 0) return;
            stub.push_back(ENTER);
            for (int b = 0; b < 4; ++b) stub.push_back((uint8_t)((uint32_t)globals.size() >> (8 * b)));
            for (const GlobalInfo& g : globals)
                if (g.t.k != K::Int) FuncGen::stubZeroGlobal(stub, g.t, g.slot);
            stubCall(initIdx);
        };
        stubGlobals();
        if (hasMain) stubCall(mi->second);
        stub.push_back(HALT);
        uint32_t initStubAt = 0;  // offset of the init stub within `stub` (only with exports)
        if (!exported.empty()) {
            initStubAt = (uint32_t)stub.size();
            stubGlobals();
            stub.push_back(HALT);
        }

        // The import table: every extern function reachable from main (in the order they were reached), with its
        // signature; the VM binds each to a host function with the same signature when it loads the program, and
        // CALLC refers to them by their index here.
        //   u32 count, then count x { u8 name length, name, u8 ret type, u8 ret depth, u8 argc, argc x { u8 type, u8 depth } }
        // A type is the VM tag (string: T_STRINGADDRESS, void: T_NONE) and depth the number of array levels.
        std::vector<int> importOf(funcs.size(), -1);
        std::vector<uint8_t> table;
        {
            std::vector<int> imports;
            for (int f : order)
                if (funcs[f].isExtern) { importOf[f] = (int)imports.size(); imports.push_back(f); }
            if (imports.size() > kMaxImports) fail(Loc{}, "a program can call at most 65535 different extern functions");
            auto put = [&](uint32_t v, int n) { for (int b = 0; b < n; ++b) table.push_back((uint8_t)(v >> (8 * b))); };
            auto type = [&](const Type& t) {
                if (t.k == K::None) { put(T_NONE, 1); put(0, 1); }
                else if (t.k == K::Array) { put(FuncGen::tagFor(Type{ t.ek }), 1); put((uint32_t)t.depth, 1); }
                else { put(FuncGen::tagFor(t), 1); put(0, 1); }
            };
            put((uint32_t)imports.size(), 4);
            for (int f : imports) {
                const FuncInfo& fi = funcs[f];
                put((uint32_t)fi.name.size(), 1);
                table.insert(table.end(), fi.name.begin(), fi.name.end());
                type(fi.ret);
                put((uint32_t)fi.ptypes.size(), 1);
                for (const Type& t : fi.ptypes) type(t);
            }
        }
        // The EXPT extension section (present only if there are exports): the init stub's address, then each
        // `export fn` with its address and signature, see IlangVM.h.
        // It sits between the import table and the entry point, where a VM that does not know it never looks.
        std::vector<uint8_t> ext;
        std::vector<std::pair<uint32_t, int>> extAddrs;  // (offset within ext of a u32 address, function; -1 = init stub)
        if (!exported.empty()) {
            auto put = [&](uint32_t v, int n) { for (int b = 0; b < n; ++b) ext.push_back((uint8_t)(v >> (8 * b))); };
            auto type = [&](const Type& t) {
                if (t.k == K::None) { put(T_NONE, 1); put(0, 1); }
                else if (t.k == K::Array) { put(FuncGen::tagFor(Type{ t.ek }), 1); put((uint32_t)t.depth, 1); }
                else { put(FuncGen::tagFor(t), 1); put(0, 1); }
            };
            ext.push_back('E'); ext.push_back('X'); ext.push_back('P'); ext.push_back('T');
            put(0, 4);  // length, patched below
            extAddrs.push_back({ (uint32_t)ext.size(), -1 });
            put(0, 4);
            put((uint32_t)exported.size(), 4);
            for (int f : exported) {
                const FuncInfo& fi = funcs[f];
                put((uint32_t)fi.name.size(), 1);
                ext.insert(ext.end(), fi.name.begin(), fi.name.end());
                extAddrs.push_back({ (uint32_t)ext.size(), f });
                put(0, 4);
                put((uint32_t)std::min<size_t>(fi.ptypes.size(), (size_t)varRegLimit()), 1);  // arguments in registers
                type(fi.ret);
                put((uint32_t)fi.ptypes.size(), 1);
                for (const Type& t : fi.ptypes) type(t);
            }
            const uint32_t len = (uint32_t)ext.size() - 8u;
            for (int b = 0; b < 4; ++b) ext[4 + b] = (uint8_t)(len >> (8 * b));
        }
        const uint32_t codeStart = kHeaderSize + (uint32_t)table.size() + (uint32_t)ext.size();

        std::vector<uint32_t> base(funcs.size(), 0);
        uint32_t pos = codeStart + (uint32_t)stub.size();
        for (int f : order) { base[f] = pos; pos += (uint32_t)funcs[f].code.size(); }

        std::vector<uint8_t> img;
        img.reserve(pos);
        auto put32 = [&](uint32_t v) { for (int b = 0; b < 4; ++b) img.push_back((uint8_t)(v >> (8 * b))); };
        img.push_back('i'); img.push_back('V'); img.push_back('M'); img.push_back(0);
        put32(kBytecodeVersion);
        put32(codeStart);  // entrypoint = stub, right after the import table (and the EXPT section)
        img.insert(img.end(), table.begin(), table.end());
        {
            const size_t extAt = img.size();
            img.insert(img.end(), ext.begin(), ext.end());
            for (auto& a : extAddrs) {
                const uint32_t v = a.second < 0 ? codeStart + initStubAt : base[a.second];
                for (int b = 0; b < 4; ++b) img[extAt + a.first + b] = (uint8_t)(v >> (8 * b));
            }
        }
        const size_t stubAt = img.size();
        img.insert(img.end(), stub.begin(), stub.end());
        for (auto& c : stubCalls) {
            uint32_t operandAt = (uint32_t)stubAt + c.first;
            uint32_t rel = base[c.second] - (operandAt + 4);
            for (int b = 0; b < 4; ++b) img[operandAt + b] = (uint8_t)(rel >> (8 * b));
        }
        for (int f : order) {
            const FuncInfo& fi = funcs[f];
            size_t at = img.size();
            img.insert(img.end(), fi.code.begin(), fi.code.end());
            for (auto& c : fi.calls) {
                uint32_t operandAt = base[f] + c.first;
                uint32_t rel = base[c.second] - (operandAt + 4);
                for (int b = 0; b < 4; ++b) img[at + c.first + b] = (uint8_t)(rel >> (8 * b));
            }
            for (auto& c : fi.hostCalls) {
                const uint32_t idx = (uint32_t)importOf[c.second];
                img[at + c.first] = (uint8_t)idx;
                img[at + c.first + 1] = (uint8_t)(idx >> 8);
            }
        }
        return img;
    }

}  // namespace il
