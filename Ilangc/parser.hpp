/**
 * @file parser.hpp
 * @brief The abstract syntax tree (AST) of Ilang and the recursive-descent / precedence-climbing parser that builds it.
 *
 * The parser does not stop at the first syntax error: it records it, skips to a safe point (the end of the statement,
 * or the end of the top-level item) and carries on, so one run reports every independent mistake. If anything was
 * reported, Parser::parse() throws all of them together and the compiler never sees the broken tree.
 *
 * The grammar is recursive descent: each parse function handles one construct. Operator precedence is in Parser::prec().
 */
#pragma once
#include "lexer.hpp"

#include <memory>

namespace il {

    /**
     * @brief A type as written in the source: a name plus array dimensions.
     *
     * It is not yet resolved: `Foo[]` is accepted here even if no struct `Foo` exists. Compiler::resolveType() turns it
     * into an il::Type.
     */
    struct TypeRef {
        std::string name;  ///< The base type name: `int`, `string`, a struct name, `void`, ...
        Loc loc;  ///< Where the type starts; `loc.len` covers the whole type including its `[]` suffixes.
        bool set = false;  ///< True if a type was written; a parameter or return type that is left out defaults to `int`.
        int dims = 0;  ///< Number of `[]` suffixes: 0 = plain type, 1 = `T[]`, 2 = `T[][]`, ...
    };

    /** @brief Forward declaration of the expression node. */
    struct Expr;
    /** @brief Owning pointer to an expression node; the tree owns its children. */
    using ExprP = std::unique_ptr<Expr>;

    /**
     * @brief An expression node. The field `k` says which of the other fields are meaningful.
     *
     * | Kind        | Fields used                                                                   |
     * | ----------- | ----------------------------------------------------------------------------- |
     * | `Int`       | `iv` (the unsigned value; the sign is a separate `Unary` node)                |
     * | `Float`     | `dv`                                                                          |
     * | `Bool`      | `iv` (0 or 1)                                                                 |
     * | `Str`       | `s` (decoded contents)                                                        |
     * | `Char`      | `iv` (the byte)                                                               |
     * | `Ident`     | `s` (the variable name)                                                       |
     * | `Unary`     | `op` (`Minus`, `Bang` or `Tilde`), `a` (operand)                              |
     * | `Binary`    | `op`, `a` (left), `b` (right)                                                 |
     * | `Call`      | `s` (function, built-in or struct name), `args`                               |
     * | `Member`    | `a` (the struct), `s` (the field name)                                        |
     * | `Index`     | `a` (array or string), `b` (index)                                            |
     * | `ArrayLit`  | `args` (the elements; empty for `[]`)                                         |
     * | `ArrayRep`  | `a` (value), `b` (count) for `[a; b]`                                         |
     * | `Ternary`   | `a` (condition), `b` (value if true), `c` (value if false)                    |
     */
    struct Expr {
        /** @brief The kind of expression. */
        enum K : uint8_t {
            Int,       ///< Integer literal.
            Float,     ///< Floating-point literal.
            Bool,      ///< `true` or `false`.
            Str,       ///< String literal.
            Char,      ///< Character literal (a byte).
            Ident,     ///< Variable reference.
            Unary,     ///< Prefix operator: `-`, `!` or `~`.
            Binary,    ///< Infix operator, including `&&` and `||`.
            Call,      ///< Call of a function, a built-in or a struct constructor.
            Member,    ///< Field access `a.s`.
            Index,     ///< Element access `a[b]`.
            ArrayLit,  ///< Array literal `[x, y, z]`.
            ArrayRep,  ///< Repeat literal `[value; count]`.
            Ternary    ///< Conditional `a ? b : c`.
        } k = Int;     ///< Which kind of node this is.
        T op = T::Eof;       ///< Operator token of a Unary / Binary node.
        Loc loc;  ///< Where the node is reported: the operator for Unary/Binary/Ternary, the `[` for Index, the field name for Member, the first token otherwise.
        int64_t iv = 0;      ///< Value of an Int / Bool / Char node.
        double dv = 0;       ///< Value of a Float node.
        std::string s;       ///< Ident name / Call name / Member field name / Str contents.
        ExprP a,             ///< First operand: Unary operand; Binary left side; Member base; Index array; ArrayRep value; Ternary condition.
              b,             ///< Second operand: Binary right side; Index index; ArrayRep count; Ternary value if true.
              c;             ///< Third operand: Ternary value if false.
        std::vector<ExprP> args;  ///< Call arguments / ArrayLit elements.
    };

    /** @brief Forward declaration of the statement node. */
    struct Stmt;
    /** @brief Owning pointer to a statement node; the tree owns its children. */
    using StmtP = std::unique_ptr<Stmt>;

    /**
     * @brief A statement node. The field `k` says which of the other fields are meaningful.
     *
     * Compound assignments and `++` / `--` on a variable or a field are rewritten by the parser to a plain assignment
     * (`x += v` becomes `x = x + v`); on an array element they keep their own form (Stmt::op) so that the element's
     * address is computed only once.
     */
    struct Stmt {
        /** @brief The kind of statement. */
        enum K : uint8_t {
            Set,          ///< `set [const] name [: type] = e;` (a local variable).
            Assign,       ///< `name = e;`
            FieldAssign,  ///< `name.field = e;`
            IndexAssign,  ///< `target[idx] = e;` or, with `op`, `target[idx] op= e;`
            ExprS,        ///< An expression used as a statement (a call, usually).
            If,           ///< `if (e) { body } else { els }`; an `else if` is a single nested If in `els`.
            While,        ///< `while (e) { body }`
            For,          ///< `for (init; e; step) { body }`
            Break,        ///< `break;`
            Continue,     ///< `continue;`
            Block,        ///< `{ body }`
            Return,       ///< `return [e];`
            Global,       ///< A top-level `set`; only appears in the body of Module::init.
            Unsafe        ///< `unsafe { body }`: array and string indexing is not bounds-checked.
        } k = ExprS;      ///< Which kind of statement this is.
        Loc loc;  ///< Where the statement starts.
        Loc nameLoc;              ///< Set / Assign / FieldAssign: where the variable's name is.
        Loc fieldLoc;             ///< FieldAssign: where the field's name is.
        std::string name,         ///< Set / Assign / FieldAssign target (IndexAssign: the root variable name, if the base is one).
                    field;        ///< FieldAssign: the field being assigned.
        TypeRef ty;               ///< Optional annotation on `set`.
        bool isConst = false;     ///< Set / Global: declared as `set const`.
        ExprP e;                  ///< Value / condition (For: the condition, null = forever).
        ExprP target;             ///< IndexAssign: the array being indexed, i.e. `a` in `a[i] = v` and `g[i]` in `g[i][j] = v`.
        ExprP idx;                ///< IndexAssign: the index expression.
        Loc opLoc;                ///< IndexAssign with `op`: where the operator (`+=`) is.
        T op = T::Eof;            ///< IndexAssign: Eof for a plain `a[i] = v`; otherwise the operator of `a[i] += v` (Plus, ...).
                                  ///< (Variable and field targets are rewritten by the parser: `x += v` becomes `x = x + v`.
                                  ///< An element is not, so that `a[f()] += 1` evaluates f() once.)
        std::vector<StmtP> body;  ///< Then-branch / loop body / block contents.
        std::vector<StmtP> els;   ///< Else-branch (an `else if` is a single If statement in here).
        StmtP init,               ///< For: the initializer of `for (init; e; step) { body }`; may be null.
              step;               ///< For: the step; may be null.
    };

