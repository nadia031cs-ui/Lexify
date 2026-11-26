/* Lexify Three Address Code generator
 *
 * Strategy: a post-order walk over the AST.
 *   - gen_expr() emits the instructions for an expression and returns the
 *     "place" (variable name, literal, or fresh temporary tN) holding its value.
 *   - Control flow is lowered to labels (Ln:), "ifFalse c goto Ln" and "goto Ln".
 *   - Variables shadowed in nested scopes get unique TAC names (x, x_1, x_2 ...)
 *     so that the flat TAC never confuses an inner x with the outer x.
 *
 * Instructions (one per line):
 *   x = y            x = y op z        x = op y  (op: - ! ~)     x = (type) y
 *   x = a[i]         a[i] = x          x = newarray type n [m]
 *   ifFalse x goto L     goto L        L:
 *   print x [, "%fmt"]   write x       manip name [x]     x = read kind
 *   param x          x = call f, n     call f, n          return [x]
 *   function f:      formal x          end f              global x      halt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "tac/tac.h"
#include "symbol_table/symbol_table.h"

static FILE *out;
static int   temp_count, label_count;
static int   has_funcs, depth;

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

/* "t12" / "L3" would collide with our own temporaries and labels; TAC keywords too */
static int looks_generated(const char *s) {
    static const char *kw[] = { "goto", "ifFalse", "print", "write", "read", "manip", "param", "call", "return",
                                "function", "formal", "end", "global", "halt", "newarray", "true", "false", NULL };
    for (int i = 0; kw[i]; i++) if (strcmp(s, kw[i]) == 0) return 1;
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

static const char *kind_name(Type t) {
    switch (t) {
    case T_INT: return "int"; case T_LONG: return "long"; case T_FLOAT: return "float"; case T_CHAR: return "char";
    case T_BOOL: return "bool"; case T_STRING: return "string"; default: return "int";
    }
}

static int is_numeric(Type t) { return t == T_INT || t == T_LONG || t == T_CHAR || t == T_BOOL; }

/* value of type 'from' stored into a variable of type 'to' */
static char *conv(char *v, Type from, Type to) {
    if (from == to || from == T_NONE || to == T_NONE || to == T_AUTO) return v;
    if (to == T_FLOAT && is_numeric(from)) return to_float(v);
    if (lang_mode == LANG_MC) return v;
    if (to == T_STRING || from == T_STRING || to == T_SCANNER || from == T_SCANNER || from == T_VOID) return v;
    if (to == T_FLOAT) return v;
    char *t = new_temp();
    emit("%s = (%s) %s", t, kind_name(to), v);
    return t;
}

/* shortest text that reads back as the same float/double, without an exponent for ordinary sizes */
static void float_text(double d, int f32, char *buf, size_t n) {
    int p;
    for (p = 1; p <= 17; p++) {
        snprintf(buf, n, "%.*g", p, d);
        if (f32 ? (float)strtod(buf, NULL) == (float)d : strtod(buf, NULL) == d) break;
    }
    double a = d < 0 ? -d : d;
    if (strchr(buf, 'e') && a >= 1e-4 && a < 1e15) {
        int digits = 1;
        while (digits < 17 && a >= 10.0) { a /= 10.0; digits++; }
        snprintf(buf, n, "%.*g", p > digits ? p : digits, d);
    }
    if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");   /* keep 2.0 looking like a float */
    if (f32) strcat(buf, "f");
}

/* 32-bit float targets (C/C++/Java 'float'): round the value; a literal just gets an 'f' */
static char *conv32(char *v, Type from, int from_f32, Type to, int to_f32) {
    if (lang_mode == LANG_MC || to != T_FLOAT) return conv(v, from, to);
    if (to_f32) {
        if (from == T_FLOAT && from_f32) return v;
        if (isdigit((unsigned char)v[0]) || (v[0] == '-' && (isdigit((unsigned char)v[1]) || v[1] == '.'))) {
            if (from == T_FLOAT || from == T_INT) {
                char buf[64];
                float_text((double)(float)strtod(v, NULL), 1, buf, sizeof buf);
                return dup_str(buf);
            }
        }
        char *t = new_temp();
        emit("%s = (float32) %s", t, v);
        return t;
    }
    if (from == T_FLOAT && from_f32) {        /* float -> double keeps the float's value */
        char *t = new_temp();
        emit("%s = (float) %s", t, v);
        return t;
    }
    return conv(v, from, to);
}

static Type promote(Type a, Type b) {
    if (a == T_FLOAT || b == T_FLOAT) return T_FLOAT;
    if (a == T_LONG || b == T_LONG) return T_LONG;
    return T_INT;
}

static Type binop_type(Type l, Type r, const char *op) {
    if (!strcmp(op, "+") && (l == T_STRING || r == T_STRING)) return T_STRING;
    return promote(l, r);
}

/* ---- loops: break / continue targets ---------------------------------- */

typedef struct { int brk, cont, is_switch; const char *name; } Loop;
static Loop loops[512];
static int  nloops;
static const char *pending_label;     /* Java  outer: for (...)  */

static void push_loop(int brk, int cont, int is_switch) {
    if (nloops < 512) {
        loops[nloops].brk = brk; loops[nloops].cont = cont; loops[nloops].is_switch = is_switch;
        loops[nloops].name = is_switch == 1 ? NULL : pending_label;
    }
    if (is_switch != 1) pending_label = NULL;
    nloops++;
}
static Loop *find_loop(const char *name) {
    for (int i = (nloops < 512 ? nloops : 512) - 1; i >= 0; i--)
        if (loops[i].name && strcmp(loops[i].name, name) == 0) return &loops[i];
    return NULL;
}
static void pop_loop(void) { nloops--; }
static int continue_label(void) {
    for (int i = (nloops < 512 ? nloops : 512) - 1; i >= 0; i--) {
        if (loops[i].is_switch) continue;      /* switch (1) and labeled blocks (2) */
        if (!loops[i].cont) loops[i].cont = new_label();   /* allocated on first use */
        return loops[i].cont;
    }
    return 0;
}

/* ---- expressions ------------------------------------------------------ */

static char *gen_expr(Node *n);
static void gen_stmts(Node *n);

static int has_effects(const Node *n) {
    if (!n) return 0;
    switch (n->kind) {
    case N_CALL: case N_INCDEC: case N_ASSIGN: case N_INDEX: case N_FIELD: case N_COND: return 1;
    case N_BINOP:
        if (!strcmp(n->op, "/") || !strcmp(n->op, "%")) return 1;
        return has_effects(n->a) || has_effects(n->b);
    case N_UNOP: case N_CAST: return has_effects(n->a);
    default: return 0;
    }
}

static char *char_literal(long long v) {
    char buf[32];
    if (v == '\'') strcpy(buf, "'\\''");
    else if (v == '\\') strcpy(buf, "'\\\\'");
    else if (v == '\n') strcpy(buf, "'\\n'");
    else if (v == '\t') strcpy(buf, "'\\t'");
    else if (v == '\r') strcpy(buf, "'\\r'");
    else if (v == 0) strcpy(buf, "'\\0'");
    else if (v > 32 && v < 127) snprintf(buf, sizeof buf, "'%c'", (int)v);
    else if (v == 32) strcpy(buf, "'\\040'");
    else if (v >= 0 && v < 256) snprintf(buf, sizeof buf, "'\\%03o'", (unsigned)v);
    else snprintf(buf, sizeof buf, "'\\u%04x'", (unsigned)(v & 0xFFFF));
    return dup_str(buf);
}

/* an lvalue (x or a[i]) evaluated once: load / store helpers */
typedef struct { const char *var; char *base, *idx; Type t; } LV;

static LV lv_of(Node *t) {
    LV lv; memset(&lv, 0, sizeof lv);
    lv.t = t->type;
    if (t->kind == N_ID) lv.var = lookup(t->name);
    else { lv.base = gen_expr(t->a); lv.idx = gen_expr(t->b); }
    return lv;
}
static char *lv_load(LV *lv) {
    if (lv->var) return dup_str(lv->var);
    char *t = new_temp();
    emit("%s = %s[%s]", t, lv->base, lv->idx);
    return t;
}
static void lv_store(LV *lv, const char *v) {
    if (lv->var) emit("%s = %s", lv->var, v);
    else emit("%s[%s] = %s", lv->base, lv->idx, v);
}

static char *gen_assign(Node *n) {
    if (n->name) {
        if (n->type == T_SCANNER) { gen_expr(n->a); return dup_str("0"); }
        char *v = gen_expr(n->a);
        if (n->dims == 0) v = conv32(v, n->a->type, n->a->f32, n->type, n->f32);
        const char *dst = lookup(n->name);
        emit("%s = %s", dst, v);
        return dup_str(dst);
    }
    LV lv = lv_of(n->c);
    if (n->op) {
        char *cur = lv_load(&lv);
        char *v = gen_expr(n->a);
        Type bt = binop_type(lv.t, n->a->type, n->op);
        if (bt == T_FLOAT && lv.t != T_FLOAT && lv.t != T_STRING) cur = to_float(cur);
        else if (bt == T_FLOAT && n->a->type != T_FLOAT) v = to_float(v);
        char *t = new_temp();
        emit("%s = %s %s %s", t, cur, n->op, v);
        v = conv32(t, bt, n->f32 && (n->a->type != T_FLOAT || n->a->f32), lv.t, n->f32);
        lv_store(&lv, v);
        return v;
    }
    char *v = gen_expr(n->a);
    if (n->c->dims == 0) v = conv32(v, n->a->type, n->a->f32, lv.t, n->f32);
    lv_store(&lv, v);
    return v;
}

static char *gen_incdec(Node *n) {
    LV lv = lv_of(n->a);
    char *cur = lv_load(&lv);
    char *old = cur;
    if (!n->flag) { old = new_temp(); emit("%s = %s", old, cur); }    /* postfix keeps the old value */
    char *t = new_temp();
    emit("%s = %s %c 1", t, cur, n->op[0]);
    char *v = conv32(t, promote(lv.t, T_INT), n->f32, lv.t, n->f32);
    lv_store(&lv, v);
    return n->flag ? (lv.var ? dup_str(lv.var) : v) : old;
}

/* evaluates the arguments first, then emits the params (so nested calls do not interleave) */
static void gen_params(Node *args, Node *params, const char *first) {
    char *places[256]; int isref[256]; int n = 0;
    if (first) { isref[n] = 0; places[n++] = dup_str(first); }
    for (Node *a = args; a && n < 256; a = a->next) {
        char *v;
        isref[n] = 0;
        if (params && params->is_ref && (a->kind == N_ID || a->kind == N_INDEX) && a->type == params->type) {
            /* C++ reference parameter: pass the variable itself (copied back on return) */
            if (a->kind == N_ID) v = dup_str(lookup(a->name));
            else {
                char *b = gen_expr(a->a), *i = gen_expr(a->b);
                v = malloc(strlen(b) + strlen(i) + 3);
                sprintf(v, "%s[%s]", b, i);
            }
            isref[n] = 1;
        } else {
            v = gen_expr(a);
            if (params && params->dims == 0 && a->dims == 0) v = conv32(v, a->type, a->f32, params->type, params->f32);
        }
        if (params) params = params->next;
        places[n++] = v;
    }
    for (int i = 0; i < n; i++) emit("%s %s", isref[i] ? "refparam" : "param", places[i]);
}

static char *gen_call(Node *n) {
    int na = list_len(n->b);
    if (n->flag == CALL_NOP) return dup_str("0");
    if (n->flag == CALL_READ) {
        if (n->b && (!strcmp(n->op, "ok") || !strcmp(n->op, "scanfret"))) {
            emit("read mark");          /* while (cin >> x) / scanf(...) == 1 */
            gen_stmts(n->b);
        } else if (!strcmp(n->op, "scanfret")) emit("read mark");
        char *t = new_temp();
        emit("%s = read %s", t, n->op);
        return t;
    }
    if (n->flag == CALL_USER) {
        gen_params(n->b, n->fn ? n->fn->a : NULL, NULL);
        if (n->type == T_VOID) { emit("call %s, %d", n->op, na); return dup_str("0"); }
        char *t = new_temp();
        emit("%s = call %s, %d", t, n->op, na);
        return t;
    }
    /* builtins that are really assignments */
    const char *f = n->op;
    if (!strcmp(f, "swap")) {
        LV a = lv_of(n->b), b = lv_of(n->b->next);
        char *va = lv_load(&a), *vb = lv_load(&b);
        char *tmp = new_temp();
        emit("%s = %s", tmp, va);
        lv_store(&a, conv(vb, b.t, a.t));
        lv_store(&b, conv(tmp, a.t, b.t));
        return dup_str("0");
    }
    if (!strcmp(f, "strcpy") || !strcmp(f, "strcat")) {
        LV a = lv_of(n->b);
        char *v = gen_expr(n->b->next);
        if (!strcmp(f, "strcat")) {
            char *cur = lv_load(&a);
            char *t = new_temp();
            emit("%s = %s + %s", t, cur, v);
            v = t;
        }
        lv_store(&a, v);
        return dup_str("0");
    }
    if (n->c && !strncmp(f, "String.", 7)) {
        /* sb.append(x) / s.push_back(c) / sb.reverse() ...: compute the new text, store it in the variable */
        char *cur = gen_expr(n->a);
        gen_params(n->b, NULL, cur);
        char *t = new_temp();
        emit("%s = call %s, %d", t, f, na + 1);
        LV lv = lv_of(n->c);
        lv_store(&lv, t);
        return t;
    }
    if (!strcmp(f, "setf") || !strcmp(f, "unsetf")) {
        const char *flag = n->b->name;
        if (!strcmp(f, "setf")) emit("manip %s", flag);
        else emit("manip %s", !strcmp(flag, "fixed") || !strcmp(flag, "scientific") ? "defaultfloat" :
                              !strcmp(flag, "boolalpha") ? "noboolalpha" : "right");
        return dup_str("0");
    }
    if (!strcmp(f, "precision")) {
        char *v = gen_expr(n->b);
        emit("manip setprecision %s", v);
        return dup_str("0");
    }
    /* receiver value passed as the first argument: s.length(), arr.clone() ... */
    char *recv = NULL;
    if (n->a && (!strncmp(f, "String.", 7) || !strcmp(f, "len") || !strcmp(f, "Arrays.copy"))) {
        recv = gen_expr(n->a);
        na++;
    }
    gen_params(n->b, NULL, recv);
    if (n->type == T_VOID) { emit("call %s, %d", f, na); return dup_str("0"); }
    char *t = new_temp();
    emit("%s = call %s, %d", t, f, na);
    return t;
}

/* allocate an array for a declaration / initializer and fill it from { ... } */
static void count_sizes(Node *list, int level, int dims, long long *sizes) {
    int n = list_len(list);
    if (n > sizes[level]) sizes[level] = n;
    if (level + 1 < dims)
        for (Node *e = list; e; e = e->next)
            if (e->kind == N_ARRLIT) count_sizes(e->a, level + 1, dims, sizes);
}

static int fill_f32;
static long long fill_sizes[16];     /* known sizes per level, for C brace elision */

/* int m[2][3] = {1, 2, 3, 4};  ->  element k goes to m[k / 3][k % 3] */
static long long fill_flat(const char *place, Node **pe, Type t, int dims, long long *sizes) {
    long long count = 0, inner = 1;
    for (int d = 1; d < dims; d++) inner *= sizes[d] > 0 ? sizes[d] : 1;
    for (long long row = 0; *pe && row < (sizes[0] > 0 ? sizes[0] : 1 << 30); row++) {
        if (dims == 1) {
            Node *e = *pe; *pe = e->next;
            char *v = gen_expr(e);
            v = conv32(v, e->type, e->f32, t, fill_f32);
            emit("%s[%lld] = %s", place, row, v);
            count++;
            continue;
        }
        if ((*pe)->kind == N_ARRLIT) break;
        char *r = new_temp();
        emit("%s = %s[%lld]", r, place, row);
        count += fill_flat(r, pe, t, dims - 1, sizes + 1);
        (void)inner;
    }
    return count;
}

static void fill_array(const char *place, Node *list, Type t, int dims) {
    long long i = 0;
    if (dims > 1 && list && list->kind != N_ARRLIT && lang_mode != LANG_JAVA && fill_sizes[1] > 0) {
        Node *e = list;
        fill_flat(place, &e, t, dims, fill_sizes);
        return;
    }
    for (Node *e = list; e; e = e->next, i++) {
        if (e->kind == N_ARRLIT && dims > 1) {
            char *row = new_temp();
            emit("%s = %s[%lld]", row, place, i);
            fill_array(row, e->a, t, dims - 1);
            continue;
        }
        char *v = gen_expr(e);
        if (dims == 1) v = conv32(v, e->type, e->f32, t, fill_f32);
        emit("%s[%lld] = %s", place, i, v);
    }
}

static char *gen_new_array(Type t, int dims, Node *size_list, Node *init, int f32) {
    long long counted[16] = { 0 };
    if (init) count_sizes(init, 0, dims > 16 ? 16 : dims, counted);
    char buf[1024];
    f32 = f32 && t == T_FLOAT && lang_mode != LANG_MC;
    int len = snprintf(buf, sizeof buf, "newarray %s", f32 ? "float32" : kind_name(t));
    int level = 0;
    Node *s = size_list;
    memset(fill_sizes, 0, sizeof fill_sizes);
    for (; level < dims && level < 16; level++) {
        if (s && !(s->kind == N_INT && s->flag == -1)) {
            if (s->kind == N_INT) fill_sizes[level] = s->ival;
            char *v = gen_expr(s);
            len += snprintf(buf + len, sizeof buf - len > 0 ? sizeof buf - len : 0, " %s", v);
        } else if (init) {
            fill_sizes[level] = counted[level];
            len += snprintf(buf + len, sizeof buf - len > 0 ? sizeof buf - len : 0, " %lld", counted[level]);
        } else break;
        if (s) s = s->next;
    }
    char *t2 = new_temp();
    emit("%s = %s", t2, buf);
    fill_f32 = f32;
    if (init) fill_array(t2, init, t, dims);
    return t2;
}

static char *gen_expr(Node *n) {
    char buf[64];
    switch (n->kind) {
    case N_INT:
        if (n->flag == 1) snprintf(buf, sizeof buf, "%lldL", n->ival);
        else snprintf(buf, sizeof buf, "%lld", n->ival);
        return dup_str(buf);
    case N_CHAR:
        return char_literal(n->ival);
    case N_FLOAT:
        if (lang_mode == LANG_MC) {
            snprintf(buf, sizeof buf, "%g", n->fval);   /* original mini-language format */
            if (strtod(buf, NULL) != n->fval) float_text(n->fval, 0, buf, sizeof buf);
            else if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");
        } else float_text(n->fval, n->f32, buf, sizeof buf);
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
        if (!strcmp(n->op, "+")) return conv(a, n->a->type, n->type);
        char *t = new_temp();
        emit("%s = %s %s", t, n->op, a);
        return t;
    }
    case N_BINOP: {
        if ((!strcmp(n->op, "&&") || !strcmp(n->op, "||")) && has_effects(n->b)) {
            /* short-circuit evaluation: the right side may divide by zero, call, or change a variable */
            int is_and = n->op[0] == '&';
            char *t = new_temp();
            char *l = gen_expr(n->a);
            int lskip = new_label(), lend = new_label();
            if (is_and) {
                emit("%s = false", t);
                emit("ifFalse %s goto L%d", l, lend);
            } else {
                emit("%s = true", t);
                emit("ifFalse %s goto L%d", l, lskip);
                emit("goto L%d", lend);
                emit_label(lskip);
            }
            char *r = gen_expr(n->b);
            emit("%s = %s", t, conv(r, n->b->type, T_BOOL));
            emit_label(lend);
            return t;
        }
        char *l = gen_expr(n->a);
        char *r = gen_expr(n->b);
        /* mixed int/float arithmetic or comparison: widen the int side */
        if (is_numeric(n->a->type) && n->b->type == T_FLOAT) l = n->b->f32 && lang_mode != LANG_MC ? conv32(l, n->a->type, 0, T_FLOAT, 1) : to_float(l);
        else if (n->a->type == T_FLOAT && is_numeric(n->b->type)) r = n->a->f32 && lang_mode != LANG_MC ? conv32(r, n->b->type, 0, T_FLOAT, 1) : to_float(r);
        char *t = new_temp();
        emit("%s = %s %s %s", t, l, n->op, r);
        return t;
    }
    case N_ASSIGN: return gen_assign(n);
    case N_INCDEC: return gen_incdec(n);
    case N_COND: {
        char *c = gen_expr(n->a);
        char *t = new_temp();
        int lf = new_label(), lend = new_label();
        emit("ifFalse %s goto L%d", c, lf);
        char *v1 = gen_expr(n->b);
        if (n->dims == 0) v1 = conv(v1, n->b->type, n->type);
        emit("%s = %s", t, v1);
        emit("goto L%d", lend);
        emit_label(lf);
        char *v2 = gen_expr(n->c);
        if (n->dims == 0) v2 = conv(v2, n->c->type, n->type);
        emit("%s = %s", t, v2);
        emit_label(lend);
        return t;
    }
    case N_CAST: {
        char *v = gen_expr(n->a);
        if (n->type == T_FLOAT && lang_mode != LANG_MC && !n->dims && (n->f32 || n->a->f32))
            return conv32(v, n->a->type, n->a->f32, T_FLOAT, n->f32);
        if (n->dims || n->type == n->a->type || n->type == T_STRING || n->type == T_NONE) return v;
        char *t = new_temp();
        emit("%s = (%s) %s", t, kind_name(n->type), v);
        return t;
    }
    case N_CALL: return gen_call(n);
    case N_INDEX: {
        char *b = gen_expr(n->a);
        char *i = gen_expr(n->b);
        char *t = new_temp();
        emit("%s = %s[%s]", t, b, i);
        return t;
    }
    case N_FIELD: {      /* arr.length */
        char *b = gen_expr(n->a);
        emit("param %s", b);
        char *t = new_temp();
        emit("%s = call len, 1", t);
        return t;
    }
    case N_NEWARR: return gen_new_array(n->type, n->dims, n->a, NULL, n->f32);
    case N_ARRLIT: return gen_new_array(n->type, n->dims, NULL, n->a, n->f32);
    case N_NEWOBJ: return dup_str("0");
    default:
        return dup_str("0");
    }
}

