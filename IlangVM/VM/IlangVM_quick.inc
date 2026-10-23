/**
 * @file IlangVM_quick.inc
 * @brief Lazily quickened interpreter (gcc / clang). Included by IlangVM.c just before IlangVM_execute().
 *
 * `dec[ip]` caches a decoded, validated, type/mode-specialised record for the instruction that starts at BYTE offset
 * `ip`. Records are decoded the first time they are reached, so only code that really runs is ever looked at. Offsets
 * stay byte offsets, so a jump into the middle of an instruction behaves exactly as before. Anything the original fast
 * paths would have declined (bad register, bad type, truncated code, ...) is decoded as QK_SLOW, which runs the original
 * slow_op() on the raw bytes, so every error message is unchanged. Runtime declines (division by zero, a memory miss,
 * stack overflow, a tag mismatch) jump to the same slow path BEFORE anything has been modified.
 *
 * Besides the one-to-one handlers there are superinstructions, each matched on the exact byte sequence the compiler
 * emits and each leaving every register, flag and allocation counter as the sequence would:
 *  - CMP + conditional jump
 *  - the bounds-checked / unchecked array element access (LOAD len, CMP, JGE, address arithmetic, LOAD/STORE)
 *  - STRLIT, one use of the literal, FREEMEM (no block is built; see memory_phantom_acquire())
 *
 * A superinstruction that meets anything unusual runs its FIRST original instruction through the slow path and carries
 * on instruction by instruction, exactly as before.
 *
 * Not used with TRACE, MAX_INSTRUCTIONS or STRICT_TYPES (see IlangVM.c).
 */

/* Labels as values are a GNU extension, on purpose: the warnings that flag exactly that are switched off for this file. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#pragma clang diagnostic ignored "-Wgnu-statement-expression"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif


/** @brief float -> integer with the same saturating rules as l_cast() (IlangVM_cast.c). */
static inline int32_t qc_f2i(float f) { if (f != f) return 0; if (f >= 2147483648.0f) return INT32_MAX; if (f <= -2147483648.0f) return INT32_MIN; return (int32_t)f; }
/** @brief float -> unsigned integer with the same saturating rules as l_cast() (IlangVM_cast.c). */
static inline uint32_t qc_f2u(float f) { if (f != f) return 0u; if (f <= 0.0f) return 0u; if (f >= 4294967296.0f) return UINT32_MAX; return (uint32_t)f; }

/**
 * @brief A decoded instruction record, one per byte offset of the code (see execute_quick()).
 *
 * Which fields are used depends on the kind; the register operands and the type tag / flag live in `a`, `b`, `c` and `x`.
 */
typedef struct QI {
    int32_t h;       /**< Handler: address of its label minus the address of Q_DECODE, so 0 (calloc) means "not decoded yet". */
    uint32_t next;   /**< Byte offset of the following instruction. */
    uint32_t imm;    /**< Immediate. */
    uint32_t tgt;    /**< Jump / call target (absolute byte offset); for the literal kinds, the literal's length. */
    uint8_t a,   /**< Register operand (first). */
            b,   /**< Register operand (second). */
            c,   /**< Register operand (third). */
            x;   /**< Type tag or flag; the meaning depends on the kind. */
    uint32_t ic;     /**< Per-site cache: index of the memory block this instruction touched last. */
} QI;

/**
 * @def QKINDS(X)
 * @brief Every kind of record, in the order of the label table (`QK_<name>` is the enumerator, `Q_<name>` the label).
 *
 * Naming: `_RR` register,register; `_RI` register,immediate; `_IR` immediate,register; ADD/SUB/MUL/DIV/MOD are followed
 * by I (int), U (uint, address) or F (float); `CJ*` = compare and conditional jump fused (I, U, B for bool / byte, F);
 * `LD` / `ST` = memory access (32-bit, 8-bit, bool), `LDX` / `STX` = bounds-checked array element, `LDU` / `STU` =
 * unchecked one; `D_*` = calls the original `op_xxx` directly; `LIT_*` = fused one-use string literal.
 */
#define QKINDS(X) \
    X(DECODE) X(SLOW) X(END) X(HALT) \
    X(MOV_RR) X(MOV_RI) \
    X(ADDI_RR) X(ADDI_RI) X(SUBI_RR) X(SUBI_RI) X(MULI_RR) X(MULI_RI) \
    X(ADDU_RR) X(ADDU_RI) X(SUBU_RR) X(SUBU_RI) X(MULU_RR) X(MULU_RI) \
    X(ADDF_RR) X(ADDF_RI) X(SUBF_RR) X(SUBF_RI) X(MULF_RR) X(MULF_RI) X(DIVF_RR) X(DIVF_RI) \
    X(DIVI_RR) X(DIVI_RI) X(MODI_RR) X(MODI_RI) X(DIVU_RR) X(DIVU_RI) X(MODU_RR) X(MODU_RI) \
    X(AND_RR) X(AND_RI) X(OR_RR) X(OR_RI) X(XOR_RR) X(XOR_RI) \
    X(SHLI_RR) X(SHLI_RI) X(SHLI_IR) X(SHLU_RR) X(SHLU_RI) X(SHLU_IR) X(SHRI_RR) X(SHRI_RI) X(SHRI_IR) X(SHRU_RR) X(SHRU_RI) X(SHRU_IR) \
    X(CJI_RR) X(CJI_RI) X(CJU_RR) X(CJU_RI) X(CJB_RR) X(CJB_RI) X(CJF_RR) X(CJF_RI) \
    X(JMP) X(JE) X(JNE) X(JGT) X(JLT) X(JGE) X(JLE) \
    X(CALL) X(RET) X(PUSH) X(POP) X(ENTER) X(LEAVE) X(LOADLOCAL) X(STORELOCAL_R) X(STORELOCAL_I) X(LEA) X(REINTERPRET) \
    X(LD32) X(LD8) X(LDB) X(ST32) X(ST8) X(STB) \
    X(LDX32) X(LDX8) X(LDXB) X(STX32) X(STX8) X(STXB) X(LDU32) X(LDU8) X(LDUB) X(STU32) X(STU8) X(STUB) \
    X(D_RESERVEMEM) X(D_FREEMEM) X(D_STRLIT) X(D_STRPRINT) X(D_STRCAT) X(D_STRCMP) X(D_STRSLICE) X(D_STRFIND) \
    X(D_STRCHR) X(D_STRFROM) X(D_PRINT) X(CAST) X(D_CAST) X(D_NEWARRAY) X(D_ARRFREE) X(D_MEMSET) \
    X(LIT_PRINT) X(LIT_CMP) X(LIT_CAT) X(LIT_FIND)

/** @brief The record kinds: one `QK_<name>` per entry of QKINDS(), plus the count. */
enum {
#define X(n) QK_##n,
    QKINDS(X)
#undef X
    QK__COUNT
};

/** @brief Byte length of an immediate of type tag `t`, or 0 if `t` cannot be an immediate. */
static int qd_imm_len(uint8_t t) {
    switch (t) {
    case T_INT: case T_UINT: case T_ADDRESS: case T_FLOAT: return 4;
    case T_BOOL: case T_BYTE: return 1;
    default: return 0;
    }
}
/** @brief The immediate of type `t` at `p` as raw 32 bits (a bool is normalised to 0 / 1). */
static uint32_t qd_imm(const uint8_t* p, uint8_t t) {
    if (t == T_BOOL) return p[0] != 0;
    if (t == T_BYTE) return p[0];
    return rd_u32(p);
}


