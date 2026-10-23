/**
 * @file image.c
 * @brief Reading the parts of an Ilang bytecode image that come before the code.
 *
 * The layout is the one documented at iHeader in `IlangVM.h`:
 * @code{.unparsed}
 * "iVM\0"  u32 version  u32 entry
 * u32 count, count x { u8 name_length, name, u8 ret_type, u8 ret_depth, u8 argc, argc x { u8 type, u8 depth } }
 * zero or more { char tag[4], u32 length, payload }      (up to the entry point)
 * code
 * @endcode
 * The checks follow `host_bind_imports()` and `host_read_sections()` in `IlangVM_host.c`.
 */
#include "ilangdis.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/** @brief Most entries an import table or the EXPT section may have (the VM's limit). */
#define MAX_TABLE_ENTRIES 65536u

/**
 * @brief Reads a little-endian 32-bit value.
 * @param p Pointer to four bytes.
 */
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief Records why a file was rejected.
 * @param error      Output buffer.
 * @param error_size Its size.
 * @param fmt        printf format.
 * @return Always false, so a caller can `return reject(...)`.
 */
static bool reject(char* error, size_t error_size, const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(error, error_size, fmt, ap);
    va_end(ap);
    return false;
}

/**
 * @brief Copies a name out of the image, replacing every byte that is not printable ASCII with '?'.
 *
 * Names come from the file, which may be damaged or hostile; they end up on a terminal.
 *
 * @param p      The characters.
 * @param length Their number.
 * @return A string to be released with free().
 */
static char* copy_name(const uint8_t* p, size_t length) {
    char* name = dis_xstrndup((const char*)p, length);
    size_t i;

    for (i = 0; i < length; i++) {
        if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7E) name[i] = '?';
    }
    return name;
}

/**
 * @brief Whether a (type, depth) pair can appear in a signature.
 * @param type   Type tag.
 * @param depth  Array levels.
 * @param is_ret true for a return type (void is allowed there only).
 */
static bool type_ok(uint8_t type, uint8_t depth, bool is_ret) {
    switch (type) {
    case T_NONE: return is_ret && depth == 0;
    case T_STRINGADDRESS: return depth == 0;
    case T_INT: case T_UINT: case T_FLOAT: case T_BOOL: case T_ADDRESS: case T_BYTE: return true;
    default: return false;
    }
}

/**
 * @brief Reads a return type, an argument count and the argument types from a table entry.
 *
 * @param[in]  p      Start of the signature (the return type byte).
 * @param[in]  avail  Bytes available from @p p to the end of the table.
 * @param[out] sig    Receives the signature.
 * @param[out] used   Receives the number of bytes the signature takes (when it is valid).
 * @return false if the signature is cut off or has an impossible type.
 */
static bool read_sig(const uint8_t* p, size_t avail, iHostSig* sig, size_t* used) {
    size_t i;

    memset(sig, 0, sizeof *sig);
    if (avail < 3) return false;
    sig->ret_type = p[0];
    sig->ret_depth = p[1];
    sig->argc = p[2];
    if (sig->argc > ILANG_HOST_MAX_ARGS || avail - 3 < 2u * sig->argc) return false;
    if (!type_ok(sig->ret_type, sig->ret_depth, true)) return false;
    for (i = 0; i < sig->argc; i++) {
        sig->arg_type[i] = p[3 + 2 * i];
        sig->arg_depth[i] = p[4 + 2 * i];
        if (!type_ok(sig->arg_type[i], sig->arg_depth[i], false)) return false;
    }
    *used = 3 + 2u * sig->argc;
    return true;
}

/**
 * @brief Reads the import table that follows the fixed header.
 * @param[in,out] img   The image being built (`data`, `size` are set).
 * @param[out]    error Explanation on failure.
 * @param[in]     error_size Its size.
 */
static bool read_imports(DisImage* img, char* error, size_t error_size) {
    uint32_t pos = 12, count, i;

    if (img->size - pos < 4u) return reject(error, error_size, "the file ends inside its import table");
    count = rd32(img->data + pos);
    pos += 4;
    if (count > MAX_TABLE_ENTRIES) return reject(error, error_size, "the import table is malformed (%u entries)", (unsigned)count);

    img->imports = (DisImport*)dis_xcalloc(count, sizeof *img->imports);
    for (i = 0; i < count; i++) {
        DisImport* im = &img->imports[i];
        uint32_t nlen;
        size_t used = 0;

        img->import_count = i;  /* so dis_image_free() releases what was read so far */
        if (pos >= img->size) return reject(error, error_size, "the file ends inside its import table");
        nlen = img->data[pos++];
        if (nlen == 0 || img->size - pos < nlen + 3u) return reject(error, error_size, "the import table is malformed");
        im->name = copy_name(img->data + pos, nlen);
        img->import_count = i + 1;
        pos += nlen;
        if (!read_sig(img->data + pos, img->size - pos, &im->sig, &used))
            return reject(error, error_size, "the import table is malformed (function '%s')", im->name);
        pos += (uint32_t)used;
    }
    img->table_end = pos;
    return true;
}

