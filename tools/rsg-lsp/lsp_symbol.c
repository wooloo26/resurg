#include "lsp_symbol.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/common.h"
#include "pass/resolve/_resolve.h"
#include "pass/resolve/_sema.h"
#include "repr/ast.h"
#include "repr/types.h"

/**
 * @file lsp_symbol.c
 * @brief Symbol indexing and hover formatting for the LSP server.
 */

// ── Portable strdup ────────────────────────────────────────────────────

static char *sym_strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

// ── String buffer helpers ──────────────────────────────────────────────

/** Append printf-formatted text to a dynamic string buffer. */
static void sbuf_printf(char **buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t old_len = *buf != NULL ? strlen(*buf) : 0;
    char *new_buf = realloc(*buf, old_len + (size_t)n + 1);
    if (new_buf == NULL) {
        return;
    }
    *buf = new_buf;
    va_start(ap, fmt);
    vsnprintf(*buf + old_len, (size_t)n + 1, fmt, ap);
    va_end(ap);
}

/** Append an AST type annotation as display text. */
static void sbuf_ast_type(char **buf, const ASTType *t) {
    if (t == NULL || t->kind == AST_TYPE_INFERRED) {
        sbuf_printf(buf, "?");
        return;
    }
    switch (t->kind) {
    case AST_TYPE_NAME:
        sbuf_printf(buf, "%s", t->name != NULL ? t->name : "?");
        if (BUF_LEN(t->type_args) > 0) {
            sbuf_printf(buf, "<");
            for (int32_t i = 0; i < BUF_LEN(t->type_args); i++) {
                if (i > 0) {
                    sbuf_printf(buf, ", ");
                }
                sbuf_ast_type(buf, t->type_args[i]);
            }
            sbuf_printf(buf, ">");
        }
        break;
    case AST_TYPE_PTR:
        sbuf_printf(buf, "*");
        sbuf_ast_type(buf, t->ptr_elem);
        break;
    case AST_TYPE_ARRAY:
        sbuf_printf(buf, "[%d]", t->array_size);
        sbuf_ast_type(buf, t->array_elem);
        break;
    case AST_TYPE_SLICE:
        sbuf_printf(buf, "[]");
        sbuf_ast_type(buf, t->slice_elem);
        break;
    case AST_TYPE_TUPLE:
        sbuf_printf(buf, "(");
        for (int32_t i = 0; i < BUF_LEN(t->tuple_elems); i++) {
            if (i > 0) {
                sbuf_printf(buf, ", ");
            }
            sbuf_ast_type(buf, t->tuple_elems[i]);
        }
        sbuf_printf(buf, ")");
        break;
    default:
        sbuf_printf(buf, "?");
        break;
    }
}

/** Append generic type parameter list to a string buffer. */
static void format_type_params(char **buf, const ASTTypeParam *tps) {
    int32_t n = BUF_LEN(tps);
    if (n == 0) {
        return;
    }
    sbuf_printf(buf, "<");
    for (int32_t i = 0; i < n; i++) {
        if (i > 0) {
            sbuf_printf(buf, ", ");
        }
        if (tps[i].is_comptime) {
            sbuf_printf(buf, "comptime ");
        }
        sbuf_printf(buf, "%s", tps[i].name);
        int32_t bc = BUF_LEN(tps[i].bounds);
        if (bc > 0) {
            sbuf_printf(buf, ": ");
            for (int32_t j = 0; j < bc; j++) {
                if (j > 0) {
                    sbuf_printf(buf, " + ");
                }
                sbuf_printf(buf, "%s", tps[i].bounds[j]);
            }
        }
    }
    sbuf_printf(buf, ">");
}

// ── Hover formatting ──────────────────────────────────────────────────

