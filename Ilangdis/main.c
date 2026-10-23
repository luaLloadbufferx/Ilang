/**
 * @file main.c
 * @brief Ilangdis: command line front end of the Ilang disassembler.
 *
 * Reads a bytecode file (`.ic`) and prints a listing of it: the instructions, the functions and labels the analysis
 * found, arrows for jumps and calls, and optionally the raw bytes.
 *
 * @code{.sh}
 * Ilangdis [options] <file.ic>
 * Ilangdis --bytes program.ic | less -R
 * Ilangdis --functions program.ic
 * Ilangdis -F fib --arrows=jumps program.ic
 * @endcode
 *
 * Exit status: 0 on success, 1 if the file cannot be read or is not valid bytecode, 2 for a bad command line.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
/** @brief Makes fileno() and isatty() visible in strict -std=c11 builds. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "ilangdis.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/** @brief Keeps windows.h small. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
/** @brief isatty() under its Windows name. */
#define dis_isatty _isatty
/** @brief fileno() under its Windows name. */
#define dis_fileno _fileno
#else
#include <unistd.h>
/** @brief isatty() (POSIX). */
#define dis_isatty isatty
/** @brief fileno() (POSIX). */
#define dis_fileno fileno
#endif

/** @brief When to use a feature that depends on the terminal. */
typedef enum When {
    WHEN_AUTO,    /**< Decide from the output and the environment. */
    WHEN_ALWAYS,  /**< Always. */
    WHEN_NEVER    /**< Never. */
} When;

/** @brief Identifies a command line option. */
typedef enum OptId {
    OPT_HELP, OPT_VERSION, OPT_BYTES, OPT_BYTES_PER_LINE, OPT_ARROWS, OPT_NO_ARROWS, OPT_ARROW_WIDTH, OPT_NO_LABELS,
    OPT_NO_COMMENTS, OPT_NO_HEADER, OPT_STRING_LIMIT, OPT_FUNCTION, OPT_RANGE, OPT_NAME, OPT_NAMES, OPT_FUNCTIONS,
    OPT_CALL_GRAPH, OPT_INFO, OPT_OUTPUT, OPT_COLOR, OPT_ASCII, OPT_UNICODE
} OptId;

/** @brief Whether an option takes a value. */
typedef enum ArgKind {
    ARG_NONE,      /**< No value. */
    ARG_REQUIRED,  /**< A value: `--name value`, `--name=value` or `-nvalue`. */
    ARG_OPTIONAL   /**< An optional value, only as `--name=value`. */
} ArgKind;

/** @brief One entry of the option table. */
typedef struct OptSpec {
    char shortname;       /**< The one-letter form, or 0. */
    const char* name;     /**< The long form (without the dashes). */
    ArgKind arg;          /**< Whether it takes a value. */
    OptId id;             /**< What it is. */
} OptSpec;

/** @brief All options. A long option may be abbreviated to any unambiguous prefix. */
static const OptSpec kOptions[] = {
    { 'h', "help", ARG_NONE, OPT_HELP },
    { 'V', "version", ARG_NONE, OPT_VERSION },
    { 'b', "bytes", ARG_NONE, OPT_BYTES },
    { 0, "bytes-per-line", ARG_REQUIRED, OPT_BYTES_PER_LINE },
    { 'a', "arrows", ARG_REQUIRED, OPT_ARROWS },
    { 0, "no-arrows", ARG_NONE, OPT_NO_ARROWS },
    { 0, "arrow-width", ARG_REQUIRED, OPT_ARROW_WIDTH },
    { 'n', "no-labels", ARG_NONE, OPT_NO_LABELS },
    { 'c', "no-comments", ARG_NONE, OPT_NO_COMMENTS },
    { 'q', "no-header", ARG_NONE, OPT_NO_HEADER },
    { 0, "string-limit", ARG_REQUIRED, OPT_STRING_LIMIT },
    { 'F', "function", ARG_REQUIRED, OPT_FUNCTION },
    { 'r', "range", ARG_REQUIRED, OPT_RANGE },
    { 'N', "name", ARG_REQUIRED, OPT_NAME },
    { 0, "names", ARG_REQUIRED, OPT_NAMES },
    { 'f', "functions", ARG_NONE, OPT_FUNCTIONS },
    { 'g', "call-graph", ARG_NONE, OPT_CALL_GRAPH },
    { 'i', "info", ARG_NONE, OPT_INFO },
    { 'o', "output", ARG_REQUIRED, OPT_OUTPUT },
    { 0, "color", ARG_OPTIONAL, OPT_COLOR },
    { 0, "ascii", ARG_NONE, OPT_ASCII },
    { 0, "unicode", ARG_NONE, OPT_UNICODE },
};

