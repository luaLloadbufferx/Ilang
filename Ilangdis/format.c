/**
 * @file format.c
 * @brief Turns a decoded instruction into text: `MNEMONIC.type  operands  ; comment`.
 *
 * Operand order is "destination first", whatever the order in the encoding. Registers are `r0`, `r1`, ...; memory
 * operands are `[rN]`; data stack slots are `stack[rN]`, `local[N]` and `param[N]`. Immediates are written the way
 * Ilang writes them (`-5`, `1.5`, `true`, `'A'`); the type an instruction works on is a suffix of its mnemonic
 * (`ADD.int`, `LOAD.byte`, `CMP.float`).
 */
#include "output.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/** @brief Column, relative to the start of the mnemonic, where the operands start. */
#define OPERAND_COLUMN 12
/** @brief Column, relative to the start of the mnemonic, where the comment starts (if the operands leave room). */
#define COMMENT_COLUMN 44

/**
 * @brief Appends a piece to a comment, separated from the previous piece by ", ".
 * @param cmt  The comment buffer.
 * @param size Its size.
 * @param fmt  printf format of the piece.
 */
static void comment_add(char* cmt, size_t size, const char* fmt, ...) {
    size_t n = strlen(cmt);
    va_list ap;

    if (n + 1 >= size) return;
    if (n) n += (size_t)snprintf(cmt + n, size - n, ", ");
    if (n + 1 >= size) return;
    va_start(ap, fmt);
    vsnprintf(cmt + n, size - n, fmt, ap);
    va_end(ap);
}

/**
 * @brief Writes a float with as few digits as it takes to read back the same value.
 * @param buf  Output buffer.
 * @param size Its size.
 * @param bits The IEEE-754 bits.
 */
static void format_float(char* buf, size_t size, uint32_t bits) {
    float f;
    int prec;

    memcpy(&f, &bits, sizeof f);
    if (isnan(f)) { snprintf(buf, size, "nan"); return; }
    if (isinf(f)) { snprintf(buf, size, "%s", f < 0 ? "-inf" : "inf"); return; }
    for (prec = 1; prec <= 9; prec++) {
        snprintf(buf, size, "%.*g", prec, (double)f);
        if (strtof(buf, NULL) == f) break;
    }
    if (!strpbrk(buf, ".e")) {
        size_t n = strlen(buf);

        if (n + 3 <= size) memcpy(buf + n, ".0", 3);  /* 2.0, not 2: it is a float */
    }
}

/**
 * @brief Writes an immediate as source text.
 * @param buf  Output buffer.
 * @param size Its size.
 * @param type Type tag the immediate has.
 * @param bits The immediate's bits.
 */
static void format_imm(char* buf, size_t size, uint8_t type, uint32_t bits) {
    switch (type) {
    case T_INT: snprintf(buf, size, "%d", (int)(int32_t)bits); break;
    case T_UINT: snprintf(buf, size, "%u", (unsigned)bits); break;
    case T_FLOAT: format_float(buf, size, bits); break;
    case T_BOOL: snprintf(buf, size, "%s", bits ? "true" : "false"); break;
    case T_BYTE:
        if (bits >= 0x20 && bits < 0x7F && bits != '\'' && bits != '\\') snprintf(buf, size, "'%c'", (int)bits);
        else if (bits == '\n') snprintf(buf, size, "'\\n'");
        else if (bits == '\r') snprintf(buf, size, "'\\r'");
        else if (bits == '\t') snprintf(buf, size, "'\\t'");
        else snprintf(buf, size, "%u", (unsigned)bits);
        break;
    default: snprintf(buf, size, "0x%X", (unsigned)bits); break;
    }
}

/**
 * @brief Prints a register operand.
 * @param p The printer.
 * @param r Register number.
 */
static void put_reg(DisPrinter* p, unsigned r) {
    pr_printf(p, DIS_ST_REG, "r%u", r);
}

/**
 * @brief Prints the separator between operands.
 * @param p The printer.
 */
static void put_sep(DisPrinter* p) {
    pr_puts(p, DIS_ST_PLAIN, ", ");
}

/**
 * @brief Prints a number operand.
 * @param p The printer.
 * @param v The number.
 */
static void put_num(DisPrinter* p, uint32_t v) {
    pr_printf(p, DIS_ST_IMM, "%u", (unsigned)v);
}

/**
 * @brief Prints a register or an immediate of a type.
 * @param p    The printer.
 * @param o    The operand.
 * @param type Type tag of the instruction.
 */