/** Build hover text for a function declaration. */
static char *format_fn_hover(Arena *arena, const ASTNode *fn, const Sema *sema,
                             const ExtDeclData *ext) {
    char *buf = NULL;
    sbuf_printf(&buf, "```resurg\n");

    if (ext != NULL) {
        sbuf_printf(&buf, "// ext %s", ext->target_name);
        format_type_params(&buf, ext->type_params);
        sbuf_printf(&buf, "\n");
    }

    const char *vis = fn->fn_decl.is_pub ? "pub " : "";
    sbuf_printf(&buf, "%sfn %s", vis, fn->fn_decl.name);
    format_type_params(&buf, fn->fn_decl.type_params);
    sbuf_printf(&buf, "(");

    bool has_recv = false;
    if (fn->fn_decl.recv_name != NULL) {
        if (fn->fn_decl.is_ptr_recv && fn->fn_decl.is_mut_recv) {
            sbuf_printf(&buf, "mut *%s", fn->fn_decl.recv_name);
        } else if (fn->fn_decl.is_ptr_recv) {
            sbuf_printf(&buf, "*%s", fn->fn_decl.recv_name);
        } else if (ext != NULL) {
            sbuf_printf(&buf, "%s /* %s */", fn->fn_decl.recv_name, ext->target_name);
        } else {
            sbuf_printf(&buf, "%s", fn->fn_decl.recv_name);
        }
        has_recv = true;
    }

    int32_t pc = BUF_LEN(fn->fn_decl.params);
    for (int32_t i = 0; i < pc; i++) {
        ASTNode *p = fn->fn_decl.params[i];
        if (has_recv || i > 0) {
            sbuf_printf(&buf, ", ");
        }
        if (p->kind == NODE_PARAM) {
            if (p->type != NULL) {
                sbuf_printf(&buf, "%s: %s", p->param.name, type_name(arena, p->type));
            } else {
                sbuf_printf(&buf, "%s: ", p->param.name);
                sbuf_ast_type(&buf, &p->param.type);
            }
        }
    }

    const Type *ret =
        fn->type != NULL && fn->type->kind == TYPE_FN ? fn->type->fn_type.return_type : NULL;
    if (ret != NULL && ret->kind != TYPE_UNIT) {
        sbuf_printf(&buf, ") -> %s", type_name(arena, ret));
    } else if (fn->fn_decl.return_type.kind != AST_TYPE_INFERRED &&
               fn->fn_decl.return_type.kind != AST_TYPE_NAME) {
        sbuf_printf(&buf, ") -> ");
        sbuf_ast_type(&buf, &fn->fn_decl.return_type);
    } else if (fn->fn_decl.return_type.kind == AST_TYPE_NAME &&
               fn->fn_decl.return_type.name != NULL) {
        sbuf_printf(&buf, ") -> %s", fn->fn_decl.return_type.name);
    } else {
        sbuf_printf(&buf, ")");
    }
    sbuf_printf(&buf, "\n```");

    if (fn->doc_comment != NULL) {
        sbuf_printf(&buf, "\n\n---\n\n%s", fn->doc_comment);
    }
    (void)sema;
    return buf;
}

/** Build hover text for a struct declaration. */
static char *format_struct_hover(Arena *arena, const StructDef *sd, const ASTNode *node) {
    char *buf = NULL;
    sbuf_printf(&buf, "```resurg\nstruct %s", sd->name);
    int32_t fc = BUF_LEN(sd->fields);
    if (fc > 0) {
        sbuf_printf(&buf, " {\n");
        for (int32_t i = 0; i < fc; i++) {
            const char *tname =
                sd->fields[i].type != NULL ? type_name(arena, sd->fields[i].type) : "?";
            sbuf_printf(&buf, "    %s: %s,\n", sd->fields[i].name, tname);
        }
        sbuf_printf(&buf, "}");
    }
    sbuf_printf(&buf, "\n```");
    if (node != NULL && node->doc_comment != NULL) {
        sbuf_printf(&buf, "\n\n---\n\n%s", node->doc_comment);
    }
    return buf;
}

