/* Lexify semantic analyzer
 * Detects: undeclared variable, redeclaration, scope violation,
 *          type mismatch, invalid assignment, invalid expression. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "semantic/semantic.h"
#include "symbol_table/symbol_table.h"

static int errors = 0;

static void err(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "Semantic error at line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    errors++;
}

static int is_num(Type t) { return t == T_INT || t == T_FLOAT; }
static int is(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* undeclared variable vs. scope violation */
static void report_unknown(const char *name, int line) {
    Symbol *dead = st_find_inactive(name);
    if (dead)
        err(line, "scope violation: '%s' (declared at line %d) is not visible here", name, dead->line);
    else
        err(line, "undeclared variable '%s'", name);
}

static Type check_expr(Node *n) {
    switch (n->kind) {
    case N_INT:   return n->type = T_INT;
    case N_FLOAT: return n->type = T_FLOAT;
    case N_BOOL:  return n->type = T_BOOL;
    case N_STR:   return n->type = T_NONE;     /* only legal inside print */

    case N_ID: {
        Symbol *s = st_lookup(n->name);
        if (!s) { report_unknown(n->name, n->line); return n->type = T_NONE; }
        return n->type = s->type;
    }

    case N_UNOP: {
        Type t = check_expr(n->a);
        if (t == T_NONE) return n->type = T_NONE;      /* already reported */
        if (is(n->op, "-")) {                           /* unary minus */
            if (!is_num(t)) {
                err(n->line, "invalid expression: operator '-' needs a numeric operand, got '%s'", type_name(t));
                return n->type = T_NONE;
            }
            return n->type = t;
        }
        if (t != T_BOOL) {
            err(n->line, "invalid expression: operator '%s' needs a bool operand, got '%s'", n->op, type_name(t));
            return n->type = T_NONE;
        }
        return n->type = T_BOOL;
    }

    case N_BINOP: {
        Type l = check_expr(n->a), r = check_expr(n->b);
        if (l == T_NONE || r == T_NONE) return n->type = T_NONE;   /* avoid cascades */
        const char *op = n->op;

        if (is(op, "+") || is(op, "-") || is(op, "*") || is(op, "/")) {
            if (!is_num(l) || !is_num(r)) {
                err(n->line, "invalid expression: operator '%s' needs numeric operands, got '%s' and '%s'",
                    op, type_name(l), type_name(r));
                return n->type = T_NONE;
            }
            return n->type = (l == T_FLOAT || r == T_FLOAT) ? T_FLOAT : T_INT;
        }
        if (is(op, "%")) {
            if (l != T_INT || r != T_INT) {
                err(n->line, "invalid expression: operator '%%' needs int operands, got '%s' and '%s'",
                    type_name(l), type_name(r));
                return n->type = T_NONE;
            }
            return n->type = T_INT;
        }
        if (is(op, "<") || is(op, ">") || is(op, "<=") || is(op, ">=")) {
            if (!is_num(l) || !is_num(r)) {
                err(n->line, "invalid expression: operator '%s' needs numeric operands, got '%s' and '%s'",
                    op, type_name(l), type_name(r));
                return n->type = T_NONE;
            }
            return n->type = T_BOOL;
        }
        if (is(op, "==") || is(op, "!=")) {
            if ((is_num(l) && is_num(r)) || (l == T_BOOL && r == T_BOOL))
                return n->type = T_BOOL;
            err(n->line, "type mismatch: cannot compare '%s' with '%s'", type_name(l), type_name(r));
            return n->type = T_NONE;
        }
        /* && || */
        if (l != T_BOOL || r != T_BOOL) {
            err(n->line, "invalid expression: operator '%s' needs bool operands, got '%s' and '%s'",
                op, type_name(l), type_name(r));
            return n->type = T_NONE;
        }
        return n->type = T_BOOL;
    }
    default:
        return T_NONE;
    }
}

static void check_stmts(Node *n);

/* body of if/while: a lone statement gets its own scope; a block opens one itself */
static void check_body(Node *n) {
    if (!n) return;
    if (n->kind == N_BLOCK) { check_stmts(n); return; }
    st_enter();
    check_stmts(n);
    st_exit();
}

static void check_condition(Node *cond, const char *what) {
    Type t = check_expr(cond);
    if (t != T_NONE && t != T_BOOL)
        err(cond->line, "condition of '%s' must be bool, got '%s'", what, type_name(t));
}

static void check_stmts(Node *n) {
    for (; n; n = n->next) {
        switch (n->kind) {
        case N_DECL: {
            Symbol *old = st_lookup_current(n->name);
            if (old)
                err(n->line, "redeclaration of '%s' (first declared at line %d)", n->name, old->line);
            else
                st_insert(n->name, n->type, n->line);
            break;
        }
        case N_ASSIGN: {
            Symbol *s = st_lookup(n->name);
            if (!s) report_unknown(n->name, n->line);
            Type rt = check_expr(n->a);
            if (s && rt != T_NONE) {
                n->type = s->type;
                int ok = (s->type == rt) || (s->type == T_FLOAT && rt == T_INT);
                if (!ok)
                    err(n->line, "invalid assignment: cannot assign '%s' to '%s' variable '%s'",
                        type_name(rt), type_name(s->type), n->name);
            }
            break;
        }
        case N_PRINT:
            check_expr(n->a);
            break;
        case N_RETURN:
            if (n->a) check_expr(n->a);
            break;
        case N_IF:
            check_condition(n->a, "if");
            check_body(n->b);
            check_body(n->c);
            break;
        case N_WHILE:
            check_condition(n->a, "while");
            check_body(n->b);
            break;
        case N_BLOCK:
            st_enter();
            check_stmts(n->a);
            st_exit();
            break;
        default:
            break;
        }
    }
}

int semantic_check(Node *root) {
    errors = 0;
    st_init();
    if (root) check_stmts(root->a);
    return errors;
}