/* ---- statements ------------------------------------------------------- */

/* body of if/while: a block opens its own scope; a lone statement gets one too */
static void gen_body(Node *n) {
    if (!n) return;
    Var *mark = vars;
    depth++;
    gen_stmts(n);
    depth--;
    vars = mark;
}

static void gen_decl(Node *n) {
    declare(n->name);
    const char *x = lookup(n->name);
    if (has_funcs && depth == 0) emit("global %s", x);
    if (n->type == T_SCANNER) return;
    Node *init = n->b;
    if (n->dims > 0) {
        int has_size = n->a && !(n->a->kind == N_INT && n->a->flag == -1);
        if (init && init->kind == N_ARRLIT && !init->flag) {
            char *arr = gen_new_array(n->type, n->dims, n->a, init->a, n->f32);
            emit("%s = %s", x, arr);
        } else if (init) {
            char *v = gen_expr(init);
            emit("%s = %s", x, v);
        } else if (has_size) {
            char *arr = gen_new_array(n->type, n->dims, n->a, NULL, n->f32);
            emit("%s = %s", x, arr);
        }
        return;
    }
    if (init) {
        if (init->kind == N_ARRLIT) init = init->a;         /* int x = {5};  char s[] = {"hi"} */
        if (!init) return;
        char *v = gen_expr(init);
        v = conv32(v, init->type, init->f32, n->type, n->f32);
        emit("%s = %s", x, v);
        return;
    }
    if (lang_mode == LANG_MC) return;
    switch (n->type) {          /* typed default so later arithmetic uses the right kind */
    case T_FLOAT:  emit("%s = 0.0%s", x, n->f32 ? "f" : ""); break;
    case T_STRING: emit("%s = \"\"", x); break;
    case T_CHAR:   emit("%s = '\\0'", x); break;
    case T_BOOL:   emit("%s = false", x); break;
    case T_LONG:   emit("%s = 0L", x); break;
    default: break;
    }
}