/**
 * @brief Element size in bytes of an array of type `t` (0 if unsupported).
 *
 * Array element superinstructions are pattern-matched from the exact sequences the compiler emits:
 * @code{.unparsed}
 * checked:    LOAD.uint t,[base]; CMP.uint idx,t; JGE oob; (MUL.uint t,idx,#sz; ADD.addr t,t,base | ADD.addr t,base,idx);
 *             ADD.addr t,t,#4; LOAD|STORE.type [t]
 * unchecked:  (MUL.uint t,idx,#sz; ADD.addr t,t,base | ADD.addr t,base,idx); ADD.addr t,t,#4; LOAD|STORE.type [t]
 * @endcode
 * A fused op does everything in one go on the common path; on ANY surprise it runs the first original instruction
 * through the slow path and execution continues instruction by instruction, exactly as before.
 */
static int q_elem_size(uint8_t t) {
    if (t == T_INT || t == T_UINT || t == T_FLOAT || t == T_ADDRESS) return 4;
    if (t == T_BYTE || t == T_BOOL) return 1;
    return 0;
}

/**
 * @brief Tries the address+access tail at `c + pos` (after any bounds check).
 * @param[in]  c      The code.
 * @param[in]  pos    Offset of the tail.
 * @param[in]  n      Size of the code.
 * @param[out] q      On success, filled with `a`/`b`/`c`/`imm`/`x`.
 * @param[out] store  1 for a STORE, 0 for a LOAD.
 * @param[out] sz_out Element size.
 * @return The byte length of the tail on success, or 0 if there is no match.
 */
static uint32_t q_match_tail(const uint8_t* c, uint32_t pos, uint32_t n, QI* q, int* store, int* sz_out) {
    const uint8_t* p = c + pos;
    uint32_t len = 0;
    uint8_t t, idx, base;
    int sz;

    /* first: either MUL.uint t,idx,#sz + ADD.addr t,t,base   (sz 4)   or   ADD.addr t,base,idx   (sz 1) */
    if (n - pos >= 9 + 6 + 9 + 4 && p[0] == MUL && p[1] == REG_IMM && p[2] == T_UINT && p[3] < REG_COUNT && p[4] < REG_COUNT && rd_u32(p + 5) == 4u) {
        t = p[3]; idx = p[4]; sz = 4;
        if (!(p[9] == ADD && p[10] == REG_REG && p[11] == T_ADDRESS && p[12] == t && p[13] == t && p[14] < REG_COUNT)) return 0;
        base = p[14];
        len = 15;
    }
    else if (n - pos >= 6 + 9 + 4 && p[0] == ADD && p[1] == REG_REG && p[2] == T_ADDRESS && p[3] < REG_COUNT && p[4] < REG_COUNT && p[5] < REG_COUNT) {
        t = p[3]; base = p[4]; idx = p[5]; sz = 1;
        len = 6;
    }
    else return 0;

    p = c + pos + len;
    if (!(p[0] == ADD && p[1] == REG_IMM && p[2] == T_ADDRESS && p[3] == t && p[4] == t && rd_u32(p + 5) == 4u)) return 0;
    len += 9;
    p = c + pos + len;
    if (!((p[0] == LOAD || p[0] == STORE) && q_elem_size(p[1]) == sz && p[2] == t && p[3] < REG_COUNT)) return 0;
    *store = (p[0] == STORE);
    /* aliasing that would make the fused op differ from the sequence: base / idx must survive until the end, and a stored
       value must not be the temp (the sequence overwrites it with the address first) */
    if (t == base || t == idx || (*store && p[3] == t)) return 0;
    q->x = p[1]; q->b = idx; q->a = base; q->c = t; q->imm = p[3];
    len += 4;
    *sz_out = sz;
    return len;
}

/**
 * @brief Tries to match a fused array element access at `ip`.
 * @param[in]  c  The code.
 * @param[in]  ip Offset of the first instruction.
 * @param[in]  n  Size of the code.
 * @param[out] q  Filled in on a match.
 * @return The record kind (QK_LDX* / STX* when a bounds check comes first, QK_LDU* / STU* otherwise), or -1 if the
 *         bytes are not exactly such a sequence.
 */
static int q_match_elem(const uint8_t* c, uint32_t ip, uint32_t n, QI* q) {
    uint32_t rem = n - ip, off, tl;
    int store = 0, sz = 0;
    uint8_t lt, idx, base;
    const uint8_t* p = c + ip;

    if (p[0] == LOAD && rem >= 4 + 5 + 7 && p[1] == T_UINT && p[2] < REG_COUNT && p[3] < REG_COUNT
        && p[4] == CMP && p[5] == REG_REG && p[6] == T_UINT && p[7] < REG_COUNT && p[8] == p[3]
        && p[9] == JGE && p[11] == 0) {
        base = p[2]; lt = p[3]; idx = p[7];
        off = 4 + 5 + 7;
        tl = q_match_tail(c, ip + off, n, q, &store, &sz);
        if (!tl) return -1;
        if (q->c != lt || q->a != base || q->b != idx) return -1;
        q->next = ip + off + tl;
        return (store ? QK_STX32 : QK_LDX32) + (sz == 4 ? 0 : (q->x == T_BYTE ? 1 : 2));
    }
    if ((p[0] == MUL || p[0] == ADD)) {
        tl = q_match_tail(c, ip, n, q, &store, &sz);
        if (!tl) return -1;
        q->next = ip + tl;
        return (store ? QK_STU32 : QK_LDU32) + (sz == 4 ? 0 : (q->x == T_BYTE ? 1 : 2));
    }
    return -1;
}

/**
 * @brief Tries to match `STRLIT t,"..."; <one use of t>; FREEMEM t` (what the compiler emits for a string literal that
 *        is used once).
 *
 * The use is `STRPRINT t` or `STRCMP/STRCAT/STRFIND to,x,t` or `to,t,x` (`to != t`, `x != t`). On a match it fills
 * `a` = t, `b` = to, `c` = x, `x` = "the literal is the first operand", `imm` = offset of the literal's bytes and `tgt` =
 * its length.
 *
 * @param[in]  c    The code.
 * @param[in]  ip   Offset of the STRLIT.
 * @param[in]  size Size of the code.
 * @param[out] q    Filled in on a match.
 * @return QK_LIT_PRINT / QK_LIT_CMP / QK_LIT_CAT / QK_LIT_FIND, or -1.
 */
static int q_match_lit(const uint8_t* c, uint32_t ip, uint32_t size, QI* q) {
    const uint8_t* p = c + ip;
    uint32_t rem = size - ip, n, e;
    uint8_t t;
    int kind;

    if (rem < 6 || p[1] >= REG_COUNT) return -1;
    t = p[1];
    n = rd_u32(p + 2);
    if (n > rem - 6 || n > UINT32_MAX - 4u) return -1;
    e = 6 + n;                 /* offset (from ip) of the instruction that uses the literal */
    p += e; rem -= e;
    if (rem < 1) return -1;

    if (p[0] == STRPRINT) {
        if (rem < 4 || p[1] != t || p[2] != FREEMEM || p[3] != t) return -1;
        q->next = ip + e + 4;
        kind = QK_LIT_PRINT;
    }
    else if (p[0] == STRCMP || p[0] == STRCAT || p[0] == STRFIND) {
        uint8_t to, ra, rb;
        if (rem < 6 || p[4] != FREEMEM || p[5] != t) return -1;
        to = p[1]; ra = p[2]; rb = p[3];
        if (to >= REG_COUNT || ra >= REG_COUNT || rb >= REG_COUNT) return -1;
        if (to == t || (ra == t) == (rb == t)) return -1;   /* the literal must be exactly one operand */
        q->b = to; q->c = (ra == t) ? rb : ra; q->x = (ra == t);
        q->next = ip + e + 6;
        kind = (p[0] == STRCMP) ? QK_LIT_CMP : (p[0] == STRCAT) ? QK_LIT_CAT : QK_LIT_FIND;
    }
    else return -1;

    q->a = t; q->imm = ip + 6; q->tgt = n;
    return kind;
}