    /**
     * @brief A function parameter or a struct field: a name with an optional type.
     */
    struct Param {
        std::string name;  ///< The parameter or field name.
        TypeRef ty;  ///< The declared type; `ty.set` is false for a parameter written without one (it defaults to `int`).
        Loc loc;  ///< Where the name is.
    };

    /**
     * @brief A function declaration, with its body unless it is an `extern fn`.
     *
     * `ret.set` is false when no `->` was written (the function then returns `int`).
     */
    struct FuncDef {
        std::string name;  ///< The function's name.
        bool isExtern = false;    ///< `extern fn name(...) -> T;`: implemented in C by the host, no body.
        bool isExport = false;    ///< `export fn name(...) { ... }`: the host may call it.
        Loc loc;                  ///< Where the function's name is.
        std::vector<Param> params;  ///< The parameters, in order.
        TypeRef ret;  ///< The return type (`ret.set` is false when `->` was left out).
        std::vector<StmtP> body;  ///< The statements of the body (empty for an extern function).
    };

    /**
     * @brief A struct declaration: a name and its fields.
     */
    struct StructDef {
        std::string name;  ///< The struct's name.
        Loc loc;                  ///< Where the struct's name is.
        std::vector<Param> fields;  ///< The fields in declaration order (each is a name with a type).
    };

    /**
     * @brief A whole parsed program: all its structs, all its functions and the global initializers.
     */
    struct Module {
        std::vector<StructDef> structs;  ///< Every `struct` in the program, in source order.
        std::vector<FuncDef> funcs;  ///< Every `fn`, `extern fn` and `export fn`, in source order.
        /**
         * @brief The global initializers.
         *
         * Every top-level `set` is a global. They are collected as Stmt::Global statements in the body of this synthetic
         * function (named `<globals>`), which the startup stub runs (in source order) before it calls `main`.
         */
        FuncDef init;
    };

    /**
     * @brief Builds a Module from the tokens.
     *
     * Syntax errors are collected (see the top of this file) and parse() throws them together once the whole input has
     * been read.
     *
     * `extern` and `export` are not keywords; they only mean something in front of `fn` at the top level.
     */
    class Parser {
    public:
        /**
         * @brief Creates a parser over a token list.
         * @param t The tokens, as produced by Lexer::run (ending in an `Eof` token). The list must outlive the parser.
         */
        explicit Parser(const std::vector<Token>& t) : t_(t) {}

        /**
         * @brief Parses the whole token list.
         *
         * Reads top-level items (`fn`, `extern fn`, `export fn`, `struct`, `set`) until the end of the input. A syntax error
         * is recorded, the parser skips to the end of the broken item (syncTop()) and carries on.
         *
         * @return The parsed program.
         * @throws CompileErrors with every syntax error found, if there was at least one.
         */
        Module parse() {
            Module m;
            m.init.name = "<globals>";
            m.init.ret.name = "void";
            m.init.ret.set = true;
            while (peek().k != T::Eof) {
                const size_t before = p_;
                try {
                    if (peek().k == T::KFn) m.funcs.push_back(parseFunc());
                    else if (isExternStart()) m.funcs.push_back(parseFunc(true));
                    else if (isExportStart()) {
                        next();  // 'export'
                        m.funcs.push_back(parseFunc());
                        m.funcs.back().isExport = true;
                    }
                    else if (peek().k == T::KStruct) m.structs.push_back(parseStruct());
                    else if (peek().k == T::KSet) {
                        if (m.init.body.empty()) m.init.loc = peek().loc;
                        StmtP g = parseSimple();
                        expectSemi();
                        g->k = Stmt::Global;
                        m.init.body.push_back(std::move(g));
                    }
                    else throw topLevelError();
                }
                catch (CompileError& e) {
                    record(std::move(e));
                    syncTop();
                    if (p_ == before) next();  // always make progress
                }
            }
            if (!errs_.empty()) throw CompileErrors{ std::move(errs_), truncated_ };
            return m;
        }

    private:
        const std::vector<Token>& t_;  ///< The token list being parsed.
        size_t p_ = 0;  ///< Index of the next token to read.
        std::vector<CompileError> errs_;  ///< Syntax errors recorded so far (see record()).
        bool truncated_ = false;  ///< True once the parse was abandoned after kMaxErrors errors.
        bool stmtOps_ = false;  ///< True while parsing the first expression of a simple statement, where `+=` and `++` after it are legal (see parseBinary()).