/** Build hover text for an enum declaration. */
static char *format_enum_hover(const ASTNode *node) {
    char *buf = NULL;
    EnumDeclData *ed = node->enum_decl;
    sbuf_printf(&buf, "```resurg\nenum %s", ed->name);
    int32_t vc = BUF_LEN(ed->variants);
    if (vc > 0) {
        sbuf_printf(&buf, " {\n");
        for (int32_t i = 0; i < vc; i++) {
            sbuf_printf(&buf, "    %s,\n", ed->variants[i].name);
        }
        sbuf_printf(&buf, "}");
    }
    sbuf_printf(&buf, "\n```");
    if (node->doc_comment != NULL) {
        sbuf_printf(&buf, "\n\n---\n\n%s", node->doc_comment);
    }
    return buf;
}

/** Build hover text for a pact declaration. */
static char *format_pact_hover(const ASTNode *node) {
    char *buf = NULL;
    PactDeclData *pd = node->pact_decl;
    sbuf_printf(&buf, "```resurg\npact %s\n```", pd->name);
    if (node->doc_comment != NULL) {
        sbuf_printf(&buf, "\n\n---\n\n%s", node->doc_comment);
    }
    return buf;
}

/** Build hover text for a variable declaration. */
static char *format_var_hover(Arena *arena, const ASTNode *node) {
    char *buf = NULL;
    const char *kw = node->var_decl.is_var ? "var" : "let";
    if (node->var_decl.is_immut) {
        kw = "immut";
    }
    const char *tname = node->type != NULL ? type_name(arena, node->type) : "?";
    sbuf_printf(&buf, "```resurg\n%s %s: %s\n```", kw, node->var_decl.name, tname);
    if (node->doc_comment != NULL) {
        sbuf_printf(&buf, "\n\n---\n\n%s", node->doc_comment);
    }
    return buf;
}

// ── Symbol index builders ─────────────────────────────────────────────

/** Recursively walk an AST node to index local variable declarations. */
static void index_body_vars(LspSymEntry **syms, Arena *arena, const ASTNode *node) {
    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case NODE_VAR_DECL: {
        LspSymEntry e = {0};
        e.name = sym_strdup(node->var_decl.name);
        e.loc = node->loc;
        e.lsp_kind = node->var_decl.is_immut ? LSP_SK_CONSTANT : LSP_SK_VARIABLE;
        e.hover = format_var_hover(arena, node);
        BUF_PUSH(*syms, e);
        break;
    }
    case NODE_BLOCK:
        for (int32_t i = 0; i < BUF_LEN(node->block.stmts); i++) {
            index_body_vars(syms, arena, node->block.stmts[i]);
        }
        index_body_vars(syms, arena, node->block.result);
        break;
    case NODE_IF:
        index_body_vars(syms, arena, node->if_expr.then_body);
        index_body_vars(syms, arena, node->if_expr.else_body);
        break;
    case NODE_FOR:
        index_body_vars(syms, arena, node->for_loop.body);
        break;
    case NODE_LOOP:
        index_body_vars(syms, arena, node->loop.body);
        break;
    case NODE_WHILE:
        index_body_vars(syms, arena, node->while_loop.body);
        break;
    case NODE_DEFER:
        index_body_vars(syms, arena, node->defer_stmt.body);
        break;
    case NODE_MATCH:
        for (int32_t i = 0; i < BUF_LEN(node->match_expr.arms); i++) {
            index_body_vars(syms, arena, node->match_expr.arms[i].body);
        }
        break;
    case NODE_EXPR_STMT:
        index_body_vars(syms, arena, node->expr_stmt.expr);
        break;
    default:
        break;
    }
}

