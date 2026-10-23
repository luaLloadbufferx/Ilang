/**
 * @file main.c
 * @brief ilangvm: command line front end of the Ilang VM.
 *
 * It loads the host libraries (`--lib`), reads a bytecode file and runs it.
 *
 * @code{.sh}
 * ilangvm [--lib <library>]... <file.ic>
 * @endcode
 */

/* Standard library */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VM Headers */
#include "./VM/IlangVM.h"

/* Host libraries (--lib) are loaded with dlopen / LoadLibrary unless ILANG_VM_NO_DYNLIB is defined. */
#ifndef ILANG_VM_NO_DYNLIB
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

/** @brief The one VM instance. It must start zeroed: the host function definitions live in it (see IlangVM_define()). */
iState g_State = {0};

/**
 * @brief Prints the command line help.
 * @param out Where to print it (stdout for --help, stderr after a mistake).
 * @param self The program name (`argv[0]`).
 */
static void usage(FILE* out, const char* self) {
    fprintf(out, "[*] Usage: %s [--lib <library>]... <file.ic>\n", self);
    fprintf(out, "    --lib, -l <library>   load a host library (.so / .dll / .dylib) whose C functions the program calls\n");
}

/**
 * @brief Loads one host library and lets it define its functions.
 *
 * The library is opened with dlopen / LoadLibrary (unless ILANG_VM_NO_DYNLIB is defined) and its `ilang_open` function
 * is run against the VM. The library stays loaded until the process ends: its functions are called while the program
 * runs.
 *
 * @param path Path of the `.so` / `.dll` / `.dylib`.
 * @return false (after printing why) if it cannot be loaded, has no `ilang_open`, or `ilang_open` fails.
 */
static bool load_library(const char* path) {
#ifdef ILANG_VM_NO_DYNLIB
    fprintf(stderr, "[-] Cannot load '%s': this VM was built without host library support (ILANG_VM_NO_DYNLIB).\n", path);
    return false;
#else
    IlangOpenFn open;
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(path);
    if (!lib) {
        fprintf(stderr, "[-] Cannot load host library '%s' (Windows error %lu).\n", path, (unsigned long)GetLastError());
        return false;
    }
    open = (IlangOpenFn)(void (*)(void))GetProcAddress(lib, "ilang_open");
#else
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "[-] Cannot load host library '%s': %s\n", path, dlerror());
        return false;
    }
    *(void**)(&open) = dlsym(lib, "ilang_open");
#endif
    if (!open) {
        fprintf(stderr, "[-] '%s' is not an Ilang host library (it has no ilang_open function).\n", path);
        return false;
    }
    if (!IlangVM_open_library(&g_State, open)) {
        fprintf(stderr, "[-] Host library '%s' failed to initialise.\n", path);
        return false;
    }
    /* the library stays loaded until the process ends: its functions are called while the program runs */
    return true;
#endif
}

/**
 * @brief Entry point. Usage: `ilangvm [--lib <library>]... <file.ic>`.
 *
 * Options: `--lib` / `-l <library>` loads a host library, `--help` / `-h` prints the usage.
 *
 * @return 0 if the program ran to the end, 1 on a usage, load or runtime error.
 */
int main(int argc, char* argv[]) {
    const char* file = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0 || strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc) {
                usage(stderr, argv[0]);
                return 1;
            }
            if (!load_library(argv[++i])) {
                fprintf(stderr, "[!] Aborting!\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        else if (!file) {
            file = argv[i];
        }
        else {
            usage(stderr, argv[0]);
            return 1;
        }
    }

    if (!file) {
        usage(stderr, argv[0]);
        return 1;
    }

    FILE* fp;

    fp = fopen(file, "rb");
    if (!fp) {
        fprintf(stderr, "[-] There was an error opening the file.\n[!] Aborting!\n");
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    /* Anything that cannot even hold the header is not bytecode */
    if (size < 0 || (size_t)size <= sizeof(iHeader)) {
        fprintf(stderr, "[-] The file is empty or too small to be a valid bytecode file.\n[!] Aborting!\n");
        if (fclose(fp) != 0) {
            fprintf(stderr, "[-] There was an error closing the file.\n");
        }
        return 1;
    }

    uint8_t* buffer = malloc((size_t)size);

    if (!buffer) {
        fprintf(stderr, "[-] There was an error allocating memory for the file.\n[!] Aborting!\n");

        if (fclose(fp) != 0) {
            fprintf(stderr, "[-] There was an error closing the file.\n");
        }

        return 1;
    }

    if (fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "[-] There was an error reading the file.\n[!] Aborting!\n");

        free(buffer);
        fclose(fp);

        return 1;
    }

    /* The whole file is in memory now, so the handle is no longer needed */
    if (fclose(fp) != 0) {
        fprintf(stderr, "[-] There was an error closing the file.\n");
    }

    /* Ready up the VM (this also binds the program's host functions to the ones the libraries defined) */
    bool ready = IlangVM_load(&g_State, buffer, size);
    if (!ready) {
        if (g_State.load_error[0] != '\0') {
            fprintf(stderr, "[-] Cannot load '%s': %s\n[!] Aborting!\n", file, g_State.load_error);
        }
        else {
            fprintf(stderr, "[-] Something went wrong during loading. The bytecode version may not be supported.\n[!] Aborting!\n");
        }

        free(buffer);

        return 1;
    }

    /* Start the execution. A panic (or any other runtime error) makes the process exit with 1 */
    bool ok = IlangVM_execute(&g_State);

    /* Make sure everything the program printed is out before we exit */
    fflush(stdout);

    free(buffer);
    IlangVM_host_reset(&g_State);

    return ok ? 0 : 1;
}