        /**
         * @brief Looks at a token without consuming it.
         *
         * The token cursor is peek(), next() and accept(): peek(o) looks @p o tokens ahead (clamped to the final `Eof`),
         * next() consumes one token, accept(k) consumes the next token only if it is a @p k.
         *
         * @param o How many tokens ahead to look (0 = the next one).
         * @return The token; the final `Eof` token if @p o runs past the end.
         */
        const Token& peek(size_t o = 0) const { return t_[std::min(p_ + o, t_.size() - 1)]; }
        /** @brief The token consumed last (the first token if nothing has been consumed yet). */
        const Token& prev() const { return t_[p_ ? p_ - 1 : 0]; }
        /**
         * @brief Consumes one token.
         * @return The consumed token. The cursor never moves past the final `Eof`.
         */
        const Token& next() { const Token& t = peek(); if (p_ < t_.size() - 1) ++p_; return t; }
        /**
         * @brief Consumes the next token only if it has kind @p k.
         * @param k The expected kind.
         * @return true if the token was consumed.
         */
        bool accept(T k) { if (peek().k == k) { next(); return true; } return false; }

        // ------------------------------------------------------------ diagnostics
        /**
         * @brief Remembers a syntax error, ignoring exact repeats.
         * @param e The error.
         * @throws CompileErrors as soon as kMaxErrors errors are recorded: the parse is abandoned.
         */
        void record(CompileError e) {
            for (const CompileError& o : errs_)
                if (o.loc.file == e.loc.file && o.loc.line == e.loc.line && o.loc.col == e.loc.col && o.msg == e.msg) return;
            errs_.push_back(std::move(e));
            if (errs_.size() >= kMaxErrors) {
                truncated_ = true;
                throw CompileErrors{ std::move(errs_), true };
            }
        }

        /**
         * @brief Describes a token for an "expected X, found Y" message.
         * @param t The token found.
         * @return For example `identifier 'x'`, `number '5'`, `keyword 'fn'`, `'}'` or `the end of the file`.
         */
        static std::string describe(const Token& t) {
            switch (t.k) {
            case T::Eof: return "the end of the file";
            case T::Ident: return "identifier '" + std::string(t.text) + "'";
            case T::Int: case T::Float: return "number '" + std::string(t.text) + "'";
            case T::Str: return "a string literal";
            case T::Char: return "a character literal";
            default:
                if (t.k >= T::KSet && t.k <= T::KUnsafe) return "keyword '" + std::string(t.text) + "'";
                return "'" + std::string(t.text) + "'";
            }
        }

        /**
         * @brief The spot just after the last token consumed: where a missing `;` or `)` belongs.
         * @return A one-character location.
         */
        Loc endOfPrev() const {
            Loc l = prev().loc;
            l.col += l.len;
            l.len = 1;
            return l;
        }

        /**
         * @brief Tells whether the next token is on a later line (or in another file) than the one before it.
         * @return true if it is.
         */
        bool onNewLine() const {
            return p_ > 0 && (peek().loc.line > prev().loc.line || peek().loc.file != prev().loc.file);
        }

        /**
         * @brief Builds the "expected X, found Y" error.
         *
         * A missing closer (`;`, `)`, `]`) is reported right behind the last good token, where the author will look for it,
         * not on whatever happens to follow on a later line.
         *
         * @param what What was expected, e.g. `"';'"`.
         * @param k    The kind of token that was expected.
         * @param open If not null, a note is added pointing at this bracket as "to match this '...'".
         * @return The error (not thrown).
         */
        CompileError expectedError(const std::string& what, T k, const Token* open) const {
            const Token& f = peek();
            Loc l = f.loc;
            // A missing closer is reported right behind the last good token, where the author will look for it,
            // not on whatever happens to follow on a later line.
            const bool closer = k == T::Semi || k == T::RParen || k == T::RBracket;
            if (f.k == T::Eof || (closer && onNewLine())) l = endOfPrev();
            CompileError e{ l, "expected " + what + ", found " + describe(f), {}, {} };
            if (open) e.note(open->loc, "to match this '" + std::string(open->text) + "'");
            return e;
        }

        /**
         * @brief Consumes a token of kind @p k, or throws expectedError().
         * @param k    The required kind.
         * @param what Description for the error message.
         * @param open Opening bracket to point at in the error, or null.
         * @return The consumed token.
         * @throws CompileError if the next token is not a @p k.
         */
        const Token& expect(T k, const char* what, const Token* open = nullptr) {
            if (peek().k != k) throw expectedError(what, k, open);
            return next();
        }
        /**
         * @brief Consumes an identifier and returns its text.
         * @param what Description for the error message, e.g. `"a type name"`.
         * @throws CompileError if the next token is not an identifier.
         */
        std::string expectIdent(const char* what) { return std::string(expect(T::Ident, what).text); }

        /**
         * @brief Consumes the `;` that ends a statement.
         *
         * When it is missing but the statement is plainly over (the next token is on a new line, or closes the block) the
         * error is recorded and parsing goes on as if it had been there, so a forgotten semicolon costs one error, not a
         * cascade.
         *
         * @throws CompileError if the `;` is missing and the statement does not look finished.
         */
        void expectSemi() {
            if (peek().k == T::Semi) { next(); return; }
            CompileError e = expectedError("';'", T::Semi, nullptr);
            if (onNewLine() || peek().k == T::RBrace || peek().k == T::Eof) {
                record(std::move(e));
                return;
            }
            throw e;
        }

        /**
         * @brief The error for a token that cannot start a top-level item, with a hint for the usual habits from other
         *        languages (`def`, `let`, `class`, `pub`, ...).
         * @return The error (not thrown).
         */
        CompileError topLevelError() const {
            const Token& t = peek();
            CompileError e{ t.loc, "expected 'fn', 'export fn', 'extern fn', 'struct' or 'set' at top level, found " + describe(t), {}, {} };
            const std::string w(t.text);
            if (t.k == T::Ident && w == "extern")
                e.hint("a C function is declared with 'extern fn': extern fn name(a: int) -> int;");
            else if (t.k == T::Ident && (w == "export" || w == "pub" || w == "public"))
                e.hint("a function C may call is written 'export fn': export fn name(a: int) -> int { ... }");
            else if (t.k == T::Ident && (w == "def" || w == "func" || w == "function" || w == "fun" || w == "void"))
                e.hint("functions are declared with 'fn': fn name(a: int) -> int { ... }");
            else if (t.k == T::Ident && (w == "let" || w == "var" || w == "int" || w == "uint" || w == "float" || w == "bool" ||
                w == "byte" || w == "string" || w == "auto"))
                e.hint("global variables are declared with 'set': set name = value;");
            else if (t.k == T::Ident && (w == "class" || w == "record"))
                e.hint("data types are declared with 'struct': struct Name { field: int; }");
            else if (t.k == T::KConst)
                e.hint("'const' goes after 'set': set const name = value;");
            else if (t.k == T::RBrace)
                e.hint("this '}' has no matching '{'");
            else if (t.k == T::Ident && peek(1).k == T::LParen)
                e.hint("statements belong inside a function; to run code at startup put it in 'fn main() { ... }'");
            return e;
        }