/**
 * @brief Decodes the instruction at byte offset `ip` into `*q` and returns its kind (QK_*).
 *
 * Never fails: anything unusual becomes QK_SLOW. (It returns a kind rather than a label address on purpose: label
 * addresses must stay inside execute_quick_run(), or gcc -flto -fprofile-* can emit references to labels of the wrong
 * function.)
 *
 * @param[in]  s  The VM.
 * @param[out] q  The record to fill.
 * @param[in]  ip Byte offset of the instruction.
 */
__attribute__((noinline, cold))
static int q_decode(iState* s, QI* q, uint32_t ip) {
    const uint8_t* c = s->code;
    uint32_t rem = s->code_size - ip;          /* bytes available including the opcode */
    const uint8_t* p = c + ip + 1;             /* first operand byte */
    uint8_t op = c[ip];
    int k = QK_SLOW;

    q->a = q->b = q->c = q->x = 0; q->imm = q->tgt = 0; q->next = ip + 1;

    if (op == LOAD || op == MUL || op == ADD || op == STRLIT) {
        int fk = (op == STRLIT) ? q_match_lit(c, ip, s->code_size, q) : q_match_elem(c, ip, s->code_size, q);
        if (fk >= 0) return fk;
        q->a = q->b = q->c = q->x = 0; q->imm = q->tgt = 0; q->next = ip + 1;
    }

    switch (op) {
    case MOV:
        if (rem >= 5 && p[2] < REG_COUNT) {
            if (p[0] == REG_REG && p[3] < REG_COUNT) { k = QK_MOV_RR; q->a = p[2]; q->b = p[3]; q->next = ip + 5; }
            else if (p[0] == REG_IMM) {
                int l = qd_imm_len(p[1]);
                if (l && rem >= 4u + l) { k = QK_MOV_RI; q->a = p[2]; q->x = p[1]; q->imm = qd_imm(p + 3, p[1]); q->next = ip + 4 + l; }
            }
        }
        break;

    case ADD: case SUB: case MUL: case DIV: case MOD:
    case AND: case OR: case XOR: {
        /* [op][mode][type][to][a][b] */
        uint8_t mode, type;
        int rr, il;
        if (rem < 6) break;
        mode = p[0]; type = p[1];
        if ((mode != REG_REG && mode != REG_IMM) || p[2] >= REG_COUNT || p[3] >= REG_COUNT) break;
        rr = (mode == REG_REG);
        il = qd_imm_len(type);
        if (!il || type == T_BOOL || type == T_BYTE) break;   /* 32-bit types only */
        if ((op == AND || op == OR || op == XOR) && type != T_INT && type != T_UINT) break;   /* ADDRESS / FLOAT panic in the old path */
        if (rr) { if (p[4] >= REG_COUNT) break; }
        else if (rem < 1u + 4 + il) break;
        {
            int fam = (type == T_INT) ? 0 : (type == T_FLOAT) ? 2 : 1;   /* 0 int, 1 uint/addr, 2 float */
            int kk = -1;
            switch (op) {
            case ADD: kk = fam == 0 ? QK_ADDI_RR : fam == 1 ? QK_ADDU_RR : QK_ADDF_RR; break;
            case SUB: kk = fam == 0 ? QK_SUBI_RR : fam == 1 ? QK_SUBU_RR : QK_SUBF_RR; break;
            case MUL: kk = fam == 0 ? QK_MULI_RR : fam == 1 ? QK_MULU_RR : QK_MULF_RR; break;
            case DIV: kk = fam == 0 ? QK_DIVI_RR : fam == 1 ? QK_DIVU_RR : QK_DIVF_RR; break;
            case MOD: kk = fam == 2 ? -1 : fam == 0 ? QK_MODI_RR : QK_MODU_RR; break;
            case AND: kk = fam == 2 ? -1 : QK_AND_RR; break;
            case OR:  kk = fam == 2 ? -1 : QK_OR_RR; break;
            case XOR: kk = fam == 2 ? -1 : QK_XOR_RR; break;
            }
            if (kk < 0) break;
            /* each family lists its _RR kind immediately followed by its _RI kind (DIVF too), hence kk + 1 for the immediate form */
            k = kk + (rr ? 0 : 1);
            q->x = type;
            q->a = p[2]; q->b = p[3];
            if (rr) { q->c = p[4]; q->next = ip + 6; }
            else { q->imm = rd_u32(p + 4); q->next = ip + 9; }
        }
        break;
    }

    case SHL: case SHR: {
        /* [op][mode][type][to][a][b]: int and uint only (byte and anything else take the slow path). Three forms:
         * register,register  register,immediate  immediate,register (1 << n). An int count below zero panics: an
         * immediate one is left to the slow path here, a register one is checked when it runs. */
        uint8_t mode, type;
        int base;
        if (rem < 6) break;
        mode = p[0]; type = p[1];
        if ((mode != REG_REG && mode != REG_IMM && mode != IMM_REG) || p[2] >= REG_COUNT) break;
        if (type != T_INT && type != T_UINT) break;
        base = (op == SHL) ? ((type == T_INT) ? QK_SHLI_RR : QK_SHLU_RR) : ((type == T_INT) ? QK_SHRI_RR : QK_SHRU_RR);
        q->x = type;
        q->a = p[2];
        if (mode == REG_REG) {
            if (p[3] >= REG_COUNT || p[4] >= REG_COUNT) break;
            q->b = p[3]; q->c = p[4]; q->next = ip + 6;
            k = base;
        }
        else if (mode == REG_IMM) {
            if (rem < 9 || p[3] >= REG_COUNT) break;
            q->imm = rd_u32(p + 4);
            if (type == T_INT && (int32_t)q->imm < 0) break;
            q->b = p[3]; q->next = ip + 9;
            k = base + 1;
        }
        else {
            if (rem < 9 || p[7] >= REG_COUNT) break;
            q->imm = rd_u32(p + 3);
            q->c = p[7]; q->next = ip + 9;
            k = base + 2;
        }
        break;
    }

    case CMP: {
        /* [CMP][mode][type][a][b] followed (we fuse) by [Jcc][rel][reg=0][target:4] */
        uint8_t mode, type;
        int rr, il;
        uint32_t len;
        const uint8_t* j;
        uint32_t jn;
        if (rem < 5) break;
        mode = p[0]; type = p[1];
        if ((mode != REG_REG && mode != REG_IMM) || p[2] >= REG_COUNT) break;
        rr = (mode == REG_REG);
        il = qd_imm_len(type);
        if (!il || type == T_STRINGADDRESS) break;
        if (rr) { if (p[3] >= REG_COUNT) break; len = 1 + 2 + 2; }
        else { len = 1 + 2 + 1 + il; if (rem < len) break; }
        if (rem < len + 7) break;
        j = c + ip + len;
        if (j[0] < JE || j[0] > JLE || j[2] != 0) break;       /* not followed by a conditional jump with an immediate target */
        jn = ip + len + 7;
        q->tgt = j[1] ? jn + rd_u32(j + 3) : rd_u32(j + 3);
        { static const uint8_t mask[] = { /*JE*/ 0x4, /*JNE*/ 0x3, /*JGT*/ 0x1, /*JLT*/ 0x2, /*JGE*/ 0x5, /*JLE*/ 0x6 };
          q->x = mask[j[0] - JE]; }
        q->a = p[2];
        if (rr) q->b = p[3]; else q->imm = qd_imm(p + 3, type);
        q->next = jn;
        switch (type) {
        case T_INT:   k = QK_CJI_RR; break;
        case T_UINT: case T_ADDRESS: k = QK_CJU_RR; break;
        case T_BOOL: case T_BYTE: k = QK_CJB_RR; break;
        case T_FLOAT: k = QK_CJF_RR; break;
        default: k = QK_SLOW; break;
        }
        if (k != QK_SLOW) k += rr ? 0 : 1;
        break;
    }

    case JMP: case JE: case JNE: case JGT: case JLT: case JGE: case JLE:
        if (rem >= 7 && p[1] == 0) {
            uint32_t jn = ip + 7;
            q->tgt = p[0] ? jn + rd_u32(p + 2) : rd_u32(p + 2);
            q->next = jn;
            k = QK_JMP + (op - JMP);
        }
        break;

    case CALL:
        if (rem >= 7 && p[1] == 0) {
            uint32_t jn = ip + 7;
            q->tgt = p[0] ? jn + rd_u32(p + 2) : rd_u32(p + 2);
            q->next = jn;
            k = QK_CALL;
        }
        break;

    case RET: k = QK_RET; q->next = ip + 1; break;
    case PUSH: if (rem >= 2 && p[0] < REG_COUNT) { k = QK_PUSH; q->a = p[0]; q->next = ip + 2; } break;
    case POP:  if (rem >= 2 && p[0] < REG_COUNT) { k = QK_POP;  q->a = p[0]; q->next = ip + 2; } break;
    case ENTER: if (rem >= 5) { k = QK_ENTER; q->imm = rd_u32(p); q->next = ip + 5; } break;
    case LEAVE: if (rem >= 5) { k = QK_LEAVE; q->imm = rd_u32(p); q->next = ip + 5; } break;
    case LOADLOCAL: if (rem >= 3 && p[0] < REG_COUNT && p[1] < REG_COUNT) { k = QK_LOADLOCAL; q->a = p[0]; q->b = p[1]; q->next = ip + 3; } break;
    case STORELOCAL:
        /* [STORELOCAL][offreg][immtype][isreg][from | imm] */
        if (rem >= 5 && p[0] < REG_COUNT) {
            if (p[2] != 0) { if (p[3] < REG_COUNT) { k = QK_STORELOCAL_R; q->a = p[0]; q->b = p[3]; q->next = ip + 5; } }
            else {
                int l = qd_imm_len(p[1]);
                if (l && rem >= 4u + l) { k = QK_STORELOCAL_I; q->a = p[0]; q->x = p[1]; q->imm = qd_imm(p + 3, p[1]); q->next = ip + 4 + l; }
            }
        }
        break;
    case LEA: if (rem >= 7 && p[0] < REG_COUNT) { k = QK_LEA; q->a = p[0]; q->x = p[1]; q->imm = rd_u32(p + 2); q->next = ip + 7; } break;
    case REINTERPRET: if (rem >= 3 && p[0] < REG_COUNT && p[1] >= T_INT && p[1] <= T_BYTE) { k = QK_REINTERPRET; q->a = p[0]; q->x = p[1]; q->next = ip + 3; } break;

    case LOAD: case STORE:
        if (rem >= 4 && p[1] < REG_COUNT && p[2] < REG_COUNT) {
            uint8_t t = p[0];
            int ld = (op == LOAD);
            if (t == T_INT || t == T_UINT || t == T_FLOAT || t == T_ADDRESS) k = ld ? QK_LD32 : QK_ST32;
            else if (t == T_BYTE) k = ld ? QK_LD8 : QK_ST8;
            else if (t == T_BOOL) k = ld ? QK_LDB : QK_STB;
            if (k != QK_SLOW) { q->x = t; q->a = p[1]; q->b = p[2]; q->next = ip + 4; }
        }
        break;

    case HALT: k = QK_HALT; break;

    case RESERVEMEM: k = QK_D_RESERVEMEM; break;
    case FREEMEM:    k = QK_D_FREEMEM; break;
    case STRLIT:     k = QK_D_STRLIT; break;   /* (the fused literal forms were tried above) */
    case STRPRINT:   k = QK_D_STRPRINT; break;
    case STRCAT:     k = QK_D_STRCAT; break;
    case STRCMP:     k = QK_D_STRCMP; break;
    case STRSLICE:   k = QK_D_STRSLICE; break;
    case STRFIND:    k = QK_D_STRFIND; break;
    case STRCHR:     k = QK_D_STRCHR; break;
    case STRFROM:    k = QK_D_STRFROM; break;
    case PRINT:      k = QK_D_PRINT; break;
    case CAST:
        k = QK_D_CAST;
        /* [CAST][reg][to_type]: the common numeric casts run inline (see Q_CAST) */
        if (rem >= 3 && p[0] < REG_COUNT && (p[1] == T_INT || p[1] == T_UINT || p[1] == T_FLOAT)) {
            k = QK_CAST; q->a = p[0]; q->x = p[1]; q->next = ip + 3;
        }
        break;
    case NEWARRAY:   k = QK_D_NEWARRAY; break;
    case ARRFREE:    k = QK_D_ARRFREE; break;
    case MEMSET:     k = QK_D_MEMSET; break;
    default: break;
    }

    return k;
}

