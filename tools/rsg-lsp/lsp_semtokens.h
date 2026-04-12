#ifndef RSG_LSP_SEMTOKENS_H
#define RSG_LSP_SEMTOKENS_H

#include <stdint.h>

/**
 * @file lsp_semtokens.h
 * @brief Semantic token generation for LSP.
 */

struct ASTNode;

// LSP semantic token type indices (must match legend in initialize).
#define SEM_TYPE_NAMESPACE 0
#define SEM_TYPE_TYPE 1
#define SEM_TYPE_ENUM 3
#define SEM_TYPE_INTERFACE 4
#define SEM_TYPE_STRUCT 5
#define SEM_TYPE_TYPE_PARAMETER 6
#define SEM_TYPE_PARAMETER 7
#define SEM_TYPE_VARIABLE 8
#define SEM_TYPE_PROPERTY 9
#define SEM_TYPE_ENUM_MEMBER 10
#define SEM_TYPE_FUNCTION 12
#define SEM_TYPE_METHOD 13

// LSP semantic token modifier bitmask.
#define SEM_MOD_DECLARATION (1 << 0)
#define SEM_MOD_DEFINITION (1 << 1)
#define SEM_MOD_READONLY (1 << 2)

/** A single semantic token entry (position + classification). */
typedef struct {
    int32_t line;       // 0-based
    int32_t start_char; // 0-based
    int32_t length;
    int32_t token_type;
    int32_t modifiers;
} SemToken;

/** Build semantic tokens for a document.  Caller must BUF_FREE the result. */
SemToken *lsp_build_semantic_tokens(const struct ASTNode *file_node, const char *file_path);

/** Comparison function for sorting semantic tokens by position. */
int lsp_sem_token_cmp(const void *a, const void *b);

#endif // RSG_LSP_SEMTOKENS_H
