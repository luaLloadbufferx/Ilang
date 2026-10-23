/**
 * @file listing.c
 * @brief The things Ilangdis prints: the listing, the file summary, the function table and the call graph.
 *
 * A listing line is built from these columns, left to right:
 * @code{.unparsed}
 * address  [bytes]  [arrow margin]  text
 * @endcode
 * `text` is a function name or label at its own column, and instructions indented by four spaces. Lines that are not
 * instructions (blank lines, banners, labels) are printed with the arrow margin too, so arrows that pass them stay
 * connected.
 */
#include "output.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/** @brief Spaces an instruction is indented relative to the labels. */
#define INDENT 4
/** @brief Column (relative to the start of an instruction) where comments start; the same as in format.c. */
#define COMMENT_COLUMN 44
/** @brief Most call sites listed in a function banner or a label comment before "+N more". */
#define MAX_LISTED_XREFS 6
/** @brief Deepest level the call graph is expanded to. */
#define MAX_GRAPH_DEPTH 100

/**
 * @brief Number of hex digits in the addresses of the listing: enough for the file, at least four.
 * @param size Size of the file.
 */
static int address_digits(uint32_t size) {
    return size > 0xFFFFFFu ? 8 : size > 0xFFFFu ? 6 : 4;
}

/**
 * @brief Prints a `; ---...` rule.
 * @param p The printer.
 */
static void put_rule(DisPrinter* p) {
    pr_puts(p, DIS_ST_COMMENT, "; ------------------------------------------------------------------");
    pr_nl(p);
}

/**
 * @brief Prints a whole comment line.
 * @param p     The printer.
 * @param style The style of the text (DIS_ST_COMMENT or DIS_ST_BOLD).
 * @param fmt   printf format; the "; " in front is added.
 */
static void put_comment_line(DisPrinter* p, DisStyle style, const char* fmt, ...) {
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pr_puts(p, DIS_ST_COMMENT, "; ");
    pr_puts(p, style, buf);
    pr_nl(p);
}

/**
 * @brief The plural ending for a count.
 * @param n The count.
 * @return `""` for exactly one, `"s"` otherwise.
 */
static const char* plural(unsigned n) {
    return n == 1 ? "" : "s";
}

/**
 * @brief Says how many arguments travel in registers ("2 arguments, all in registers").
 * @param[out] buf  Output buffer.
 * @param[in]  size Its size.
 * @param[in]  nreg Number of register arguments.
 * @param[in]  argc Total number of arguments.
 */
static void describe_args(char* buf, size_t size, unsigned nreg, unsigned argc) {
    if (argc == 0) snprintf(buf, size, "no arguments");
    else if (nreg == argc) snprintf(buf, size, "%u argument%s, all in registers", argc, argc == 1 ? "" : "s");
    else snprintf(buf, size, "%u argument%s, %u in registers and %u on the stack", argc, argc == 1 ? "" : "s", nreg, argc - nreg);
}

/**
 * @brief Prints the summary of the file as comment lines: size, entry, code, imports, exports, sections.
 * @param p    The printer.
 * @param prog The analysed program.
 * @param opt  The options (for the file name).
 */
