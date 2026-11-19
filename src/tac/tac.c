/* Lexify Three Address Code generator
 *
 * Strategy: a post-order walk over the AST.
 *   - gen_expr() emits the instructions for an expression and returns the
 *     "place" (variable name, literal, or fresh temporary tN) holding its value.
 *   - Control flow is lowered to labels (Ln:), "ifFalse c goto Ln" and "goto Ln".
 *   - Variables shadowed in nested scopes get unique TAC names (x, x_1, x_2 ...)
 *     so that the flat TAC never confuses an inner x with the outer x.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "tac/tac.h"

static FILE *out;
static int   temp_count, label_count;

/* ---- name management (scope-aware) ---------------------------------- */

typedef struct Var  { char *name; char *tac; struct Var *next; } Var;
typedef struct Used { char *name; int count; struct Used *next; } Used;

static Var  *vars;   /* variables visible right now, innermost first */
static Used *used;   /* how many times each source name has been declared */

static char *dup_str(const char *s) {
    char *p = strdup(s);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

/* "t12" / "L3" would collide with our own temporaries and labels */
static int looks_generated(const char *s) {
    if ((s[0] != 't' && s[0] != 'L') || !s[1]) return 0;
    for (const char *p = s + 1; *p; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

static void declare(const char *name) {
    Used *u = used;
    while (u && strcmp(u->name, name) != 0) u = u->next;
    if (!u) {
        u = calloc(1, sizeof(Used));
        if (!u) { fprintf(stderr, "out of memory\n"); exit(2); }
        u->name = dup_str(name);
        u->next = used;
        used = u;
    }
    char buf[256];
    if (u->count == 0 && !looks_generated(name)) snprintf(buf, sizeof buf, "%s", name);
    else snprintf(buf, sizeof buf, "%s_%d", name, u->count);
    u->count++;

    Var *v = calloc(1, sizeof(Var));
    if (!v) { fprintf(stderr, "out of memory\n"); exit(2); }
    v->name = dup_str(name);
    v->tac  = dup_str(buf);
    v->next = vars;
    vars = v;
}

static const char *lookup(const char *name) {
    for (Var *v = vars; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v->tac;
    return name;   /* unreachable after a clean semantic pass */
}

/* ---- output helpers --------------------------------------------------- */

static void emit(const char *fmt, ...) {
    va_list ap;
    fputs("    ", out);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

static void emit_label(int n) { fprintf(out, "L%d:\n", n); }

static char *new_temp(void) {
    char buf[32];
    snprintf(buf, sizeof buf, "t%d", ++temp_count);
    return dup_str(buf);
}

static int new_label(void) { return ++label_count; }

/* 'a' converted to float if needed; returns the place to use */
static char *to_float(char *place) {
    char *t = new_temp();
    emit("%s = (float) %s", t, place);
    return t;
}

/* ---- expressions ------------------------------------------------------ */

static char *gen_expr(const Node *n) {
    char buf[64];
    switch (n->kind) {
    case N_INT:
        snprintf(buf, sizeof buf, "%d", n->ival);
        return dup_str(buf);
    case N_FLOAT:
        snprintf(buf, sizeof buf, "%g", n->fval);
        if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");   /* keep 2.0 looking like a float */
        return dup_str(buf);
    case N_BOOL:
        return dup_str(n->ival ? "true" : "false");
    case N_STR: {
        char *q = malloc(strlen(n->name) + 3);
        if (!q) { fprintf(stderr, "out of memory\n"); exit(2); }
        sprintf(q, "\"%s\"", n->name);
        return q;
    }
    case N_ID:
        return dup_str(lookup(n->name));

    case N_UNOP: {
        char *a = gen_expr(n->a);
        char *t = new_temp();
        emit("%s = %s %s", t, n->op, a);
        return t;
    }
    case N_BINOP: {
        char *l = gen_expr(n->a);
        char *r = gen_expr(n->b);
        /* mixed int/float arithmetic or comparison: widen the int side */
        if (n->a->type == T_INT && n->b->type == T_FLOAT) l = to_float(l);
        else if (n->a->type == T_FLOAT && n->b->type == T_INT) r = to_float(r);
        char *t = new_temp();
        emit("%s = %s %s %s", t, l, n->op, r);
        return t;
    }
    default:
        return dup_str("?");
    }
}

/* ---- statements ------------------------------------------------------- */

static void gen_stmts(const Node *n);

/* body of if/while: a block opens its own scope; a lone statement gets one too */
static void gen_body(const Node *n) {
    if (!n) return;
    Var *mark = vars;
    gen_stmts(n);
    vars = mark;
}

static void gen_stmts(const Node *n) {
    for (; n; n = n->next) {
        switch (n->kind) {
        case N_DECL:
            declare(n->name);          /* no instruction: just bookkeeping */
            break;

        case N_ASSIGN: {
            char *v = gen_expr(n->a);
            if (n->type == T_FLOAT && n->a->type == T_INT) v = to_float(v);
            emit("%s = %s", lookup(n->name), v);
            break;
        }
        case N_RETURN: {
            if (n->a) { char *v = gen_expr(n->a); emit("return %s", v); }
            else emit("return");
            break;
        }
        case N_PRINT: {
            char *v = gen_expr(n->a);
            if (n->name)
                emit("print %s, \"%s\"", v, n->name);      /* printf specifier kept: %f, %.2f, %c ... */
            else
                emit("print %s", v);
            break;
        }
        case N_IF: {
            char *c = gen_expr(n->a);
            if (!n->c) {
                int lend = new_label();
                emit("ifFalse %s goto L%d", c, lend);
                gen_body(n->b);
                emit_label(lend);
            } else {
                int lelse = new_label(), lend = new_label();
                emit("ifFalse %s goto L%d", c, lelse);
                gen_body(n->b);
                emit("goto L%d", lend);
                emit_label(lelse);
                gen_body(n->c);
                emit_label(lend);
            }
            break;
        }
        case N_WHILE: {
            int lstart = new_label(), lend = new_label();
            emit_label(lstart);
            char *c = gen_expr(n->a);
            emit("ifFalse %s goto L%d", c, lend);
            gen_body(n->b);
            emit("goto L%d", lstart);
            emit_label(lend);
            break;
        }
        case N_BLOCK: {
            Var *mark = vars;
            gen_stmts(n->a);
            vars = mark;
            break;
        }
        default:
            break;
        }
    }
}

void tac_generate(const Node *root, FILE *f) {
    out = f;
    temp_count = label_count = 0;
    vars = NULL;
    used = NULL;
    if (root) gen_stmts(root->a);
}
