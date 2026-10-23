/**
 * @file decode.c
 * @brief The instruction decoder: opcode table, operand layouts and type names.
 *
 * The layouts are those documented at OpCode in `IlangVM.h` and implemented by the `op_*` functions of the VM
 * (`IlangVM.c`, `IlangVM_host.c`). All multi-byte values are little-endian. Two things in the VM's header comments are
 * easy to misread, so they are spelled out here: ENTER, LEAVE and LEA take 32-bit operands, and a jump or call target
 * that is relative counts from the end of the instruction.
 */
#include "ilangdis.h"

#include <string.h>

/** @brief What the decoder and the printer need to know about an opcode. */
typedef struct OpInfo {
    const char* name;  /**< Mnemonic. */
    DisClass cls;      /**< Family, for colouring. */
} OpInfo;

/*
 * The VM's opcode list is positional: adding, removing or reordering an opcode renumbers the others. If this fails to
 * compile the list in IlangVM.h changed: add the new opcode here, in dis_decode() and in format.c, then update the
 * number below.
 */
_Static_assert(CALLC == 62 && OP__END == 63, "Ilangdis: the VM opcode list changed; update decode.c and format.c");

/** @brief Opcode table, indexed by opcode. */
static const OpInfo kOps[OP__END] = {
    [MOV] = { "MOV", DIS_CLASS_ALU },
    [ADD] = { "ADD", DIS_CLASS_ALU },
    [SUB] = { "SUB", DIS_CLASS_ALU },
    [MUL] = { "MUL", DIS_CLASS_ALU },
    [DIV] = { "DIV", DIS_CLASS_ALU },
    [MOD] = { "MOD", DIS_CLASS_ALU },
    [RESERVEMEM] = { "RESERVEMEM", DIS_CLASS_MEMORY },
    [FREEMEM] = { "FREEMEM", DIS_CLASS_MEMORY },
    [LOAD] = { "LOAD", DIS_CLASS_MEMORY },
    [STORE] = { "STORE", DIS_CLASS_MEMORY },
    [LOADLOCAL] = { "LOADLOCAL", DIS_CLASS_MEMORY },
    [STORELOCAL] = { "STORELOCAL", DIS_CLASS_MEMORY },
    [LEA] = { "LEA", DIS_CLASS_STACK },
    [ENTER] = { "ENTER", DIS_CLASS_STACK },
    [LEAVE] = { "LEAVE", DIS_CLASS_STACK },
    [CMP] = { "CMP", DIS_CLASS_ALU },
    [JMP] = { "JMP", DIS_CLASS_CONTROL },
    [JE] = { "JE", DIS_CLASS_CONTROL },
    [JNE] = { "JNE", DIS_CLASS_CONTROL },
    [JGT] = { "JGT", DIS_CLASS_CONTROL },
    [JLT] = { "JLT", DIS_CLASS_CONTROL },
    [JGE] = { "JGE", DIS_CLASS_CONTROL },
    [JLE] = { "JLE", DIS_CLASS_CONTROL },
    [AND] = { "AND", DIS_CLASS_ALU },
    [OR] = { "OR", DIS_CLASS_ALU },
    [XOR] = { "XOR", DIS_CLASS_ALU },
    [CALL] = { "CALL", DIS_CLASS_CONTROL },
    [RET] = { "RET", DIS_CLASS_CONTROL },
    [PUSH] = { "PUSH", DIS_CLASS_STACK },
    [POP] = { "POP", DIS_CLASS_STACK },
    [PRINT] = { "PRINT", DIS_CLASS_IO },
    [CAST] = { "CAST", DIS_CLASS_ALU },
    [REINTERPRET] = { "REINTERPRET", DIS_CLASS_ALU },
    [HALT] = { "HALT", DIS_CLASS_CONTROL },
    [STRLIT] = { "STRLIT", DIS_CLASS_IO },
    [STRPRINT] = { "STRPRINT", DIS_CLASS_IO },
    [STRCAT] = { "STRCAT", DIS_CLASS_IO },
    [STRCMP] = { "STRCMP", DIS_CLASS_IO },
    [STRSLICE] = { "STRSLICE", DIS_CLASS_IO },
    [STRFIND] = { "STRFIND", DIS_CLASS_IO },
    [STRCHR] = { "STRCHR", DIS_CLASS_IO },
    [STRFROM] = { "STRFROM", DIS_CLASS_IO },
    [STRPARSE] = { "STRPARSE", DIS_CLASS_IO },
    [READLINE] = { "READLINE", DIS_CLASS_IO },
    [ATEOF] = { "ATEOF", DIS_CLASS_IO },
    [MEMSET] = { "MEMSET", DIS_CLASS_MEMORY },
    [NEWARRAY] = { "NEWARRAY", DIS_CLASS_MEMORY },
    [ARRFREE] = { "ARRFREE", DIS_CLASS_MEMORY },
    [ARRCLONE] = { "ARRCLONE", DIS_CLASS_MEMORY },
    [ARRREP] = { "ARRREP", DIS_CLASS_MEMORY },
    [ABORT] = { "ABORT", DIS_CLASS_CONTROL },
    [FOPEN] = { "FOPEN", DIS_CLASS_IO },
    [FCLOSE] = { "FCLOSE", DIS_CLASS_IO },
    [FREAD] = { "FREAD", DIS_CLASS_IO },
    [FREADLINE] = { "FREADLINE", DIS_CLASS_IO },
    [FWRITE] = { "FWRITE", DIS_CLASS_IO },
    [FEOF] = { "FEOF", DIS_CLASS_IO },
    [FERROR] = { "FERROR", DIS_CLASS_IO },
    [FREADFILE] = { "FREADFILE", DIS_CLASS_IO },
    [SHL] = { "SHL", DIS_CLASS_ALU },
    [SHR] = { "SHR", DIS_CLASS_ALU },
    [CALLC] = { "CALLC", DIS_CLASS_CONTROL },
};