        // ------------------------------------------------------------ error recovery
        /**
         * @brief Error recovery inside a function: skips to the end of the broken statement.
         *
         * Skips past its `;`, past the `}` of a block it opened, or up to (not past) the `}` that closes the enclosing block
         * or a keyword that starts the next statement. A `;` inside parentheses or brackets (a `for` header, `[0; 5]`) ends
         * nothing.
         */
        void syncStmt() {
            int n = 0, par = 0;  // braces / parentheses and brackets: a ';' inside ( ) or [ ] (for headers, [0; 5]) ends nothing
            const size_t start = p_;
            for (;;) {
                switch (peek().k) {
                case T::Eof: return;
                case T::Semi: if (n == 0 && par == 0) { next(); return; } break;
                case T::LParen: case T::LBracket: ++par; break;
                case T::RParen: case T::RBracket: if (par > 0) --par; break;
                case T::LBrace: ++n; par = 0; break;
                case T::RBrace:
                    par = 0;
                    if (n == 0) return;
                    if (--n == 0) { next(); return; }
                    break;
                case T::KSet: case T::KIf: case T::KWhile: case T::KFor: case T::KReturn: case T::KBreak:
                case T::KContinue: case T::KUnsafe:
                    if (n == 0 && p_ != start) return;
                    break;
                default: break;
                }
                next();
            }
        }

        /**
         * @brief Error recovery at the top level: skips to the end of the broken top-level item (a whole function body counts
         *        as one) or to the start of the next one.
         */
        void syncTop() {
            int n = 0, par = 0;
            const size_t start = p_;
            for (;;) {
                switch (peek().k) {
                case T::Eof: return;
                case T::Semi: if (n == 0 && par == 0) { next(); return; } break;
                case T::LParen: case T::LBracket: ++par; break;
                case T::RParen: case T::RBracket: if (par > 0) --par; break;
                case T::LBrace: ++n; par = 0; break;
                case T::RBrace:
                    par = 0;
                    if (n > 0 && --n == 0) { next(); return; }
                    break;
                case T::KFn: case T::KStruct: case T::KSet:
                    if (n == 0 && p_ != start) return;
                    break;
                case T::Ident:
                    if (n == 0 && p_ != start && (isExternStart() || isExportStart())) return;
                    break;
                default: break;
                }
                next();
            }
        }

        // ------------------------------------------------------------ declarations
        /**
         * @brief Parses a type: `type = name { "[" "]" }`.
         * @return The type as written.
         * @throws CompileError if the name is missing or a dimension has a size (`int[10]`).
         */
        TypeRef parseType() {
            TypeRef r;
            r.loc = peek().loc;
            r.name = expectIdent("a type name");
            r.set = true;
            while (peek().k == T::LBracket) {  // array type: T[] (T[][] is an array of arrays)
                if (peek(1).k != T::RBracket) {
                    Loc l = peek().loc;
                    l.len = 1;
                    throw CompileError{ l, "an array type is written 'T[]' without a size", {}, {} }
                        .hint("the length belongs to the value, not the type: set a: " + r.name + "[] = [0; 10];");
                }
                next();
                next();
                ++r.dims;
            }
            if (prev().loc.line == r.loc.line && prev().loc.file == r.loc.file)
                r.loc.len = prev().loc.col + prev().loc.len - r.loc.col;  // cover 'int[][]' as a whole
            return r;
        }

        /**
         * @brief Parses `struct Name { field: type; ... }`.
         *
         * A broken field is recorded and skipped, so the other fields are still checked.
         *
         * @return The struct definition.
         * @throws CompileError if the name or the opening brace is missing, or the struct is not closed.
         */
        StructDef parseStruct() {
            StructDef s;
            next();
            s.loc = peek().loc;
            s.name = expectIdent("struct name");
            const Token& open = expect(T::LBrace, "'{'");
            for (;;) {
                if (peek().k == T::RBrace) break;
                if (peek().k == T::Eof) {
                    CompileError e{ endOfPrev(), "unexpected end of file: the struct '" + s.name + "' is not closed", {}, {} };
                    throw e.note(open.loc, "this '{' is never closed");
                }
                const size_t before = p_;
                try {
                    Param f;
                    f.loc = peek().loc;
                    f.name = expectIdent("field name");
                    expect(T::Colon, "':'");
                    f.ty = parseType();
                    expectSemi();
                    s.fields.push_back(std::move(f));
                }
                catch (CompileError& e) {
                    record(std::move(e));
                    syncStmt();
                    if (p_ == before && peek().k != T::RBrace && peek().k != T::Eof) next();
                }
            }
            next();
            return s;
        }

        /**
         * @brief Tells whether the next tokens are `extern fn`.
         *
         * `extern` is not a keyword (it stays usable as a name): it only means something as `extern fn` at the top level.
         */
        bool isExternStart() const {
            return peek().k == T::Ident && peek().text == "extern" && peek(1).k == T::KFn;
        }
        /** @brief Tells whether the next tokens are `export fn` (`export` is not a keyword either). */
        bool isExportStart() const {
            return peek().k == T::Ident && peek().text == "export" && peek(1).k == T::KFn;
        }

