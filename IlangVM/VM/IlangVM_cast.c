/**
 * @file IlangVM_cast.c
 * @brief Ilang casting system (implementation of IlangVM_cast.h).
 *
 * @section cast_rules Conversion rules
 *  - integer -> integer: wraps (keeps the low bits, like a C cast / wasm wrap)
 *  - float -> integer: saturates to the target range, NaN becomes 0
 *  - anything -> bool: true if non-zero
 *  - integer -> float: nearest float
 *
 * Supported sources and targets: INT, UINT, FLOAT, BOOL, BYTE, ADDRESS.
 * Not allowed: FLOAT <-> ADDRESS and BOOL -> ADDRESS (they make no sense).
 *
 * float -> integer needs explicit rules because in C (and C++) it is undefined behaviour for NaN and out-of-range
 * values.
 */

/* Required */
#include "IlangVM_cast.h"
#include "IlangVM_panic.h"

#include <stdint.h>

/*
 * Conversion rules
 * ----------------
 *   integer -> integer    wraps (keeps the low bits, like a C cast / wasm wrap)
 *   float   -> integer    saturates to the target range, NaN becomes 0
 *   anything -> bool      true if non-zero
 *   integer -> float      nearest float
 *
 * Supported sources:  INT, UINT, FLOAT, BOOL, BYTE, ADDRESS
 * Supported targets:  INT, UINT, FLOAT, BOOL, BYTE, ADDRESS
 * Not allowed:        FLOAT <-> ADDRESS, BOOL -> ADDRESS (they make no sense)
 *
 * float -> integer needs explicit rules because in C (and C++) it is
 * undefined behaviour for NaN and out-of-range values.
 */

/**
 * @brief float -> int32 with saturation; NaN becomes 0.
 * @param f The float.
 */
static int32_t float_to_i32(float f) {
    if (f != f) {                  /* NaN */
        return 0;
    }
    if (f >= 2147483648.0f) {      /* 2^31, exactly representable */
        return INT32_MAX;
    }
    if (f <= -2147483648.0f) {     /* -2^31 == INT32_MIN */
        return INT32_MIN;
    }
    return (int32_t)f;
}

/**
 * @brief float -> uint32 with saturation; NaN and negatives (and -0.0) become 0.
 * @param f The float.
 */
static uint32_t float_to_u32(float f) {
    if (f != f) {                  /* NaN */
        return 0u;
    }
    if (f <= 0.0f) {               /* negatives (and -0.0) clamp to 0 */
        return 0u;
    }
    if (f >= 4294967296.0f) {      /* 2^32, exactly representable */
        return UINT32_MAX;
    }
    return (uint32_t)f;
}

/**
 * @brief float -> uint8 with saturation; NaN and negatives become 0.
 * @param f The float.
 */
static uint8_t float_to_u8(float f) {
    if (f != f) {                  /* NaN */
        return 0u;
    }
    if (f <= 0.0f) {
        return 0u;
    }
    if (f >= 255.0f) {
        return 255u;
    }
    return (uint8_t)f;
}

/**
 * @brief Keeps the low 32 bits of a (possibly negative) 64-bit value.
 * @param i The value.
 */
static uint32_t wrap_u32(int64_t i) {
    return (uint32_t)(uint64_t)i;
}

/**
 * @brief Error message for a given target type.
 * @param target_type A T_* tag.
 * @return The message, or NULL if the target is unknown.
 */
static const char* invalid_cast_message(uint8_t target_type) {
    switch (target_type) {
    case T_INT:     return "Invalid cast to int!";
    case T_UINT:    return "Invalid cast to uint!";
    case T_FLOAT:   return "Invalid cast to float!";
    case T_BOOL:    return "Invalid cast to bool!";
    case T_BYTE:    return "Invalid cast to byte!";
    case T_ADDRESS: return "Invalid cast to address!";
    default:        return NULL;
    }
}

/**
 * @brief Reports a failed cast and returns a T_NONE value instead of a half-valid one.
 * @param[in,out] state   The VM.
 * @param[in]     message The panic message.
 */
static Value cast_fail(iState* state, const char* message) {
    Value none = { .tag = T_NONE };

    IlangVM_panic(state, message);
    return none;
}

/** @details Follows the rules in @ref cast_rules; panics and returns T_NONE for an invalid combination. */
Value l_cast(iState* state, Value value, uint8_t target_type) {
    const char* invalid = invalid_cast_message(target_type);
    Value res = { .tag = (Types)target_type };
    bool is_float = false;
    int64_t i = 0;      /* integer-like sources (sign- or zero-extended) */
    float f = 0.0f;     /* float source */

    if (invalid == NULL) {
        return cast_fail(state, "Unknown target cast type!");
    }

    /* Decode the source once. */
    switch (value.tag) {
    case T_INT:
        i = value.data.i32;
        break;
    case T_UINT:
    case T_ADDRESS:
        i = value.data.u32;
        break;
    case T_BYTE:
        i = value.data.u8;
        break;
    case T_BOOL:
        i = (value.data.u8 != 0) ? 1 : 0;
        break;
    case T_FLOAT:
        is_float = true;
        f = value.data.f32;
        break;
    default:
        return cast_fail(state, invalid);
    }

    /* Combinations that make no sense. */
    if (target_type == T_ADDRESS && (is_float || value.tag == T_BOOL)) {
        return cast_fail(state, invalid);
    }
    if (target_type == T_FLOAT && value.tag == T_ADDRESS) {
        return cast_fail(state, invalid);
    }

    switch (target_type) {
    case T_INT:
        res.data.i32 = is_float ? float_to_i32(f) : (int32_t)wrap_u32(i);
        break;

    case T_UINT:
    case T_ADDRESS:
        res.data.u32 = is_float ? float_to_u32(f) : wrap_u32(i);
        break;

    case T_FLOAT:
        res.data.f32 = is_float ? f : (float)i;
        break;

    case T_BOOL:
        res.data.u8 = is_float ? (f != 0.0f) : (i != 0);
        break;

    case T_BYTE:
        res.data.u8 = is_float ? float_to_u8(f) : (uint8_t)wrap_u32(i);
        break;

    default:
        break; /* unreachable: target_type was validated above */
    }

    return res;
}