const char* dis_mnemonic(uint8_t op) {
    return (op > 0 && op < OP__END && kOps[op].name) ? kOps[op].name : "???";
}

DisClass dis_class(uint8_t op) {
    return (op > 0 && op < OP__END && kOps[op].name) ? kOps[op].cls : DIS_CLASS_ALU;
}

bool dis_is_jump(uint8_t op) {
    return op >= JMP && op <= JLE;
}

bool dis_is_cond_jump(uint8_t op) {
    return op > JMP && op <= JLE;
}

const char* dis_type_name(uint8_t tag) {
    switch (tag) {
    case T_NONE: return "void";
    case T_INT: return "int";
    case T_UINT: return "uint";
    case T_FLOAT: return "float";
    case T_BOOL: return "bool";
    case T_ADDRESS: return "address";
    case T_STRINGADDRESS: return "string";
    case T_BYTE: return "byte";
    default: return "?";
    }
}

void dis_format_type(char* buf, size_t size, uint8_t tag, uint8_t depth) {
    size_t n = (size_t)snprintf(buf, size, "%s", dis_type_name(tag));
    uint8_t i;

    for (i = 0; i < depth && n + 2 < size; i++) {
        buf[n++] = '[';
        buf[n++] = ']';
    }
    if (size) buf[n < size ? n : size - 1] = '\0';
}

void dis_format_sig(char* buf, size_t size, const char* name, const iHostSig* sig) {
    size_t n = 0;
    uint8_t i;
    char t[48];

    if (!size) return;
    n += (size_t)snprintf(buf + n, size - n, "%s(", name);
    for (i = 0; i < sig->argc && n < size; i++) {
        dis_format_type(t, sizeof t, sig->arg_type[i], sig->arg_depth[i]);
        n += (size_t)snprintf(buf + n, size - n, "%s%s", i ? ", " : "", t);
    }
    if (n < size) n += (size_t)snprintf(buf + n, size - n, ")");
    if (sig->ret_type != T_NONE && n < size) {
        dis_format_type(t, sizeof t, sig->ret_type, sig->ret_depth);
        snprintf(buf + n, size - n, " -> %s", t);
    }
}

/* ------------------------------------------------------------------------------------------------------------------ */

/** @brief A read position in the image. Reading past the end, or an impossible value, sets `error` for good. */
typedef struct Cursor {
    const uint8_t* data;  /**< The image. */
    uint32_t pos;         /**< Next byte to read. */
    uint32_t size;        /**< Size of the image. */
    const char* error;    /**< First problem met, or NULL. */
} Cursor;