static void gen_read(Node *n) {
    if (!n->a) { emit("read %s", n->op ? n->op : "ignore1"); return; }
    if (n->a->kind == N_ID) { emit("%s = read %s", lookup(n->a->name), n->op); return; }
    LV lv = lv_of(n->a);
    char *t = new_temp();
    emit("%s = read %s", t, n->op);
    lv_store(&lv, t);
}

static void gen_stmts(Node *n) {
    for (; n; n = n->next) {
        switch (n->kind) {
        case N_DECL:
            gen_decl(n);
            break;

        case N_ASSIGN:
            gen_assign(n);
            break;
        case N_EXPRSTMT:
            gen_expr(n->a);
            break;
        case N_RETURN: {
            if (n->a) {
                char *v = gen_expr(n->a);
                if (n->fn && n->a->dims == 0) v = conv32(v, n->a->type, n->a->f32, n->fn->type, n->fn->f32);
                emit("return %s", v);
            }
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
        case N_WRITE: {
            char *v = gen_expr(n->a);
            emit("write %s", v);
            break;
        }
        case N_READ:
            gen_read(n);
            break;
        case N_MANIP:
            if (n->a) { char *v = gen_expr(n->a); emit("manip %s %s", n->name, v); }
            else emit("manip %s", n->name);
            break;
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
            push_loop(lend, lstart, 0);
            gen_body(n->b);
            pop_loop();
            emit("goto L%d", lstart);
            emit_label(lend);
            break;
        }
        case N_DOWHILE: {
            int lstart = new_label(), lend = new_label();
            emit_label(lstart);
            push_loop(lend, 0, 0);
            gen_body(n->b);
            int lcont = loops[nloops - 1 < 512 ? nloops - 1 : 511].cont;
            pop_loop();
            if (lcont) emit_label(lcont);
            char *c = gen_expr(n->a);
            emit("ifFalse %s goto L%d", c, lend);
            emit("goto L%d", lstart);
            emit_label(lend);
            break;
        }
        case N_FOR: {
            /* for (init; cond; step) body  ==>  init; Ls: ifFalse cond goto Le; body; [Lc:] step; goto Ls; Le: */
            Var *mark = vars;
            depth++;
            gen_stmts(n->a);
            int lstart = new_label(), lend = new_label();
            emit_label(lstart);
            char *c = gen_expr(n->b);
            emit("ifFalse %s goto L%d", c, lend);
            push_loop(lend, 0, 0);
            gen_body(n->d);
            int lcont = loops[nloops - 1 < 512 ? nloops - 1 : 511].cont;
            pop_loop();
            if (lcont) emit_label(lcont);
            gen_stmts(n->c);
            emit("goto L%d", lstart);
            emit_label(lend);
            depth--;
            vars = mark;
            break;
        }
        case N_FOREACH: {
            Var *mark = vars;
            depth++;
            char *coll = gen_expr(n->a);
            char *ti = new_temp(), *tn = new_temp();
            emit("%s = 0", ti);
            emit("param %s", coll);
            emit("%s = call len, 1", tn);
            int lstart = new_label(), lend = new_label();
            emit_label(lstart);
            char *c = new_temp();
            emit("%s = %s < %s", c, ti, tn);
            emit("ifFalse %s goto L%d", c, lend);
            declare(n->name);
            char *e = new_temp();
            emit("%s = %s[%s]", e, coll, ti);
            Type et = n->a->dims > 0 ? n->a->type : T_CHAR;
            if (n->dims == 0) e = conv(e, et, n->type);
            emit("%s = %s", lookup(n->name), e);
            push_loop(lend, 0, 0);
            gen_body(n->b);
            int lcont = loops[nloops - 1 < 512 ? nloops - 1 : 511].cont;
            pop_loop();
            if (lcont) emit_label(lcont);
            if (n->is_ref && (n->a->dims > 0 || n->a->kind == N_ID))       /* for (auto &x : arr) / (char &c : s) */
                emit("%s[%s] = %s", coll, ti, conv(dup_str(lookup(n->name)), n->type, n->a->dims > 0 ? n->a->type : T_CHAR));
            emit("%s = %s + 1", ti, ti);
            emit("goto L%d", lstart);
            emit_label(lend);
            depth--;
            vars = mark;
            break;
        }
        case N_SWITCH: {
            char *v = gen_expr(n->a);
            int lend = new_label(), ldefault = 0;
            /* one label per case, then the dispatch tests */
            for (Node *c = n->b; c; c = c->next)
                if (c->kind == N_CASE) { c->ival = new_label(); if (!c->a) ldefault = (int)c->ival; }
            for (Node *c = n->b; c; c = c->next) {
                if (c->kind != N_CASE || !c->a) continue;
                char *cv = gen_expr(c->a);
                char *t = new_temp();
                emit("%s = %s != %s", t, v, cv);
                emit("ifFalse %s goto L%lld", t, c->ival);
            }
            emit("goto L%d", ldefault ? ldefault : lend);
            Var *mark = vars;
            depth++;
            push_loop(lend, 0, 1);
            gen_stmts(n->b);
            pop_loop();
            depth--;
            vars = mark;
            emit_label(lend);
            break;
        }
        case N_CASE:
            emit_label((int)n->ival);
            break;
        case N_BREAK:
            if (n->name) { Loop *lp = find_loop(n->name); if (lp) emit("goto L%d", lp->brk); }
            else {
                /* innermost loop or switch (a labeled plain block is not a target of a bare break) */
                for (int i = (nloops < 512 ? nloops : 512) - 1; i >= 0; i--)
                    if (loops[i].is_switch != 2) { emit("goto L%d", loops[i].brk); break; }
            }
            break;
        case N_CONTINUE: {
            int l = 0;
            if (n->name) {
                Loop *lp = find_loop(n->name);
                if (lp) { if (!lp->cont) lp->cont = new_label(); l = lp->cont; }
            } else l = continue_label();
            if (l) emit("goto L%d", l);
            break;
        }
        case N_LABELED: {
            NodeKind k = n->a ? n->a->kind : N_BLOCK;
            if (k == N_WHILE || k == N_FOR || k == N_DOWHILE || k == N_FOREACH) {
                pending_label = n->name;
                gen_stmts(n->a);
            } else {
                int lend = new_label();
                pending_label = n->name;
                push_loop(lend, 0, 2);
                gen_stmts(n->a);
                pop_loop();
                emit_label(lend);
            }
            break;
        }
        case N_BLOCK: {
            Var *mark = vars;
            depth++;
            gen_stmts(n->a);
            depth--;
            vars = mark;
            break;
        }
        case N_FUNC:          /* generated separately (see tac_generate) */
        default:
            break;
        }
    }
}