static void print_header(DisPrinter* p, const DisProgram* prog, const DisOptions* opt) {
    const DisImage* img = prog->image;
    uint32_t i;
    char sig[512], args[96];

    pr_puts(p, DIS_ST_COMMENT, "; ");
    pr_printf(p, DIS_ST_BOLD, "Ilangdis %s", ILANGDIS_VERSION);
    if (opt->file_name) pr_printf(p, DIS_ST_COMMENT, " - %s", opt->file_name);
    pr_nl(p);
    put_rule(p);
    put_comment_line(p, DIS_ST_COMMENT, "file      %u byte%s, bytecode version %u", (unsigned)img->size, plural((unsigned)img->size), (unsigned)img->version);
    put_comment_line(p, DIS_ST_COMMENT, "entry     0x%04X", (unsigned)img->entry);
    put_comment_line(p, DIS_ST_COMMENT, "code      0x%04X - 0x%04X: %u byte%s, %u instruction%s, %u function%s",
        (unsigned)img->entry, (unsigned)img->size, (unsigned)(img->size - img->entry), plural((unsigned)(img->size - img->entry)),
        (unsigned)prog->insn_count, plural((unsigned)prog->insn_count), (unsigned)prog->func_count, plural((unsigned)prog->func_count));

    put_comment_line(p, DIS_ST_COMMENT, "imports   %u%s", (unsigned)img->import_count, img->import_count ? " (host functions called with CALLC)" : "");
    for (i = 0; i < img->import_count; i++) {
        dis_format_sig(sig, sizeof sig, img->imports[i].name, &img->imports[i].sig);
        put_comment_line(p, DIS_ST_COMMENT, "  [%u] %s", (unsigned)i, sig);
    }
    if (img->has_expt) {
        put_comment_line(p, DIS_ST_COMMENT, "exports   %u (functions the host may call)", (unsigned)img->export_count);
        for (i = 0; i < img->export_count; i++) {
            const DisExport* e = &img->exports[i];

            dis_format_sig(sig, sizeof sig, e->name, &e->sig);
            describe_args(args, sizeof args, e->nreg, e->sig.argc);
            put_comment_line(p, DIS_ST_COMMENT, "  %s  at 0x%04X, %s", sig, (unsigned)e->address, args);
        }
        put_comment_line(p, DIS_ST_COMMENT, "init      0x%04X (runs the global initializers for the host)", (unsigned)img->init_address);
    }
    if (img->section_count) {
        char list[256] = "";
        size_t n = 0;

        for (i = 0; i < img->section_count && n + 32 < sizeof list; i++) {
            n += (size_t)snprintf(list + n, sizeof list - n, "%s%s (%u byte%s)", i ? ", " : "", img->sections[i].tag, (unsigned)img->sections[i].length, plural((unsigned)img->sections[i].length));
        }
        put_comment_line(p, DIS_ST_COMMENT, "sections  %s", list);
    }
}

void dis_print_info(FILE* out, const DisProgram* prog, const DisOptions* opt) {
    DisPrinter p;

    pr_init(&p, out, opt);
    print_header(&p, prog, opt);
}

/**
 * @brief Whether a cross reference is a call (a CALL or a tail call) rather than a jump.
 * @param x The reference.
 */
static bool xref_is_call(const DisXref* x) {
    return x->kind == DIS_XREF_CALL || x->kind == DIS_XREF_TAIL;
}

/**
 * @brief Counts the references of one family.
 * @param refs  The references.
 * @param count How many.
 * @param calls true: calls and tail calls; false: jumps.
 */
static size_t count_xrefs(const DisXref* refs, size_t count, bool calls) {
    size_t i, n = 0;

    for (i = 0; i < count; i++) n += xref_is_call(&refs[i]) == calls;
    return n;
}

/**
 * @brief Writes the first few references of one family as text.
 *
 * Calls come out as `function+0xOFFSET` (with "(tail call)" where it applies), jumps as `0xADDRESS MNEMONIC`.
 *
 * @param[in]  prog  The program.
 * @param[in]  refs  The references.
 * @param[in]  count How many.
 * @param[in]  calls true: list calls and tail calls; false: list jumps.
 * @param[out] buf   Output buffer.
 * @param[in]  size  Its size.
 */
