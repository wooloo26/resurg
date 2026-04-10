#include "_check.h"

/**
 * @file check_main.c
 * @brief Check pass — type-check the AST after resolve has populated tables.
 */

// ── Public API ─────────────────────────────────────────────────────────

bool sema_check(Sema *sema, ASTNode *file) {
    sema->base.phase = SEMA_PHASE_CHECK;
    sema->method_checker = check_struct_method_body;
    check_node(sema, file);
    return sema->base.err_count == 0;
}

void sema_check_fn_body(Sema *sema, ASTNode *fn_node) {
    check_fn_body(sema, fn_node);
}
