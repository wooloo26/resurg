#ifndef RSG_LSP_SYMBOL_H
#define RSG_LSP_SYMBOL_H

#include <stdint.h>

#include "core/common.h"

/**
 * @file lsp_symbol.h
 * @brief Symbol indexing and hover formatting for the LSP server.
 */

struct ASTNode;
struct Sema;

// LSP SymbolKind values.
#define LSP_SK_FUNCTION 12
#define LSP_SK_METHOD 6
#define LSP_SK_STRUCT 23
#define LSP_SK_ENUM 10
#define LSP_SK_INTERFACE 11
#define LSP_SK_VARIABLE 13
#define LSP_SK_CONSTANT 14
#define LSP_SK_FIELD 8
#define LSP_SK_ENUM_MEMBER 22

/** A resolved symbol entry in the document index. */
typedef struct LspSymEntry {
    char *name;       // symbol name (owned)
    char *container;  // owning type name, or NULL (owned)
    char *hover;      // pre-formatted hover markdown (owned)
    SrcLoc loc;       // definition location
    int32_t lsp_kind; // LSP SymbolKind
    bool is_std;      // true when symbol originates from a std library file
} LspSymEntry;

/** Free the contents of a symbol entry (not the entry itself). */
void lsp_sym_entry_free(LspSymEntry *e);

/** Return hover text for a builtin primitive type, or NULL if not a builtin. */
const char *lsp_builtin_type_hover(const char *name);

/** Build the complete symbol index for a document.  Caller must BUF_FREE the result.
 *  @p std_path — path prefix for std library files; their symbols are included but
 *                marked @c is_std = true so the caller can filter them from documentSymbol.
 *                Pass NULL to exclude all non-file_path symbols. */
LspSymEntry *lsp_build_symbol_index(Arena *arena, const struct ASTNode *file_node,
                                    const struct Sema *sema, const char *file_path,
                                    const char *std_path);

/** Extract the identifier under the cursor from document text. */
char *lsp_extract_word_at(const char *text, int32_t line, int32_t col);

/** Find a symbol by name in the index.  Returns NULL if not found. */
const LspSymEntry *lsp_find_symbol(const LspSymEntry *syms, const char *name);

/** Find a symbol by name, preferring the closest definition at or before the given position. */
const LspSymEntry *lsp_find_symbol_at(const LspSymEntry *syms, const char *name, int32_t line,
                                      int32_t col);

#endif // RSG_LSP_SYMBOL_H
