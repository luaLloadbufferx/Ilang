/**
 * @file arrows.c
 * @brief The arrow margin of the listing: jump arrows (single lines) and call arrows (double lines).
 *
 * @code{.unparsed}
 *   ┌──►  main:
 *   │         CMP.int   r0, 2
 *   │ ┌──►    JLT       .L1
 *   │ │       ...
 *   │ └───    JMP       .L1
 *   ╚═════    CALL      fib
 * @endcode
 *
 * Layout. The margin is a row of lane columns, followed by one column for the arrow heads. Lane 0 is the one next to the
 * text. A branch is drawn as a vertical line in its lane that runs from its first to its last row; a horizontal stub
 * connects each source and the target to the text, and the target ends in an arrow head. All branches to the same
 * place form one arrow with several sources, so a loop head with three `continue`s, or a function called from ten
 * places, takes one lane.
 *
 * Lane assignment. Arrows are placed shortest first, each in the lowest lane where it overlaps nothing. That keeps
 * short arrows next to the text and the long ones outside, which is what makes a nested structure read as nested.
 */
#include "output.h"

#include <stdlib.h>
#include <string.h>

/** @brief One arrow: every branch to one target row. */
typedef struct Group {
    uint32_t lo;      /**< First row the arrow touches. */
    uint32_t hi;      /**< Last row the arrow touches. */
    int lane;         /**< Its lane, or -1 if there was no room for it. */
    bool call;        /**< A call arrow (double line) rather than a jump arrow. */
} Group;

/** @brief The branches that share one lane, ordered by their rows (they never overlap). */
typedef struct Lane {
    int32_t* groups;  /**< Indices into DisArrows::groups, by first row. */
    size_t count;     /**< Number of arrows in the lane. */
    size_t capacity;  /**< Allocated entries. */
} Lane;

/** @brief See DisArrows in output.h. */
struct DisArrows {
    size_t rows;        /**< Number of rows. */
    int lane_count;     /**< Number of lanes in use. */
    int32_t* out_group; /**< Per row: the arrow its own branch belongs to, or -1. */
    int32_t* in_group;  /**< Per row: the arrow that ends here, or -1. */
    Group* groups;      /**< All arrows. */
    size_t group_count; /**< Number of arrows. */
    Lane* lanes;        /**< The lanes (index 0 is the one next to the text). */
    size_t omitted;     /**< Arrows that got no lane. */
};

/** @brief Glyphs of single-line arrows, indexed by which sides of the cell have a line: up 1, down 2, left 4, right 8. */
static const char* const kSingle[16] = {
    " ", "\xE2\x94\x82", "\xE2\x94\x82", "\xE2\x94\x82", "\xE2\x94\x80", "\xE2\x94\x98", "\xE2\x94\x90", "\xE2\x94\xA4",
    "\xE2\x94\x80", "\xE2\x94\x94", "\xE2\x94\x8C", "\xE2\x94\x9C", "\xE2\x94\x80", "\xE2\x94\xB4", "\xE2\x94\xAC", "\xE2\x94\xBC"
};
/** @brief Glyphs of double-line arrows (same indexing). */
static const char* const kDouble[16] = {
    " ", "\xE2\x95\x91", "\xE2\x95\x91", "\xE2\x95\x91", "\xE2\x95\x90", "\xE2\x95\x9D", "\xE2\x95\x97", "\xE2\x95\xA3",
    "\xE2\x95\x90", "\xE2\x95\x9A", "\xE2\x95\x94", "\xE2\x95\xA0", "\xE2\x95\x90", "\xE2\x95\xA9", "\xE2\x95\xA6", "\xE2\x95\xAC"
};
/** @brief Plain ASCII replacements for single lines. */
static const char* const kAsciiSingle[16] = { " ", "|", "|", "|", "-", "+", "+", "+", "-", "+", "+", "+", "-", "+", "+", "+" };
/** @brief Plain ASCII replacements for double lines. */
static const char* const kAsciiDouble[16] = { " ", "|", "|", "|", "=", "+", "+", "+", "=", "+", "+", "+", "=", "+", "+", "+" };

/** @brief Colours of jump arrows, cycled by target. */
static const char* const kJumpColors[] = { "33", "36", "35", "32", "34", "31" };
/** @brief Colours of call arrows, cycled by target. */
static const char* const kCallColors[] = { "93", "96", "95", "92", "94", "91" };

/** @brief The arrows being sorted by by_length(): qsort_r() is not portable, so the comparator reads them from here. */
static const struct DisArrows* g_sort_arrows;

/**
 * @brief qsort() comparator: shorter arrows first, then the one that starts earlier.
 * @param pa Pointer to a group index.
 * @param pb Pointer to a group index.
 */