/** @brief Out-of-line memory lookup for the inline block-cache check in MEMPTR(), which keeps the common path small. */
__attribute__((noinline))
static uint8_t* q_memptr_miss(iMemory* m, uint32_t address, uint32_t size) {
    return memory_ptr(m, address, size);
}

/**
 * @brief The quickening loop itself.
 *
 * `dec` is this nesting level's decode table (see execute_quick()). It is a `restrict` parameter on purpose: that tells
 * the compiler the records never overlap the registers, the VM state or VM memory, which is what it could prove when
 * the table was a fresh calloc() inside this function. Without it every write to a record (the decode, the per-site
 * memory cache) forces registers and state to be reloaded, and the PGO build ran about 8% slower. The promise holds
 * because a loop running nested inside this one (a host function calling back into the program) uses the table of the
 * next level, never this one.
 *
 * @param[in,out] state The VM.
 * @param[in,out] dec   The decode table: one QI per byte offset of the code, plus one for the end.
 * @return 1 for a clean run, 0 for a failed one (like finish_run()).
 */
__attribute__((noinline))
ILANG_GOTO_ATTR ILANG_DISPATCH static int execute_quick_run(iState* state, QI* restrict dec) {
    /**
     * @par Local macros
     * The handlers below are written with a few macros that only exist inside this function (`NEXT()`, `JUMP()`,
     * `MEMPTR()`, the `ARITH_*` / `DIVMOD_*` families, `DIRECT_Q()`, ...). Each is described in a comment where it is defined.
     */
    static const void* const L[QK__COUNT] = {
#define X(n) &&Q_##n,
        QKINDS(X)
#undef X
    };
    const uint32_t code_size = state->code_size;
    const uint8_t* const code = state->code;
    Value* const R = state->r;
    iMemory* const mem = state->memory;
    bool halt_executed = false;
    uint32_t ip = state->ip;
    QI* q;

/* Q_OFF(kind): a handler is stored as its distance from Q_DECODE, so that a zeroed record means "not decoded yet". */
#define Q_OFF(kind) ((int32_t)((const char*)L[(kind)] - (const char*)&&Q_DECODE))

    if (dec[code_size].h == 0) {   /* a new table (see execute_quick) */
        dec[code_size].h = Q_OFF(QK_END);
        dec[code_size].next = code_size;
    }

    if (state->halt) goto done;

/* QGO(): jumps to the handler of the current record `q`. */
#define QGO() goto *(void*)((const char*)&&Q_DECODE + q->h)
/* QDISPATCH(): selects the record at `ip` and jumps to its handler. */
#define QDISPATCH() do { q = dec + ip; QGO(); } while (0)
/* NEXT(): continues with the instruction after the current one. */
#define NEXT() do { ip = q->next; QDISPATCH(); } while (0)
/* JUMP(t): continues at byte offset `t` (ends the run if it is past the end of the code). */
#define JUMP(t) do { ip = (t); if (ip > code_size) goto done; QDISPATCH(); } while (0)
/* MEMPTR(dst, addr, sz): Host pointer for a VM-memory range: the global one-entry block cache, then this site's
   cache, then a real lookup; NULL if the range is not inside one live block. */
#define MEMPTR(dst, addr, sz) do { \
        const iMemoryBlock* blk_; uint32_t off_; \
        if (mem->last_block < mem->block_count && (blk_ = &mem->blocks[mem->last_block], off_ = (addr) - blk_->address, \
                off_ < blk_->size && (sz) <= blk_->size - off_)) (dst) = blk_->data + off_; \
        else if (q->ic < mem->block_count && (blk_ = &mem->blocks[q->ic], off_ = (addr) - blk_->address, \
                off_ < blk_->size && (sz) <= blk_->size - off_)) { mem->last_block = q->ic; (dst) = blk_->data + off_; } \
        else { (dst) = q_memptr_miss(mem, (addr), (sz)); q->ic = mem->last_block; } \
    } while (0)

    if (ip >= code_size) goto done;
    QDISPATCH();