/** @brief Number of entries in kOptions. */
#define OPTION_COUNT (sizeof kOptions / sizeof kOptions[0])

/**
 * @brief Prints the command line help.
 * @param out Where to print it (stdout for --help, stderr after a mistake).
 */
static void usage(FILE* out) {
    fputs(
        "Ilangdis " ILANGDIS_VERSION " - disassembler for Ilang bytecode (.ic files)\n"
        "\n"
        "usage: ilangdis [options] <file.ic>\n"
        "\n"
        "Listing:\n"
        "  -b, --bytes               show the raw bytes of every instruction\n"
        "      --bytes-per-line <n>  bytes per row in the byte view, 1..32 (default 8)\n"
        "  -a, --arrows <mode>       arrows in the margin: all, jumps, calls or none\n"
        "                            (default all: jumps are single lines, calls double lines)\n"
        "      --no-arrows           same as --arrows none\n"
        "      --arrow-width <n>     widest margin, in lanes, 0..32 (default 12)\n"
        "  -n, --no-labels           print raw addresses; no function names, labels or banners\n"
        "  -c, --no-comments         leave out the comments after instructions\n"
        "  -q, --no-header           leave out the summary at the top\n"
        "      --string-limit <n>    longest string literal shown, in bytes (default 60, 0 = all)\n"
        "\n"
        "Selection:\n"
        "  -F, --function <name|addr>  only this function (a name, or an address inside it);\n"
        "                            may be given several times\n"
        "  -r, --range <from>[-<to>]   only instructions that start in this range of addresses\n"
        "                            (decimal or 0x hexadecimal)\n"
        "\n"
        "Naming:\n"
        "  -N, --name <name|addr>=<new>  call a function <new>, e.g. -N 0x67=fib (a name, or an address\n"
        "                            inside the function); may be given several times\n"
        "      --names <file>        read such names from a file, one '<name|addr> <new>' per line\n"
        "                            (blank lines and lines starting with # or ; are ignored)\n"
        "\n"
        "Reports (they replace the listing):\n"
        "  -f, --functions           table of the functions that were found\n"
        "  -g, --call-graph          call graph from the entry point and the exports\n"
        "  -i, --info                header, imports and exports only\n"
        "\n"
        "Output:\n"
        "  -o, --output <file>       write to a file instead of standard output\n"
        "      --color[=<when>]      colour the output: auto (default), always or never\n"
        "      --ascii, --unicode    draw arrows with plain ASCII or with box-drawing characters\n"
        "                            (default: Unicode if the terminal looks able to show it)\n"
        "  -h, --help                show this help\n"
        "  -V, --version             show the version\n"
        "\n"
        "Use '-' as the file name to read the bytecode from standard input.\n"
        "\n"
        "Functions have no names in the bytecode. Ilangdis names them from the export table,\n"
        "the startup stub (main) and the runtime-error messages the compiler leaves in them;\n"
        "the rest are called func_<address>. Use --name to give those names of your own.\n",
        out);
}

/**
 * @brief Prints a message about a bad command line, then a hint, and returns the exit status.
 * @param fmt printf format of the message.
 * @return 2.
 */