/** Index a fn declaration.  @p container is non-NULL for methods. */
static void index_fn_decl(LspSymEntry **syms, Arena *arena, const ASTNode *fn, const Sema *sema,
                          const char *container, const ExtDeclData *ext) {
    LspSymEntry e = {0};
    e.name = sym_strdup(fn->fn_decl.name);
    e.container = container != NULL ? sym_strdup(container) : NULL;
    e.loc = fn->loc;
    e.lsp_kind = container != NULL ? LSP_SK_METHOD : LSP_SK_FUNCTION;
    e.hover = format_fn_hover(arena, fn, sema, ext);
    BUF_PUSH(*syms, e);

    // Index parameters as local variables
    for (int32_t i = 0; i < BUF_LEN(fn->fn_decl.params); i++) {
        const ASTNode *p = fn->fn_decl.params[i];
        if (p->kind == NODE_PARAM && p->param.name != NULL) {
            LspSymEntry pe = {0};
            pe.name = sym_strdup(p->param.name);
            pe.loc = p->loc;
            pe.lsp_kind = LSP_SK_VARIABLE;
            char *buf = NULL;
            const char *tname = p->type != NULL ? type_name(arena, p->type) : "?";
            sbuf_printf(&buf, "```resurg\n%s: %s\n```\n\n(parameter)", p->param.name, tname);
            pe.hover = buf;
            BUF_PUSH(*syms, pe);
        }
    }

    // Walk function body to index local variables
    if (fn->fn_decl.body != NULL) {
        index_body_vars(syms, arena, fn->fn_decl.body);
    }
}

/** Index a struct and its methods. */
static void index_struct_decl(LspSymEntry **syms, Arena *arena, const ASTNode *node,
                              const Sema *sema) {
    StructDeclData *sd_ast = node->struct_decl;
    StructDef *sd = sema_lookup_struct(sema, sd_ast->name);

    LspSymEntry e = {0};
    e.name = sym_strdup(sd_ast->name);
    e.loc = node->loc;
    e.lsp_kind = LSP_SK_STRUCT;
    e.hover = sd != NULL ? format_struct_hover(arena, sd, node) : NULL;
    BUF_PUSH(*syms, e);

    int32_t mc = BUF_LEN(sd_ast->methods);
    for (int32_t i = 0; i < mc; i++) {
        index_fn_decl(syms, arena, sd_ast->methods[i], sema, sd_ast->name, NULL);
    }

    if (sd != NULL) {
        int32_t fc = BUF_LEN(sd->fields);
        for (int32_t i = 0; i < fc; i++) {
            LspSymEntry fe = {0};
            fe.name = sym_strdup(sd->fields[i].name);
            fe.container = sym_strdup(sd_ast->name);
            fe.lsp_kind = LSP_SK_FIELD;
            fe.loc = node->loc;
            const char *tname =
                sd->fields[i].type != NULL ? type_name(arena, sd->fields[i].type) : "?";
            char *hbuf = NULL;
            sbuf_printf(&hbuf, "```resurg\n%s.%s: %s\n```", sd_ast->name, sd->fields[i].name,
                        tname);
            fe.hover = hbuf;
            BUF_PUSH(*syms, fe);
        }
    }
}

/** Index an enum and its methods. */
static void index_enum_decl(LspSymEntry **syms, Arena *arena, const ASTNode *node,
                            const Sema *sema) {
    EnumDeclData *ed = node->enum_decl;

    LspSymEntry e = {0};
    e.name = sym_strdup(ed->name);
    e.loc = node->loc;
    e.lsp_kind = LSP_SK_ENUM;
    e.hover = format_enum_hover(node);
    BUF_PUSH(*syms, e);

    int32_t vc = BUF_LEN(ed->variants);
    for (int32_t i = 0; i < vc; i++) {
        LspSymEntry ve = {0};
        ve.name = sym_strdup(ed->variants[i].name);
        ve.container = sym_strdup(ed->name);
        ve.loc = node->loc;
        ve.lsp_kind = LSP_SK_ENUM_MEMBER;
        char *hbuf = NULL;
        sbuf_printf(&hbuf, "```resurg\n%s::%s\n```", ed->name, ed->variants[i].name);
        ve.hover = hbuf;
        BUF_PUSH(*syms, ve);
    }

    int32_t mc = BUF_LEN(ed->methods);
    for (int32_t i = 0; i < mc; i++) {
        index_fn_decl(syms, arena, ed->methods[i], sema, ed->name, NULL);
    }
    (void)arena;
}