static int by_length(const void* pa, const void* pb) {
    const Group* a = &g_sort_arrows->groups[*(const int32_t*)pa];
    const Group* b = &g_sort_arrows->groups[*(const int32_t*)pb];
    uint32_t la = a->hi - a->lo, lb = b->hi - b->lo;

    if (la != lb) return la < lb ? -1 : 1;
    if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
    return (*(const int32_t*)pa < *(const int32_t*)pb) ? -1 : 1;
}

/**
 * @brief Tries to put an arrow into a lane.
 * @param a    The arrows.
 * @param lane The lane.
 * @param g    Index of the arrow.
 * @return true if it fitted (and was added).
 */
static bool lane_place(struct DisArrows* a, Lane* lane, int32_t g) {
    const Group* grp = &a->groups[g];
    size_t lo = 0, hi = lane->count;

    while (lo < hi) {  /* the first arrow that starts after this one does */
        size_t mid = lo + (hi - lo) / 2;

        if (a->groups[lane->groups[mid]].lo <= grp->lo) lo = mid + 1;
        else hi = mid;
    }
    if (lo > 0 && a->groups[lane->groups[lo - 1]].hi >= grp->lo) return false;
    if (lo < lane->count && a->groups[lane->groups[lo]].lo <= grp->hi) return false;

    dis_reserve(&lane->groups, &lane->capacity, sizeof *lane->groups, lane->count + 1);
    memmove(lane->groups + lo + 1, lane->groups + lo, (lane->count - lo) * sizeof *lane->groups);
    lane->groups[lo] = g;
    lane->count++;
    return true;
}

/**
 * @brief The arrow that covers a row in a lane.
 * @param a    The arrows.
 * @param lane The lane.
 * @param row  The row.
 * @param gap  false: the arrow must include the row; true: it must continue past it (cover the space below the row).
 * @return The arrow index, or -1.
 */
