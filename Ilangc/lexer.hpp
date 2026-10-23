/**
 * @file lexer.hpp
 * @brief Tokenizer for Ilang source text.
 *
 * The `\@include` preprocessor is folded into lexing, so included files are tokenized in place (no text copying,
 * correct line information). Comments (`//` and non-nesting block comments) and white space are skipped; the result
 * is a flat list of il::Token ending in an `Eof` token.
 *
 * The lexer reports what it can step over (a stray character, a bad escape, an oversized literal) as a list of
 * errors and keeps going, so one run shows every lexical problem.
 */
#pragma once
#include "ilang.hpp"

#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <unordered_map>

namespace il {
    /** @brief Shorthand for `std::filesystem`, used for `\@include` path handling. */
    namespace fs = std::filesystem;

    /**
     * @brief Token kinds. Keywords are `K*`.
     *
     * `extern` and `export` are deliberately not keywords: they are plain identifiers that the parser recognises only in
     * front of `fn` at the top level. `+=`, `++` and the other compound operators are not tokens of their own either; the
     * parser recognises two adjacent tokens (`+` directly followed by `=`).
     */
    enum class T : uint8_t {
        Eof,       ///< End of input (always the last token).
        Ident,     ///< Identifier `[A-Za-z_][A-Za-z0-9_]*` that is not a keyword.
        Int,       ///< Integer literal, decimal or `0x` hex, 0 .. 4294967295 (value in Token::iv).
        Float,     ///< Float literal such as `1.5`, `2e10` (value in Token::dv).
        Str,       ///< String literal; escapes are decoded into Token::sv.
        Char,      ///< Character literal such as `'a'` or `'\n'` (a byte, value in Token::iv).
        // keywords (the order KSet..KUnsafe matters: Parser::describe() tests the range)
        KSet,      ///< `set`
        KConst,    ///< `const`
        KFn,       ///< `fn`
        KStruct,   ///< `struct`
        KIf,       ///< `if`
        KElse,     ///< `else`
        KWhile,    ///< `while`
        KFor,      ///< `for`
        KBreak,    ///< `break`
        KContinue, ///< `continue`
        KReturn,   ///< `return`
        KTrue,     ///< `true`
        KFalse,    ///< `false`
        KUnsafe,   ///< `unsafe`
        // punctuation and operators
        LBrace,    ///< `{`
        RBrace,    ///< `}`
        LParen,    ///< `(`
        RParen,    ///< `)`
        LBracket,  ///< `[`
        RBracket,  ///< `]`
        Semi,      ///< `;`
        Comma,     ///< `,`
        Colon,     ///< `:`
        Dot,       ///< `.`
        Arrow,     ///< `->`
        Assign,    ///< `=`
        Eq,        ///< `==`
        Ne,        ///< `!=`
        Lt,        ///< `<`
        Gt,        ///< `>`
        Le,        ///< `<=`
        Ge,        ///< `>=`
        Shl,       ///< `<<`
        Shr,       ///< `>>`
        Plus,      ///< `+`
        Minus,     ///< `-`
        Star,      ///< `*`
        Slash,     ///< `/`
        Percent,   ///< `%`
        Amp,       ///< `&`
        Pipe,      ///< `|`
        Caret,     ///< `^`
        AndAnd,    ///< `&&`
        OrOr,      ///< `||`
        Bang,      ///< `!`
        Tilde,     ///< `~`
        Question   ///< `?`
    };

    /**
     * @brief One token.
     *
     * `text` is the token as written (a view into the lexer's copy of the source); the value is in `iv` (Int, Char),
     * `dv` (Float) or `sv` (Str, with its escape sequences decoded).
     */
    struct Token {
        T k = T::Eof;  ///< The kind of token.
        Loc loc;  ///< Where the token starts; `loc.len` is its length in bytes.
        std::string_view text;  ///< The token exactly as written in the source (points into Lexer's storage, so it stays valid while the Lexer lives).
        int64_t iv = 0;  ///< Value of an Int or Char token.
        double dv = 0;  ///< Value of a Float token.
        std::string sv;  ///< Decoded contents of a Str token (escape sequences resolved; may contain NUL bytes).
    };

