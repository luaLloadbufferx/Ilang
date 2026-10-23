/**
 * @file main.cpp
 * @brief `ilangc`, the command-line driver of the Ilang compiler.
 *
 * Translates an Ilang source file (`.i`) into a bytecode image (`.ic`) for the Ilang VM. The pipeline is
 * il::Lexer (tokens, with `@include` expanded) -> il::Parser (syntax tree, il::Module) -> il::Compiler (semantic
 * checks and bytecode generation in one walk) -> the image is written to disk.
 *
 * Build and usage:
 * @code{.sh}
 * g++ -std=c++17 -O2 main.cpp -o ilangc
 * ./ilangc program.i [-o program.ic] [--regs N] [--temps N] [--zero-init] [--strict-tags] [-v]
 * @endcode
 *
 * Exit status: 0 on success, 1 for compile errors or an unwritable output file, 2 for bad usage.
 */

#include "codegen.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

/**
 * @brief Prints the command-line help.
 * @param out Where to print it (stdout for --help, stderr after a mistake).
 */
static void usage(std::FILE* out) {
    std::fputs(
        "usage: ilangc <input.i> [-o output.ic] [options]\n"
        "  --regs N       number of VM registers (REG_COUNT), 8..64   [default 24]\n"
        "  --temps N      registers reserved for temporaries          [default max(4, regs/3)]\n"
        "  --zero-init    zero struct fields (redundant: VM memory is already zeroed)\n"
        "  --strict-tags  retag int array indices as uint (for VMs built with ILANG_VM_STRICT_TYPES; costs 2 instructions per checked access)\n"
        "  -v             print compile statistics\n",
        out);
}

/**
 * @brief Entry point of the compiler.
 *
 * Parses the command line (one input file plus options; an option that takes a value reads the next argument),
 * runs the lexer, parser and code generator, and writes the bytecode image. Compile errors are printed to standard
 * error with printErrors().
 *
 * Options:
 *  - `-o file`      output file (default: the input with the extension `.ic`);
 *  - `--regs N`     number of VM registers (`REG_COUNT`), 8..64, default 24 - must equal the VM's setting;
 *  - `--temps N`    registers reserved for expression temporaries, default `max(4, regs/3)`;
 *  - `--zero-init`  explicitly zero struct fields (redundant: VM memory is already zeroed);
 *  - `--strict-tags` retag int array indices as uint, for a VM built with `ILANG_VM_STRICT_TYPES`;
 *  - `-v`           print compile statistics to standard error;
 *  - `-h`, `--help` print the usage text to standard output.
 *
 * @param argc Number of command-line arguments.
 * @param argv The arguments; `argv[0]` is the program name.
 * @return 0 on success; 1 if the program has compile errors or the output cannot be written;
 *         2 for a bad command line (unknown option, missing value or no input file).
 */
int main(int argc, char** argv) {
    using namespace il;
    std::string input, output;
    Target tg;
    int temps = -1;
    bool verbose = false;

    // One input file plus options; an option that takes a value reads the next argument.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) { usage(stderr); std::exit(2); }
            return argv[++i];
            };
        if (a == "-o") output = val();
        else if (a == "--regs") tg.regs = std::atoi(val());
        else if (a == "--temps") temps = std::atoi(val());
        else if (a == "--zero-init") tg.zeroInit = true;
        else if (a == "--strict-tags") tg.strictTags = true;
        else if (a == "-v") verbose = true;
        else if (a == "-h" || a == "--help") { usage(stdout); return 0; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option '%s'\n", a.c_str()); usage(stderr); return 2; }
        else input = a;
    }
    if (input.empty()) { usage(stderr); return 2; }
    // Unless told otherwise, a third of the registers (at least four) are kept for expression temporaries.
    tg.temps = temps >= 0 ? temps : std::max(4, tg.regs / 3);
    if (output.empty()) output = fs::path(input).replace_extension(".ic").string();

    // Lex, parse and compile the program, then write the image. Problems are collected and printed together.
    auto t0 = std::chrono::steady_clock::now();
    try {
        Lexer lx;
        lx.run(input);
        Parser ps(lx.toks);
        Module mod = ps.parse();
        Compiler comp(mod, tg);
        std::vector<uint8_t> img = comp.compile();

        std::FILE* f = std::fopen(output.c_str(), "wb");
        if (!f || std::fwrite(img.data(), 1, img.size(), f) != img.size()) {
            std::fprintf(stderr, "error: cannot write '%s'\n", output.c_str());
            return 1;
        }
        std::fclose(f);

        if (verbose) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "%zu tokens, %zu functions, %zu structs -> %zu bytes in %.2f ms (%d registers, %d for temporaries, %d spills)\n",
                lx.toks.size(), comp.funcs.size(), comp.structs.size(), img.size(), ms, tg.regs, tg.temps, comp.spills);
        }
    }
    catch (const CompileErrors& es) {  // several problems, collected in one pass
        printErrors(stderr, es);
        return 1;
    }
    catch (const CompileError& e) {    // a single problem that stopped the compiler on the spot
        CompileErrors es;
        es.list.push_back(e);
        printErrors(stderr, es);
        return 1;
    }
    return 0;
}