/* first visit to this offset: decode the record, remember its handler, then run it */
Q_DECODE:
    q->h = Q_OFF(q_decode(state, q, ip));
    QGO();

/* run the original implementation of the instruction on the raw bytes (nothing has been modified yet) */
Q_SLOW:
q_slow:
    state->ip = ip + 1;
    slow_op(state, (OpCode)code[ip]);
    ip = state->ip;
    if (state->halt) goto done;
    JUMP(ip);

Q_END:
    goto done;

Q_HALT:
    state->halt = true;
    halt_executed = true;
    ip++;
    goto done;

Q_MOV_RR: R[q->a] = R[q->b]; NEXT();
Q_MOV_RI: R[q->a] = (Value){ .tag = (Types)q->x, .data.u32 = q->imm }; NEXT();

/* ARITH_I(NAME, EXPR): Defines the `_RR` and `_RI` handlers of an int operation (wrapping, result tagged T_INT). */
#define ARITH_I(NAME, EXPR) \
    Q_##NAME##_RR: { uint32_t x_ = R[q->b].data.u32, y_ = R[q->c].data.u32; R[q->a] = (Value){ .tag = T_INT, .data.u32 = (EXPR) }; NEXT(); } \
    Q_##NAME##_RI: { uint32_t x_ = R[q->b].data.u32, y_ = q->imm;           R[q->a] = (Value){ .tag = T_INT, .data.u32 = (EXPR) }; NEXT(); }
/* ARITH_U(NAME, EXPR): Defines the `_RR` and `_RI` handlers of an unsigned / bitwise operation (result keeps the
   instruction's type tag `x`). */
#define ARITH_U(NAME, EXPR) \
    Q_##NAME##_RR: { uint32_t x_ = R[q->b].data.u32, y_ = R[q->c].data.u32; R[q->a] = (Value){ .tag = (Types)q->x, .data.u32 = (EXPR) }; NEXT(); } \
    Q_##NAME##_RI: { uint32_t x_ = R[q->b].data.u32, y_ = q->imm;           R[q->a] = (Value){ .tag = (Types)q->x, .data.u32 = (EXPR) }; NEXT(); }
    ARITH_I(ADDI, x_ + y_)
    ARITH_I(SUBI, x_ - y_)
    ARITH_I(MULI, x_ * y_)
    ARITH_U(ADDU, x_ + y_)
    ARITH_U(SUBU, x_ - y_)
    ARITH_U(MULU, x_ * y_)
    ARITH_U(AND, x_ & y_)
    ARITH_U(OR, x_ | y_)
    ARITH_U(XOR, x_ ^ y_)

/* SHIFT_I(NAME, EXPR): Defines the `_RR`, `_RI` and `_IR` handlers of an int shift. Shifts follow shl_u32 / shr_u32
   / sar_i32 in IlangVM.c: a count of 32 or more shifts everything out. An int count below zero panics, so the
   register form hands it to the slow path before anything is written; the immediate form was checked when the
   instruction was decoded. */
#define SHIFT_I(NAME, EXPR) \
    Q_##NAME##_RR: { uint32_t x_ = R[q->b].data.u32, y_ = R[q->c].data.u32; if ((int32_t)y_ < 0) goto q_slow; R[q->a] = (Value){ .tag = T_INT, .data.u32 = (EXPR) }; NEXT(); } \
    Q_##NAME##_RI: { uint32_t x_ = R[q->b].data.u32, y_ = q->imm;           R[q->a] = (Value){ .tag = T_INT, .data.u32 = (EXPR) }; NEXT(); } \
    Q_##NAME##_IR: { uint32_t x_ = q->imm,           y_ = R[q->c].data.u32; if ((int32_t)y_ < 0) goto q_slow; R[q->a] = (Value){ .tag = T_INT, .data.u32 = (EXPR) }; NEXT(); }
/* SHIFT_U(NAME, EXPR): Defines the `_RR`, `_RI` and `_IR` handlers of a uint shift. */
#define SHIFT_U(NAME, EXPR) \
    Q_##NAME##_RR: { uint32_t x_ = R[q->b].data.u32, y_ = R[q->c].data.u32; R[q->a] = (Value){ .tag = (Types)q->x, .data.u32 = (EXPR) }; NEXT(); } \
    Q_##NAME##_RI: { uint32_t x_ = R[q->b].data.u32, y_ = q->imm;           R[q->a] = (Value){ .tag = (Types)q->x, .data.u32 = (EXPR) }; NEXT(); } \
    Q_##NAME##_IR: { uint32_t x_ = q->imm,           y_ = R[q->c].data.u32; R[q->a] = (Value){ .tag = (Types)q->x, .data.u32 = (EXPR) }; NEXT(); }
    SHIFT_I(SHLI, shl_u32(x_, y_))
    SHIFT_I(SHRI, (uint32_t)sar_i32((int32_t)x_, y_))
    SHIFT_U(SHLU, shl_u32(x_, y_))
    SHIFT_U(SHRU, shr_u32(x_, y_))

/* ARITH_F(NAME, EXPR): Defines the `_RR` and `_RI` handlers of a float operation. */
#define ARITH_F(NAME, EXPR) \
    Q_##NAME##_RR: { float x_ = R[q->b].data.f32, y_ = R[q->c].data.f32; R[q->a] = (Value){ .tag = T_FLOAT, .data.f32 = (EXPR) }; NEXT(); } \
    Q_##NAME##_RI: { float x_ = R[q->b].data.f32, y_; memcpy(&y_, &q->imm, 4); R[q->a] = (Value){ .tag = T_FLOAT, .data.f32 = (EXPR) }; NEXT(); }
    ARITH_F(ADDF, x_ + y_)
    ARITH_F(SUBF, x_ - y_)
    ARITH_F(MULF, x_ * y_)

    Q_DIVF_RR: { float x_ = R[q->b].data.f32, y_ = R[q->c].data.f32; if (y_ == 0.0f) goto q_slow; R[q->a] = (Value){ .tag = T_FLOAT, .data.f32 = x_ / y_ }; NEXT(); }
    Q_DIVF_RI: { float x_ = R[q->b].data.f32, y_; memcpy(&y_, &q->imm, 4); if (y_ == 0.0f) goto q_slow; R[q->a] = (Value){ .tag = T_FLOAT, .data.f32 = x_ / y_ }; NEXT(); }