    /**
     * @brief Turns source text into tokens, following `\@include` directives as it goes.
     *
     * Usage: default-construct, call run() with the main file, then read #toks.
     * @code
     * Lexer lx;
     * lx.run("program.i");        // throws CompileErrors if the source has lexical errors
     * Parser ps(lx.toks);
     * @endcode
     *
     * `\@include "file"` or `\@include <file>` lexes the named file in place. The path is resolved relative to the main
     * file's directory first, then next to the including file. A file is included at most once, which also breaks include
     * cycles.
     */
    class Lexer {
    public:
        std::vector<Token> toks;  ///< The tokens of the main file and everything it includes, ending with an `Eof` token. Valid after run() returns.

        /**
         * @brief Lexes the main file (and everything it `\@include`s).
         *
         * Problems the lexer can step over (a stray character, a bad escape, an oversized literal) are collected and thrown
         * together at the end; unrecoverable ones (an unreadable file, an unterminated comment) stop lexing of that file.
         *
         * @param mainFile The program's main source file.
         * @throws CompileErrors if any lexical error was found.
         */
        void run(const fs::path& mainFile) {
            std::error_code ec;
            mainDir_ = fs::absolute(mainFile, ec).parent_path();
            try {
                lexFile(mainFile, Loc{});
            }
            catch (CompileError& e) {  // the unrecoverable kind (unreadable file, unterminated comment, ...)
                errs_.push_back(std::move(e));
            }
            if (!errs_.empty()) throw CompileErrors{ std::move(errs_), false };
            Token eof;
            if (!toks.empty()) eof.loc = toks.back().loc;
            eof.loc.len = 0;
            toks.push_back(eof);
        }

    private:
        std::deque<std::string> srcs_;  ///< Source text of every file read. Tokens point into it; a deque keeps the addresses stable as files are added.
        std::vector<CompileError> errs_;  ///< Errors found so far that did not stop the lexer.

        /**
         * @brief Records an error the lexer can step over.
         * @param l   Where the problem is.
         * @param msg What is wrong.
         * @return The stored error, so the caller can attach a hint with CompileError::hint().
         */
        CompileError& soft(Loc l, std::string msg) {
            errs_.push_back(CompileError{ l, std::move(msg), {}, {} });
            return errs_.back();
        }
        std::set<std::string> seen_;  ///< Canonical paths of the files already lexed: a second `\@include` of one of them is skipped.
        fs::path mainDir_;            ///< Directory of the main file, the base for `\@include` paths.

        /** @brief True if @p c can start an identifier (a letter or `_`). */
        static bool idStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
        /** @brief True if @p c can continue an identifier (a letter, digit or `_`). */
        static bool idChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

        /**
         * @brief Classifies a word.
         * @param w An identifier-shaped word.
         * @return The keyword token kind for @p w, or T::Ident if it is not a keyword.
         */
        static T keyword(std::string_view w) {
            static const std::unordered_map<std::string_view, T> kw = {
                {"set", T::KSet}, {"const", T::KConst}, {"fn", T::KFn}, {"struct", T::KStruct}, {"if", T::KIf},
                {"else", T::KElse}, {"while", T::KWhile}, {"for", T::KFor}, {"break", T::KBreak},
                {"continue", T::KContinue}, {"return", T::KReturn},
                {"true", T::KTrue}, {"false", T::KFalse}, {"unsafe", T::KUnsafe} };
            auto it = kw.find(w);
            return it == kw.end() ? T::Ident : it->second;
        }

