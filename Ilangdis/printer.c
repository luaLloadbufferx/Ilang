/**
 * @file printer.c
 * @brief DisPrinter: coloured output with column tracking.
 */
#include "output.h"

#include <stdarg.h>
#include <string.h>

/**
 * @brief The SGR parameters of a style.
 * @param style The style.
 * @return Parameters for an `ESC [ 0 ; ... m` sequence, or NULL for the terminal's default colour.
 */
static const char* sgr_of(DisStyle style) {
    switch (style) {
    case DIS_ST_ADDR:
    case DIS_ST_BYTES:
    case DIS_ST_COMMENT: return "90";
    case DIS_ST_FUNC:    return "1;32";
    case DIS_ST_LABEL:   return "1;33";
    case DIS_ST_EXTERN:  return "1;36";
    case DIS_ST_IMM:     return "36";
    case DIS_ST_STR:     return "32";
    case DIS_ST_TARGET:  return "1";
    case DIS_ST_ALU:     return "94";
    case DIS_ST_MEM:     return "33";
    case DIS_ST_CTRL:    return "1;35";
    case DIS_ST_IO:      return "36";
    case DIS_ST_BAD:     return "1;31";
    case DIS_ST_BOLD:    return "1";
    case DIS_ST_PLAIN:
    case DIS_ST_REG:
    case DIS_ST_STACK:
    default:             return NULL;
    }
}

void pr_init(DisPrinter* p, FILE* out, const DisOptions* options) {
    p->out = out;
    p->color = options->color;
    p->unicode = options->unicode;
    p->col = 0;
    p->pending = 0;
    p->sgr = NULL;
}

void pr_sgr(DisPrinter* p, const char* sgr) {
    if (!p->color) return;
    if (sgr == p->sgr || (sgr && p->sgr && strcmp(sgr, p->sgr) == 0)) return;
    if (sgr) fprintf(p->out, "\x1b[0;%sm", sgr);
    else fputs("\x1b[0m", p->out);
    p->sgr = sgr;
}

void pr_puts(DisPrinter* p, DisStyle style, const char* text) {
    pr_puts_sgr(p, sgr_of(style), text);
}

/**
 * @brief Writes the spaces that were held back, now that something visible follows them.
 * @param p The printer.
 */
static void flush_pending(DisPrinter* p) {
    while (p->pending > 0) {
        fputc(' ', p->out);
        p->pending--;
    }
}

void pr_puts_sgr(DisPrinter* p, const char* sgr, const char* text) {
    const unsigned char* c;
    bool blank = true;

    if (!*text) return;
    for (c = (const unsigned char*)text; *c; c++) {
        if (*c != ' ') blank = false;
        if ((*c & 0xC0) != 0x80) p->col++;  /* UTF-8 continuation bytes take no room */
    }
    if (blank) {  /* spaces are held back: they are only written when something visible follows */
        p->pending += (int)strlen(text);
        return;
    }
    flush_pending(p);
    pr_sgr(p, sgr);
    fputs(text, p->out);
}

void pr_printf(DisPrinter* p, DisStyle style, const char* fmt, ...) {
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pr_puts(p, style, buf);
}

void pr_spaces(DisPrinter* p, int n) {
    if (n <= 0) return;
    p->col += n;
    p->pending += n;
}

void pr_pad(DisPrinter* p, int col) {
    pr_spaces(p, col - p->col);
}

void pr_nl(DisPrinter* p) {
    pr_sgr(p, NULL);
    p->pending = 0;  /* no trailing whitespace */
    fputc('\n', p->out);
    p->col = 0;
}