/* DIVMOD_I(NAME, OPR): Defines the handlers of int division / modulo; a zero or -1 divisor goes to the slow path (it
   panics or needs the INT_MIN rule). */
#define DIVMOD_I(NAME, OPR) \
    Q_##NAME##_RR: { int32_t x_ = R[q->b].data.i32, y_ = R[q->c].data.i32; if (y_ == 0 || y_ == -1) goto q_slow; R[q->a] = (Value){ .tag = T_INT, .data.i32 = x_ OPR y_ }; NEXT(); } \
    Q_##NAME##_RI: { int32_t x_ = R[q->b].data.i32, y_ = (int32_t)q->imm;  if (y_ == 0 || y_ == -1) goto q_slow; R[q->a] = (Value){ .tag = T_INT, .data.i32 = x_ OPR y_ }; NEXT(); }
/* DIVMOD_U(NAME, OPR): Defines the handlers of unsigned division / modulo; a zero divisor goes to the slow path. */
#define DIVMOD_U(NAME, OPR) \
    Q_##NAME##_RR: { uint32_t x_ = R[q->b].data.u32, y_ = R[q->c].data.u32; if (y_ == 0) goto q_slow; R[q->a] = (Value){ .tag = (Types)q->x, .data.u32 = x_ OPR y_ }; NEXT(); } \
    Q_##NAME##_RI: { uint32_t x_ = R[q->b].data.u32, y_ = q->imm;           if (y_ == 0) goto q_slow; R[q->a] = (Value){ .tag = (Types)q->x, .data.u32 = x_ OPR y_ }; NEXT(); }
    DIVMOD_I(DIVI, /)
    DIVMOD_I(MODI, %)
    DIVMOD_U(DIVU, /)
    DIVMOD_U(MODU, %)

/* CJ_TAIL(Z, LT, CF): The tail of a fused compare + conditional jump: stores the flags, then jumps or falls through.
   The flags are still stored (`sF` = "less", `oF` = 0 gives the same six conditions as before). */
#define CJ_TAIL(Z, LT, CF) \
    { uint32_t fl_ = (uint32_t)(Z) | ((uint32_t)(LT) << 8) | ((uint32_t)(CF) << 16); \
      memcpy(&state->zF, &fl_, 4); \
      if ((q->x >> (((Z) << 1) | (LT))) & 1u) { JUMP(q->tgt); } \
      NEXT(); }
    Q_CJI_RR: { int32_t a_ = R[q->a].data.i32, b_ = R[q->b].data.i32; unsigned z_ = a_ == b_, l_ = a_ < b_; CJ_TAIL(z_, l_, (uint32_t)a_ < (uint32_t)b_) }
    Q_CJI_RI: { int32_t a_ = R[q->a].data.i32, b_ = (int32_t)q->imm;   unsigned z_ = a_ == b_, l_ = a_ < b_; CJ_TAIL(z_, l_, (uint32_t)a_ < (uint32_t)b_) }
    Q_CJU_RR: { uint32_t a_ = R[q->a].data.u32, b_ = R[q->b].data.u32; unsigned z_ = a_ == b_, l_ = a_ < b_; CJ_TAIL(z_, l_, l_) }
    Q_CJU_RI: { uint32_t a_ = R[q->a].data.u32, b_ = q->imm;           unsigned z_ = a_ == b_, l_ = a_ < b_; CJ_TAIL(z_, l_, l_) }
    Q_CJB_RR: { uint8_t a_ = R[q->a].data.u8, b_ = R[q->b].data.u8;    unsigned z_ = a_ == b_, l_ = a_ < b_; CJ_TAIL(z_, l_, l_) }
    Q_CJB_RI: { uint8_t a_ = R[q->a].data.u8, b_ = (uint8_t)q->imm;    unsigned z_ = a_ == b_, l_ = a_ < b_; CJ_TAIL(z_, l_, l_) }
    Q_CJF_RR: { float a_ = R[q->a].data.f32, b_ = R[q->b].data.f32; if (a_ != a_ || b_ != b_) goto q_slow; unsigned z_ = a_ == b_, l_ = a_ < b_; CJ_TAIL(z_, l_, l_) }
    Q_CJF_RI: { float a_ = R[q->a].data.f32, b_; memcpy(&b_, &q->imm, 4); if (a_ != a_ || b_ != b_) goto q_slow; unsigned z_ = a_ == b_, l_ = a_ < b_; CJ_TAIL(z_, l_, l_) }

    Q_JMP: JUMP(q->tgt);
    Q_JE:  if (state->zF) JUMP(q->tgt); NEXT();
    Q_JNE: if (!state->zF) JUMP(q->tgt); NEXT();
    Q_JGT: if (!state->zF && state->sF == state->oF) JUMP(q->tgt); NEXT();
    Q_JLT: if (state->sF != state->oF) JUMP(q->tgt); NEXT();
    Q_JGE: if (state->sF == state->oF) JUMP(q->tgt); NEXT();
    Q_JLE: if (state->zF || state->sF != state->oF) JUMP(q->tgt); NEXT();

    Q_CALL:
        if ((size_t)(state->callTop - state->callStack) + 2 > MAX_CALL_DEPTH) goto q_slow;
        *state->callTop++ = q->next;
        *state->callTop++ = (uint32_t)(state->bp - state->stack);
        state->bp = state->top;
        JUMP(q->tgt);
    Q_RET:
        if ((size_t)(state->callTop - state->callStack) < 2) goto q_slow;
        state->bp = state->stack + *--state->callTop;
        ip = *--state->callTop;
        if (ip > code_size) goto done;
        QDISPATCH();
    Q_PUSH:
        if (state->top >= state->stack + MAX_STACK_SIZE) goto q_slow;
        *state->top++ = R[q->a];
        NEXT();
    Q_POP:
        if (state->top <= state->stack) goto q_slow;
        R[q->a] = *--state->top;
        NEXT();
    Q_ENTER: {
        size_t used = (size_t)(state->top - state->stack);
        uint32_t n = q->imm;
        if (used > MAX_STACK_SIZE || (size_t)n > MAX_STACK_SIZE - used) goto q_slow;
        for (uint32_t i = 0; i < n; i++) *state->top++ = (Value){ .tag = T_INT, .data.i32 = 0 };
        NEXT();
    }
    Q_LEAVE:
        if ((size_t)q->imm > (size_t)(state->top - state->stack)) goto q_slow;
        state->top -= q->imm;
        NEXT();
    Q_LOADLOCAL: {
        uint32_t index = R[q->a].data.u32;
        if ((size_t)index >= (size_t)(state->top - state->stack)) goto q_slow;
        R[q->b] = state->stack[index];
        NEXT();
    }
    Q_STORELOCAL_R: {
        uint32_t index = R[q->a].data.u32;
        if ((size_t)index >= (size_t)(state->top - state->stack)) goto q_slow;
        state->stack[index] = R[q->b];
        NEXT();
    }
    Q_STORELOCAL_I: {
        uint32_t index = R[q->a].data.u32;
        if ((size_t)index >= (size_t)(state->top - state->stack)) goto q_slow;
        state->stack[index] = (Value){ .tag = (Types)q->x, .data.u32 = q->imm };
        NEXT();
    }
    Q_LEA: {
        ptrdiff_t base = state->bp - state->stack, off = (ptrdiff_t)q->imm;
        ptrdiff_t index = (q->x == 0) ? base + off : base - (off + 1);
        R[q->a] = (Value){ .tag = T_ADDRESS, .data.u32 = (uint32_t)index };
        NEXT();
    }
    Q_REINTERPRET: R[q->a].tag = (Types)q->x; NEXT();

    Q_LD32: {
        Value av = R[q->a]; uint8_t* p; uint32_t v;
        if (!is_address(av)) goto q_slow;
        MEMPTR(p, av.data.u32, 4);
        if (p == NULL) goto q_slow;
        memcpy(&v, p, 4);
        R[q->b] = (Value){ .tag = (Types)q->x, .data.u32 = v };
        NEXT();
    }
    Q_LD8: {
        Value av = R[q->a]; uint8_t* p;
        if (!is_address(av)) goto q_slow;
        MEMPTR(p, av.data.u32, 1);
        if (p == NULL) goto q_slow;
        R[q->b] = (Value){ .tag = T_BYTE, .data.u32 = *p };
        NEXT();
    }
    Q_LDB: {
        Value av = R[q->a]; uint8_t* p;
        if (!is_address(av)) goto q_slow;
        MEMPTR(p, av.data.u32, 1);
        if (p == NULL) goto q_slow;
        R[q->b] = (Value){ .tag = T_BOOL, .data.u32 = (*p != 0) };
        NEXT();
    }
    Q_ST32: {
        Value av = R[q->a], v = R[q->b]; uint8_t* p;
        if (!is_address(av) || (uint8_t)v.tag != q->x) goto q_slow;
        MEMPTR(p, av.data.u32, 4);
        if (p == NULL) goto q_slow;
        memcpy(p, &v.data, 4);
        NEXT();
    }
    Q_ST8: {
        Value av = R[q->a], v = R[q->b]; uint8_t* p;
        if (!is_address(av) || (uint8_t)v.tag != q->x) goto q_slow;
        MEMPTR(p, av.data.u32, 1);
        if (p == NULL) goto q_slow;
        *p = v.data.u8;
        NEXT();
    }
    Q_STB: {
        Value av = R[q->a], v = R[q->b]; uint8_t* p;
        if (!is_address(av) || (uint8_t)v.tag != q->x) goto q_slow;
        MEMPTR(p, av.data.u32, 1);
        if (p == NULL) goto q_slow;
        *p = (v.data.u8 != 0);
        NEXT();
    }