static void format_xrefs(const DisProgram* prog, const DisXref* refs, size_t count, bool calls, char* buf, size_t size) {
    size_t i, shown = 0, n = 0;

    buf[0] = '\0';
    for (i = 0; i < count && shown < MAX_LISTED_XREFS && n + 64 < size; i++) {
        const DisInsn* from = &prog->insns[refs[i].from];

        if (xref_is_call(&refs[i]) != calls) continue;
        if (shown) n += (size_t)snprintf(buf + n, size - n, ", ");
        if (calls) {
            char where[300];

            dis_describe_position(prog, refs[i].from, where, sizeof where);
            n += (size_t)snprintf(buf + n, size - n, "%s%s", where, refs[i].kind == DIS_XREF_TAIL ? " (tail call)" : "");
        }
        else {
            n += (size_t)snprintf(buf + n, size - n, "0x%04X %s", (unsigned)from->addr, dis_mnemonic(from->op));
        }
        shown++;
    }
}

/** @brief Everything needed to print the lines of a listing. */
typedef struct Layout {
    DisPrinter* p;           /**< The printer. */
    const DisProgram* prog;  /**< The program. */
    const DisOptions* opt;   /**< The options. */
    DisArrows* arrows;       /**< The arrow margin. */
    int addr_width;          /**< Hex digits of an address. */
    int byte_width;          /**< Columns of the byte view (0 when it is off). */
    int margin;              /**< Width of the arrow margin including the arrow head column (0 if there is none). */
} Layout;

/**
 * @brief Starts a line that has no address: blank address and byte columns, then the arrow margin.
 * @param L    The layout.
 * @param row  The row the line belongs to.
 * @param line Where the line sits relative to the row.
 */
static void line_start_blank(Layout* L, size_t row, DisGutterLine line) {
    pr_spaces(L->p, L->addr_width + 2 + (L->byte_width ? L->byte_width + 2 : 0));
    if (L->margin) {
        dis_arrows_print(L->p, L->arrows, row, line);
        pr_spaces(L->p, 1);
    }
}

/**
 * @brief Prints one comment line of a banner, in the text column.
 * @param L     The layout.
 * @param row   The row of the function's first instruction.
 * @param style The style of the text.
 * @param fmt   printf format.
 */
static void banner_line(Layout* L, size_t row, DisStyle style, const char* fmt, ...) {
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    line_start_blank(L, row, DIS_GUTTER_ABOVE);
    put_comment_line(L->p, style, "%s", buf);
}

/**
 * @brief Prints the banner above a function: a rule, its name and what is known about it.
 * @param L            The layout.
 * @param row          The row of the function's first instruction.
 * @param insn         Index of that instruction.
 * @param blank_before Start with an empty line.
 */
static void print_banner(Layout* L, size_t row, uint32_t insn, bool blank_before) {
    const DisProgram* prog = L->prog;
    const DisFunc* fn = &prog->funcs[prog->insns[insn].func];
    const DisImage* img = prog->image;
    char what[160], frame[48] = "", sig[512], args[96], buf[640];
    size_t count, calls;
    const DisXref* xr = dis_xrefs_to(prog, insn, &count);

    if (blank_before) {
        line_start_blank(L, row, DIS_GUTTER_ABOVE);
        pr_nl(L->p);
    }
    line_start_blank(L, row, DIS_GUTTER_ABOVE);
    put_rule(L->p);

    switch (fn->source) {
    case DIS_NAME_EXPORT:
        snprintf(what, sizeof what, "%s", fn->is_entry ? "entry point" : fn->is_init ? "runs the global initializers for the host" : "exported to the host");
        break;
    case DIS_NAME_USER:
        snprintf(what, sizeof what, "named by you");
        break;
    case DIS_NAME_STARTUP:
        snprintf(what, sizeof what, "named from the startup stub");
        break;
    case DIS_NAME_FAILSTUB:
        snprintf(what, sizeof what, "named from its runtime-error message");
        break;
    default:
        snprintf(what, sizeof what, "no name in the file");
        break;
    }
    banner_line(L, row, DIS_ST_BOLD, "%s  (%s)", fn->name, what);

    if (fn->has_frame) snprintf(frame, sizeof frame, ", stack frame of %u slot%s", (unsigned)fn->frame, fn->frame == 1 ? "" : "s");
    banner_line(L, row, DIS_ST_COMMENT, "  0x%04X - 0x%04X: %u byte%s, %u instruction%s%s",
        (unsigned)fn->start, (unsigned)fn->end, (unsigned)(fn->end - fn->start), plural((unsigned)(fn->end - fn->start)),
        (unsigned)fn->count, plural((unsigned)fn->count), frame);

    if (fn->export_index >= 0) {
        const DisExport* e = &img->exports[fn->export_index];

        dis_format_sig(sig, sizeof sig, e->name, &e->sig);
        describe_args(args, sizeof args, e->nreg, e->sig.argc);
        banner_line(L, row, DIS_ST_COMMENT, "  exported as %s (%s)", sig, args);
    }

    calls = count_xrefs(xr, count, true);
    if (calls) {
        format_xrefs(prog, xr, count, true, buf, sizeof buf);
        banner_line(L, row, DIS_ST_COMMENT, "  called from: %s%s", buf, calls > MAX_LISTED_XREFS ? ", ..." : "");
        if (calls > MAX_LISTED_XREFS) banner_line(L, row, DIS_ST_COMMENT, "               (%u call sites in all)", (unsigned)calls);
    }
    else if (!fn->is_entry && !fn->is_init && fn->export_index < 0) {
        banner_line(L, row, DIS_ST_COMMENT, "  called from: nowhere directly (only through a register, if at all)");
    }
}