static int bad_usage(const char* fmt, ...) {
    va_list ap;

    fputs("Ilangdis: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputs("\nTry 'ilangdis --help' for the list of options.\n", stderr);
    return 2;
}

/**
 * @brief Reads a whole file into memory; "-" reads standard input.
 * @param[in]  path Where to read from.
 * @param[out] size Receives the number of bytes.
 * @return The contents (release with free()), or NULL after printing why not.
 */
static uint8_t* read_file(const char* path, size_t* size) {
    FILE* fp;
    uint8_t* data = NULL;
    size_t used = 0, capacity = 0, got;
    bool is_stdin = strcmp(path, "-") == 0;

    if (is_stdin) {
#if defined(_WIN32)
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        fp = stdin;
    }
    else {
        fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "Ilangdis: cannot open '%s': %s\n", path, strerror(errno));
            return NULL;
        }
    }
    for (;;) {
        dis_reserve(&data, &capacity, 1, used + 65536);
        got = fread(data + used, 1, capacity - used, fp);
        used += got;
        if (got == 0) break;
        if (used > UINT32_MAX) {
            fprintf(stderr, "Ilangdis: '%s' is too large to be Ilang bytecode\n", path);
            free(data);
            if (!is_stdin) fclose(fp);
            return NULL;
        }
    }
    if (ferror(fp)) {
        fprintf(stderr, "Ilangdis: error reading '%s'\n", path);
        free(data);
        data = NULL;
    }
    if (!is_stdin) fclose(fp);
    *size = used;
    return data;
}

/**
 * @brief Parses a non-negative whole number option value.
 * @param[in]  text  The text.
 * @param[in]  max   Largest value allowed.
 * @param[out] value Receives the number.
 * @return false if @p text is not a number between 0 and @p max.
 */
static bool parse_count(const char* text, unsigned long max, int* value) {
    char* end = NULL;
    unsigned long v;

    if (!*text || !isdigit((unsigned char)*text)) return false;
    errno = 0;
    v = strtoul(text, &end, 10);
    if (*end || errno || v > max) return false;
    *value = (int)v;
    return true;
}

/**
 * @brief Parses an address range such as `0x40-0x80`, `64:128` or `0x40` (from there to the end).
 * @param[in]  text The text.
 * @param[out] from First address.
 * @param[out] to   Address just past the range.
 * @return false if the text is not a range.
 */
static bool parse_range(const char* text, uint32_t* from, uint32_t* to) {
    char* end = NULL;
    unsigned long a, b = 0xFFFFFFFFul;

    errno = 0;
    if (!isdigit((unsigned char)*text)) return false;
    a = strtoul(text, &end, 0);
    if (errno || a > 0xFFFFFFFFul) return false;
    if (*end == '-' || *end == ':') end++;
    else if (end[0] == '.' && end[1] == '.') end += 2;
    else if (*end) return false;
    if (*end) {
        char* end2 = NULL;

        if (!isdigit((unsigned char)*end)) return false;
        errno = 0;
        b = strtoul(end, &end2, 0);
        if (errno || *end2 || b > 0xFFFFFFFFul) return false;
    }
    if (b <= a) return false;
    *from = (uint32_t)a;
    *to = (uint32_t)b;
    return true;
}

#if !defined(_WIN32)
/**
 * @brief Whether the locale looks like it can show UTF-8 (LC_ALL, LC_CTYPE or LANG names it).
 */
static bool locale_is_utf8(void) {
    const char* names[3] = { "LC_ALL", "LC_CTYPE", "LANG" };
    size_t i;

    for (i = 0; i < 3; i++) {
        const char* v = getenv(names[i]);
        size_t k, n;

        if (!v || !*v) continue;
        n = strlen(v);
        for (k = 0; k + 4 <= n; k++) {
            if (tolower((unsigned char)v[k]) == 'u' && tolower((unsigned char)v[k + 1]) == 't' && tolower((unsigned char)v[k + 2]) == 'f' &&
                (v[k + 3] == '8' || (v[k + 3] == '-' && k + 5 <= n && v[k + 4] == '8'))) return true;
        }
        return false;  /* the first one that is set decides */
    }
    return false;
}
#endif

/**
 * @brief Decides whether to colour the output and whether to use box-drawing characters.
 *
 * On Windows a console is switched to UTF-8 and to ANSI escape processing for the run.
 *
 * @param[in]     out      The output stream.
 * @param[in]     is_file  The output is a file chosen with -o.
 * @param[in]     color    The --color choice.
 * @param[in]     charset  0 = decide, 1 = ASCII (--ascii), 2 = Unicode (--unicode).
 * @param[in,out] opt      Receives `color` and `unicode`.
 */