/* ELEM_LOCATE(SZ): Locates element `idx` of the array whose header is at `base`. Declares `ep_` (host pointer) and
   `addr_` (VM address); goes to the slow path unless the whole element is inside the array's block and `idx <
   length`. */
#define ELEM_LOCATE(SZ) \
        Value bv_ = R[q->a]; const iMemoryBlock* b_; uint32_t ba_, boff_, len_, i_, addr_; uint64_t eo_; uint8_t* ep_; \
        if (!is_address(bv_)) goto q_slow; \
        ba_ = bv_.data.u32; \
        if (!(mem->last_block < mem->block_count && ((b_ = &mem->blocks[mem->last_block]), (ba_ - b_->address) < b_->size))) { \
            if (q->ic < mem->block_count && ((b_ = &mem->blocks[q->ic]), (ba_ - b_->address) < b_->size)) { mem->last_block = q->ic; } \
            else { \
                (void)q_memptr_miss(mem, ba_, 4); \
                if (!(mem->last_block < mem->block_count)) goto q_slow; \
                b_ = &mem->blocks[mem->last_block]; \
                if ((ba_ - b_->address) >= b_->size) goto q_slow; \
                q->ic = mem->last_block; \
            } \
        } \
        boff_ = ba_ - b_->address; \
        if (b_->size - boff_ < 4) goto q_slow; \
        memcpy(&len_, b_->data + boff_, 4); \
        i_ = R[q->b].data.u32; \
        if (i_ >= len_) goto q_slow; \
        eo_ = (uint64_t)boff_ + 4u + (uint64_t)i_ * (SZ); \
        if (eo_ + (SZ) > b_->size) goto q_slow; \
        ep_ = b_->data + eo_; \
        addr_ = ba_ + 4u + i_ * (SZ);

/* ELEM_FLAGS(): Sets the flags the bounds check (CMP + JGE not taken) would have left. */
#define ELEM_FLAGS() do { const uint32_t fl_ = 0x00010100u; memcpy(&state->zF, &fl_, 4); } while (0)
/* ELEM_TEMP(): Leaves the address in the temp register, as the original sequence would (unless it is the
   destination). */
#define ELEM_TEMP() do { if (q->c != q->imm) R[q->c] = (Value){ .tag = T_ADDRESS, .data.u32 = addr_ }; } while (0)

    Q_LDX32: { ELEM_LOCATE(4u) uint32_t v; memcpy(&v, ep_, 4); ELEM_FLAGS(); ELEM_TEMP(); R[q->imm] = (Value){ .tag = (Types)q->x, .data.u32 = v }; NEXT(); }
    Q_LDX8:  { ELEM_LOCATE(1u) ELEM_FLAGS(); ELEM_TEMP(); R[q->imm] = (Value){ .tag = T_BYTE, .data.u32 = *ep_ }; NEXT(); }
    Q_LDXB:  { ELEM_LOCATE(1u) ELEM_FLAGS(); ELEM_TEMP(); R[q->imm] = (Value){ .tag = T_BOOL, .data.u32 = (*ep_ != 0) }; NEXT(); }
    Q_STX32: { Value v = R[q->imm]; if ((uint8_t)v.tag != q->x) goto q_slow; ELEM_LOCATE(4u) memcpy(ep_, &v.data, 4); ELEM_FLAGS(); R[q->c] = (Value){ .tag = T_ADDRESS, .data.u32 = addr_ }; NEXT(); }
    Q_STX8:  { Value v = R[q->imm]; if ((uint8_t)v.tag != q->x) goto q_slow; ELEM_LOCATE(1u) *ep_ = v.data.u8; ELEM_FLAGS(); R[q->c] = (Value){ .tag = T_ADDRESS, .data.u32 = addr_ }; NEXT(); }
    Q_STXB:  { Value v = R[q->imm]; if ((uint8_t)v.tag != q->x) goto q_slow; ELEM_LOCATE(1u) *ep_ = (v.data.u8 != 0); ELEM_FLAGS(); R[q->c] = (Value){ .tag = T_ADDRESS, .data.u32 = addr_ }; NEXT(); }

/* UADDR(SZ): Unchecked ('unsafe') element access: address arithmetic + load/store in one op. */
#define UADDR(SZ) \
        Value bv_ = R[q->a]; uint32_t addr_; uint8_t* ep_; \
        if (!is_address(bv_)) goto q_slow; \
        addr_ = bv_.data.u32 + 4u + R[q->b].data.u32 * (SZ); \
        MEMPTR(ep_, addr_, (SZ)); \
        if (ep_ == NULL) goto q_slow;

    Q_LDU32: { UADDR(4u) uint32_t v; memcpy(&v, ep_, 4); ELEM_TEMP(); R[q->imm] = (Value){ .tag = (Types)q->x, .data.u32 = v }; NEXT(); }
    Q_LDU8:  { UADDR(1u) ELEM_TEMP(); R[q->imm] = (Value){ .tag = T_BYTE, .data.u32 = *ep_ }; NEXT(); }
    Q_LDUB:  { UADDR(1u) ELEM_TEMP(); R[q->imm] = (Value){ .tag = T_BOOL, .data.u32 = (*ep_ != 0) }; NEXT(); }
    Q_STU32: { Value v = R[q->imm]; if ((uint8_t)v.tag != q->x) goto q_slow; UADDR(4u) memcpy(ep_, &v.data, 4); R[q->c] = (Value){ .tag = T_ADDRESS, .data.u32 = addr_ }; NEXT(); }
    Q_STU8:  { Value v = R[q->imm]; if ((uint8_t)v.tag != q->x) goto q_slow; UADDR(1u) *ep_ = v.data.u8; R[q->c] = (Value){ .tag = T_ADDRESS, .data.u32 = addr_ }; NEXT(); }
    Q_STUB:  { Value v = R[q->imm]; if ((uint8_t)v.tag != q->x) goto q_slow; UADDR(1u) *ep_ = (v.data.u8 != 0); R[q->c] = (Value){ .tag = T_ADDRESS, .data.u32 = addr_ }; NEXT(); }

