#include "_check.h"

/**
 * @file check_main.c
 * @brief Check pass — type-check the AST after resolve has populated tables.
 */

// ── Public API ─────────────────────────────────────────────────────────

bool sema_check(Sema *sema, ASTNode *file) {
    sema->method_checker = check_struct_method_body;
    check_node(sema, file);
    return sema->err_count == 0;
}