/** Index a pact declaration. */
static void index_pact_decl(LspSymEntry **syms, Arena *arena, const ASTNode *node,
                            const Sema *sema) {
    PactDeclData *pd = node->pact_decl;

    LspSymEntry e = {0};
    e.name = sym_strdup(pd->name);
    e.loc = node->loc;
    e.lsp_kind = LSP_SK_INTERFACE;
    e.hover = format_pact_hover(node);
    BUF_PUSH(*syms, e);

    int32_t mc = BUF_LEN(pd->methods);
    for (int32_t i = 0; i < mc; i++) {
        index_fn_decl(syms, arena, pd->methods[i], sema, pd->name, NULL);
    }
    (void)arena;
}

/** Index methods inside an ext block. */
static void index_ext_decl(LspSymEntry **syms, Arena *arena, const ASTNode *node,
                           const Sema *sema) {
    ExtDeclData *ed = node->ext_decl;
    int32_t mc = BUF_LEN(ed->methods);
    for (int32_t i = 0; i < mc; i++) {
        index_fn_decl(syms, arena, ed->methods[i], sema, ed->target_name, ed);
    }
}

// ── Public API ─────────────────────────────────────────────────────────

void lsp_sym_entry_free(LspSymEntry *e) {
    free(e->name);
    free(e->container);
    free(e->hover);
}

const char *lsp_builtin_type_hover(const char *name) {
    static const struct {
        const char *name;
        const char *desc;
    } builtins[] = {
        {"i8", "8-bit signed integer (-128 to 127)"},
        {"i16", "16-bit signed integer (-32768 to 32767)"},
        {"i32", "32-bit signed integer"},
        {"i64", "64-bit signed integer"},
        {"i128", "128-bit signed integer"},
        {"u8", "8-bit unsigned integer (0 to 255)"},
        {"u16", "16-bit unsigned integer (0 to 65535)"},
        {"u32", "32-bit unsigned integer"},
        {"u64", "64-bit unsigned integer"},
        {"u128", "128-bit unsigned integer"},
        {"f32", "32-bit floating-point number (IEEE 754)"},
        {"f64", "64-bit floating-point number (IEEE 754)"},
        {"bool", "Boolean type (true or false)"},
        {"char", "Unicode scalar value (32-bit)"},
        {"str", "UTF-8 string type"},
        {"usize", "Platform-sized unsigned integer"},
        {"isize", "Platform-sized signed integer"},
        {"unit", "Unit type — zero-size type with a single value ()"},
        {"never", "Never type — represents computations that never complete"},
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcmp(name, builtins[i].name) == 0) {
            return builtins[i].desc;
        }
    }
    return NULL;
}

const char *lsp_builtin_fn_hover(const char *name) {
    static const struct {
        const char *name;
        const char *sig;
        const char *desc;
    } builtins[] = {
        {"print", "fn print<T>(value: T)",
         "Print a value to stdout without a trailing newline."},
        {"println", "fn println<T>(value: T)",
         "Print a value to stdout followed by a newline."},
        {"assert", "fn assert(cond: bool, msg: str)",
         "Assert that `cond` is true; panics with `msg` if false."},
        {"panic", "fn panic(msg: str) -> never",
         "Terminate the program immediately with an error message."},
        {"catch_panic", "fn catch_panic<T>(f: Fn() -> T) -> T!str",
         "Call `f` and catch any panic, returning `Ok(result)` or `Err(message)`."},
        {"len", "fn len<T>(value: T) -> i32",
         "Return the length of a slice, array, or string."},
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcmp(name, builtins[i].name) == 0) {
            // Return a pre-formatted markdown hover string.
            size_t sig_len = strlen(builtins[i].sig);
            size_t desc_len = strlen(builtins[i].desc);
            size_t total = sig_len + desc_len + 32;
            char *result = malloc(total);
            if (result != NULL) {
                snprintf(result, total, "```resurg\n%s\n```\n\n%s", builtins[i].sig,
                         builtins[i].desc);
            }
            return result;
        }
    }
    return NULL;
}