static int32_t lane_at(const struct DisArrows* a, const Lane* lane, size_t row, bool gap) {
    size_t lo = 0, hi = lane->count;

    while (lo < hi) {  /* the first arrow that starts after the row */
        size_t mid = lo + (hi - lo) / 2;

        if (a->groups[lane->groups[mid]].lo <= row) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return -1;
    return a->groups[lane->groups[lo - 1]].hi >= row + (gap ? 1u : 0u) ? lane->groups[lo - 1] : -1;
}

DisArrows* dis_arrows_build(const DisProgram* prog, const uint32_t* rows, size_t row_count, DisArrowMode mode, int max_lanes) {
    struct DisArrows* a = (struct DisArrows*)dis_xcalloc(1, sizeof *a);
    int32_t* row_of = NULL;
    int32_t* jump_at = NULL;   /* per row: the jump arrow that ends here */
    int32_t* call_at = NULL;   /* per row: the call arrow that ends here */
    int32_t* order;
    size_t r, capacity = 0, g;

    if (max_lanes > 64) max_lanes = 64;  /* dis_arrows_print() keeps its per-lane state in fixed arrays */
    a->rows = row_count;
    a->out_group = (int32_t*)dis_xmalloc(row_count * sizeof *a->out_group);
    a->in_group = (int32_t*)dis_xmalloc(row_count * sizeof *a->in_group);
    for (r = 0; r < row_count; r++) a->out_group[r] = a->in_group[r] = -1;
    if (mode == DIS_ARROWS_NONE || max_lanes <= 0 || row_count == 0) return a;

    row_of = (int32_t*)dis_xmalloc(prog->insn_count * sizeof *row_of);
    memset(row_of, 0xFF, prog->insn_count * sizeof *row_of);
    for (r = 0; r < row_count; r++) row_of[rows[r]] = (int32_t)r;
    jump_at = (int32_t*)dis_xmalloc(row_count * sizeof *jump_at);
    call_at = (int32_t*)dis_xmalloc(row_count * sizeof *call_at);
    for (r = 0; r < row_count; r++) jump_at[r] = call_at[r] = -1;

    /* one arrow per target and kind; every branch with a visible target joins it */
    for (r = 0; r < row_count; r++) {
        const DisInsn* in = &prog->insns[rows[r]];
        int32_t t, *slot;
        Group* grp;
        bool call;

        if ((in->op != CALL && !dis_is_jump(in->op)) || in->target_idx < 0) continue;
        t = row_of[in->target_idx];
        if (t < 0) continue;
        call = in->op == CALL || (in->flags & DIS_F_TAIL_CALL) != 0;
        if (call ? mode == DIS_ARROWS_JUMPS : mode == DIS_ARROWS_CALLS) continue;

        slot = call ? &call_at[t] : &jump_at[t];
        if (*slot < 0) {
            dis_reserve(&a->groups, &capacity, sizeof *a->groups, a->group_count + 1);
            *slot = (int32_t)a->group_count++;
            grp = &a->groups[*slot];
            grp->lo = grp->hi = (uint32_t)t;
            grp->lane = -1;
            grp->call = call;
            if (a->in_group[t] < 0) a->in_group[t] = *slot;
        }
        grp = &a->groups[*slot];
        if (r < grp->lo) grp->lo = (uint32_t)r;
        if (r > grp->hi) grp->hi = (uint32_t)r;
        a->out_group[r] = *slot;
    }
    free(row_of);
    free(jump_at);
    free(call_at);

    /* lanes: shortest arrows first, each in the lowest lane with room */
    a->lanes = (Lane*)dis_xcalloc((size_t)max_lanes, sizeof *a->lanes);
    order = (int32_t*)dis_xmalloc((a->group_count + 1) * sizeof *order);
    for (g = 0; g < a->group_count; g++) order[g] = (int32_t)g;
    g_sort_arrows = a;
    qsort(order, a->group_count, sizeof *order, by_length);
    for (g = 0; g < a->group_count; g++) {
        int lane;

        for (lane = 0; lane < max_lanes; lane++) {
            if (lane_place(a, &a->lanes[lane], order[g])) {
                a->groups[order[g]].lane = lane;
                if (lane + 1 > a->lane_count) a->lane_count = lane + 1;
                break;
            }
        }
        if (a->groups[order[g]].lane < 0) a->omitted++;
    }
    free(order);
    return a;
}

void dis_arrows_free(DisArrows* a) {
    int lane;

    if (!a) return;
    if (a->lanes) {
        /* the lane array was allocated for max_lanes entries, of which only lane_count can have been touched */
        for (lane = 0; lane < a->lane_count; lane++) free(a->lanes[lane].groups);
    }
    free(a->lanes);
    free(a->groups);
    free(a->out_group);
    free(a->in_group);
    free(a);
}

int dis_arrows_width(const DisArrows* a) {
    return a->lane_count > 0 ? a->lane_count + 1 : 0;
}

size_t dis_arrows_omitted(const DisArrows* a) {
    return a->omitted;
}

void dis_arrows_print(DisPrinter* p, const DisArrows* a, size_t row, DisGutterLine line) {
    int cell, lane, top = -1;
    int32_t at[64], head_group = -1;
    unsigned dirs[64];
    bool stub[64];
    const char* const* single = p->unicode ? kSingle : kAsciiSingle;
    const char* const* dbl = p->unicode ? kDouble : kAsciiDouble;

    if (a->lane_count == 0) return;
    if (line == DIS_GUTTER_ABOVE && row == 0) {  /* nothing above the first row */
        pr_spaces(p, dis_arrows_width(a));
        return;
    }

    /* what each lane holds at this line */
    for (lane = 0; lane < a->lane_count && lane < 64; lane++) {
        const Group* g;

        stub[lane] = false;
        dirs[lane] = 0;
        if (line == DIS_GUTTER_ROW) at[lane] = lane_at(a, &a->lanes[lane], row, false);
        else at[lane] = lane_at(a, &a->lanes[lane], line == DIS_GUTTER_BELOW ? row : row - 1, true);
        if (at[lane] < 0) continue;
        g = &a->groups[at[lane]];
        if (line == DIS_GUTTER_ROW) {
            if (g->lo < row) dirs[lane] |= 1;
            if (g->hi > row) dirs[lane] |= 2;
            stub[lane] = a->out_group[row] == at[lane] || a->in_group[row] == at[lane];
            if (stub[lane] && lane > top) top = lane;
        }
        else {
            dirs[lane] = 3;
        }
    }
    if (top >= 0 && a->in_group[row] >= 0 && a->groups[a->in_group[row]].lane >= 0) head_group = a->in_group[row];

    for (cell = 0; cell < a->lane_count; cell++) {
        const char* glyph;
        const Group* g;
        int src;
        unsigned d;

        lane = a->lane_count - 1 - cell;
        d = dirs[lane];
        if (top >= 0 && top >= lane) d |= 8;  /* the stub of this lane or of an outer one runs through */
        if (top >= 0 && top > lane) d |= 4;
        if (d == 0) {
            pr_spaces(p, 1);
            continue;
        }
        src = at[lane] >= 0 ? at[lane] : (top >= 0 ? at[top] : -1);
        g = &a->groups[src];
        glyph = g->call ? dbl[d] : single[d];
        pr_puts_sgr(p, g->call ? kCallColors[src % 6] : kJumpColors[src % 6], glyph);
    }

    /* the column of the arrow heads */
    if (top >= 0) {
        int32_t src = head_group >= 0 ? head_group : at[top];
        const Group* g = &a->groups[src];
        const char* color = g->call ? kCallColors[src % 6] : kJumpColors[src % 6];

        if (head_group >= 0) pr_puts_sgr(p, color, p->unicode ? "\xE2\x96\xBA" : ">");
        else pr_puts_sgr(p, color, g->call ? dbl[12] : single[12]);
    }
    else {
        pr_spaces(p, 1);
    }
}