/**
 * @brief Prints the line that carries a function name or a label, with the jumps that lead to a label.
 * @param L    The layout.
 * @param row  The row of the instruction it is placed at.
 * @param insn Index of that instruction.
 */
static void print_label_line(Layout* L, size_t row, uint32_t insn) {
    const DisProgram* prog = L->prog;
    const DisInsn* in = &prog->insns[insn];
    DisPrinter* p = L->p;
    size_t count, jumps;
    const DisXref* xr = dis_xrefs_to(prog, insn, &count);
    char buf[640];
    int text_col;

    line_start_blank(L, row, DIS_GUTTER_ABOVE);
    text_col = p->col;
    if (in->flags & DIS_F_FUNC_START) {
        pr_printf(p, DIS_ST_FUNC, "%s:", prog->funcs[in->func].name);
        pr_nl(p);
        return;
    }
    pr_printf(p, DIS_ST_LABEL, "%s:", prog->labels[in->label].name);
    jumps = count_xrefs(xr, count, false);
    if (jumps && L->opt->comments) {
        int col = text_col + INDENT + COMMENT_COLUMN;

        format_xrefs(prog, xr, count, false, buf, sizeof buf);
        if (p->col + 2 > col) col = p->col + 2;
        pr_pad(p, col);
        pr_printf(p, DIS_ST_COMMENT, "; from %s", buf);
        if (jumps > MAX_LISTED_XREFS) pr_printf(p, DIS_ST_COMMENT, ", +%u more", (unsigned)(jumps - MAX_LISTED_XREFS));
    }
    pr_nl(p);
}

/**
 * @brief Prints one row of the byte view: up to `bytes_per_line` bytes of an instruction, padded to the column width.
 * @param L      The layout.
 * @param in     The instruction.
 * @param offset Offset of the first byte of the row inside the instruction.
 */
static void print_bytes_row(Layout* L, const DisInsn* in, uint32_t offset) {
    const uint8_t* data = L->prog->image->data + in->addr;
    uint32_t i, per_line = (uint32_t)L->opt->bytes_per_line;
    int start = L->p->col;

    for (i = 0; i < per_line && offset + i < in->len; i++) {
        pr_printf(L->p, DIS_ST_BYTES, "%s%02X", i ? " " : "", (unsigned)data[offset + i]);
    }
    pr_pad(L->p, start + L->byte_width + 2);
}