        /**
         * @brief Parses `fn name(param[: type], ...) [-> type] { body }`.
         *
         * An extern function has `;` instead of a body. The `export` prefix is handled by parse().
         *
         * @param isExtern True if the `extern` word is the next token (it is consumed here).
         * @return The function definition.
         */
        FuncDef parseFunc(bool isExtern = false) {
            FuncDef f;
            f.isExtern = isExtern;
            if (isExtern) next();  // 'extern'
            next();
            f.loc = peek().loc;
            f.name = expectIdent("function name");
            const Token& open = expect(T::LParen, "'('");
            if (peek().k != T::RParen) {
                do {
                    Param p;
                    p.loc = peek().loc;
                    p.name = expectIdent("parameter name");
                    if (accept(T::Colon)) p.ty = parseType();
                    f.params.push_back(std::move(p));
                } while (accept(T::Comma));
            }
            expect(T::RParen, "')'", &open);
            if (accept(T::Arrow)) f.ret = parseType();
            else if (peek().k == T::Colon || peek().k == T::Ident)
                throw expectedError(isExtern ? "'->' or ';'" : "'->' or '{'", isExtern ? T::Semi : T::LBrace, nullptr)
                    .hint("a return type is written after '->': " + std::string(isExtern ? "extern " : "") + "fn " +
                          f.name + "(...) -> int" + (isExtern ? ";" : " { ... }"));
            if (isExtern) {
                if (peek().k == T::LBrace)
                    throw CompileError{ peek().loc, "an extern function has no body", {}, {} }
                        .hint("its code is a C function in a host library; end the declaration with ';'");
                expectSemi();
                return f;
            }
            f.body = parseBlock("fn");
            return f;
        }

        /**
         * @brief Parses `"{" statement... "}"`.
         *
         * A broken statement is recorded and skipped with syncStmt().
         *
         * @param owner Names the construct that wants the block (`"if"`, `"while"`, ...), for the error hint; null for a
         *              plain block.
         * @return The statements of the block.
         * @throws CompileError if the `{` is missing or the block is never closed.
         */
        std::vector<StmtP> parseBlock(const char* owner = nullptr) {
            if (peek().k != T::LBrace) {
                CompileError e = expectedError("'{'", T::LBrace, nullptr);
                if (owner && std::string(owner) != "fn")
                    e.hint(std::string("the body of '") + owner + "' must be a block in braces: " + owner + " (...) { ... }");
                throw e;
            }
            const Token& open = next();
            std::vector<StmtP> v;
            for (;;) {
                if (peek().k == T::RBrace) break;
                if (peek().k == T::Eof) {
                    CompileError e{ endOfPrev(), "unexpected end of file: missing '}'", {}, {} };
                    throw e.note(open.loc, "this '{' is never closed");
                }
                const size_t before = p_;
                try {
                    v.push_back(parseStmt());
                }
                catch (CompileError& e) {
                    record(std::move(e));
                    syncStmt();
                    if (p_ == before && peek().k != T::RBrace && peek().k != T::Eof) next();
                }
            }
            next();
            return v;
        }

        /**
         * @brief Allocates a statement node.
         * @param k The kind.
         * @param l Its location.
         */
        static StmtP mk(Stmt::K k, Loc l) {
            auto s = std::make_unique<Stmt>();
            s->k = k;
            s->loc = l;
            return s;
        }

        /**
         * @brief Parses a statement that needs no block: `set`, an assignment (variable, field or element), a compound
         *        assignment, `++` / `--`, or an expression.
         *
         * The terminating `;` is NOT consumed (a `for` header uses these without one).
         *
         * @return The statement.
         * @throws CompileError on a syntax error, with hints for habits from other languages (`let x = 1;`, `int x = 1;`).
         */
        StmtP parseSimple() {
            const Token& t = peek();
            if (t.k == T::KSet) {
                next();
                auto s = mk(Stmt::Set, t.loc);
                if (accept(T::KConst)) s->isConst = true;  // set const x = ...
                s->nameLoc = peek().loc;
                s->name = expectIdent("variable name");
                if (accept(T::Colon)) s->ty = parseType();
                if (peek().k != T::Assign) {
                    std::string tyText;
                    if (s->ty.set) {
                        tyText = ": " + s->ty.name;
                        for (int d = 0; d < s->ty.dims; ++d) tyText += "[]";
                    }
                    throw expectedError("'='", T::Assign, nullptr)
                        .hint("a variable needs an initial value: set " + s->name + tyText + " = <value>;");
                }
                next();
                s->e = parseExpr();
                return s;
            }
            // ++x; --x;   (the postfix form is recognised below, once the target has been parsed)
            if ((t.k == T::Plus || t.k == T::Minus) && peek(1).k == t.k && adjacent(t, peek(1))) {
                Loc ol = t.loc;
                ol.len = 2;
                const T opk = t.k;
                next();
                next();
                return makeCompound(parsePostfix(), opk, nullptr, ol, t.loc);
            }
            if (t.k == T::Ident && peek(1).k == T::Assign) {
                auto s = mk(Stmt::Assign, t.loc);
                s->nameLoc = t.loc;
                s->name = std::string(next().text);
                next();
                s->e = parseExpr();
                return s;
            }
            if (t.k == T::Ident && peek(1).k == T::Dot && peek(2).k == T::Ident && peek(3).k == T::Assign) {
                auto s = mk(Stmt::FieldAssign, t.loc);
                s->nameLoc = t.loc;
                s->name = std::string(next().text);
                next();
                s->fieldLoc = peek().loc;
                s->field = std::string(next().text);
                next();
                s->e = parseExpr();
                return s;
            }
            auto s = mk(Stmt::ExprS, t.loc);
            const size_t p0 = p_;
            stmtOps_ = true;  // this expression may be followed by '+=', '++', ... (see parseBinary)
            s->e = parseExpr();
            stmtOps_ = false;
            if (const T cop = compoundAt(0); cop != T::Eof) {  // x += v;   p.f *= 2;   a[i] -= 1;   x <<= 2;
                Loc ol = peek().loc;
                ol.len += 1;  // the operator plus its '='
                next();
                next();
                return makeCompound(std::move(s->e), cop, parseExpr(), ol, t.loc);
            }
            if (const T iop = incAt(0); iop != T::Eof) {      // x++;   a[i]--;
                Loc ol = peek().loc;
                ol.len = 2;
                next();
                next();
                return makeCompound(std::move(s->e), iop, nullptr, ol, t.loc);
            }
            if (peek().k == T::Assign) {  // name[index] = value;   grid[i][j] = value;
                const Token& eq = peek();
                if (s->e->k != Expr::Index) {
                    Loc l = eq.loc;
                    l.len = 1;
                    CompileError e{ l, s->e->k == Expr::Call ? "cannot assign to the result of a function call" : "cannot assign to this expression", {}, {} };
                    throw e.hint("the left side of '=' must be a variable (x), a struct field (p.x) or an array element (a[i], g[i][j])");
                }
                next();
                s->k = Stmt::IndexAssign;
                s->target = std::move(s->e->a);
                s->idx = std::move(s->e->b);
                if (s->target->k == Expr::Ident) s->name = s->target->s;
                s->e = parseExpr();
                return s;
            }
            // `let x = 1;`, `int x = 1;`, `Point p = Point();`: the word before a name is not a statement of its own
            if (t.k == T::Ident && p_ == p0 + 1 && peek().k == T::Ident) {
                const std::string w(t.text), name(peek().text);
                CompileError e{ t.loc, "unexpected '" + w + "' before '" + name + "'", {}, {} };
                if (w == "let" || w == "var" || w == "auto")
                    e.hint("variables are declared with 'set': set " + name + " = <value>;");
                else
                    e.hint("variables are declared with 'set': set " + name + ": " + w + " = <value>;   (the type is optional)");
                throw e;
            }
            return s;
        }