static void detect_terminal(FILE* out, bool is_file, When color, int charset, DisOptions* opt) {
    bool tty = !is_file && dis_isatty(dis_fileno(out));
    const char* no_color = getenv("NO_COLOR");
    const char* term = getenv("TERM");

    opt->color = color == WHEN_ALWAYS || (color == WHEN_AUTO && tty && !(no_color && *no_color) && !(term && strcmp(term, "dumb") == 0));
#if defined(_WIN32)
    if (tty) {
        HANDLE h = (HANDLE)_get_osfhandle(dis_fileno(out));
        DWORD mode = 0;

        if (GetConsoleMode(h, &mode)) {
            if (opt->color && !SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */) && color == WHEN_AUTO) opt->color = false;
            if (SetConsoleOutputCP(CP_UTF8)) opt->unicode = charset != 1;
            else opt->unicode = charset == 2;
            return;
        }
    }
    opt->unicode = charset == 2;
#else
    opt->unicode = charset == 2 || (charset == 0 && locale_is_utf8());
#endif
}

/** @brief A request to name a function: `--name which=name`, or one line of a `--names` file. */
typedef struct Rename {
    char* which;   /**< The function: a name or an offset inside it. */
    char* name;    /**< The new name. */
    char* origin;  /**< Where the request came from, for error messages (`--name` or `file:line`). */
} Rename;

/** @brief Everything the command line sets. */
typedef struct Cli {
    DisOptions opt;          /**< Options that reach the output code. */
    const char* input;       /**< The file to disassemble ("-" = standard input). */
    const char* output;      /**< The file to write, or NULL for standard output. */
    const char** wanted;     /**< The -F arguments. */
    size_t wanted_count;     /**< Number of -F arguments. */
    size_t wanted_capacity;  /**< Allocated entries of #wanted. */
    Rename* renames;         /**< The --name and --names requests, in order. */
    size_t rename_count;     /**< Number of #renames. */
    size_t rename_capacity;  /**< Allocated entries of #renames. */
    When color;              /**< --color. */
    int charset;             /**< 0 = decide, 1 = --ascii, 2 = --unicode. */
    bool want_functions;     /**< -f */
    bool want_graph;         /**< -g */
    bool want_info;          /**< -i */
} Cli;

/** @brief Value of apply_option() and parse_args() that means "carry on". */
#define CONTINUE (-1)

/**
 * @brief Records a request to name a function.
 * @param cli       The settings.
 * @param which     The function (not NUL-terminated).
 * @param which_len Length of @p which.
 * @param name      The new name (not NUL-terminated).
 * @param name_len  Length of @p name.
 * @param origin    Where the request came from, for error messages.
 */
static void add_rename(Cli* cli, const char* which, size_t which_len, const char* name, size_t name_len, const char* origin) {
    Rename* r;

    dis_reserve(&cli->renames, &cli->rename_capacity, sizeof *cli->renames, cli->rename_count + 1);
    r = &cli->renames[cli->rename_count++];
    r->which = dis_xstrndup(which, which_len);
    r->name = dis_xstrndup(name, name_len);
    r->origin = dis_xstrndup(origin, strlen(origin));
}

/**
 * @brief Releases everything a Cli owns.
 * @param cli The settings.
 */
static void cli_free(Cli* cli) {
    size_t i;

    for (i = 0; i < cli->rename_count; i++) {
        free(cli->renames[i].which);
        free(cli->renames[i].name);
        free(cli->renames[i].origin);
    }
    free(cli->renames);
    free(cli->wanted);
}

/**
 * @brief Reads a `--names` file: one `<name|addr> <new name>` per line (a `=` may stand in for the space), with blank
 *        lines and `#` or `;` comments allowed.
 * @param cli  The settings; receives the requests.
 * @param path The file.
 * @return CONTINUE, or the exit status of the program after an error was printed.
 */