        /**
         * @brief Finds the file named by an `\@include` directive.
         *
         * The path is taken relative to the main file's directory first, then
         * relative to the including file.
         *
         * @param name   The path as written in the directive.
         * @param curDir Directory of the file containing the directive.
         * @param l      Location of the directive, for the error message.
         * @return The path of the file to read.
         * @throws CompileError if neither location has the file.
         */
        fs::path resolveInclude(const std::string& name, const fs::path& curDir, Loc l) {
            std::error_code ec;
            fs::path a = mainDir_ / name;  // first choice: relative to the main file's directory
            if (fs::exists(a, ec)) return a;
            fs::path b = curDir / name;    // fallback: relative to the including file
            if (fs::exists(b, ec)) return b;
            fail(CompileError{ l, "cannot find include file '" + name + "'", {}, {} }
                .note(Loc{}, "looked in the main file's directory (" + mainDir_.string() + ") and next to the including file")
                .hint("the path is taken relative to the file being compiled, e.g. @include \"lib/util.i\""));
        }

        /**
         * @brief Tokenizes one file into #toks, recursing into its `\@include`s.
         *
         * Skips a UTF-8 byte order mark, white space and comments; recognises identifiers and keywords, numbers
         * (lexNumber()), string and character literals with their escapes, `\@include` directives and punctuation (taking the
         * longest match, so `<<` is one token). A file already seen is skipped, which also stops include cycles.
         *
         * @param path The file to read.
         * @param from The `\@include` that pulled the file in, for error messages (`Loc{}` for the main file).
         * @throws CompileError for an unrecoverable problem (an unreadable file, an unterminated comment or character
         *         literal, a bad directive).
         */
        void lexFile(const fs::path& path, Loc from) {
            std::error_code ec;
            std::string key = fs::weakly_canonical(path, ec).string();
            if (!seen_.insert(key).second) return;  // already included: skip (also stops cycles)

            std::ifstream in(path, std::ios::binary);
            if (!in) fail(from, "cannot open '" + path.string() + "'");
            srcs_.emplace_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const std::string& s = srcs_.back();
            const size_t n = s.size();
            const int fid = (int)fileNames().size();
            fileNames().push_back(path.string());
            fileSources().push_back(s);  // kept for error messages (the lexer itself does not outlive the parse)

            size_t i = 0, lineStart = 0;
            int line = 1;
            if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
                i = 3;
                lineStart = 3;
            }
            auto loc = [&](size_t pos) { return Loc{ fid, line, (int)(pos - lineStart) + 1 }; };
            auto push = [&](T k, size_t start, size_t len) -> Token& {
                Token t;
                t.k = k;
                t.loc = loc(start);
                t.loc.len = (int)len;
                t.text = std::string_view(s).substr(start, len);
                toks.push_back(std::move(t));
                return toks.back();
                };

            while (i < n) {
                char c = s[i];
                if (c == '\n') { ++line; ++i; lineStart = i; continue; }
                if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++i; continue; }

                if (c == '/' && i + 1 < n && s[i + 1] == '/') {
                    while (i < n && s[i] != '\n') ++i;
                    continue;
                }
                if (c == '/' && i + 1 < n && s[i + 1] == '*') {
                    size_t st = i;
                    Loc where = loc(st);
                    where.len = 2;
                    i += 2;
                    while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                        if (s[i] == '\n') { ++line; lineStart = i + 1; }
                        ++i;
                    }
                    if (i + 1 >= n) fail(CompileError{ where, "unterminated block comment", {}, {} }
                        .hint("a comment that starts with /* must be closed with */"));
                    i += 2;
                    continue;
                }

                if (idStart(c)) {
                    size_t st = i;
                    while (i < n && idChar(s[i])) ++i;
                    push(keyword(std::string_view(s).substr(st, i - st)), st, i - st);
                    continue;
                }

                if (std::isdigit((unsigned char)c)) { lexNumber(s, i, push, loc); continue; }