        /**
         * @brief Parses one statement:
         *        `statement = if | while | for | break | continue | return | block | unsafe | simple ";"`.
         * @return The statement.
         */
        StmtP parseStmt() {
            const Token& t = peek();
            switch (t.k) {
            case T::KIf: return parseIf();
            case T::KWhile: {
                next();
                auto s = mk(Stmt::While, t.loc);
                s->e = parseCondition("while");
                s->body = parseBlock("while");
                return s;
            }
            case T::KFor: {  // for (init; condition; step) { ... }   every part is optional
                next();
                auto s = mk(Stmt::For, t.loc);
                const Token& open = expect(T::LParen, "'('");
                if (peek().k != T::Semi) s->init = parseSimple();
                expect(T::Semi, "';' after the for-loop initializer");
                if (peek().k != T::Semi) s->e = parseExpr();
                expect(T::Semi, "';' after the for-loop condition");
                if (peek().k != T::RParen) {
                    if (peek().k == T::KSet) throw CompileError{ peek().loc, "the for-loop step cannot declare a variable", {}, {} };
                    s->step = parseSimple();
                }
                expect(T::RParen, "')'", &open);
                s->body = parseBlock("for");
                return s;
            }
            case T::KBreak:
            case T::KContinue: {
                next();
                auto s = mk(t.k == T::KBreak ? Stmt::Break : Stmt::Continue, t.loc);
                expectSemi();
                return s;
            }
            case T::KReturn: {
                next();
                auto s = mk(Stmt::Return, t.loc);
                if (peek().k != T::Semi) s->e = parseExpr();
                expectSemi();
                return s;
            }
            case T::LBrace: {
                auto s = mk(Stmt::Block, t.loc);
                s->body = parseBlock();
                return s;
            }
            case T::KUnsafe: {  // unsafe { ... }: a block whose array/string indexing is not bounds-checked
                next();
                auto s = mk(Stmt::Unsafe, t.loc);
                s->body = parseBlock("unsafe");
                return s;
            }
            default: {
                auto s = parseSimple();
                expectSemi();
                return s;
            }
            }
        }

        /**
         * @brief Parses the `( expression )` after `if` / `while`.
         * @param kw The keyword, for the error message.
         * @return The condition.
         * @throws CompileError if the parentheses are missing, or the condition contains a `=` (assignment).
         */
        ExprP parseCondition(const char* kw) {
            if (peek().k != T::LParen)
                throw expectedError("'('", T::LParen, nullptr)
                    .hint(std::string("the condition of '") + kw + "' goes in parentheses: " + kw + " (condition) { ... }");
            const Token& open = next();
            ExprP c = parseExpr();
            if (peek().k == T::Assign) {
                Loc l = peek().loc;
                l.len = 1;
                throw CompileError{ l, "'=' assigns; it cannot be used inside a condition", {}, {} }
                    .hint("to compare two values write '=='");
            }
            expect(T::RParen, "')'", &open);
            return c;
        }

        /**
         * @brief Parses `if (condition) { ... } [ else if ... | else { ... } ]`; the `if` token is the next one.
         * @return The If statement; an `else if` becomes a single nested If in `els`.
         */
        StmtP parseIf() {
            auto s = mk(Stmt::If, next().loc);
            s->e = parseCondition("if");
            s->body = parseBlock("if");
            if (accept(T::KElse)) {
                if (peek().k == T::KIf) s->els.push_back(parseIf());
                else s->els = parseBlock("else");
            }
            return s;
        }

        /**
         * @brief Binary operator precedence, C-style.
         *
         * Bitwise operators bind looser than comparisons, as in C; shifts bind tighter than comparisons and looser than `+`
         * and `-`, so `1 << n - 1` is `1 << (n - 1)`.
         *
         * | Level | Operators           |
         * | ----- | ------------------- |
         * | 1     | `\|\|`              |
         * | 2     | `&&`                |
         * | 3     | `\|`                |
         * | 4     | `^`                 |
         * | 5     | `&`                 |
         * | 6     | `==` `!=`           |
         * | 7     | `<` `>` `<=` `>=`   |
         * | 8     | `<<` `>>`           |
         * | 9     | `+` `-`             |
         * | 10    | `*` `/` `%`         |
         *
         * @param k A token kind.
         * @return The precedence of @p k as a binary operator, or -1 if it is not one.
         */
        static int prec(T k) {
            switch (k) {
            case T::OrOr: return 1;
            case T::AndAnd: return 2;
            case T::Pipe: return 3;
            case T::Caret: return 4;
            case T::Amp: return 5;
            case T::Eq: case T::Ne: return 6;
            case T::Lt: case T::Gt: case T::Le: case T::Ge: return 7;
            case T::Shl: case T::Shr: return 8;
            case T::Plus: case T::Minus: return 9;
            case T::Star: case T::Slash: case T::Percent: return 10;
            default: return -1;
            }
        }

        /**
         * @brief The source text of the binary operator that a compound assignment is built on (`<<` for `<<=`).
         * @param k The operator token kind.
         */
        static const char* binopText(T k) {
            switch (k) {
            case T::Plus: return "+"; case T::Minus: return "-"; case T::Star: return "*"; case T::Slash: return "/";
            case T::Percent: return "%"; case T::Amp: return "&"; case T::Pipe: return "|"; case T::Shl: return "<<";
            case T::Shr: return ">>"; default: return "^";
            }
        }