static int load_names(Cli* cli, const char* path) {
    size_t size, pos = 0;
    unsigned line_no = 0;
    uint8_t* data;
    char* text;

    if (strcmp(path, "-") == 0) return bad_usage("--names cannot read from standard input");
    data = read_file(path, &size);
    if (!data) return 1;
    if (memchr(data, 0, size)) {
        fprintf(stderr, "Ilangdis: %s: not a text file\n", path);
        free(data);
        return 1;
    }
    text = (char*)dis_xmalloc(size + 1);
    memcpy(text, data, size);
    text[size] = '\0';
    free(data);

    while (pos < size) {
        char* line = text + pos;
        char* end = strchr(line, '\n');
        char* cut;
        char* which;
        char* name;
        char origin[512];

        pos += end ? (size_t)(end - line) + 1 : strlen(line);
        if (end) *end = '\0';
        line_no++;
        if ((cut = strpbrk(line, "#;")) != NULL) *cut = '\0';

        while (isspace((unsigned char)*line)) line++;
        if (!*line) continue;
        which = line;
        while (*line && !isspace((unsigned char)*line) && *line != '=') line++;
        if (*line) {  /* the separator: white space, with at most one '=' among it */
            bool has_eq = false;

            for (;;) {
                if (isspace((unsigned char)*line)) {
                    *line++ = '\0';
                }
                else if (*line == '=' && !has_eq) {
                    has_eq = true;
                    *line++ = '\0';
                }
                else {
                    break;
                }
            }
        }
        name = line;
        while (*line && !isspace((unsigned char)*line)) line++;
        if (*line) {
            *line++ = '\0';
            while (isspace((unsigned char)*line)) line++;
        }
        snprintf(origin, sizeof origin, "%s:%u", path, line_no);
        if (!*which || !*name || *line) {
            fprintf(stderr, "Ilangdis: %s: expected '<name|addr> <new name>'\n", origin);
            free(text);
            return 2;
        }
        add_rename(cli, which, strlen(which), name, strlen(name), origin);
    }
    free(text);
    return CONTINUE;
}

/**
 * @brief Applies one option.
 * @param cli   The settings.
 * @param spec  The option.
 * @param value Its value (NULL for an option without one, or an optional value that was not given).
 * @return CONTINUE, or the exit status of the program (after printing what there is to print).
 */
static int apply_option(Cli* cli, const OptSpec* spec, const char* value) {
    DisOptions* opt = &cli->opt;

    switch (spec->id) {
    case OPT_HELP:
        usage(stdout);
        return 0;
    case OPT_VERSION:
        printf("Ilangdis %s (bytecode version %d)\n", ILANGDIS_VERSION, SUPPORTED_VERSION);
        return 0;
    case OPT_BYTES: opt->bytes = true; break;
    case OPT_BYTES_PER_LINE:
        if (!parse_count(value, 32, &opt->bytes_per_line) || opt->bytes_per_line < 1)
            return bad_usage("--bytes-per-line needs a number from 1 to 32, not '%s'", value);
        break;
    case OPT_ARROWS:
        if (strcmp(value, "all") == 0) opt->arrows = DIS_ARROWS_ALL;
        else if (strcmp(value, "jumps") == 0) opt->arrows = DIS_ARROWS_JUMPS;
        else if (strcmp(value, "calls") == 0) opt->arrows = DIS_ARROWS_CALLS;
        else if (strcmp(value, "none") == 0) opt->arrows = DIS_ARROWS_NONE;
        else return bad_usage("--arrows must be all, jumps, calls or none, not '%s'", value);
        break;
    case OPT_NO_ARROWS: opt->arrows = DIS_ARROWS_NONE; break;
    case OPT_ARROW_WIDTH:
        if (!parse_count(value, 32, &opt->arrow_width)) return bad_usage("--arrow-width needs a number from 0 to 32, not '%s'", value);
        break;
    case OPT_NO_LABELS: opt->labels = false; break;
    case OPT_NO_COMMENTS: opt->comments = false; break;
    case OPT_NO_HEADER: opt->header = false; break;
    case OPT_STRING_LIMIT:
        if (!parse_count(value, 1000000, &opt->string_limit)) return bad_usage("--string-limit needs a number (0 = no limit), not '%s'", value);
        break;
    case OPT_FUNCTION:
        dis_reserve(&cli->wanted, &cli->wanted_capacity, sizeof *cli->wanted, cli->wanted_count + 1);
        cli->wanted[cli->wanted_count++] = value;
        break;
    case OPT_RANGE:
        if (!parse_range(value, &opt->range_from, &opt->range_to)) return bad_usage("--range needs <from>[-<to>] with from < to, not '%s'", value);
        opt->has_range = true;
        break;
    case OPT_NAME: {
        const char* eq = strchr(value, '=');

        if (!eq || eq == value || !eq[1]) return bad_usage("--name needs <name|addr>=<new name>, not '%s'", value);
        add_rename(cli, value, (size_t)(eq - value), eq + 1, strlen(eq + 1), "--name");
        break;
    }
    case OPT_NAMES: {
        int status = load_names(cli, value);

        if (status != CONTINUE) return status;
        break;
    }
    case OPT_FUNCTIONS: cli->want_functions = true; break;
    case OPT_CALL_GRAPH: cli->want_graph = true; break;
    case OPT_INFO: cli->want_info = true; break;
    case OPT_OUTPUT: cli->output = value; break;
    case OPT_COLOR:
        if (!value || strcmp(value, "always") == 0) cli->color = WHEN_ALWAYS;
        else if (strcmp(value, "auto") == 0) cli->color = WHEN_AUTO;
        else if (strcmp(value, "never") == 0) cli->color = WHEN_NEVER;
        else return bad_usage("--color must be auto, always or never, not '%s'", value);
        break;
    case OPT_ASCII: cli->charset = 1; break;
    case OPT_UNICODE: cli->charset = 2; break;
    }
    return CONTINUE;
}