LspSymEntry *lsp_build_symbol_index(Arena *arena, const ASTNode *file_node, const Sema *sema,
                                    const char *file_path) {
    LspSymEntry *syms = NULL;
    int32_t dc = BUF_LEN(file_node->file.decls);
    for (int32_t i = 0; i < dc; i++) {
        const ASTNode *decl = file_node->file.decls[i];
        if (decl->loc.file != NULL && file_path != NULL && strcmp(decl->loc.file, file_path) != 0) {
            continue;
        }
        switch (decl->kind) {
        case NODE_FN_DECL:
            index_fn_decl(&syms, arena, decl, sema, NULL, NULL);
            break;
        case NODE_STRUCT_DECL:
            index_struct_decl(&syms, arena, decl, sema);
            break;
        case NODE_ENUM_DECL:
            index_enum_decl(&syms, arena, decl, sema);
            break;
        case NODE_PACT_DECL:
            index_pact_decl(&syms, arena, decl, sema);
            break;
        case NODE_EXT_DECL:
            index_ext_decl(&syms, arena, decl, sema);
            break;
        case NODE_VAR_DECL: {
            LspSymEntry e = {0};
            e.name = sym_strdup(decl->var_decl.name);
            e.loc = decl->loc;
            e.lsp_kind = decl->var_decl.is_immut ? LSP_SK_CONSTANT : LSP_SK_VARIABLE;
            e.hover = format_var_hover(arena, decl);
            BUF_PUSH(syms, e);
            break;
        }
        default:
            break;
        }
    }
    return syms;
}

char *lsp_extract_word_at(const char *text, int32_t line, int32_t col) {
    const char *p = text;
    for (int32_t i = 0; i < line && *p != '\0'; i++) {
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }
    const char *line_start = p;
    for (int32_t i = 0; i < col && *p != '\0' && *p != '\n'; i++) {
        p++;
    }
    const char *start = p;
    while (start > line_start && (isalnum((unsigned char)start[-1]) || start[-1] == '_')) {
        start--;
    }
    const char *end = p;
    while (*end != '\0' && *end != '\n' && (isalnum((unsigned char)*end) || *end == '_')) {
        end++;
    }
    if (start == end) {
        return NULL;
    }
    size_t len = (size_t)(end - start);
    char *word = malloc(len + 1);
    if (word == NULL) {
        return NULL;
    }
    memcpy(word, start, len);
    word[len] = '\0';
    return word;
}

const LspSymEntry *lsp_find_symbol(const LspSymEntry *syms, const char *name) {
    int32_t n = BUF_LEN(syms);
    for (int32_t i = 0; i < n; i++) {
        if (strcmp(syms[i].name, name) == 0) {
            return &syms[i];
        }
    }
    return NULL;
}

const LspSymEntry *lsp_find_symbol_at(const LspSymEntry *syms, const char *name, int32_t line,
                                      int32_t col) {
    (void)col;
    int32_t n = BUF_LEN(syms);
    const LspSymEntry *best = NULL;
    for (int32_t i = 0; i < n; i++) {
        if (strcmp(syms[i].name, name) != 0) {
            continue;
        }
        // Prefer the symbol defined closest to (but at or before) the cursor.
        int32_t sym_line = syms[i].loc.line - 1; // SrcLoc is 1-based, LSP is 0-based
        if (sym_line > line) {
            continue;
        }
        if (best == NULL || sym_line > (best->loc.line - 1)) {
            best = &syms[i];
        }
    }
    // Fall back to first name match if no position-aware match was found.
    if (best == NULL) {
        return lsp_find_symbol(syms, name);
    }
    return best;
}