        /**
         * @brief True if @p b starts exactly where @p a ends: that is how `+=` (and `++`) differ from `+ =` (and `+ +`).
         */
        static bool adjacent(const Token& a, const Token& b) {
            return a.loc.file == b.loc.file && a.loc.line == b.loc.line && a.loc.col + a.loc.len == b.loc.col;
        }

        // ---- compound assignment and ++ / -- --------------------------------------------------------------------
        // `x += v` is a statement, not an expression (like `x = v`): it may only follow a variable, a field or an
        // element, and only where a statement starts. They are written as two adjacent tokens ('+' '='), so
        // `x + =` is still an error and nothing that parsed before changes meaning.

        /**
         * @brief Recognises a compound assignment operator.
         * @param o Offset of the operator token from the cursor.
         * @return The binary operator of a compound assignment (`Plus` for `+=`) whose operator token is at offset @p o, or
         *         `T::Eof` if there is none.
         */
        T compoundAt(size_t o) const {
            const Token& op = peek(o);
            switch (op.k) {
            case T::Plus: case T::Minus: case T::Star: case T::Slash: case T::Percent:
            case T::Amp: case T::Pipe: case T::Caret: case T::Shl: case T::Shr: break;
            default: return T::Eof;
            }
            const Token& n = peek(o + 1);
            return n.k == T::Assign && adjacent(op, n) ? op.k : T::Eof;
        }

        /**
         * @brief Recognises a postfix `++` / `--` that ends a statement or a `for` step.
         *
         * The two operator tokens must be adjacent and followed by `;`, `)` or `}`. Anything else (`a++b`, `a - -b`) stays
         * an ordinary expression.
         *
         * @param o Offset of the first operator token from the cursor.
         * @return `T::Plus` or `T::Minus`, or `T::Eof` if there is no increment/decrement.
         */
        T incAt(size_t o) const {
            const Token& a = peek(o);
            if (a.k != T::Plus && a.k != T::Minus) return T::Eof;
            const Token& b = peek(o + 1);
            if (b.k != a.k || !adjacent(a, b)) return T::Eof;
            const T after = peek(o + 2).k;
            return after == T::Semi || after == T::RParen || after == T::RBrace || after == T::Eof ? a.k : T::Eof;
        }

        /**
         * @brief Copies an identifier or `ident.field` expression.
         *
         * These are the only things a rewrite from `x += v` to `x = x + v` repeats; evaluating a name has no effect.
         */
        static ExprP cloneName(const Expr& e) {  // an identifier or `ident.field`: the only things a rewrite repeats
            auto c = std::make_unique<Expr>();
            c->k = e.k;
            c->loc = e.loc;
            c->s = e.s;
            if (e.a) c->a = cloneName(*e.a);
            return c;
        }

        /**
         * @brief Builds the statement for `target op= value` (or `target++` / `target--`).
         *
         * A variable or a field is rewritten as `target = target op value`; both evaluate the target twice, which is harmless
         * because evaluating a name has no effect. An element keeps its own form (Stmt::op) so that its array and index
         * expressions are evaluated once.
         *
         * @param target  The variable, field or element being changed.
         * @param op      The binary operator (`Plus` for `+=`).
         * @param value   The right-hand side, or null for `++` / `--` (that means `+= 1` / `-= 1`).
         * @param opLoc   Location of the operator, for error messages.
         * @param stmtLoc Location of the statement.
         * @return The assignment statement.
         * @throws CompileError if @p target is not something that can be assigned to.
         */
        StmtP makeCompound(ExprP target, T op, ExprP value, Loc opLoc, Loc stmtLoc) {
            const std::string o = binopText(op);
            const bool inc = !value;
            const std::string shown = inc ? o + o : o + "=";
            if (inc) {
                value = std::make_unique<Expr>();
                value->k = Expr::Int;
                value->loc = opLoc;
                value->iv = 1;
            }
            const bool fieldOfName = target->k == Expr::Member && target->a && target->a->k == Expr::Ident;
            if (target->k != Expr::Ident && !fieldOfName && target->k != Expr::Index) {
                CompileError e{ opLoc, "'" + shown + "' needs a variable, a struct field or an array element on its left",
                    {}, {} };
                throw e.hint(target->k == Expr::Call ? "the result of a function call cannot be changed"
                                                     : "the left side must be a variable (x), a field (p.x) or an element (a[i], g[i][j])");
            }
            auto binary = [&](ExprP lhs) {
                auto b = std::make_unique<Expr>();
                b->k = Expr::Binary;
                b->op = op;
                b->loc = opLoc;
                b->a = std::move(lhs);
                b->b = std::move(value);
                return b;
            };
            if (target->k == Expr::Index) {
                auto s = mk(Stmt::IndexAssign, stmtLoc);
                s->target = std::move(target->a);
                s->idx = std::move(target->b);
                if (s->target->k == Expr::Ident) s->name = s->target->s;
                s->op = op;
                s->opLoc = opLoc;
                s->e = std::move(value);
                return s;
            }
            if (target->k == Expr::Ident) {
                auto s = mk(Stmt::Assign, stmtLoc);
                s->nameLoc = target->loc;
                s->name = target->s;
                s->e = binary(cloneName(*target));
                return s;
            }
            auto s = mk(Stmt::FieldAssign, stmtLoc);
            s->nameLoc = target->a->loc;
            s->name = target->a->s;
            s->fieldLoc = target->loc;
            s->field = target->s;
            s->e = binary(cloneName(*target));
            return s;
        }

        /**
         * @brief Reports a statement-only operator found where an expression is expected, e.g. `y = x++;` or `f(x += 1)`.
         * @param cop The compound operator found, or `T::Eof`.
         * @param iop The increment/decrement found, or `T::Eof`.
         * @throws CompileError always.
         */
        [[noreturn]] void statementOnly(T cop, T iop) {
            Loc l = peek().loc;
            l.len = cop != T::Eof ? l.len + 1 : 2;  // the operator plus its '=', or '++' / '--'
            const T k = cop != T::Eof ? cop : iop;
            const std::string o = binopText(k);
            const std::string shown = cop != T::Eof ? o + "=" : o + o;
            throw CompileError{ l, "'" + shown + "' is a statement, not an expression", {}, {} }
                .hint(cop != T::Eof ? "write it on its own line, then use the variable: x " + o + "= v; ... x ..."
                                    : "write it on its own line, then use the variable: x" + o + o + "; ... x ...");
        }