/**
 * @brief Looks up a long option by name; an unambiguous prefix is enough.
 * @param[in]  text  The name, without the dashes and without any `=value`.
 * @param[in]  len   Its length.
 * @param[out] spec  Receives the option.
 * @return CONTINUE if it was found, else the exit status (a message was printed).
 */
static int find_long(const char* text, size_t len, const OptSpec** spec) {
    size_t k, matches = 0;

    *spec = NULL;
    for (k = 0; k < OPTION_COUNT; k++) {
        if (strncmp(kOptions[k].name, text, len) != 0) continue;
        if (strlen(kOptions[k].name) == len) {  /* an exact name wins over longer names it is a prefix of */
            *spec = &kOptions[k];
            return CONTINUE;
        }
        *spec = &kOptions[k];
        matches++;
    }
    if (matches == 0) return bad_usage("unknown option '--%.*s'", (int)len, text);
    if (matches > 1) return bad_usage("option '--%.*s' is ambiguous", (int)len, text);
    return CONTINUE;
}

/**
 * @brief Reads the command line into @p cli.
 * @param argc Number of arguments.
 * @param argv The arguments.
 * @param cli  The settings (set to their defaults by the caller).
 * @return CONTINUE if the program should go on, otherwise its exit status.
 */
static int parse_args(int argc, char** argv, Cli* cli) {
    bool options_done = false;
    int i, rc;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const OptSpec* spec = NULL;
        const char* value = NULL;

        if (options_done || a[0] != '-' || a[1] == '\0') {  /* the file ("-" is standard input) */
            if (cli->input) return bad_usage("more than one input file ('%s' and '%s')", cli->input, a);
            cli->input = a;
        }
        else if (strcmp(a, "--") == 0) {
            options_done = true;
        }
        else if (a[1] == '-') {  /* --name, --name=value */
            size_t len = strcspn(a + 2, "=");

            if ((rc = find_long(a + 2, len, &spec)) != CONTINUE) return rc;
            if (a[2 + len] == '=') value = a + 3 + len;
            if (spec->arg == ARG_NONE && value) return bad_usage("option '--%s' does not take a value", spec->name);
            if (spec->arg == ARG_REQUIRED && !value) {
                if (i + 1 >= argc) return bad_usage("option '--%s' needs a value", spec->name);
                value = argv[++i];
            }
            if ((rc = apply_option(cli, spec, value)) != CONTINUE) return rc;
        }
        else {  /* -x, -x value, -xvalue, or several flags in one: -bf */
            const char* c;

            for (c = a + 1; *c; c++) {
                size_t k;

                spec = NULL;
                value = NULL;
                for (k = 0; k < OPTION_COUNT; k++) {
                    if (kOptions[k].shortname == *c) spec = &kOptions[k];
                }
                if (!spec) return bad_usage("unknown option '-%c'", *c);
                if (spec->arg == ARG_REQUIRED) {
                    if (c[1]) value = c + 1;
                    else if (i + 1 < argc) value = argv[++i];
                    else return bad_usage("option '-%c' needs a value", *c);
                }
                if ((rc = apply_option(cli, spec, value)) != CONTINUE) return rc;
                if (value) break;  /* the rest of the argument was the value */
            }
        }
    }
    return CONTINUE;
}