                if (c == '"') {
                    size_t st = i++;
                    Loc where = loc(st);
                    std::string val;
                    bool ok = true;
                    for (;;) {
                        if (i >= n || s[i] == '\n') {
                            where.len = (int)(i - st);
                            soft(where, "unterminated string literal").hint("add the closing '\"' before the end of the line");
                            ok = false;
                            break;
                        }
                        char ch = s[i++];
                        if (ch == '"') break;
                        if (ch != '\\') { val += ch; continue; }
                        if (i >= n || s[i] == '\n') continue;  // the unterminated-string error above reports it
                        char e = s[i++];
                        switch (e) {
                        case 'n': val += '\n'; break;
                        case 't': val += '\t'; break;
                        case 'r': val += '\r'; break;
                        case '0': val += '\0'; break;
                        case '\\': val += '\\'; break;
                        case '"': val += '"'; break;
                        case '\'': val += '\''; break;
                        case 'x': {
                            auto hex = [&](char h) -> int {
                                if (h >= '0' && h <= '9') return h - '0';
                                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                                return -1;
                                };
                            if (i + 1 >= n || hex(s[i]) < 0 || hex(s[i + 1]) < 0) {
                                Loc l = loc(i - 2);
                                l.len = 2;
                                soft(l, "bad \\x escape").hint("\\x must be followed by exactly two hex digits, e.g. \\x41");
                                break;
                            }
                            val += (char)(hex(s[i]) * 16 + hex(s[i + 1]));
                            i += 2;
                            break;
                        }
                        default: {
                            Loc l = loc(i - 2);
                            l.len = 2;
                            soft(l, std::string("unknown escape sequence '\\") + e + "'")
                                .hint("valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\' and \\xHH");
                            break;
                        }
                        }
                    }
                    if (ok) push(T::Str, st, i - st).sv = std::move(val);
                    continue;
                }

                if (c == '\'') {  // character literal: 'a', '\n', '\x41'  (a byte)
                    size_t st = i++;
                    Loc where = loc(st);
                    where.len = 1;
                    if (i >= n || s[i] == '\n') fail(CompileError{ where, "unterminated character literal", {}, {} }
                        .hint("a character literal looks like 'a' or '\\n'"));
                    int64_t val;
                    char ch = s[i++];
                    if (ch == '\'') {
                        where.len = 2;
                        fail(CompileError{ where, "empty character literal", {}, {} }.hint("put one character between the quotes, e.g. 'a'"));
                    }
                    if (ch != '\\') {
                        val = (unsigned char)ch;
                    }
                    else {
                        if (i >= n) fail(where, "unterminated character literal");
                        char e = s[i++];
                        switch (e) {
                        case 'n': val = '\n'; break;
                        case 't': val = '\t'; break;
                        case 'r': val = '\r'; break;
                        case '0': val = 0; break;
                        case '\\': val = '\\'; break;
                        case '\'': val = '\''; break;
                        case '"': val = '"'; break;
                        case 'x': {
                            auto hex = [&](char h) -> int {
                                if (h >= '0' && h <= '9') return h - '0';
                                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                                return -1;
                                };
                            if (i + 1 >= n || hex(s[i]) < 0 || hex(s[i + 1]) < 0) {
                                Loc l = loc(i - 2);
                                l.len = 2;
                                fail(CompileError{ l, "bad \\x escape", {}, {} }.hint("\\x must be followed by exactly two hex digits, e.g. \\x41"));
                            }
                            val = hex(s[i]) * 16 + hex(s[i + 1]);
                            i += 2;
                            break;
                        }
                        default: {
                            Loc l = loc(i - 2);
                            l.len = 2;
                            fail(CompileError{ l, std::string("unknown escape sequence '\\") + e + "'", {}, {} }
                                .hint("valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\' and \\xHH"));
                        }
                        }
                    }
                    if (i >= n || s[i] != '\'') {
                        where.len = (int)(i - st);
                        fail(CompileError{ where, "character literals hold exactly one byte", {}, {} }
                            .hint("use a double-quoted string for more than one character, e.g. \"abc\""));
                    }
                    ++i;
                    push(T::Char, st, i - st).iv = val;
                    continue;
                }