static void put_operand(DisPrinter* p, const DisOperand* o, uint8_t type) {
    char buf[48];

    if (o->is_reg) {
        put_reg(p, o->reg);
        return;
    }
    format_imm(buf, sizeof buf, type, o->bits);
    pr_puts(p, DIS_ST_IMM, buf);
}

/**
 * @brief Prints a memory operand: `[rN]`.
 * @param p The printer.
 * @param r Register that holds the address.
 */
static void put_mem(DisPrinter* p, unsigned r) {
    pr_puts(p, DIS_ST_PLAIN, "[");
    put_reg(p, r);
    pr_puts(p, DIS_ST_PLAIN, "]");
}

/**
 * @brief Prints the bytes of a STRLIT as a quoted, escaped string.
 * @param[in]  p     The printer.
 * @param[in]  prog  The program.
 * @param[in]  in    The STRLIT.
 * @param[in]  limit Most bytes to show (0 = all).
 * @param[out] cmt   Comment buffer; gets the length.
 * @param[in]  cmtsz Its size.
 */
static void put_string(DisPrinter* p, const DisProgram* prog, const DisInsn* in, int limit, char* cmt, size_t cmtsz) {
    const uint8_t* s = prog->image->data + in->data_off;
    uint32_t n = in->value;
    uint32_t shown = (limit > 0 && n > (uint32_t)limit) ? (uint32_t)limit : n;
    char chunk[128];
    size_t used = 0;
    uint32_t i;

    pr_puts(p, DIS_ST_STR, "\"");
    for (i = 0; i < shown; i++) {
        uint8_t c = s[i];
        char piece[8];

        switch (c) {
        case '"': snprintf(piece, sizeof piece, "\\\""); break;
        case '\\': snprintf(piece, sizeof piece, "\\\\"); break;
        case '\n': snprintf(piece, sizeof piece, "\\n"); break;
        case '\r': snprintf(piece, sizeof piece, "\\r"); break;
        case '\t': snprintf(piece, sizeof piece, "\\t"); break;
        case 0: snprintf(piece, sizeof piece, "\\0"); break;
        default:
            if (c < 0x20 || c >= 0x7F) snprintf(piece, sizeof piece, "\\x%02X", (unsigned)c);
            else { piece[0] = (char)c; piece[1] = '\0'; }
            break;
        }
        if (used + strlen(piece) >= sizeof chunk) {
            chunk[used] = '\0';
            pr_puts(p, DIS_ST_STR, chunk);
            used = 0;
        }
        used += (size_t)snprintf(chunk + used, sizeof chunk - used, "%s", piece);
    }
    chunk[used] = '\0';
    pr_puts(p, DIS_ST_STR, chunk);
    pr_puts(p, DIS_ST_STR, "\"");
    if (shown < n) pr_puts(p, DIS_ST_COMMENT, "...");
    comment_add(cmt, cmtsz, "%u byte%s", (unsigned)n, n == 1 ? "" : "s");
}

/**
 * @brief Prints the destination of a jump or call.
 *
 * With labels on: the function name, the label (`.loop1`, or `function.label` when it is in another function), or a
 * raw address if the target is not an instruction. A target in a register is printed as the register.
 *
 * @param[in]  p     The printer.
 * @param[in]  prog  The program.
 * @param[in]  in    The jump or call.
 * @param[in]  opt   The output options.
 * @param[out] cmt   Comment buffer.
 * @param[in]  cmtsz Its size.
 */
static void put_target(DisPrinter* p, const DisProgram* prog, const DisInsn* in, const DisOptions* opt, char* cmt, size_t cmtsz) {
    if (in->indirect) {
        put_reg(p, in->reg[0]);
        comment_add(cmt, cmtsz, "indirect");
        return;
    }
    if (opt->labels && in->target_idx >= 0) {
        const DisInsn* t = &prog->insns[in->target_idx];
        const DisFunc* tf = &prog->funcs[t->func];

        if (t->flags & DIS_F_FUNC_START) {
            pr_puts(p, DIS_ST_FUNC, tf->name);
        }
        else if (t->label >= 0 && t->func == in->func) {
            pr_puts(p, DIS_ST_LABEL, prog->labels[t->label].name);
        }
        else if (t->label >= 0) {
            pr_printf(p, DIS_ST_LABEL, "%s%s", tf->name, prog->labels[t->label].name);
        }
        else {
            pr_printf(p, DIS_ST_TARGET, "%s+0x%X", tf->name, (unsigned)(t->addr - tf->start));
        }
    }
    else {
        pr_printf(p, DIS_ST_TARGET, "0x%04X", (unsigned)in->target);
        if (in->target_idx < 0) comment_add(cmt, cmtsz, in->target >= prog->image->size ? "target is outside the file" : "target is not the start of an instruction");
    }
    if (in->op == CALL || (in->flags & DIS_F_TAIL_CALL)) {
        if (opt->labels) comment_add(cmt, cmtsz, "0x%04X", (unsigned)in->target);
    }
    if (in->flags & DIS_F_TAIL_CALL) comment_add(cmt, cmtsz, "tail call");
}