static void declare_params(Node *f, int emit_formals) {
    for (Node *p = f->a; p; p = p->next) {
        declare(p->name);
        if (emit_formals) emit("formal %s", lookup(p->name));
    }
}

void tac_generate(const Node *root_c, FILE *f) {
    Node *root = (Node *)root_c;
    out = f;
    temp_count = label_count = 0;
    vars = NULL;
    used = NULL;
    nloops = 0;
    depth = 0;
    has_funcs = 0;
    if (!root) return;

    Node *main_fn = NULL;
    for (Node *n = root->a; n; n = n->next)
        if (n->kind == N_FUNC) {
            if (!main_fn && strcmp(n->name, "main") == 0) main_fn = n;
            else has_funcs = 1;
        }

    /* 1. global declarations and top-level statements, in source order */
    gen_stmts(root->a);

    /* 2. main's body runs next, as a plain block */
    if (main_fn) {
        Var *mark = vars;
        depth++;
        declare_params(main_fn, 0);
        gen_stmts(main_fn->b);
        depth--;
        vars = mark;
    }

    /* 3. the other functions, after a halt */
    if (has_funcs) {
        emit("halt");
        for (Node *n = root->a; n; n = n->next) {
            if (n->kind != N_FUNC || n == main_fn) continue;
            Var *mark = vars;
            depth++;
            fprintf(out, "function %s:\n", n->op ? n->op : n->name);
            declare_params(n, 1);
            gen_stmts(n->b->a);
            fprintf(out, "end %s\n", n->op ? n->op : n->name);
            depth--;
            vars = mark;
        }
    }
}