                if (c == '@') {
                    size_t st = i++;
                    size_t b = i;
                    while (i < n && idChar(s[i])) ++i;
                    std::string_view dir = std::string_view(s).substr(b, i - b);
                    Loc dl = loc(st);
                    dl.len = (int)(i - st);
                    if (dir != "include")
                        fail(CompileError{ dl, "unknown directive '@" + std::string(dir) + "'", {}, {} }.hint("the only directive is @include \"file\""));
                    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
                    char close = 0;
                    if (i < n && s[i] == '"') close = '"';
                    else if (i < n && s[i] == '<') close = '>';
                    else fail(CompileError{ dl, "expected \"file\" or <file> after @include", {}, {} });
                    ++i;
                    size_t f0 = i;
                    while (i < n && s[i] != close && s[i] != '\n') ++i;
                    if (i >= n || s[i] != close) fail(CompileError{ dl, "unterminated @include", {}, {} }
                        .hint(std::string("close the file name with '") + close + "'"));
                    std::string name = s.substr(f0, i - f0);
                    ++i;
                    dl.len = (int)(i - st);
                    lexFile(resolveInclude(name, path.parent_path(), dl), dl);
                    continue;
                }

                // punctuation
                char d = (i + 1 < n) ? s[i + 1] : '\0';
                auto two = [&](T k) { push(k, i, 2); i += 2; };
                auto one = [&](T k) { push(k, i, 1); i += 1; };
                switch (c) {
                case '-': if (d == '>') two(T::Arrow); else one(T::Minus); break;
                case '=': if (d == '=') two(T::Eq); else one(T::Assign); break;
                case '!': if (d == '=') two(T::Ne); else one(T::Bang); break;
                case '<': if (d == '=') two(T::Le); else if (d == '<') two(T::Shl); else one(T::Lt); break;
                case '>': if (d == '=') two(T::Ge); else if (d == '>') two(T::Shr); else one(T::Gt); break;
                case '&': if (d == '&') two(T::AndAnd); else one(T::Amp); break;
                case '|': if (d == '|') two(T::OrOr); else one(T::Pipe); break;
                case '^': one(T::Caret); break;
                case '~': one(T::Tilde); break;
                case '{': one(T::LBrace); break;
                case '}': one(T::RBrace); break;
                case '(': one(T::LParen); break;
                case ')': one(T::RParen); break;
                case '[': one(T::LBracket); break;
                case ']': one(T::RBracket); break;
                case ';': one(T::Semi); break;
                case ',': one(T::Comma); break;
                case ':': one(T::Colon); break;
                case '?': one(T::Question); break;
                case '.': one(T::Dot); break;
                case '+': one(T::Plus); break;
                case '*': one(T::Star); break;
                case '/': one(T::Slash); break;
                case '%': one(T::Percent); break;
                default: {
                    Loc l = loc(i);
                    const unsigned char uc = (unsigned char)c;
                    if (uc >= 0x80) {  // a whole UTF-8 sequence is one "character" to the reader
                        size_t k = i + 1;
                        while (k < n && ((unsigned char)s[k] & 0xC0) == 0x80) ++k;
                        l.len = (int)(k - i);
                        soft(l, "unexpected non-ASCII character")
                            .hint("outside strings and comments only ASCII is allowed (typographic quotes pasted from a document are a common cause)");
                        i = k;
                        break;
                    }
                    l.len = 1;
                    CompileError& err = soft(l, std::isprint(uc) ? std::string("unexpected character '") + c + "'"
                        : "unexpected byte 0x" + std::string(1, "0123456789ABCDEF"[uc >> 4]) + "0123456789ABCDEF"[uc & 15]);
                    if (c == '#') err.hint("comments are written // like this, or /* like this */");
                    else if (c == '$' || c == '`') err.hint("this character has no meaning in Ilang");
                    ++i;
                    break;
                }
                }
            }
        }

        /**
         * @brief Lexes a decimal or `0x` hex integer, or a float, starting at `s[i]`, and advances @p i past it.
         *
         * A bad literal (too large, no hex digits, glued to letters as in `12abc`) is recorded with soft() and still produces
         * a token (value 0) so that lexing can go on.
         *
         * @tparam Push  Callable `Token&(T kind, size_t start, size_t length)` that appends a token.
         * @tparam LocFn Callable `Loc(size_t position)` that converts an offset into a source location.
         * @param s    The text of the file being lexed.
         * @param i    In: offset of the first digit. Out: offset just past the literal.
         * @param push Appends a token.
         * @param loc  Converts an offset into a Loc.
         */
        template <class Push, class LocFn>
        void lexNumber(const std::string& s, size_t& i, Push& push, LocFn& loc) {
            const size_t n = s.size();
            size_t st = i;
            auto here = [&](size_t end) { Loc l = loc(st); l.len = (int)(end - st); return l; };
            // `12abc`: a number glued to letters is almost certainly a typo (or a suffix this language does not have)
            auto junkSuffix = [&]() {
                if (i < n && idStart(s[i])) {
                    size_t k = i;
                    while (k < n && idChar(s[k])) ++k;
                    soft(here(k), "invalid number '" + s.substr(st, k - st) + "'")
                        .hint("a number cannot be directly followed by letters; there are no type suffixes (use a cast, e.g. byte(5))");
                    i = k;
                    return true;
                }
                return false;
            };
            if (s[i] == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                i += 2;
                size_t d0 = i;
                uint64_t v = 0;
                bool big = false;
                while (i < n && std::isxdigit((unsigned char)s[i])) {
                    char h = s[i];
                    if (!big) v = v * 16 + (uint64_t)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                    if (v > 0xFFFFFFFFull) big = true;
                    ++i;
                }
                if (i == d0) {
                    soft(here(i), "expected hex digits after '0x'");
                    push(T::Int, st, i - st).iv = 0;
                    return;
                }
                if (junkSuffix()) { push(T::Int, st, i - st).iv = 0; return; }
                if (big) {
                    soft(here(i), "integer literal " + s.substr(st, i - st) + " is too large").hint("the largest integer literal is 0xFFFFFFFF (4294967295)");
                    v = 0;
                }
                push(T::Int, st, i - st).iv = (int64_t)v;
                return;
            }
            while (i < n && std::isdigit((unsigned char)s[i])) ++i;
            bool isFloat = false;
            if (i + 1 < n && s[i] == '.' && std::isdigit((unsigned char)s[i + 1])) {
                isFloat = true;
                ++i;
                while (i < n && std::isdigit((unsigned char)s[i])) ++i;
            }
            if (i < n && (s[i] == 'e' || s[i] == 'E')) {
                size_t j = i + 1;
                if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
                if (j < n && std::isdigit((unsigned char)s[j])) {
                    isFloat = true;
                    i = j;
                    while (i < n && std::isdigit((unsigned char)s[i])) ++i;
                }
            }
            const size_t numEnd = i;
            if (junkSuffix()) {
                push(T::Int, st, i - st).iv = 0;
                return;
            }
            std::string txt = s.substr(st, numEnd - st);
            if (isFloat) {
                push(T::Float, st, numEnd - st).dv = std::strtod(txt.c_str(), nullptr);
            }
            else {
                uint64_t v = 0;
                bool big = false;
                for (char ch : txt) {
                    v = v * 10 + (uint64_t)(ch - '0');
                    if (v > 0xFFFFFFFFull) { big = true; break; }
                }
                if (big) {
                    soft(here(numEnd), "integer literal " + txt + " is too large").hint("the largest integer literal is 4294967295");
                    v = 0;
                }
                push(T::Int, st, numEnd - st).iv = (int64_t)v;
            }
        }
    };

}  // namespace il