/**
 * @brief Reads the EXPT section (the functions the host may call).
 *
 * @param[in,out] img    The image being built.
 * @param[in]     p      Payload of the section.
 * @param[in]     len    Its length.
 * @param[out]    error  Explanation on failure.
 * @param[in]     error_size Its size.
 */
static bool read_exports(DisImage* img, const uint8_t* p, uint32_t len, char* error, size_t error_size) {
    uint32_t pos = 8, count, i;

    if (len < 8u) return reject(error, error_size, "the EXPT section is malformed");
    img->init_address = rd32(p);
    count = rd32(p + 4);
    if (img->init_address < img->entry || img->init_address >= img->size || count > MAX_TABLE_ENTRIES)
        return reject(error, error_size, "the EXPT section is malformed");

    img->exports = (DisExport*)dis_xcalloc(count, sizeof *img->exports);
    for (i = 0; i < count; i++) {
        DisExport* ex = &img->exports[i];
        uint32_t nlen;
        size_t used = 0;

        if (len - pos < 1u) return reject(error, error_size, "the EXPT section is malformed");
        nlen = p[pos++];
        if (nlen == 0 || len - pos < nlen + 8u) return reject(error, error_size, "the EXPT section is malformed");
        ex->name = copy_name(p + pos, nlen);
        img->export_count = i + 1;
        pos += nlen;
        ex->address = rd32(p + pos);
        pos += 4;
        ex->nreg = p[pos++];
        if (ex->address < img->entry || ex->address >= img->size ||
            !read_sig(p + pos, len - pos, &ex->sig, &used) || ex->nreg > ex->sig.argc)
            return reject(error, error_size, "the EXPT section is malformed (function '%s')", ex->name);
        pos += (uint32_t)used;
    }
    img->has_expt = true;
    return true;
}

/**
 * @brief Reads the extension sections between the import table and the code.
 * @param[in,out] img   The image being built.
 * @param[out]    error Explanation on failure.
 * @param[in]     error_size Its size.
 */
static bool read_sections(DisImage* img, char* error, size_t error_size) {
    uint32_t pos = img->table_end;
    size_t capacity = 0;

    while (pos < img->entry) {
        DisSection* s;
        uint32_t len;
        size_t k;

        if (img->entry - pos < 8u) return reject(error, error_size, "the extension sections before the code are malformed");
        len = rd32(img->data + pos + 4);
        if (len > img->entry - pos - 8u) return reject(error, error_size, "the extension sections before the code are malformed");

        dis_reserve(&img->sections, &capacity, sizeof *img->sections, (size_t)img->section_count + 1);
        s = &img->sections[img->section_count++];
        for (k = 0; k < 4; k++) {
            unsigned char c = img->data[pos + k];
            s->tag[k] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
        }
        s->tag[4] = '\0';
        s->offset = pos + 8;
        s->length = len;

        if (memcmp(img->data + pos, "EXPT", 4) == 0) {
            if (img->has_expt) return reject(error, error_size, "the program has two EXPT sections");
            if (!read_exports(img, img->data + pos + 8, len, error, error_size)) return false;
        }
        pos += 8u + len;
    }
    return true;
}

bool dis_image_parse(DisImage* img, const uint8_t* data, size_t size, char* error, size_t error_size) {
    memset(img, 0, sizeof *img);
    if (size < 12) return reject(error, error_size, "the file is too small to be Ilang bytecode");
    if (size > UINT32_MAX) return reject(error, error_size, "the file is too large");
    if (memcmp(data, "iVM\0", 4) != 0) return reject(error, error_size, "not Ilang bytecode (the file does not start with \"iVM\")");

    img->data = data;
    img->size = (uint32_t)size;
    img->version = rd32(data + 4);
    img->entry = rd32(data + 8);

    if (img->version != SUPPORTED_VERSION)
        return reject(error, error_size, "the bytecode is version %u, but this Ilangdis reads version %d: use a matching build",
            (unsigned)img->version, SUPPORTED_VERSION);
    if (!read_imports(img, error, error_size)) {
        dis_image_free(img);
        return false;
    }
    if (img->entry < img->table_end || img->entry >= img->size) {
        dis_image_free(img);
        return reject(error, error_size, "the entry point lies outside the code");
    }
    if (!read_sections(img, error, error_size)) {
        dis_image_free(img);
        return false;
    }
    return true;
}

void dis_image_free(DisImage* img) {
    uint32_t i;

    for (i = 0; i < img->import_count; i++) free(img->imports[i].name);
    for (i = 0; i < img->export_count; i++) free(img->exports[i].name);
    free(img->imports);
    free(img->exports);
    free(img->sections);
    memset(img, 0, sizeof *img);
}