/**
 * @brief Selects the instructions to print: all, only the chosen functions, only the chosen address range.
 * @param[in]  prog  The program.
 * @param[in]  opt   The options.
 * @param[out] count Receives the number of rows.
 * @return The instruction indices, in order; release with free().
 */
static uint32_t* select_rows(const DisProgram* prog, const DisOptions* opt, size_t* count) {
    uint32_t* rows = (uint32_t*)dis_xmalloc((prog->insn_count + 1) * sizeof *rows);
    bool* wanted = NULL;
    size_t i, n = 0;

    if (opt->only_count) {
        wanted = (bool*)dis_xcalloc(prog->func_count + 1, sizeof *wanted);
        for (i = 0; i < opt->only_count; i++) wanted[opt->only[i]] = true;
    }
    for (i = 0; i < prog->insn_count; i++) {
        const DisInsn* in = &prog->insns[i];

        if (wanted && !wanted[in->func]) continue;
        if (opt->has_range && (in->addr < opt->range_from || in->addr >= opt->range_to)) continue;
        rows[n++] = (uint32_t)i;
    }
    free(wanted);
    *count = n;
    return rows;
}

/**
 * @brief Prints the notes at the end of a listing: undecodable bytes, odd jump targets, arrows that did not fit.
 * @param L     The layout.
 * @param count Number of rows that were printed.
 */
static void print_notes(Layout* L, size_t count) {
    const DisProgram* prog = L->prog;
    DisPrinter* p = L->p;
    size_t omitted = dis_arrows_omitted(L->arrows);
    bool any = false;

/** @brief Prints one note under the listing, preceded by a blank line before the first. printf-style arguments. */
#define NOTE(...) do { if (!any) pr_nl(p); any = true; put_comment_line(p, DIS_ST_COMMENT, __VA_ARGS__); } while (0)
    if (count == 0) NOTE("nothing to show: no instruction matches the selection");
    if (prog->bad_bytes)
        NOTE("note: %u byte%s could not be decoded as instructions (shown as .byte)", (unsigned)prog->bad_bytes, prog->bad_bytes == 1 ? "" : "s");
    if (prog->bad_targets)
        NOTE("note: %u jump%s or call%s do not point at the start of an instruction", (unsigned)prog->bad_targets,
            prog->bad_targets == 1 ? "" : "s", prog->bad_targets == 1 ? "" : "s");
    if (omitted)
        NOTE("note: %u arrow%s did not fit in the margin (see --arrow-width)", (unsigned)omitted, omitted == 1 ? "" : "s");
#undef NOTE
}