/* ---- string literal used once: STRLIT t; use; FREEMEM t ----
 * No block is built. What the three instructions would leave behind is reproduced: t holds the (now dead) literal address,
 * next_address has moved past it, the output / result is the same. Everything that could fail is checked before the first
 * change, so a failure runs the original STRLIT instead (and the original error, if any, follows from the real sequence). */
    Q_LIT_PRINT: {
        uint32_t a_ = memory_phantom_acquire(mem, 4u + q->tgt);
        if (a_ == 0) goto q_slow;
        memory_phantom_release(mem, 4u + q->tgt);
        if (q->tgt != 0) ilang_fwrite(code + q->imm, 1, q->tgt, stdout);
        R[q->a] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = a_ };
        NEXT();
    }
    Q_LIT_CMP: {
        const uint8_t* dx; uint32_t lx, a_; int c_;
        if (str_resolve(mem, R[q->c], &dx, &lx) != STR_OK) goto q_slow;
        a_ = memory_phantom_acquire(mem, 4u + q->tgt);
        if (a_ == 0) goto q_slow;
        memory_phantom_release(mem, 4u + q->tgt);
        c_ = (q->x & 1) ? str_compare(code + q->imm, q->tgt, dx, lx) : str_compare(dx, lx, code + q->imm, q->tgt);
        R[q->a] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = a_ };
        R[q->b] = (Value){ .tag = T_INT, .data.i32 = c_ };
        NEXT();
    }
    Q_LIT_FIND: {
        const uint8_t* dx; uint32_t lx, a_; int32_t f_;
        if (str_resolve(mem, R[q->c], &dx, &lx) != STR_OK) goto q_slow;
        a_ = memory_phantom_acquire(mem, 4u + q->tgt);
        if (a_ == 0) goto q_slow;
        memory_phantom_release(mem, 4u + q->tgt);
        f_ = (q->x & 1) ? str_find(code + q->imm, q->tgt, dx, lx) : str_find(dx, lx, code + q->imm, q->tgt);
        R[q->a] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = a_ };
        R[q->b] = (Value){ .tag = T_INT, .data.i32 = f_ };
        NEXT();
    }
    Q_LIT_CAT: {
        const uint8_t* dx; uint32_t lx, a_, res_, total_, n_ = q->tgt, next_before = mem->next_address;
        uint8_t* out_;
        /* the result block must be a plain append too (the real sequence has the literal's block in the table while
           the result is placed); far from the end of the 32-bit address space that is always so */
        if (str_resolve(mem, R[q->c], &dx, &lx) != STR_OK || lx > UINT32_MAX - 4u - n_ || next_before >= 0x80000000u) goto q_slow;
        a_ = memory_phantom_acquire(mem, 4u + n_);
        if (a_ == 0) goto q_slow;
        total_ = lx + n_;
        res_ = memory_reserve(mem, 4u + total_);
        memory_phantom_release(mem, 4u + n_);
        if (res_ == 0) { mem->next_address = next_before; goto q_slow; }
        out_ = memory_ptr(mem, res_, 4u + total_);
        memcpy(out_, &total_, 4);
        if (q->x & 1) { memcpy(out_ + 4, code + q->imm, n_); if (lx != 0) memcpy(out_ + 4 + n_, dx, lx); }
        else          { if (lx != 0) memcpy(out_ + 4, dx, lx); memcpy(out_ + 4 + lx, code + q->imm, n_); }
        R[q->a] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = a_ };
        R[q->b] = (Value){ .tag = T_STRINGADDRESS, .data.u32 = res_ };
        NEXT();
    }

    Q_CAST: {
        /* inline version of op_cast for targets int / uint / float and the usual sources; anything else (bool, byte,
         * address, bad tags) takes the original path, which also reports every error */
        Value v_ = R[q->a];
        Value r_;
        int64_t i_;
        switch (v_.tag) {
        case T_INT:   i_ = v_.data.i32; break;
        case T_UINT:  i_ = v_.data.u32; break;
        case T_BYTE:  i_ = v_.data.u8; break;
        case T_FLOAT:
            if (q->x == T_FLOAT) { NEXT(); }
            if (q->x == T_INT)  { r_ = (Value){ .tag = T_INT,  .data.i32 = qc_f2i(v_.data.f32) }; }
            else                { r_ = (Value){ .tag = T_UINT, .data.u32 = qc_f2u(v_.data.f32) }; }
            R[q->a] = r_; NEXT();
        default: goto Q_D_CAST;
        }
        if (q->x == T_FLOAT)     r_ = (Value){ .tag = T_FLOAT, .data.f32 = (float)i_ };
        else if (q->x == T_INT)  r_ = (Value){ .tag = T_INT,   .data.i32 = (int32_t)(uint32_t)(uint64_t)i_ };
        else                     r_ = (Value){ .tag = T_UINT,  .data.u32 = (uint32_t)(uint64_t)i_ };
        R[q->a] = r_;
        NEXT();
    }

/* DIRECT_Q(NAME, CALL_): Defines the handler of an instruction that simply calls the original `op_xxx` (with `ip`
   handed to the VM state and taken back). */
#define DIRECT_Q(NAME, CALL_) \
    Q_D_##NAME: state->ip = ip + 1; CALL_; ip = state->ip; if (state->halt) goto done; JUMP(ip);
    DIRECT_Q(RESERVEMEM, op_reservemem(state))
    DIRECT_Q(FREEMEM, op_freemem(state))
    DIRECT_Q(STRLIT, op_strlit(state))
    DIRECT_Q(STRPRINT, op_strprint(state))
    DIRECT_Q(STRCAT, op_strcat(state))
    DIRECT_Q(STRCMP, op_strcmp(state))
    DIRECT_Q(STRSLICE, op_strslice(state))
    DIRECT_Q(STRFIND, op_strfind(state))
    DIRECT_Q(STRCHR, op_strchr(state))
    DIRECT_Q(STRFROM, op_strfrom(state))
    DIRECT_Q(PRINT, op_print(state))
    DIRECT_Q(CAST, op_cast(state))
    DIRECT_Q(NEWARRAY, op_newarray(state))
    DIRECT_Q(ARRFREE, op_arrfree(state))
    DIRECT_Q(MEMSET, op_memset(state))

done:
    state->ip = ip;
    return finish_run(state, halt_executed) ? 1 : 0;
}

/**
 * @brief Runs the program with the quickening loop.
 *
 * The table is calloc'ed so that every record starts as "not decoded" (`h == 0`) and pages of code that never run are
 * never touched. Each table lives as long as the program (freed by vm_teardown()), so a later run at the same nesting
 * level - the next call of a callback, the next IlangVM_call() - finds everything already decoded.
 *
 * @param[in,out] state The VM.
 * @return 1 / 0 for a clean / failed run, like finish_run(), or -1 if the decode table could not be allocated (nothing
 *         has run yet, and the caller uses another loop).
 */
static int execute_quick(iState* state) {
    void** table = &state->quick_dec[state->host_depth];

    if (*table == NULL) {
        *table = calloc((size_t)state->code_size + 1, sizeof(QI));
        if (*table == NULL) {
            return -1;
        }
    }
    return execute_quick_run(state, (QI*)*table);
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#undef Q_OFF
#undef QGO
#undef QDISPATCH
#undef NEXT
#undef JUMP
#undef MEMPTR