        /**
         * @brief Parses an expression: `expr = binary [ "?" expr ":" expr ]`.
         *
         * The conditional operator is right associative and the loosest operator of all.
         *
         * @return The expression tree.
         */
        ExprP parseExpr() {
            const bool top = stmtOps_;
            stmtOps_ = false;
            ExprP c = parseBinary(1, top);
            if (peek().k != T::Question) return c;
            const Token& q = next();
            auto e = std::make_unique<Expr>();
            e->k = Expr::Ternary;
            e->loc = q.loc;
            e->a = std::move(c);
            e->b = parseExpr();
            expect(T::Colon, "':' for the else part of the '?' expression", &q);
            e->c = parseExpr();
            return e;
        }

        /**
         * @brief Precedence climbing for the binary operators.
         *
         * @param minPrec The loosest precedence to accept (see prec()).
         * @param top     True if this is the first expression of a simple statement, so `+=` and `++` after it end it
         *                instead of being an error (parseSimple() then picks them up).
         * @return The expression tree.
         */
        ExprP parseBinary(int minPrec, bool top) {
            ExprP lhs = parseUnary();
            for (;;) {
                int p = prec(peek().k);
                if (p < minPrec) break;
                const T cop = compoundAt(0), iop = cop == T::Eof ? incAt(0) : T::Eof;
                if (cop != T::Eof || iop != T::Eof) {
                    if (top) break;
                    statementOnly(cop, iop);
                }
                const Token& op = next();
                auto e = std::make_unique<Expr>();
                e->k = Expr::Binary;
                e->op = op.k;
                e->loc = op.loc;
                e->a = std::move(lhs);
                e->b = parseBinary(p + 1, top);
                lhs = std::move(e);
            }
            return lhs;
        }

        /**
         * @brief Parses `unary = ("-" | "!" | "~") unary | postfix`.
         */
        ExprP parseUnary() {
            if (peek().k == T::Minus || peek().k == T::Bang || peek().k == T::Tilde) {
                const Token& op = next();
                auto e = std::make_unique<Expr>();
                e->k = Expr::Unary;
                e->op = op.k;
                e->loc = op.loc;
                e->a = parseUnary();
                return e;
            }
            return parsePostfix();
        }

        /**
         * @brief Parses `postfix = primary { "." field | "[" index "]" }`.
         */
        ExprP parsePostfix() {
            ExprP e = parsePrimary();
            for (;;) {
                if (peek().k == T::Dot) {
                    next();
                    auto m = std::make_unique<Expr>();
                    m->k = Expr::Member;
                    m->loc = peek().loc;
                    m->s = expectIdent("field name");
                    m->a = std::move(e);
                    e = std::move(m);
                }
                else if (peek().k == T::LBracket) {
                    auto ix = std::make_unique<Expr>();
                    ix->k = Expr::Index;
                    const Token& open = next();
                    ix->loc = open.loc;
                    ix->a = std::move(e);
                    ix->b = parseExpr();
                    expect(T::RBracket, "']'", &open);
                    e = std::move(ix);
                }
                else {
                    break;
                }
            }
            return e;
        }

        /**
         * @brief Parses `primary = literal | name | name(args) | ( expr ) | [elements] | [value; count]`.
         * @throws CompileError "expected an expression" for anything else, with hints for a few misplaced keywords.
         */
        ExprP parsePrimary() {
            const Token& t = peek();
            auto e = std::make_unique<Expr>();
            e->loc = t.loc;
            switch (t.k) {
            case T::Int: next(); e->k = Expr::Int; e->iv = t.iv; return e;
            case T::Float: next(); e->k = Expr::Float; e->dv = t.dv; return e;
            case T::Str: next(); e->k = Expr::Str; e->s = t.sv; return e;
            case T::Char: next(); e->k = Expr::Char; e->iv = t.iv; return e;
            case T::KTrue: next(); e->k = Expr::Bool; e->iv = 1; return e;
            case T::KFalse: next(); e->k = Expr::Bool; e->iv = 0; return e;
            case T::LParen: {
                const Token& open = next();
                ExprP inner = parseExpr();
                expect(T::RParen, "')'", &open);
                return inner;
            }
            case T::LBracket: {
                const Token& open = next();
                e->k = Expr::ArrayLit;
                if (accept(T::RBracket)) return e;  // [] (type comes from the context)
                ExprP first = parseExpr();
                if (accept(T::Semi)) {              // [value; count]
                    e->k = Expr::ArrayRep;
                    e->a = std::move(first);
                    e->b = parseExpr();
                    expect(T::RBracket, "']'", &open);
                    return e;
                }
                e->args.push_back(std::move(first));
                while (accept(T::Comma)) {
                    if (peek().k == T::RBracket) break;  // trailing comma
                    e->args.push_back(parseExpr());
                }
                expect(T::RBracket, "',' or ']'", &open);
                return e;
            }
            case T::Ident:
                next();
                e->s = std::string(t.text);
                if (peek().k == T::LParen) {
                    const Token& open = next();
                    e->k = Expr::Call;
                    if (peek().k != T::RParen) {
                        do e->args.push_back(parseExpr());
                        while (accept(T::Comma));
                    }
                    expect(T::RParen, "',' or ')'", &open);
                }
                else {
                    e->k = Expr::Ident;
                }
                return e;
            case T::KConst:
                throw CompileError{ t.loc, "'const' must follow 'set'", {}, {} }.hint("write: set const x = ...;");
            default: {
                CompileError err{ t.loc, "expected an expression, found " + describe(t), {}, {} };
                if (t.k == T::Eof || t.k == T::Semi || t.k == T::RParen || t.k == T::RBracket || t.k == T::Comma || t.k == T::RBrace)
                    err.loc = (t.k == T::Eof || onNewLine()) ? endOfPrev() : t.loc;
                if (t.k == T::KElse) err.hint("'else' must come right after the closing '}' of an 'if' block");
                else if (t.k == T::KFn) err.hint("functions can only be declared at the top level, not inside another function");
                else if (t.k == T::KStruct) err.hint("structs can only be declared at the top level");
                throw err;
            }
            }
        }
    };

}  // namespace il