void dis_print_listing(FILE* out, const DisProgram* prog, const DisOptions* opt) {
    DisPrinter p;
    Layout L;
    size_t count = 0, r;
    uint32_t* rows = select_rows(prog, opt, &count);

    pr_init(&p, out, opt);
    L.p = &p;
    L.prog = prog;
    L.opt = opt;
    L.arrows = dis_arrows_build(prog, rows, count, opt->arrows, opt->arrow_width);
    L.addr_width = address_digits(prog->image->size);
    L.byte_width = opt->bytes ? opt->bytes_per_line * 3 - 1 : 0;
    L.margin = dis_arrows_width(L.arrows);

    if (opt->header) print_header(&p, prog, opt);

    for (r = 0; r < count; r++) {
        uint32_t i = rows[r], offset;
        const DisInsn* in = &prog->insns[i];

        if (opt->labels && (in->flags & DIS_F_FUNC_START)) {
            print_banner(&L, r, i, r > 0 || opt->header);
            print_label_line(&L, r, i);
        }
        else if (opt->labels && in->label >= 0) {
            print_label_line(&L, r, i);
        }

        /* the row of the instruction: address, bytes, margin, text */
        pr_printf(&p, DIS_ST_ADDR, "%0*X", L.addr_width, (unsigned)in->addr);
        pr_spaces(&p, 2);
        if (opt->bytes) print_bytes_row(&L, in, 0);
        if (L.margin) {
            dis_arrows_print(&p, L.arrows, r, DIS_GUTTER_ROW);
            pr_spaces(&p, 1);
        }
        pr_spaces(&p, INDENT);
        dis_print_insn_text(&p, prog, i, opt);
        pr_nl(&p);

        /* the rest of a long instruction in the byte view */
        if (opt->bytes) {
            for (offset = (uint32_t)opt->bytes_per_line; offset < in->len; offset += (uint32_t)opt->bytes_per_line) {
                pr_spaces(&p, L.addr_width + 2);
                print_bytes_row(&L, in, offset);
                if (L.margin) {
                    dis_arrows_print(&p, L.arrows, r, DIS_GUTTER_BELOW);
                    pr_spaces(&p, 1);
                }
                pr_nl(&p);
            }
        }
    }

    if (opt->comments || count == 0) print_notes(&L, count);

    dis_arrows_free(L.arrows);
    free(rows);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Function table                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------------ */

/**
 * @brief Whether a function calls itself (directly, with a CALL or a tail call).
 * @param prog The program.
 * @param f    Function index.
 */
static bool is_recursive(const DisProgram* prog, int32_t f) {
    size_t count, i;
    const DisXref* xr = dis_xrefs_to(prog, prog->funcs[f].first, &count);

    for (i = 0; i < count; i++) {
        if (xref_is_call(&xr[i]) && prog->insns[xr[i].from].func == f) return true;
    }
    return false;
}

void dis_print_functions(FILE* out, const DisProgram* prog, const DisOptions* opt) {
    DisPrinter p;
    int name_width = 4, addr_width = address_digits(prog->image->size) + 2;
    size_t f;

    pr_init(&p, out, opt);
    for (f = 0; f < prog->func_count; f++) {
        int n = (int)strlen(prog->funcs[f].name);

        if (n > name_width) name_width = n;
    }

    pr_printf(&p, DIS_ST_BOLD, "%-*s  %6s  %6s  %7s  %-*s  %s", addr_width, "Address", "Size", "Insns", "Callers", name_width, "Name", "Notes");
    pr_nl(&p);
    for (f = 0; f < prog->func_count; f++) {
        const DisFunc* fn = &prog->funcs[f];
        size_t count, callers;
        const DisXref* xr = dis_xrefs_to(prog, fn->first, &count);
        char notes[700] = "", sig[512];
        size_t n = 0;

        callers = count_xrefs(xr, count, true);
/** @brief Appends one printf-style note to `notes`, separated from the previous one by `"; "`. */
#define NOTE(...) do { n += (size_t)snprintf(notes + n, sizeof notes - n, "%s", n ? "; " : ""); n += (size_t)snprintf(notes + n, sizeof notes - n, __VA_ARGS__); } while (0)
        if (fn->is_entry) NOTE("entry point");
        if (fn->is_init) NOTE("init stub");
        if (fn->export_index >= 0) {
            dis_format_sig(sig, sizeof sig, prog->image->exports[fn->export_index].name, &prog->image->exports[fn->export_index].sig);
            NOTE("exported: %s", sig);
        }
        if (is_recursive(prog, (int32_t)f)) NOTE("recursive");
        if (fn->has_frame) NOTE("frame %u", (unsigned)fn->frame);
        if (fn->source == DIS_NAME_GENERATED) NOTE("unnamed");
#undef NOTE
        if (n >= sizeof notes) notes[sizeof notes - 1] = '\0';

        pr_printf(&p, DIS_ST_ADDR, "0x%0*X", addr_width - 2, (unsigned)fn->start);
        pr_printf(&p, DIS_ST_PLAIN, "  %6u  %6u  %7u  ", (unsigned)(fn->end - fn->start), (unsigned)fn->count, (unsigned)callers);
        pr_puts(&p, DIS_ST_FUNC, fn->name);
        pr_spaces(&p, name_width - (int)strlen(fn->name) + 2);
        pr_puts(&p, DIS_ST_COMMENT, notes);
        pr_nl(&p);
    }
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Call graph                                                                                                          */
/* ------------------------------------------------------------------------------------------------------------------ */

/** @brief A callee: a function of the program or a host function. */
typedef struct Callee {
    int32_t func;    /**< Function index, or -1 for a host function. */
    uint32_t import; /**< Import index of a host function. */
} Callee;

/** @brief State of the call graph walk. */
typedef struct Graph {
    DisPrinter* p;          /**< The printer. */
    const DisProgram* prog; /**< The program. */
    bool* shown;            /**< Per function: its callees have been listed somewhere above. */
    bool* open;             /**< Per function: it is on the path from the root to the node being printed. */
    char prefix[16 * MAX_GRAPH_DEPTH + 16];  /**< The tree lines in front of the current children. */
    const char* tee;        /**< "├── ". */
    const char* last;       /**< "└── ". */
    const char* bar;        /**< "│   ". */
} Graph;

/**
 * @brief Lists what a function calls, each callee once, in order of first appearance.
 *
 * Calls, tail calls and host calls (CALLC) count.
 *
 * @param[in]  prog The program.
 * @param[in]  f    Function index.
 * @param[out] n    Receives the number of callees.
 * @return The callees; release with free().
 */
static Callee* callees_of(const DisProgram* prog, int32_t f, size_t* n) {
    const DisFunc* fn = &prog->funcs[f];
    Callee* list = (Callee*)dis_xmalloc((fn->count + 1) * sizeof *list);
    size_t i, k, count = 0;

    for (i = fn->first; i < fn->first + fn->count; i++) {
        const DisInsn* in = &prog->insns[i];
        Callee c;

        if (in->op == CALLC) { c.func = -1; c.import = in->import; }
        else if ((in->op == CALL || (in->flags & DIS_F_TAIL_CALL)) && in->target_idx >= 0) { c.func = prog->insns[in->target_idx].func; c.import = 0; }
        else continue;
        for (k = 0; k < count; k++) {
            if (list[k].func == c.func && (c.func >= 0 || list[k].import == c.import)) break;
        }
        if (k == count) list[count++] = c;
    }
    *n = count;
    return list;
}

/**
 * @brief Prints the name of a graph node.
 * @param G    The walk state.
 * @param c    The callee.
 * @param note Text after the name ("(recursive)", "..."), or NULL.
 */
static void graph_node_text(Graph* G, const Callee* c, const char* note) {
    const DisImage* img = G->prog->image;

    if (c->func < 0) {
        pr_puts(G->p, DIS_ST_EXTERN, c->import < img->import_count ? img->imports[c->import].name : "?");
        pr_puts(G->p, DIS_ST_COMMENT, "  (host function)");
    }
    else {
        pr_puts(G->p, DIS_ST_FUNC, G->prog->funcs[c->func].name);
        pr_printf(G->p, DIS_ST_COMMENT, "  0x%04X", (unsigned)G->prog->funcs[c->func].start);
    }
    if (note) pr_printf(G->p, DIS_ST_COMMENT, "  %s", note);
}

/**
 * @brief Prints the tree below a function: one line per callee, expanded the first time a function is met.
 * @param G     The walk state.
 * @param f     The function whose callees are printed.
 * @param depth Nesting level.
 */
static void graph_children(Graph* G, int32_t f, size_t depth) {
    size_t n, k, keep = strlen(G->prefix);
    Callee* list = callees_of(G->prog, f, &n);

    for (k = 0; k < n; k++) {
        bool is_last = k + 1 == n;
        const char* note = NULL;
        bool expand = false;
        size_t sub_n;

        if (list[k].func >= 0) {
            Callee* sub;

            if (G->open[list[k].func]) {
                note = "(recursive)";
            }
            else if (G->shown[list[k].func]) {
                sub = callees_of(G->prog, list[k].func, &sub_n);
                free(sub);
                if (sub_n) note = "...";
            }
            else if (depth + 1 >= MAX_GRAPH_DEPTH) {
                note = "... (too deep)";
            }
            else {
                expand = true;
            }
        }
        pr_puts(G->p, DIS_ST_COMMENT, G->prefix);
        pr_puts(G->p, DIS_ST_COMMENT, is_last ? G->last : G->tee);
        graph_node_text(G, &list[k], note);
        pr_nl(G->p);
        if (expand) {
            G->shown[list[k].func] = true;
            G->open[list[k].func] = true;
            strcat(G->prefix, is_last ? "    " : G->bar);
            graph_children(G, list[k].func, depth + 1);
            G->prefix[keep] = '\0';
            G->open[list[k].func] = false;
        }
    }
    free(list);
}

/**
 * @brief Prints a root of the call graph and everything below it.
 * @param G The walk state.
 * @param f The root function.
 */
static void graph_root(Graph* G, int32_t f) {
    Callee c;

    c.func = f;
    c.import = 0;
    G->prefix[0] = '\0';
    graph_node_text(G, &c, NULL);
    pr_nl(G->p);
    G->shown[f] = true;
    G->open[f] = true;
    graph_children(G, f, 0);
    G->open[f] = false;
}

void dis_print_callgraph(FILE* out, const DisProgram* prog, const DisOptions* opt) {
    DisPrinter p;
    Graph G;
    size_t f, e;
    bool* has_caller = (bool*)dis_xcalloc(prog->func_count + 1, sizeof *has_caller);

    pr_init(&p, out, opt);
    G.p = &p;
    G.prog = prog;
    G.shown = (bool*)dis_xcalloc(prog->func_count + 1, sizeof *G.shown);
    G.open = (bool*)dis_xcalloc(prog->func_count + 1, sizeof *G.open);
    G.tee = p.unicode ? "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 " : "|-- ";
    G.last = p.unicode ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 " : "`-- ";
    G.bar = p.unicode ? "\xE2\x94\x82   " : "|   ";

    put_comment_line(&p, DIS_ST_COMMENT, "call graph: each function is expanded where it is first called; (recursive) = a call to a function that is being expanded, ... = expanded above");

    for (f = 0; f < prog->func_count; f++) {
        size_t count, i;
        const DisXref* xr = dis_xrefs_to(prog, prog->funcs[f].first, &count);

        for (i = 0; i < count; i++) {
            if (xref_is_call(&xr[i]) && prog->insns[xr[i].from].func != (int32_t)f) has_caller[f] = true;
        }
    }

    /* the entry point, the init stub and the exports first, then whatever nothing calls, then the rest (cycles) */
    for (f = 0; f < prog->func_count; f++) {
        if (prog->funcs[f].is_entry && !G.shown[f]) graph_root(&G, (int32_t)f);
    }
    for (f = 0; f < prog->func_count; f++) {
        if (prog->funcs[f].is_init && !G.shown[f]) graph_root(&G, (int32_t)f);
    }
    for (e = 0; e < prog->image->export_count; e++) {
        int32_t fi = -1;

        for (f = 0; f < prog->func_count; f++) {
            if (prog->funcs[f].export_index == (int32_t)e) fi = (int32_t)f;
        }
        if (fi >= 0 && !G.shown[fi]) graph_root(&G, fi);
    }
    for (f = 0; f < prog->func_count; f++) {
        if (!has_caller[f] && !G.shown[f]) graph_root(&G, (int32_t)f);
    }
    for (f = 0; f < prog->func_count; f++) {
        if (!G.shown[f]) graph_root(&G, (int32_t)f);
    }

    free(has_caller);
    free(G.shown);
    free(G.open);
}