/**
 * @brief Reads one byte.
 * @param c The cursor.
 * @return The byte, or 0 once the cursor has failed.
 */
static uint8_t get8(Cursor* c) {
    if (c->error) return 0;
    if (c->pos >= c->size) {
        c->error = "instruction runs past the end of the code";
        return 0;
    }
    return c->data[c->pos++];
}

/**
 * @brief Reads a little-endian 16-bit value.
 * @param c The cursor.
 */
static uint16_t get16(Cursor* c) {
    uint16_t lo = get8(c);
    uint16_t hi = get8(c);

    return (uint16_t)(lo | (hi << 8));
}

/**
 * @brief Reads a little-endian 32-bit value.
 * @param c The cursor.
 */
static uint32_t get32(Cursor* c) {
    uint32_t b0 = get8(c), b1 = get8(c), b2 = get8(c), b3 = get8(c);

    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

/**
 * @brief Reads a register number; one the VM would refuse makes the instruction undecodable.
 * @param c The cursor.
 */
static uint8_t get_reg(Cursor* c) {
    uint8_t r = get8(c);

    if (!c->error && r >= DIS_MAX_REGS) {
        c->error = "register number out of range";
        return 0;
    }
    return r;
}

/**
 * @brief Reads an inline immediate of a type: 1 byte for bool and byte, 4 bytes for the others.
 * @param c    The cursor.
 * @param type Type tag.
 * @return The immediate's bits, or 0 (with the cursor failed) for a type that cannot be an immediate.
 */
static uint32_t get_imm(Cursor* c, uint8_t type) {
    switch (type) {
    case T_BOOL: return get8(c) != 0;
    case T_BYTE: return get8(c);
    case T_INT: case T_UINT: case T_FLOAT: case T_ADDRESS: return get32(c);
    default:
        if (!c->error) c->error = "immediate of an unknown type";
        return 0;
    }
}

/**
 * @brief Reads one operand of a binary instruction.
 * @param c      The cursor.
 * @param o      Receives the operand.
 * @param is_reg Whether the operand is a register (otherwise an immediate of @p type).
 * @param type   Type tag of the instruction.
 */
static void get_operand(Cursor* c, DisOperand* o, bool is_reg, uint8_t type) {
    o->is_reg = is_reg;
    if (is_reg) o->reg = get_reg(c);
    else o->bits = get_imm(c, type);
}

/**
 * @brief Reads the register operands of an instruction that only has registers.
 * @param c  The cursor.
 * @param in The instruction; its `reg` and `nreg` are filled in.
 * @param n  How many registers to read (at most 5).
 */
static void get_regs(Cursor* c, DisInsn* in, unsigned n) {
    unsigned i;

    for (i = 0; i < n; i++) in->reg[i] = get_reg(c);
    in->nreg = (uint8_t)n;
}

/**
 * @brief Reads the operands of a binary instruction: `[MODE][TYPE]`, then the destination (unless @p with_dest is
 *        false, as for CMP) and both operands.
 * @param c         The cursor.
 * @param in        The instruction.
 * @param with_dest Whether a destination register comes first.
 */
static void get_binary(Cursor* c, DisInsn* in, bool with_dest) {
    in->mode = get8(c);
    in->type = get8(c);
    if (in->mode > IMM_IMM && !c->error) c->error = "invalid operand mode";
    if (with_dest) get_regs(c, in, 1);
    if (c->error) return;
    get_operand(c, &in->a, in->mode == REG_REG || in->mode == REG_IMM, in->type);
    get_operand(c, &in->b, in->mode == REG_REG || in->mode == IMM_REG, in->type);
}

/**
 * @brief Reads the operands of a jump or a call: `[RELATIVE][REGISTER][u32 target | register]`.
 * @param c  The cursor.
 * @param in The instruction; the absolute target is worked out as the VM does (relative to the next instruction).
 */
static void get_branch(Cursor* c, DisInsn* in) {
    in->relative = get8(c) != 0;
    in->indirect = get8(c) != 0;
    in->mode = in->relative;
    if (in->indirect) {
        get_regs(c, in, 1);
        return;
    }
    in->value = get32(c);
    if (c->error) return;
    in->target = in->relative ? (uint32_t)(c->pos + in->value) : in->value;
    in->has_target = true;
}

uint32_t dis_decode(const DisImage* img, uint32_t addr, DisInsn* in) {
    Cursor c;
    uint8_t op;

    memset(in, 0, sizeof *in);
    in->addr = addr;
    in->target_idx = -1;
    in->func = -1;
    in->label = -1;

    c.data = img->data;
    c.pos = addr;
    c.size = img->size;
    c.error = NULL;

    op = get8(&c);
    in->op = op;
    if (op == 0 || op >= OP__END || !kOps[op].name) {
        c.error = "not an opcode";
    }
    else switch ((OpCode)op) {
    case MOV:
        in->mode = get8(&c);
        in->type = get8(&c);
        get_regs(&c, in, 1);
        if (in->mode == REG_REG) get_operand(&c, &in->a, true, in->type);
        else if (in->mode == REG_IMM) get_operand(&c, &in->a, false, in->type);
        else if (!c.error) c.error = "invalid MOV mode";
        break;
    case ADD: case SUB: case MUL: case DIV: case MOD:
    case AND: case OR: case XOR: case SHL: case SHR:
        get_binary(&c, in, true);
        break;
    case CMP:
        get_binary(&c, in, false);
        break;
    case RESERVEMEM:
        get_regs(&c, in, 1);
        in->value = get32(&c);
        break;
    case LOAD: case STORE:
        in->type = get8(&c);
        get_regs(&c, in, 2);
        break;
    case LOADLOCAL:
        get_regs(&c, in, 2);
        break;
    case STORELOCAL:
        get_regs(&c, in, 1);
        in->type = get8(&c);
        get_operand(&c, &in->a, get8(&c) != 0, in->type);
        break;
    case LEA:
        get_regs(&c, in, 1);
        in->mode = get8(&c);
        in->value = get32(&c);
        break;
    case ENTER: case LEAVE:
        in->value = get32(&c);
        break;
    case JMP: case JE: case JNE: case JGT: case JLT: case JGE: case JLE: case CALL:
        get_branch(&c, in);
        break;
    case RET: case HALT:
        break;
    case FREEMEM: case PUSH: case POP: case PRINT: case STRPRINT: case READLINE: case ATEOF: case ABORT: case FERROR:
        get_regs(&c, in, 1);
        break;
    case CAST: case REINTERPRET:
        get_regs(&c, in, 1);
        in->type = get8(&c);
        break;
    case STRLIT:
        get_regs(&c, in, 1);
        in->value = get32(&c);
        in->data_off = c.pos;
        if (!c.error) {
            if (c.size - c.pos < in->value) c.error = "string literal runs past the end of the code";
            else c.pos += in->value;
        }
        break;
    case STRCHR: case STRFROM: case FCLOSE: case FREADLINE: case FEOF: case FREADFILE:
        get_regs(&c, in, 2);
        break;
    case STRCAT: case STRCMP: case STRFIND: case FOPEN: case FREAD: case FWRITE:
        get_regs(&c, in, 3);
        break;
    case STRSLICE:
        get_regs(&c, in, 4);
        break;
    case STRPARSE:
        get_regs(&c, in, 3);
        in->type = get8(&c);
        break;
    case MEMSET:
        in->type = get8(&c);
        get_regs(&c, in, 3);
        break;
    case NEWARRAY:
        get_regs(&c, in, 2);
        in->small = get8(&c);
        break;
    case ARRFREE:
        get_regs(&c, in, 1);
        in->small = get8(&c);
        break;
    case ARRCLONE:
        get_regs(&c, in, 2);
        in->small = get8(&c);
        break;
    case ARRREP:
        get_regs(&c, in, 3);
        in->small = get8(&c);
        break;
    case CALLC:
        in->import = get16(&c);
        in->small = get8(&c);
        break;
    case OP__END:
        c.error = "not an opcode";
        break;
    }

    if (c.error) {
        /* Not an instruction: report the single byte and let the caller resynchronise on the next one. */
        uint8_t byte = img->data[addr];

        memset(in, 0, sizeof *in);
        in->addr = addr;
        in->len = 1;
        in->op = 0;
        in->type = byte;
        in->error = c.error;
        in->target_idx = -1;
        in->func = -1;
        in->label = -1;
        return 1;
    }

    in->len = c.pos - addr;
    return in->len;
}
