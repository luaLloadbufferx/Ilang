/**
 * @file output.h
 * @brief Internal interface of Ilangdis' output modules: the colour-aware printer, the instruction formatter and the
 *        arrow margin.
 *
 * Only listing.c uses the arrow margin directly; format.c and arrows.c are written against DisPrinter so that colours
 * and column tracking work the same everywhere.
 */
#pragma once

#include "ilangdis.h"

/** @brief The kinds of text the output distinguishes; the printer maps each to a colour. */
typedef enum DisStyle {
    DIS_ST_PLAIN,    /**< Default colour. */
    DIS_ST_ADDR,     /**< Addresses in the left column. */
    DIS_ST_BYTES,    /**< The raw bytes of the byte view. */
    DIS_ST_COMMENT,  /**< Comments and banners. */
    DIS_ST_FUNC,     /**< Function names. */
    DIS_ST_LABEL,    /**< Label names. */
    DIS_ST_EXTERN,   /**< Names of host (extern) functions. */
    DIS_ST_REG,      /**< Registers. */
    DIS_ST_IMM,      /**< Numeric immediates. */
    DIS_ST_STR,      /**< String literals. */
    DIS_ST_TARGET,   /**< Raw jump targets. */
    DIS_ST_ALU,      /**< Mnemonics: moves, arithmetic, conversions. */
    DIS_ST_MEM,      /**< Mnemonics: memory and arrays. */
    DIS_ST_CTRL,     /**< Mnemonics: jumps, calls, returns. */
    DIS_ST_IO,       /**< Mnemonics: strings and I/O. */
    DIS_ST_STACK,    /**< Mnemonics: frame and stack. */
    DIS_ST_BAD,      /**< Bytes that are not an instruction. */
    DIS_ST_BOLD      /**< Headings. */
} DisStyle;

/**
 * @brief Writes text to a stream, with colours, and knows which column it is in.
 *
 * The column counts characters, not bytes, so UTF-8 box-drawing characters do not throw the alignment off. Spaces are
 * held back until something visible follows them, so lines never end in trailing whitespace.
 */
typedef struct DisPrinter {
    FILE* out;        /**< Destination. */
    bool color;       /**< Emit ANSI colour sequences. */
    bool unicode;     /**< Box-drawing characters are allowed. */
    int col;          /**< Column the next character lands in (0 = start of a line). */
    int pending;      /**< Spaces held back; written only if visible text follows them on the line. */
    const char* sgr;  /**< The SGR parameters currently in effect, or NULL for the default. */
} DisPrinter;

/**
 * @brief Prepares a printer.
 * @param[out] p       The printer.
 * @param[in]  out     Destination stream.
 * @param[in]  options Supplies the colour and character set choices.
 */
void pr_init(DisPrinter* p, FILE* out, const DisOptions* options);
/**
 * @brief Switches to a colour given as SGR parameters (such as `"1;32"`); NULL switches back to the default.
 * @param p   The printer.
 * @param sgr The parameters.
 */
void pr_sgr(DisPrinter* p, const char* sgr);
/**
 * @brief Writes a string (without newlines) in a style.
 * @param p     The printer.
 * @param style The style.
 * @param text  The text.
 */
void pr_puts(DisPrinter* p, DisStyle style, const char* text);
/**
 * @brief Writes a string (without newlines) in a colour given as SGR parameters.
 * @param p    The printer.
 * @param sgr  The parameters (such as `"1;32"`), or NULL for the default colour.
 * @param text The text.
 */
void pr_puts_sgr(DisPrinter* p, const char* sgr, const char* text);
/**
 * @brief printf() in a style. The result is cut at 1023 bytes.
 * @param p     The printer.
 * @param style The style.
 * @param fmt   Format string.
 */
void pr_printf(DisPrinter* p, DisStyle style, const char* fmt, ...);
/**
 * @brief Writes spaces up to a column; if the printer is already past it, writes nothing.
 * @param p   The printer.
 * @param col The column to reach.
 */
void pr_pad(DisPrinter* p, int col);
/**
 * @brief Writes @p n spaces.
 * @param p The printer.
 * @param n How many.
 */
void pr_spaces(DisPrinter* p, int n);
/**
 * @brief Ends the line (and resets the colour first).
 * @param p The printer.
 */
void pr_nl(DisPrinter* p);

/**
 * @brief Prints an instruction: mnemonic, operands and (if enabled) a trailing comment.
 *
 * Starts at the current column; the operands begin 12 columns after the mnemonic and the comment at column 44 of the
 * text (or two spaces after the operands if they are longer). Jump and call targets are shown as names when
 * `options->labels` is set and as raw addresses otherwise.
 *
 * @param p       The printer.
 * @param program The analysed program.
 * @param insn    Index of the instruction.
 * @param options The output options.
 */
void dis_print_insn_text(DisPrinter* p, const DisProgram* program, size_t insn, const DisOptions* options);

/** @brief Where a line sits relative to the instruction rows, for drawing the arrow margin. */
typedef enum DisGutterLine {
    DIS_GUTTER_ROW,    /**< The line of the instruction itself: stubs and arrow heads are drawn. */
    DIS_GUTTER_ABOVE,  /**< A line between the previous instruction and this one (a label, a banner). */
    DIS_GUTTER_BELOW   /**< A line between this instruction and the next (the continuation of a byte row). */
} DisGutterLine;

/** @brief The arrows of a listing: which jump and call arrows are drawn and in which lane. */
typedef struct DisArrows DisArrows;

/**
 * @brief Works out the arrows for the rows of a listing.
 *
 * All the jumps (single lines) and calls (double lines) that go to the same place share one arrow, which is drawn
 * as a vertical line with a stub at every source and an arrow head at the target. Arrows that do not overlap share a
 * lane. Short arrows get the lanes closest to the text. When the lanes run out, the longest remaining arrows are left
 * out (see dis_arrows_omitted()).
 *
 * @param program   The analysed program.
 * @param rows      Instruction indices of the rows that will be printed, in order.
 * @param row_count Number of rows.
 * @param mode      Which arrows to draw.
 * @param max_lanes Most lanes allowed.
 * @return The arrows (release with dis_arrows_free()); never NULL.
 */
DisArrows* dis_arrows_build(const DisProgram* program, const uint32_t* rows, size_t row_count, DisArrowMode mode, int max_lanes);
/**
 * @brief Releases the arrows.
 * @param arrows The arrows (may be NULL).
 */
void dis_arrows_free(DisArrows* arrows);
/**
 * @brief Width of the margin in columns (0 if nothing is drawn). It includes the column of the arrow heads.
 * @param arrows The arrows.
 */
int dis_arrows_width(const DisArrows* arrows);
/**
 * @brief Number of arrows that did not get a lane.
 * @param arrows The arrows.
 */
size_t dis_arrows_omitted(const DisArrows* arrows);
/**
 * @brief Draws the margin for one line.
 * @param p      The printer; exactly dis_arrows_width() columns are written.
 * @param arrows The arrows.
 * @param row    The row the line belongs to.
 * @param line   Where the line sits relative to the row.
 */
void dis_arrows_print(DisPrinter* p, const DisArrows* arrows, size_t row, DisGutterLine line);