/**
 * @brief Whether an instruction's mnemonic carries a type suffix (`ADD.int`).
 * @param in The instruction.
 */
static bool has_type_suffix(const DisInsn* in) {
    switch ((OpCode)in->op) {
    case ADD: case SUB: case MUL: case DIV: case MOD:
    case AND: case OR: case XOR: case SHL: case SHR: case CMP:
    case LOAD: case STORE: case MEMSET: case STRPARSE:
        return true;
    case MOV: case STORELOCAL:
        return !in->a.is_reg;
    default:
        return false;
    }
}

/**
 * @brief Maps an opcode's family to a style.
 * @param in The instruction.
 */
static DisStyle mnemonic_style(const DisInsn* in) {
    if (in->op == 0) return DIS_ST_BAD;
    switch (dis_class(in->op)) {
    case DIS_CLASS_ALU: return DIS_ST_ALU;
    case DIS_CLASS_MEMORY: return DIS_ST_MEM;
    case DIS_CLASS_CONTROL: return DIS_ST_CTRL;
    case DIS_CLASS_IO: return DIS_ST_IO;
    case DIS_CLASS_STACK: return DIS_ST_STACK;
    }
    return DIS_ST_PLAIN;
}

void dis_print_insn_text(DisPrinter* p, const DisProgram* prog, size_t index, const DisOptions* opt) {
    const DisInsn* in = &prog->insns[index];
    const DisImage* img = prog->image;
    char mn[48], cmt[320] = "";
    int base = p->col;
    unsigned i;

    /* the mnemonic, with the type it works on */
    if (in->op == 0) snprintf(mn, sizeof mn, ".byte");
    else if (has_type_suffix(in)) snprintf(mn, sizeof mn, "%s.%s", dis_mnemonic(in->op), dis_type_name(in->type));
    else snprintf(mn, sizeof mn, "%s", dis_mnemonic(in->op));
    pr_puts(p, mnemonic_style(in), mn);

    if (in->op != RET && in->op != HALT) {
        pr_spaces(p, base + OPERAND_COLUMN - p->col < 1 ? 1 : base + OPERAND_COLUMN - p->col);
    }

    switch ((OpCode)in->op) {
    case MOV:
        put_reg(p, in->reg[0]); put_sep(p); put_operand(p, &in->a, in->type);
        break;
    case ADD: case SUB: case MUL: case DIV: case MOD:
    case AND: case OR: case XOR: case SHL: case SHR:
        put_reg(p, in->reg[0]); put_sep(p); put_operand(p, &in->a, in->type); put_sep(p); put_operand(p, &in->b, in->type);
        break;
    case CMP:
        put_operand(p, &in->a, in->type); put_sep(p); put_operand(p, &in->b, in->type);
        break;
    case RESERVEMEM:
        put_reg(p, in->reg[0]); put_sep(p); put_num(p, in->value);
        comment_add(cmt, sizeof cmt, "new zeroed block");
        break;
    case LOAD:
        put_reg(p, in->reg[1]); put_sep(p); put_mem(p, in->reg[0]);
        break;
    case STORE:
        put_mem(p, in->reg[0]); put_sep(p); put_reg(p, in->reg[1]);
        break;
    case LOADLOCAL:
        put_reg(p, in->reg[1]); put_sep(p);
        pr_puts(p, DIS_ST_PLAIN, "stack"); put_mem(p, in->reg[0]);
        break;
    case STORELOCAL:
        pr_puts(p, DIS_ST_PLAIN, "stack"); put_mem(p, in->reg[0]); put_sep(p); put_operand(p, &in->a, in->type);
        break;
    case LEA:
        put_reg(p, in->reg[0]); put_sep(p);
        pr_puts(p, DIS_ST_PLAIN, in->mode == 0 ? "local[" : "param[");
        put_num(p, in->value);
        pr_puts(p, DIS_ST_PLAIN, "]");
        if (in->mode == 0) comment_add(cmt, sizeof cmt, "stack slot bp+%u", (unsigned)in->value);
        else comment_add(cmt, sizeof cmt, "stack slot bp-%u", (unsigned)in->value + 1u);
        break;
    case ENTER:
        put_num(p, in->value);
        comment_add(cmt, sizeof cmt, "reserve %u slot%s", (unsigned)in->value, in->value == 1 ? "" : "s");
        break;
    case LEAVE:
        put_num(p, in->value);
        comment_add(cmt, sizeof cmt, "release %u slot%s", (unsigned)in->value, in->value == 1 ? "" : "s");
        break;
    case JMP: case JE: case JNE: case JGT: case JLT: case JGE: case JLE: case CALL:
        put_target(p, prog, in, opt, cmt, sizeof cmt);
        break;
    case RET: case HALT:
        break;
    case FREEMEM: case PUSH: case POP: case PRINT: case STRPRINT: case READLINE: case ATEOF: case ABORT: case FERROR:
        put_reg(p, in->reg[0]);
        break;
    case CAST: case REINTERPRET:
        put_reg(p, in->reg[0]); put_sep(p); pr_puts(p, DIS_ST_IMM, dis_type_name(in->type));
        break;
    case STRLIT:
        put_reg(p, in->reg[0]); put_sep(p); put_string(p, prog, in, opt->string_limit, cmt, sizeof cmt);
        break;
    case STRCHR: case STRFROM: case FCLOSE: case FREADLINE: case FEOF: case FREADFILE:
    case STRCAT: case STRCMP: case STRFIND: case FOPEN: case FREAD: case FWRITE: case STRSLICE:
        for (i = 0; i < in->nreg; i++) {
            if (i) put_sep(p);
            put_reg(p, in->reg[i]);
        }
        break;
    case STRPARSE:
        for (i = 0; i < in->nreg; i++) {
            if (i) put_sep(p);
            put_reg(p, in->reg[i]);
        }
        comment_add(cmt, sizeof cmt, "r%u = value, r%u = success flag", (unsigned)in->reg[0], (unsigned)in->reg[1]);
        break;
    case MEMSET:
        put_mem(p, in->reg[0]); put_sep(p); put_reg(p, in->reg[1]); put_sep(p); put_reg(p, in->reg[2]);
        comment_add(cmt, sizeof cmt, "r%u elements", (unsigned)in->reg[2]);
        break;
    case NEWARRAY:
        put_reg(p, in->reg[0]); put_sep(p); put_reg(p, in->reg[1]); put_sep(p); put_num(p, in->small);
        comment_add(cmt, sizeof cmt, "element size %u", (unsigned)in->small);
        break;
    case ARRFREE:
        put_reg(p, in->reg[0]); put_sep(p); put_num(p, in->small);
        comment_add(cmt, sizeof cmt, "depth %u", (unsigned)in->small);
        break;
    case ARRCLONE: case ARRREP:
        for (i = 0; i < in->nreg; i++) { put_reg(p, in->reg[i]); put_sep(p); }
        put_num(p, in->small);
        comment_add(cmt, sizeof cmt, "depth %u", (unsigned)in->small);
        break;
    case CALLC:
        if (in->import < img->import_count) {
            const DisImport* im = &img->imports[in->import];
            char sig[512];

            pr_puts(p, DIS_ST_EXTERN, im->name);
            dis_format_sig(sig, sizeof sig, im->name, &im->sig);
            comment_add(cmt, sizeof cmt, "extern fn %s", sig);
            comment_add(cmt, sizeof cmt, "%u argument%s in registers", (unsigned)in->small, in->small == 1 ? "" : "s");
        }
        else {
            pr_printf(p, DIS_ST_BAD, "#%u", (unsigned)in->import);
            comment_add(cmt, sizeof cmt, "no such import");
        }
        break;
    case OP__END:
        break;
    default:  /* op 0: a byte that is not an instruction */
        pr_printf(p, DIS_ST_IMM, "0x%02X", (unsigned)in->type);
        comment_add(cmt, sizeof cmt, "%s", in->error ? in->error : "invalid");
        break;
    }

    if (cmt[0] && (opt->comments || in->op == 0)) {
        int col = base + COMMENT_COLUMN;

        if (p->col + 2 > col) col = p->col + 2;
        pr_pad(p, col);
        pr_puts(p, DIS_ST_COMMENT, "; ");
        pr_puts(p, DIS_ST_COMMENT, cmt);
    }
}