/**
 * @brief Entry point.
 * @param argc Number of arguments.
 * @param argv The arguments.
 * @return 0 on success, 1 if the file cannot be read or is not valid bytecode, 2 for a bad command line.
 */
int main(int argc, char** argv) {
    Cli cli;
    DisOptions* opt = &cli.opt;
    FILE* out = stdout;
    uint8_t* data;
    size_t size, i;
    DisImage image;
    DisProgram prog;
    char error[256];
    int32_t* only = NULL;
    int status;

    memset(&cli, 0, sizeof cli);
    opt->bytes_per_line = 8;
    opt->arrows = DIS_ARROWS_ALL;
    opt->arrow_width = 12;
    opt->labels = true;
    opt->comments = true;
    opt->header = true;
    opt->string_limit = 60;
    cli.color = WHEN_AUTO;

    status = parse_args(argc, argv, &cli);
    if (status != CONTINUE) {
        cli_free(&cli);
        return status;
    }
    if (!cli.input) {
        usage(stderr);
        cli_free(&cli);
        return 2;
    }
    status = 0;

    data = read_file(cli.input, &size);
    if (!data) {
        cli_free(&cli);
        return 1;
    }
    if (!dis_image_parse(&image, data, size, error, sizeof error)) {
        fprintf(stderr, "Ilangdis: %s: %s\n", cli.input, error);
        free(data);
        cli_free(&cli);
        return 1;
    }
    dis_analyze(&prog, &image);

    /* the names given with --name and --names */
    for (i = 0; i < cli.rename_count; i++) {
        const Rename* r = &cli.renames[i];

        if (!dis_rename_function(&prog, r->which, r->name, error, sizeof error)) {
            fprintf(stderr, "Ilangdis: %s: %s\n", r->origin, error);
            status = 2;
            goto done;
        }
    }

    /* the functions asked for with -F */
    if (cli.wanted_count) {
        only = (int32_t*)dis_xmalloc(cli.wanted_count * sizeof *only);
        for (i = 0; i < cli.wanted_count; i++) {
            only[i] = dis_find_function(&prog, cli.wanted[i]);
            if (only[i] < 0) {
                status = bad_usage("no function named '%s' (--functions lists them)", cli.wanted[i]);
                goto done;
            }
        }
        opt->only = only;
        opt->only_count = cli.wanted_count;
    }

    if (cli.output) {
        out = fopen(cli.output, "w");
        if (!out) {
            fprintf(stderr, "Ilangdis: cannot write '%s': %s\n", cli.output, strerror(errno));
            status = 1;
            goto done;
        }
    }
    detect_terminal(out, cli.output != NULL, cli.color, cli.charset, opt);
    opt->file_name = strcmp(cli.input, "-") == 0 ? "(standard input)" : cli.input;

    if (cli.want_info || cli.want_functions || cli.want_graph) {
        if (cli.want_info) dis_print_info(out, &prog, opt);
        if (cli.want_functions) {
            if (cli.want_info) fputc('\n', out);
            dis_print_functions(out, &prog, opt);
        }
        if (cli.want_graph) {
            if (cli.want_info || cli.want_functions) fputc('\n', out);
            dis_print_callgraph(out, &prog, opt);
        }
    }
    else {
        dis_print_listing(out, &prog, opt);
    }

    if (fflush(out) != 0 || ferror(out)) {
        fprintf(stderr, "Ilangdis: error writing the output\n");
        status = 1;
    }
    if (cli.output && fclose(out) != 0) status = 1;

done:
    dis_program_free(&prog);
    dis_image_free(&image);
    free(only);
    cli_free(&cli);
    free(data);
    return status;
}
