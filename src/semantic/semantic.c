/* Lexify semantic analyzer
 * Detects: undeclared variable, redeclaration, scope violation,
 *          type mismatch, invalid assignment, invalid expression,
 *          plus the C / C++ / Java checks (break outside loop, const assignment,
 *          unknown function, wrong argument count, bad array use ...).
 *
 * The mini language (.mc) keeps its strict rules (bool conditions, no int<->bool).
 * C and C++ follow C's rules (any scalar is a condition, numbers convert freely).
 * Java follows Java's rules (boolean conditions, no narrowing without a cast). */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "semantic/semantic.h"
#include "symbol_table/symbol_table.h"

static int errors = 0;
static int loop_depth = 0, switch_depth = 0;
static Node *cur_func = NULL;
static struct { const char *name; int is_loop; } labels[128];
static int nlabels_;

static void err(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "Semantic error at line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    errors++;
}

static int strict(void) { return lang_mode == LANG_MC; }
static int java(void)   { return lang_mode == LANG_JAVA; }
static int is(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) { fprintf(stderr, "out of memory\n"); exit(2); } return p; }

/* mini language: only int and float are numbers */
static int is_num_strict(Type t) { return t == T_INT || t == T_FLOAT; }
/* C / C++ / Java numbers (char is a small integer; bool counts in C/C++ only) */
static int is_num(Type t) {
    if (strict()) return is_num_strict(t);
    return t == T_INT || t == T_FLOAT || t == T_CHAR || t == T_LONG || (t == T_BOOL && !java());
}
static int is_integral(Type t) { return t == T_INT || t == T_CHAR || t == T_LONG || (t == T_BOOL && !java()); }
static Type promote(Type a, Type b) {
    if (a == T_FLOAT || b == T_FLOAT) return T_FLOAT;
    if (a == T_LONG || b == T_LONG) return T_LONG;
    return T_INT;
}
static const char *tn(const Node *n) { return type_name_dims(n->type, n->dims); }

/* turn an identifier / field into a literal (INT_MAX, Math.PI ...) */
static Type make_int(Node *n, long long v, int is_long) {
    n->kind = N_INT; n->ival = v; n->flag = is_long; n->dims = 0; n->a = NULL;
    return n->type = is_long ? T_LONG : T_INT;
}
static Type make_float(Node *n, double v) { n->kind = N_FLOAT; n->fval = v; n->dims = 0; n->a = NULL; return n->type = T_FLOAT; }

static int builtin_constant(Node *n, const char *name) {
    static const struct { const char *name; long long v; int is_long; } ic[] = {
        { "INT_MAX", 2147483647LL, 0 }, { "INT_MIN", -2147483648LL, 0 },
        { "LONG_MAX", 9223372036854775807LL, 1 }, { "LONG_MIN", (-9223372036854775807LL - 1), 1 },
        { "LLONG_MAX", 9223372036854775807LL, 1 }, { "LLONG_MIN", (-9223372036854775807LL - 1), 1 },
        { "UINT_MAX", 4294967295LL, 1 }, { "SHRT_MAX", 32767, 0 }, { "SHRT_MIN", -32768, 0 },
        { "CHAR_MAX", 127, 0 }, { "CHAR_MIN", -128, 0 }, { "UCHAR_MAX", 255, 0 },
        { "RAND_MAX", 2147483647LL, 0 }, { "EOF", -1, 0 }, { "NULL", 0, 0 }, { "nullptr", 0, 0 },
        { "npos", -1, 0 }, { "EXIT_SUCCESS", 0, 0 }, { "EXIT_FAILURE", 1, 0 },
        { NULL, 0, 0 } };
    for (int i = 0; ic[i].name; i++)
        if (is(name, ic[i].name)) { make_int(n, ic[i].v, ic[i].is_long); return 1; }
    if (is(name, "M_PI")) { make_float(n, 3.14159265358979323846); return 1; }
    if (is(name, "M_E"))  { make_float(n, 2.71828182845904523536); return 1; }
    if (is(name, "null")) { make_int(n, 0, 0); n->type = T_NONE; return 1; }
    return 0;
}

/* undeclared variable vs. scope violation */
static void report_unknown(const char *name, int line) {
    Symbol *dead = st_find_inactive(name);
    if (dead)
        err(line, "scope violation: '%s' (declared at line %d) is not visible here", name, dead->line);
    else
        err(line, "undeclared variable '%s'", name);
}

/* ---- functions (collected before checking so they can be called in any order) ---- */
#define MAX_FUNCS 512
static Node *funcs[MAX_FUNCS];
static int   nfuncs;

static void collect_funcs(Node *list) {
    for (Node *n = list; n; n = n->next) {
        if (n->kind != N_FUNC) continue;
        if (nfuncs == MAX_FUNCS) { err(n->line, "too many functions"); return; }
        int same = 0;
        for (int i = 0; i < nfuncs; i++) if (is(funcs[i]->name, n->name)) same++;
        char buf[300];
        if (same) snprintf(buf, sizeof buf, "%s_%d", n->name, same);   /* overloads get unique TAC names */
        else snprintf(buf, sizeof buf, "%s", n->name);
        n->op = xstrdup(buf);
        funcs[nfuncs++] = n;
    }
}

static Type check_expr(Node *n);
static void check_stmts(Node *n);
static void check_read(Node *n);

/* can a value of type vt/vd be stored in a tt/td variable?  val is used for Java constant rules */
static int assignable(Type tt, int td, Type vt, int vd, Node *val, int compound) {
    if (tt == T_NONE || vt == T_NONE || tt == T_AUTO) return 1;
    if (td != vd) return 0;
    if (td > 0) return tt == vt;
    if (tt == vt) return 1;
    if (strict()) return tt == T_FLOAT && vt == T_INT;
    if (tt == T_SCANNER || vt == T_SCANNER) return 0;
    if (tt == T_STRING) return vt == T_STRING || (lang_mode == LANG_CPP && vt == T_CHAR);
    if (vt == T_STRING) return 0;
    if (vt == T_VOID) return 0;
    if (!java()) return is_num(tt) && is_num(vt);
    /* Java */
    if (tt == T_BOOL || vt == T_BOOL) return 0;
    if (compound) return 1;                               /* x += 1.5 is legal Java */
    switch (tt) {
    case T_FLOAT: return 1;
    case T_LONG:  return vt != T_FLOAT;
    case T_INT:   return vt == T_CHAR || (val && val->kind == N_INT && !val->flag);
    case T_CHAR:  return val && (val->kind == N_INT || val->kind == N_CHAR) && val->ival >= 0 && val->ival <= 65535;
    default:      return 0;
    }
}

static int is_lvalue(Node *t) { return t && (t->kind == N_ID || t->kind == N_INDEX); }

/* Java: Main.count (a static field of a class in this file) is just the variable count */
static Node *norm_lvalue(Node *t) {
    if (t && t->kind == N_FIELD && t->a && t->a->kind == N_ID && !st_lookup(t->a->name) && is_class_name(t->a->name)) {
        t->kind = N_ID; t->a = NULL;
    }
    return t;
}

/* Java: double -> float needs a cast (float f = 1.5; is an error, 1.5f is fine) */
static void java_float_check(int target_f32, Type tt, int td, Node *val, int compound, int line, const char *what) {
    if (!java() || !target_f32 || compound || tt != T_FLOAT || td) return;
    if (val->type == T_FLOAT && val->dims == 0 && !val->f32)
        err(line, "incompatible types: possible lossy conversion from double to float (variable '%s', use 1.5f or a cast)", what);
}

/* the variable symbol behind an lvalue (a, a[i], a[i][j]) */
static Symbol *lvalue_symbol(Node *t) {
    while (t && t->kind == N_INDEX) t = t->a;
    return (t && t->kind == N_ID) ? t->sym : NULL;
}

static void check_condition(Node *cond, const char *what) {
    Type t = check_expr(cond);
    if (t == T_NONE) return;
    if (strict()) {
        if (t != T_BOOL) err(cond->line, "condition of '%s' must be bool, got '%s'", what, type_name(t));
        return;
    }
    if (cond->dims) { err(cond->line, "condition of '%s' cannot be an array", what); return; }
    if (java() && t != T_BOOL) {
        err(cond->line, "incompatible types: %s cannot be converted to boolean (condition of '%s')", type_name(t), what);
        return;
    }
    if (t == T_STRING || t == T_VOID || t == T_SCANNER)
        err(cond->line, "condition of '%s' must be a number or bool, got '%s'", what, type_name(t));
}

/* ---------------------------------------------------------------- calls */

static Node *arg(Node *args, int i) { while (args && i--) args = args->next; return args; }

static int args_numeric(Node *call, Node *args, int from) {
    int ok = 1;
    for (Node *a = arg(args, from); a; a = a->next)
        if (a->type != T_NONE && (!is_num(a->type) || a->dims)) {
            err(call->line, "function '%s' needs numeric arguments, got '%s'", call->name, tn(a));
            ok = 0;
        }
    return ok;
}

static int want_args(Node *call, int n, int lo, int hi) {
    if (n < lo || n > hi) {
        if (lo == hi) err(call->line, "function '%s' expects %d argument(s), got %d", call->name, lo, n);
        else err(call->line, "function '%s' expects %d to %d arguments, got %d", call->name, lo, hi, n);
        return 0;
    }
    return 1;
}

static Type set_builtin(Node *n, const char *canon, Type t) { n->flag = CALL_BUILTIN; n->op = canon; n->dims = 0; return n->type = t; }
static Type set_nop(Node *n) { n->flag = CALL_NOP; n->dims = 0; return n->type = T_VOID; }
static Type set_read(Node *n, const char *kind, Type t) { n->flag = CALL_READ; n->op = kind; n->dims = 0; return n->type = t; }

/* Math.xxx / sqrt / pow ... (shared by C, C++ and Java) */
static int math_call(Node *n, const char *f, int na, Type *out) {
    static const char *f1[] = { "sqrt", "floor", "ceil", "exp", "log", "log10", "log2", "sin", "cos", "tan", "asin",
                                "acos", "atan", "cbrt", "trunc", "sinh", "cosh", "tanh", "toRadians", "toDegrees", "signum", NULL };
    for (int i = 0; f1[i]; i++)
        if (is(f, f1[i])) { if (want_args(n, na, 1, 1)) args_numeric(n, n->b, 0); *out = set_builtin(n, f1[i], T_FLOAT); return 1; }
    static const char *f2[] = { "pow", "atan2", "hypot", "fmod", NULL };
    for (int i = 0; f2[i]; i++)
        if (is(f, f2[i])) { if (want_args(n, na, 2, 2)) args_numeric(n, n->b, 0); *out = set_builtin(n, f2[i], T_FLOAT); return 1; }
    if (is(f, "fabs")) { if (want_args(n, na, 1, 1)) args_numeric(n, n->b, 0); *out = set_builtin(n, "abs", T_FLOAT); return 1; }
    if (is(f, "abs") || is(f, "labs") || is(f, "llabs")) {
        Type t = T_INT;
        if (want_args(n, na, 1, 1) && args_numeric(n, n->b, 0)) t = promote(n->b->type, T_INT);
        *out = set_builtin(n, "abs", t);
        n->f32 = t == T_FLOAT && n->b && n->b->f32;
        return 1;
    }
    if (is(f, "max") || is(f, "min") || is(f, "fmax") || is(f, "fmin")) {
        Type t = T_INT;
        if (want_args(n, na, 2, 2) && args_numeric(n, n->b, 0)) {
            Type a = n->b->type, b = n->b->next->type;
            t = (a == T_CHAR && b == T_CHAR) ? T_CHAR : promote(a, b);
        }
        if (f[0] == 'f') t = T_FLOAT;
        *out = set_builtin(n, f[1] == 'a' || f[2] == 'a' ? "max" : "min", t);
        n->f32 = t == T_FLOAT && f[0] != 'f' && n->b && n->b->next &&
                 (n->b->type != T_FLOAT || n->b->f32) && (n->b->next->type != T_FLOAT || n->b->next->f32);
        return 1;
    }
    if (is(f, "round")) {
        if (want_args(n, na, 1, 1)) args_numeric(n, n->b, 0);
        *out = java() ? set_builtin(n, "lround", n->b && n->b->type == T_FLOAT && !n->b->f32 ? T_LONG : T_INT) : set_builtin(n, "round", T_FLOAT);
        return 1;
    }
    if (is(f, "lround") || is(f, "llround")) { if (want_args(n, na, 1, 1)) args_numeric(n, n->b, 0); *out = set_builtin(n, "lround", T_LONG); return 1; }
    if (is(f, "random") && na == 0) { *out = set_builtin(n, "random", T_FLOAT); return 1; }
    if (is(f, "floorDiv") || is(f, "floorMod")) { if (want_args(n, na, 2, 2)) args_numeric(n, n->b, 0); *out = set_builtin(n, f, promote(n->b ? n->b->type : T_INT, T_INT)); return 1; }
    return 0;
}

/* sqrt(x), strlen(s), toupper(c), to_string(x), user functions ... */
static Type check_free_call(Node *n, int na) {
    const char *f = n->name;
    Type t;
    if (n->flag == -2 && is(f, "string")) {          /* C++: string(5, '*') */
        if (want_args(n, na, 2, 2) && n->b->type != T_NONE && !is_integral(n->b->type)) err(n->line, "string(count, char) needs a count");
        return set_builtin(n, "String.fill", T_STRING);
    }
    if (n->flag == -1 && is(f, "String")) {          /* Java: new String(...) */
        want_args(n, na, 0, 1);
        if (na == 1 && n->b->type == T_CHAR && n->b->dims == 1) return set_builtin(n, "String.fromChars", T_STRING);
        if (na == 1 && n->b->type != T_STRING && n->b->type != T_NONE) err(n->line, "new String(...) needs a String or a char[]");
        return set_builtin(n, na ? "String.self" : "String.empty", T_STRING);
    }
    /* user-defined function: pick the overload whose parameters fit best */
    Node *best = NULL; int best_score = -1, found = 0;
    for (int i = 0; i < nfuncs; i++) {
        if (!is(funcs[i]->name, f)) continue;
        found = 1;
        Node *fn = funcs[i];
        if (list_len(fn->a) != na) continue;
        int score = 1;
        Node *p = fn->a, *a = n->b;
        for (; p && a; p = p->next, a = a->next) {
            if (a->type == p->type && a->dims == p->dims) score += 2;
            else if (!assignable(p->type, p->dims, a->type, a->dims, a, 0)) { score = -1; break; }
        }
        if (score > best_score) { best_score = score; best = fn; }
    }
    if (found) {
        if (!best) {
            for (int i = 0; i < nfuncs; i++)
                if (is(funcs[i]->name, f)) {
                    if (list_len(funcs[i]->a) != na)
                        err(n->line, "function '%s' expects %d argument(s), got %d", f, list_len(funcs[i]->a), na);
                    else
                        err(n->line, "no matching call to '%s': argument types do not match its parameters", f);
                    break;
                }
            n->flag = CALL_USER; return n->type = T_NONE;
        }
        n->flag = CALL_USER; n->fn = best; n->op = best->op;
        n->dims = best->dims; n->f32 = best->f32;
        for (Node *p = best->a, *a = n->b; p && a; p = p->next, a = a->next)
            if (p->is_ref && !p->is_const && !is_lvalue(a))
                err(n->line, "cannot pass a temporary value to reference parameter '%s' of '%s' (use a variable)", p->name, f);
        return n->type = best->type;
    }
    if (math_call(n, f, na, &t)) return t;
    if (is(f, "rand")) { want_args(n, na, 0, 0); return set_builtin(n, "rand", T_INT); }
    if (is(f, "time")) return set_builtin(n, "time", T_LONG);
    if (is(f, "clock")) return set_builtin(n, "time", T_LONG);
    if (is(f, "exit")) { want_args(n, na, 0, 1); return set_builtin(n, "exit", T_VOID); }
    if (is(f, "srand") || is(f, "fflush") || is(f, "system") || is(f, "getch") || is(f, "getche") ||
        is(f, "clrscr") || is(f, "sync_with_stdio") || is(f, "setlocale") || is(f, "free"))
        return set_nop(n);
    if (is(f, "getchar")) { want_args(n, na, 0, 0); return set_read(n, "rawchar", T_INT); }
    if (is(f, "strlen")) {
        if (want_args(n, na, 1, 1) && n->b->type != T_STRING && n->b->type != T_NONE) err(n->line, "strlen needs a string, got '%s'", tn(n->b));
        return set_builtin(n, "String.length", T_INT);
    }
    if (is(f, "strcmp")) { want_args(n, na, 2, 2); return set_builtin(n, "strcmp", T_INT); }
    if (is(f, "strcpy") || is(f, "strcat")) {
        if (want_args(n, na, 2, 2) && !is_lvalue(n->b)) err(n->line, "first argument of %s must be a string variable", f);
        return set_builtin(n, f, T_VOID);
    }
    if (is(f, "swap")) {
        if (want_args(n, na, 2, 2) && (!is_lvalue(n->b) || !is_lvalue(n->b->next))) err(n->line, "swap needs two variables");
        return set_builtin(n, "swap", T_VOID);
    }
    if ((is(f, "sort") || is(f, "reverse") || is(f, "count")) && na >= 2) {
        Node *a0 = n->b, *a1 = a0->next, *a2 = a1->next;
        /* sort(arr, arr + n [, greater<int>()])  /  reverse(arr, arr + n) */
        if (a0->kind == N_ID && a1->kind == N_BINOP && is(a1->op, "+") && a1->a->kind == N_ID && is(a1->a->name, a0->name)) {
            check_expr(a0);
            Type t = check_expr(a1->b);
            if (a0->type != T_NONE && a0->dims != 1) err(n->line, "%s needs a one-dimensional array", f);
            if (t != T_NONE && !is_integral(t)) err(n->line, "%s: the end must be array + count", f);
            Node *zero = mk_int(0, n->line); zero->type = T_INT;
            Node *end = a1->b; end->next = NULL;
            a0->next = zero; zero->next = end;
            int desc = a2 && a2->kind == N_ID && is(a2->name, "__greater__");
            if (is(f, "count")) {
                if (!a2) { err(n->line, "count needs a value to look for"); return n->type = T_NONE; }
                check_expr(a2);
                end->next = a2; a2->next = NULL;
                return set_builtin(n, "Arrays.count", T_INT);
            }
            return set_builtin(n, is(f, "reverse") ? "Arrays.reverse" : desc ? "Arrays.sortDesc" : "Arrays.sort", T_VOID);
        }
        /* sort(s.begin(), s.end())  /  reverse(s.begin(), s.end()) on a string */
        if (a0->kind == N_CALL && a1->kind == N_CALL && is(a0->name, "begin") && is(a1->name, "end") &&
            a0->a && a1->a && a0->a->kind == N_ID && a1->a->kind == N_ID && is(a0->a->name, a1->a->name)) {
            Node *x = a0->a;
            Type t = check_expr(x);
            if (t != T_NONE && (t != T_STRING || x->dims)) err(n->line, "%s(begin, end) is supported for strings and arrays", f);
            int desc = a2 && a2->kind == N_ID && is(a2->name, "__greater__");
            if (is(f, "count")) {
                if (!a2) { err(n->line, "count needs a value to look for"); return n->type = T_NONE; }
                check_expr(a2);
                n->a = x; n->b = a2; a2->next = NULL;
                return set_builtin(n, "String.count", T_INT);
            }
            n->a = x; n->c = x; n->b = NULL;
            return set_builtin(n, is(f, "reverse") ? "String.reverse" : desc ? "String.sortDesc" : "String.sort", T_VOID);
        }
    }
    if (is(f, "toupper") || is(f, "tolower")) { want_args(n, na, 1, 1); return set_builtin(n, f, T_INT); }
    static const char *cls[] = { "isdigit", "isalpha", "isalnum", "isupper", "islower", "isspace", "ispunct", NULL };
    for (int i = 0; cls[i]; i++) if (is(f, cls[i])) { want_args(n, na, 1, 1); return set_builtin(n, cls[i], lang_mode == LANG_CPP ? T_BOOL : T_INT); }
    if (is(f, "atoi") || is(f, "stoi")) { want_args(n, na, 1, 1); return set_builtin(n, "parseInt", T_INT); }
    if (is(f, "atol") || is(f, "atoll") || is(f, "stol") || is(f, "stoll")) { want_args(n, na, 1, 1); return set_builtin(n, "parseLong", T_LONG); }
    if (is(f, "atof") || is(f, "stod") || is(f, "stof")) { want_args(n, na, 1, 1); return set_builtin(n, "parseDouble", T_FLOAT); }
    if (is(f, "to_string")) { want_args(n, na, 1, 1); return set_builtin(n, "toString", T_STRING); }
    if (is(f, "getline")) { err(n->line, "use getline(cin, variable) as a statement"); return n->type = T_NONE; }
    if (is(f, "scanf") && n->b && n->b->kind == N_STR) {    /* while (scanf("%d", &x) == 1) */
        Node *reads = mk_scanf(n->b->name, n->b->next, n->line);
        if (reads->kind == N_BLOCK) reads = NULL;
        for (Node *r = reads; r; r = r->next) check_read(r);
        n->b = reads; n->flag = CALL_READ; n->op = "scanfret"; n->dims = 0;
        return n->type = T_INT;
    }
    if (is(f, "printf") || is(f, "scanf") || is(f, "puts") || is(f, "putchar")) {
        err(n->line, "'%s' must be used as a statement here", f);
        return n->type = T_NONE;
    }
    err(n->line, "undeclared function '%s'", f);
    return n->type = T_NONE;
}

/* obj.method(...): Math.sqrt, Integer.parseInt, sc.nextInt, s.length, cin.get ... */
static Type check_method_call(Node *n, int na) {
    const char *f = n->name;
    Node *r = n->a;
    Type t;
    /* Helper.square(3) / Main.f(): a method of a class in this file */
    if (r->kind == N_ID && !st_lookup(r->name) && is_class_name(r->name)) { n->a = NULL; return check_free_call(n, na); }
    /* static classes: the receiver is an identifier that is not a variable */
    if (r->kind == N_ID && !st_lookup(r->name)) {
        const char *c = r->name;
        if (is(c, "Math")) {
            if (math_call(n, f, na, &t)) return t;
        } else if (is(c, "Integer") || is(c, "Long") || is(c, "Short") || is(c, "Byte")) {
            if (is(f, "parseInt") || is(f, "valueOf") || is(f, "parseShort") || is(f, "parseByte"))
                { want_args(n, na, 1, 2); return set_builtin(n, "parseInt", is(c, "Long") ? T_LONG : T_INT); }
            if (is(f, "parseLong")) { want_args(n, na, 1, 1); return set_builtin(n, "parseLong", T_LONG); }
            if (is(f, "toString")) { want_args(n, na, 1, 1); return set_builtin(n, "toString", T_STRING); }
            if (is(f, "toBinaryString")) { want_args(n, na, 1, 1); return set_builtin(n, "toBinary", T_STRING); }
            if (is(f, "toHexString")) { want_args(n, na, 1, 1); return set_builtin(n, "toHex", T_STRING); }
            if (is(f, "toOctalString")) { want_args(n, na, 1, 1); return set_builtin(n, "toOctal", T_STRING); }
            if (is(f, "max") || is(f, "min") || is(f, "abs")) { if (math_call(n, f, na, &t)) return t; }
            if (is(f, "sum")) { want_args(n, na, 2, 2); n->b->next->next = NULL; return set_builtin(n, "sum", is(c, "Long") ? T_LONG : T_INT); }
        } else if (is(c, "Double") || is(c, "Float")) {
            if (is(f, "parseDouble") || is(f, "parseFloat") || is(f, "valueOf")) { want_args(n, na, 1, 1); return set_builtin(n, "parseDouble", T_FLOAT); }
            if (is(f, "toString")) { want_args(n, na, 1, 1); return set_builtin(n, "toString", T_STRING); }
            if (is(f, "max") || is(f, "min") || is(f, "abs")) { if (math_call(n, f, na, &t)) return t; }
        } else if (is(c, "String")) {
            if (is(f, "valueOf") || is(f, "copyValueOf")) { want_args(n, na, 1, 1); return set_builtin(n, "toString", T_STRING); }
            if (is(f, "format")) {
                if (na < 1 || n->b->type != T_STRING) err(n->line, "String.format needs a format string");
                return set_builtin(n, "format", T_STRING);
            }
            if (is(f, "join")) { err(n->line, "String.join is not supported"); return n->type = T_NONE; }
        } else if (is(c, "Character")) {
            if (is(f, "isDigit")) { want_args(n, na, 1, 1); return set_builtin(n, "isdigit", T_BOOL); }
            if (is(f, "isLetter") || is(f, "isAlphabetic")) { want_args(n, na, 1, 1); return set_builtin(n, "isalpha", T_BOOL); }
            if (is(f, "isLetterOrDigit")) { want_args(n, na, 1, 1); return set_builtin(n, "isalnum", T_BOOL); }
            if (is(f, "isUpperCase")) { want_args(n, na, 1, 1); return set_builtin(n, "isupper", T_BOOL); }
            if (is(f, "isLowerCase")) { want_args(n, na, 1, 1); return set_builtin(n, "islower", T_BOOL); }
            if (is(f, "isWhitespace") || is(f, "isSpaceChar")) { want_args(n, na, 1, 1); return set_builtin(n, "isspace", T_BOOL); }
            if (is(f, "toUpperCase")) { want_args(n, na, 1, 1); return set_builtin(n, "Char.toUpper", T_CHAR); }
            if (is(f, "toLowerCase")) { want_args(n, na, 1, 1); return set_builtin(n, "Char.toLower", T_CHAR); }
            if (is(f, "getNumericValue")) { want_args(n, na, 1, 1); return set_builtin(n, "Char.numeric", T_INT); }
            if (is(f, "toString") || is(f, "valueOf")) { want_args(n, na, 1, 1); return set_builtin(n, "toString", T_STRING); }
        } else if (is(c, "Boolean")) {
            if (is(f, "parseBoolean")) { want_args(n, na, 1, 1); return set_builtin(n, "parseBool", T_BOOL); }
        } else if (is(c, "System")) {
            if (is(f, "exit")) { want_args(n, na, 1, 1); return set_builtin(n, "exit", T_VOID); }
            if (is(f, "currentTimeMillis") || is(f, "nanoTime")) return set_builtin(n, "time", T_LONG);
        } else if (is(c, "Arrays")) {
            if (is(f, "sort")) {
                if (want_args(n, na, 1, 3) && n->b->dims != 1 && n->b->type != T_NONE) err(n->line, "Arrays.sort needs a one-dimensional array");
                return set_builtin(n, "Arrays.sort", T_VOID);
            }
            if (is(f, "toString")) { want_args(n, na, 1, 1); return set_builtin(n, "Arrays.toString", T_STRING); }
            if (is(f, "fill")) { want_args(n, na, 2, 2); return set_builtin(n, "Arrays.fill", T_VOID); }
        } else if (is(c, "cin")) {
            if (is(f, "get")) { if (na == 0) return set_read(n, "rawchar", T_INT); }
            if (is(f, "ignore") || is(f, "tie") || is(f, "sync_with_stdio") || is(f, "clear")) return set_nop(n);
            if (is(f, "eof") || is(f, "fail")) return set_read(n, "eof", T_BOOL);
        } else if (is(c, "cout") || is(c, "cerr")) {
            if (is(f, "tie") || is(f, "flush") || is(f, "sync_with_stdio")) return set_nop(n);
            if (is(f, "precision") && na == 1) { n->flag = CALL_BUILTIN; n->op = "precision"; return n->type = T_VOID; }
            if (is(f, "setf") || is(f, "unsetf")) {      /* cout.setf(ios::fixed) / cout.unsetf(ios::fixed) */
                Node *flag = n->b;
                if (flag && flag->kind == N_ID && (is(flag->name, "fixed") || is(flag->name, "boolalpha") || is(flag->name, "left") || is(flag->name, "scientific"))) {
                    n->flag = CALL_BUILTIN; n->op = is(f, "setf") ? "setf" : "unsetf";
                    return n->type = T_VOID;
                }
                return set_nop(n);
            }
        }
        if (is(c, "Math") || is(c, "Integer") || is(c, "Long") || is(c, "Double") || is(c, "Float") || is(c, "String") ||
            is(c, "Character") || is(c, "Boolean") || is(c, "System") || is(c, "Arrays") || is(c, "cin") || is(c, "cout"))
            err(n->line, "unknown method '%s.%s'", c, f);
        else report_unknown(c, r->line);
        return n->type = T_NONE;
    }
    if (r->kind == N_FIELD && r->a && r->a->kind == N_ID && is(r->a->name, "System")) {
        err(n->line, "System.%s.%s(...) must be used as a statement", r->name, f);
        return n->type = T_NONE;
    }

    Type rt = check_expr(r);
    if (rt == T_NONE) return n->type = T_NONE;
    if (rt == T_SCANNER && r->dims == 0) {
        if (is(f, "nextInt") || is(f, "nextShort") || is(f, "nextByte")) return set_read(n, "int", T_INT);
        if (is(f, "nextLong")) return set_read(n, "long", T_LONG);
        if (is(f, "nextDouble")) return set_read(n, "float", T_FLOAT);
        if (is(f, "nextFloat")) { set_read(n, "float32", T_FLOAT); n->f32 = 1; return T_FLOAT; }
        if (is(f, "nextBoolean")) return set_read(n, "bool", T_BOOL);
        if (is(f, "next")) return set_read(n, "word", T_STRING);
        if (is(f, "nextLine")) return set_read(n, "line", T_STRING);
        if (is(f, "hasNext") || is(f, "hasNextInt") || is(f, "hasNextLine") || is(f, "hasNextDouble")) return set_read(n, "hasnext", T_BOOL);
        if (is(f, "close")) return set_nop(n);
        err(n->line, "unknown Scanner method '%s'", f);
        return n->type = T_NONE;
    }
    if (rt == T_STRING && r->dims == 0) {
        if (is(f, "length") || is(f, "size")) { want_args(n, na, 0, 0); return set_builtin(n, "String.length", T_INT); }
        if (is(f, "charAt") || is(f, "at")) { want_args(n, na, 1, 1); return set_builtin(n, "String.charAt", T_CHAR); }
        if (is(f, "equals")) { want_args(n, na, 1, 1); return set_builtin(n, "String.equals", T_BOOL); }
        if (is(f, "equalsIgnoreCase")) { want_args(n, na, 1, 1); return set_builtin(n, "String.equalsIgnoreCase", T_BOOL); }
        if (is(f, "compareTo") || is(f, "compare")) { want_args(n, na, 1, 1); return set_builtin(n, "String.compareTo", T_INT); }
        if (is(f, "compareToIgnoreCase")) { want_args(n, na, 1, 1); return set_builtin(n, "String.compareToIgnoreCase", T_INT); }
        if (is(f, "isEmpty") || is(f, "empty")) { want_args(n, na, 0, 0); return set_builtin(n, "String.isEmpty", T_BOOL); }
        if (is(f, "toUpperCase")) { want_args(n, na, 0, 0); return set_builtin(n, "String.toUpperCase", T_STRING); }
        if (is(f, "toLowerCase")) { want_args(n, na, 0, 0); return set_builtin(n, "String.toLowerCase", T_STRING); }
        if (is(f, "trim") || is(f, "strip")) { want_args(n, na, 0, 0); return set_builtin(n, "String.trim", T_STRING); }
        if (is(f, "substring")) { want_args(n, na, 1, 2); return set_builtin(n, "String.substring", T_STRING); }
        if (is(f, "substr")) { want_args(n, na, 0, 2); return set_builtin(n, "String.substr", T_STRING); }
        if (is(f, "indexOf") || is(f, "find")) { want_args(n, na, 1, 2); return set_builtin(n, "String.indexOf", T_INT); }
        if (is(f, "lastIndexOf") || is(f, "rfind")) { want_args(n, na, 1, 1); return set_builtin(n, "String.lastIndexOf", T_INT); }
        if (is(f, "contains")) { want_args(n, na, 1, 1); return set_builtin(n, "String.contains", T_BOOL); }
        if (is(f, "startsWith")) { want_args(n, na, 1, 1); return set_builtin(n, "String.startsWith", T_BOOL); }
        if (is(f, "endsWith")) { want_args(n, na, 1, 1); return set_builtin(n, "String.endsWith", T_BOOL); }
        if (is(f, "concat")) { want_args(n, na, 1, 1); return set_builtin(n, "String.concat", T_STRING); }
        if (is(f, "replace")) { want_args(n, na, 2, 2); return set_builtin(n, "String.replace", T_STRING); }
        if (is(f, "repeat")) { want_args(n, na, 1, 1); return set_builtin(n, "String.repeat", T_STRING); }
        if (is(f, "c_str") || is(f, "toString") || is(f, "intern")) { want_args(n, na, 0, 0); return set_builtin(n, "String.self", T_STRING); }
        if (is(f, "toCharArray")) { want_args(n, na, 0, 0); set_builtin(n, "String.toCharArray", T_CHAR); n->dims = 1; return T_CHAR; }
        /* methods that change the string variable (StringBuilder / std::string) */
        static const struct { const char *name, *canon; int lo, hi; Type ret; } mut[] = {
            { "append", "String.append", 1, 1, T_STRING }, { "push_back", "String.append", 1, 1, T_VOID },
            { "insert", "String.insert", 2, 2, T_STRING }, { "reverse", "String.reverse", 0, 0, T_STRING },
            { "deleteCharAt", "String.deleteCharAt", 1, 1, T_STRING }, { "setCharAt", "String.setCharAt", 2, 2, T_VOID },
            { "setLength", "String.setLength", 1, 1, T_VOID }, { "delete", "String.delete", 2, 2, T_STRING },
            { "erase", "String.erase", 0, 2, T_STRING }, { "pop_back", "String.pop_back", 0, 0, T_VOID },
            { "clear", "String.clear", 0, 0, T_VOID }, { NULL, NULL, 0, 0, T_NONE } };
        for (int i = 0; mut[i].name; i++) {
            if (!is(f, mut[i].name)) continue;
            want_args(n, na, mut[i].lo, mut[i].hi);
            Node *root = NULL;
            if (is_lvalue(r)) root = r;
            else if (r->kind == N_CALL && r->flag == CALL_BUILTIN && r->c) root = r->c;   /* sb.append(a).append(b) */
            if (root) { Symbol *sy = lvalue_symbol(root); if (sy && sy->is_const) err(n->line, "cannot modify const string '%s'", sy->name); }
            /* a temporary (new StringBuilder(w).reverse()) just yields the changed text */
            n->c = root;
            return set_builtin(n, mut[i].canon, mut[i].ret);
        }
        err(n->line, "unknown string method '%s'", f);
        return n->type = T_NONE;
    }
    if (r->dims > 0) {
        if (is(f, "size") || is(f, "length")) { want_args(n, na, 0, 0); return set_builtin(n, "len", T_INT); }
        if (is(f, "clone")) { want_args(n, na, 0, 0); set_builtin(n, "Arrays.copy", r->type); n->dims = r->dims; return n->type; }
        err(n->line, "arrays have no method '%s' (use .length)", f);
        return n->type = T_NONE;
    }
    err(n->line, "type '%s' has no method '%s'", tn(r), f);
    return n->type = T_NONE;
}

static Type check_call(Node *n) {
    int na = 0;
    for (Node *a = n->b; a; a = a->next) {
        na++;
        if (n->a && n->a->kind == N_ID && (is(n->a->name, "cin") || is(n->a->name, "cout"))) continue;
        if (!n->a && is(n->name, "swap")) { check_expr(a); continue; }
        if (!n->a && is(n->name, "scanf")) continue;
        if (!n->a && (is(n->name, "sort") || is(n->name, "reverse") || is(n->name, "count"))) continue;
        if (a->kind == N_UNOP && is(a->op, "&")) { check_expr(a->a); a->type = a->a->type; a->dims = a->a->dims; continue; }
        check_expr(a);
    }
    if (!n->a) return check_free_call(n, na);
    return check_method_call(n, na);
}

/* ------------------------------------------------------------ expressions */

static Type check_binop_strict(Node *n, Type l, Type r) {
    const char *op = n->op;
    if (is(op, "+") || is(op, "-") || is(op, "*") || is(op, "/")) {
        if (!is_num_strict(l) || !is_num_strict(r)) {
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
        if (!is_num_strict(l) || !is_num_strict(r)) {
            err(n->line, "invalid expression: operator '%s' needs numeric operands, got '%s' and '%s'",
                op, type_name(l), type_name(r));
            return n->type = T_NONE;
        }
        return n->type = T_BOOL;
    }
    if (is(op, "==") || is(op, "!=")) {
        if ((is_num_strict(l) && is_num_strict(r)) || (l == T_BOOL && r == T_BOOL))
            return n->type = T_BOOL;
        err(n->line, "type mismatch: cannot compare '%s' with '%s'", type_name(l), type_name(r));
        return n->type = T_NONE;
    }
    if (is(op, "&&") || is(op, "||")) {
        if (l != T_BOOL || r != T_BOOL) {
            err(n->line, "invalid expression: operator '%s' needs bool operands, got '%s' and '%s'",
                op, type_name(l), type_name(r));
            return n->type = T_NONE;
        }
        return n->type = T_BOOL;
    }
    /* bitwise / shift operators are not part of the mini language, but accept them on ints */
    if (l == T_INT && r == T_INT) return n->type = T_INT;
    if (l == T_BOOL && r == T_BOOL && (is(op, "&") || is(op, "|") || is(op, "^"))) return n->type = T_BOOL;
    err(n->line, "invalid expression: operator '%s' needs int operands, got '%s' and '%s'", op, type_name(l), type_name(r));
    return n->type = T_NONE;
}

/* while (cin >> x >> y)  ->  reads, then "did the input succeed?" */
static int cin_chain(Node *n) {
    Node *p = n; int count = 0;
    while (p->kind == N_BINOP && is(p->op, ">>")) { p = p->a; count++; }
    if (!(p->kind == N_ID && is(p->name, "cin") && !st_lookup("cin"))) return 0;
    Node *items[512]; int k = 0;
    for (p = n; p->kind == N_BINOP && is(p->op, ">>") && k < 512; p = p->a) items[k++] = p->b;
    Node *reads = NULL;
    for (int i = k - 1; i >= 0; i--) {
        Node *r = mk_node(N_READ, n->line);
        r->a = items[i]; items[i]->next = NULL;
        check_read(r);
        reads = list_append(reads, r);
    }
    n->kind = N_CALL; n->name = "cin"; n->a = NULL; n->b = reads;
    n->flag = CALL_READ; n->op = "ok"; n->dims = 0; n->type = T_BOOL;
    (void)count;
    return 1;
}

static Type check_binop_inner(Node *n);
static Type check_binop(Node *n) {
    Type t = check_binop_inner(n);
    if (t == T_FLOAT) {
        int lf = n->a->type == T_FLOAT, rf = n->b->type == T_FLOAT;
        n->f32 = (!lf || n->a->f32) && (!rf || n->b->f32);
    }
    return t;
}

static Type check_binop_inner(Node *n) {
    if (!strict() && is(n->op, ">>") && cin_chain(n)) return T_BOOL;
    Type l = check_expr(n->a), r = check_expr(n->b);
    if (l == T_NONE || r == T_NONE) return n->type = T_NONE;   /* avoid cascades */
    n->dims = 0;
    if (strict() && n->a->dims == 0 && n->b->dims == 0) return check_binop_strict(n, l, r);
    const char *op = n->op;
    if (n->a->dims || n->b->dims) {
        err(n->line, "invalid expression: operator '%s' cannot be used on an array", op);
        return n->type = T_NONE;
    }
    if (l == T_VOID || r == T_VOID) { err(n->line, "invalid expression: a void value cannot be used with '%s'", op); return n->type = T_NONE; }
    if (l == T_SCANNER || r == T_SCANNER) { err(n->line, "invalid expression: operator '%s' on a Scanner", op); return n->type = T_NONE; }

    if (is(op, "+") && (l == T_STRING || r == T_STRING)) {
        if (lang_mode == LANG_C) { err(n->line, "invalid expression: '+' cannot join strings in C (use strcat)"); return n->type = T_NONE; }
        return n->type = T_STRING;
    }
    if (is(op, "+") || is(op, "-") || is(op, "*") || is(op, "/") || is(op, "%")) {
        if (!is_num(l) || !is_num(r)) {
            err(n->line, "invalid expression: operator '%s' needs numeric operands, got '%s' and '%s'", op, type_name(l), type_name(r));
            return n->type = T_NONE;
        }
        if (is(op, "%") && !java() && (l == T_FLOAT || r == T_FLOAT)) {
            err(n->line, "invalid expression: operator '%%' needs integer operands, got '%s' and '%s' (use fmod)", type_name(l), type_name(r));
            return n->type = T_NONE;
        }
        return n->type = promote(l, r);
    }
    if (is(op, "<") || is(op, ">") || is(op, "<=") || is(op, ">=")) {
        if ((is_num(l) && is_num(r)) || (l == T_STRING && r == T_STRING && !java())) return n->type = T_BOOL;
        err(n->line, "invalid expression: operator '%s' needs numeric operands, got '%s' and '%s'", op, type_name(l), type_name(r));
        return n->type = T_NONE;
    }
    if (is(op, "==") || is(op, "!=")) {
        if ((is_num(l) && is_num(r)) || (l == r) || (lang_mode == LANG_CPP && ((l == T_STRING && r == T_CHAR) || (l == T_CHAR && r == T_STRING))))
            return n->type = T_BOOL;
        err(n->line, "type mismatch: cannot compare '%s' with '%s'", type_name(l), type_name(r));
        return n->type = T_NONE;
    }
    if (is(op, "&&") || is(op, "||")) {
        if (java() ? (l == T_BOOL && r == T_BOOL) : (is_num(l) || l == T_BOOL) && (is_num(r) || r == T_BOOL))
            return n->type = T_BOOL;
        err(n->line, "invalid expression: operator '%s' needs %s operands, got '%s' and '%s'", op,
            java() ? "boolean" : "scalar", type_name(l), type_name(r));
        return n->type = T_NONE;
    }
    if (is(op, "&") || is(op, "|") || is(op, "^")) {
        if (l == T_BOOL && r == T_BOOL) return n->type = T_BOOL;
        if (is_integral(l) && is_integral(r)) return n->type = promote(l, r);
        err(n->line, "invalid expression: operator '%s' needs integer operands, got '%s' and '%s'", op, type_name(l), type_name(r));
        return n->type = T_NONE;
    }
    /* << >> >>> */
    if (is_integral(l) && is_integral(r)) return n->type = promote(l, T_INT);
    err(n->line, "invalid expression: operator '%s' needs integer operands, got '%s' and '%s'", op, type_name(l), type_name(r));
    return n->type = T_NONE;
}

/* x = v,  a[i] = v,  x += v  (also used for declarations with an initializer) */
static Type check_assign(Node *n, int is_init) {
    Type tt = T_NONE; int td = 0;
    const char *what = n->name;
    Symbol *s = NULL;
    if (n->name) {
        s = st_lookup(n->name);
        if (!s) report_unknown(n->name, n->line);
        else if (s->is_func) { err(n->line, "invalid assignment: '%s' is a function", n->name); s = NULL; }
        else { tt = s->type; td = s->dims; }
    } else if (norm_lvalue(n->c)->kind == N_ID) {
        n->name = n->c->name;
        if (n->op) {                              /* Main.count += 10  ->  count = count + 10 */
            n->a = mk_binop(n->op, n->c, n->a, n->line);
            n->op = NULL; n->flag = 2;
        }
        n->c = NULL;
        return check_assign(n, is_init);
    } else {
        Node *t = n->c;
        if (!is_lvalue(t)) {
            if (t) check_expr(t);
            err(n->line, "invalid assignment: the left side is not a variable");
        } else {
            tt = check_expr(t); td = t->dims;
            s = lvalue_symbol(t);
            what = "array element";
            if (t->kind == N_INDEX && t->a->type == T_STRING && t->a->dims == 0 && java()) {
                err(n->line, "Java strings cannot be changed by index (use a char[] or StringBuilder)");
                tt = T_NONE;
            }
        }
    }
    if (s && s->is_const && !is_init) err(n->line, "cannot assign to %s '%s' (it is const/final)", java() ? "final variable" : "const variable", s->name);
    if (s && s->type == T_SCANNER && tt == T_SCANNER) { check_expr(n->a); n->type = T_SCANNER; return T_SCANNER; }
    if (s && n->name) n->sym = s;

    Type rt;
    if (n->op) {    /* a[i] op= v */
        rt = check_expr(n->a);
        if (tt != T_NONE && rt != T_NONE) {
            Type l = tt, r = rt;
            if (td || n->a->dims) { err(n->line, "invalid expression: operator '%s=' cannot be used on an array", n->op); rt = T_NONE; }
            else if (is(n->op, "+") && (l == T_STRING || r == T_STRING)) rt = T_STRING;
            else if (!is_num(l) || !is_num(r)) {
                err(n->line, "invalid expression: operator '%s=' needs numeric operands, got '%s' and '%s'", n->op, type_name(l), type_name(r));
                rt = T_NONE;
            } else rt = promote(l, r);
        }
    } else rt = check_expr(n->a);

    if (n->name && s) n->f32 = s->f32;
    else if (!n->name && n->c) n->f32 = n->c->f32;
    if (tt != T_NONE && rt != T_NONE) {
        n->type = tt; n->dims = td;
        java_float_check(n->f32, tt, td, n->a, n->flag == 2 || n->op != NULL, n->line, what ? what : "?");
        if (!assignable(tt, td, rt, n->a->dims, n->a, n->flag == 2 || n->op != NULL)) {
            if (java() && td == 0 && n->a->dims == 0 && is_num(tt) && is_num(rt))
                err(n->line, "incompatible types: possible lossy conversion from %s to %s (variable '%s', use a cast)",
                    type_name(rt), type_name(tt), what);
            else
                err(n->line, "invalid assignment: cannot assign '%s' to '%s' variable '%s'",
                    type_name_dims(rt, n->a->dims), type_name_dims(tt, td), what);
        }
    } else n->type = tt;
    return n->type;
}

static Type check_incdec(Node *n) {
    Node *t = norm_lvalue(n->a);
    if (!is_lvalue(t)) {
        if (t) check_expr(t);
        err(n->line, "invalid expression: operator '%s' needs a variable", n->op);
        return n->type = T_NONE;
    }
    Type tt = check_expr(t);
    if (tt == T_NONE) return n->type = T_NONE;
    Symbol *s = lvalue_symbol(t);
    if (s && s->is_const) err(n->line, "cannot modify %s '%s' (it is const/final)", java() ? "final variable" : "const variable", s->name);
    if (!is_num(tt) || t->dims) {
        err(n->line, "invalid expression: operator '%s' needs a numeric variable, got '%s'", n->op, tn(t));
        return n->type = T_NONE;
    }
    n->dims = 0;
    n->f32 = t->f32;
    return n->type = tt;
}

static long long sizeof_type(Type t) {
    switch (t) { case T_CHAR: case T_BOOL: return 1; case T_INT: return 4; case T_LONG: case T_FLOAT: return 8; default: return 8; }
}

static Type check_expr(Node *n) {
    switch (n->kind) {
    case N_INT:   n->dims = 0; if (n->type == T_NONE && n->flag == 0 && n->ival == 0 && n->name) return T_NONE;
                  return n->type = n->flag ? T_LONG : T_INT;
    case N_CHAR:  return n->type = T_CHAR;
    case N_FLOAT: return n->type = T_FLOAT;
    case N_BOOL:  return n->type = T_BOOL;
    case N_STR:   return n->type = T_STRING;

    case N_ID: {
        Symbol *s = st_lookup(n->name);
        if (!s) {
            if (!strict() && builtin_constant(n, n->name)) return n->type;
            report_unknown(n->name, n->line);
            return n->type = T_NONE;
        }
        if (s->is_func) { err(n->line, "'%s' is a function; call it with ()", n->name); return n->type = T_NONE; }
        n->sym = s;
        n->dims = s->dims;
        n->f32 = s->f32;
        return n->type = s->type;
    }

    case N_UNOP: {
        if (is(n->op, "&")) {
            check_expr(n->a);
            err(n->line, "the address-of operator '&' is only supported in scanf arguments");
            return n->type = T_NONE;
        }
        Type t = check_expr(n->a);
        if (t == T_NONE) return n->type = T_NONE;      /* already reported */
        n->dims = 0;
        if (n->a->dims) { err(n->line, "invalid expression: operator '%s' cannot be used on an array", n->op); return n->type = T_NONE; }
        if (is(n->op, "-") || is(n->op, "+")) {        /* unary minus / plus */
            if (!is_num(t)) {
                err(n->line, "invalid expression: operator '%s' needs a numeric operand, got '%s'", n->op, type_name(t));
                return n->type = T_NONE;
            }
            n->f32 = n->a->f32;
            return n->type = strict() ? t : promote(t, T_INT);
        }
        if (is(n->op, "~")) {
            if (!is_integral(t)) { err(n->line, "invalid expression: operator '~' needs an integer operand, got '%s'", type_name(t)); return n->type = T_NONE; }
            return n->type = promote(t, T_INT);
        }
        /* ! */
        if (strict() || java() ? t != T_BOOL : !(is_num(t) || t == T_BOOL)) {
            err(n->line, "invalid expression: operator '%s' needs a bool operand, got '%s'", n->op, type_name(t));
            return n->type = T_NONE;
        }
        return n->type = T_BOOL;
    }

    case N_BINOP: return check_binop(n);
    case N_ASSIGN: return check_assign(n, 0);
    case N_INCDEC: return check_incdec(n);

    case N_COND: {
        check_condition(n->a, "?:");
        Type b = check_expr(n->b), c = check_expr(n->c);
        if (b == T_NONE) { n->dims = n->c->dims; return n->type = c; }
        if (c == T_NONE) { n->dims = n->b->dims; return n->type = b; }
        n->dims = n->b->dims;
        n->f32 = (b != T_FLOAT || n->b->f32) && (c != T_FLOAT || n->c->f32);
        if (b == c) return n->type = b;
        if (is_num(b) && is_num(c) && b != T_BOOL && c != T_BOOL) return n->type = promote(b, c);
        if (b == T_STRING || c == T_STRING) return n->type = T_STRING;
        if (!strict() && !java() && is_num(b) && is_num(c)) return n->type = promote(b, c);
        err(n->line, "type mismatch: the two results of ?: have different types ('%s' and '%s')", type_name(b), type_name(c));
        return n->type = T_NONE;
    }

    case N_CAST: {
        Type t = check_expr(n->a);
        if (t == T_NONE) return n->type;
        if (n->a->dims && n->dims == 0) { err(n->line, "cannot cast an array to '%s'", type_name(n->type)); return n->type = T_NONE; }
        if (n->type == T_STRING && t != T_STRING) { err(n->line, "cannot cast '%s' to a string (use to_string / String.valueOf)", type_name(t)); return n->type = T_NONE; }
        if (n->type != T_STRING && t == T_STRING) { err(n->line, "cannot cast a string to '%s' (use stoi / Integer.parseInt)", type_name(n->type)); return n->type = T_NONE; }
        if (java() && ((n->type == T_BOOL) != (t == T_BOOL))) { err(n->line, "incompatible types: %s cannot be converted to %s", type_name(t), type_name(n->type)); return n->type = T_NONE; }
        return n->type;
    }

    case N_SIZEOF: {
        long long size = n->ival;
        if (n->a) {
            Type t = check_expr(n->a);
            if (n->a->kind == N_ID && n->a->sym && n->a->sym->dims > 0 && n->a->sym->func) {
                /* sizeof(array): element size * declared element count */
                Node *d = n->a->sym->func;   /* the declaration node */
                long long count = 1; int known = 1;
                for (Node *s = d->a; s; s = s->next) { if (s->kind == N_INT && s->flag != -1) count *= s->ival; else known = 0; }
                if (!known && d->b && d->b->kind == N_ARRLIT && d->a && !d->a->next) count = list_len(d->b->a), known = 1;
                Type et = n->a->sym->type;
                size = (known ? count : 1) * sizeof_type(et);
            } else if (n->a->dims > 0) size = 8;
            else size = sizeof_type(t);
        } else if (n->type == T_STRING) size = 32;
        if (java()) { err(n->line, "sizeof does not exist in Java"); return n->type = T_NONE; }
        return make_int(n, size, 0);
    }

    case N_CALL: return check_call(n);

    case N_INDEX: {
        Type bt = check_expr(n->a);
        Type it = check_expr(n->b);
        if (it != T_NONE && (!is_integral(it) || n->b->dims)) err(n->line, "array index must be an integer, got '%s'", tn(n->b));
        if (bt == T_NONE) return n->type = T_NONE;
        if (n->a->dims > 0) { n->dims = n->a->dims - 1; n->f32 = n->a->f32; return n->type = bt; }
        if (bt == T_STRING) {
            if (java()) err(n->line, "use s.charAt(i) to read a character of a Java string");
            n->dims = 0; return n->type = T_CHAR;
        }
        err(n->line, "invalid expression: '%s' is not an array", n->a->kind == N_ID ? n->a->name : type_name(bt));
        return n->type = T_NONE;
    }

    case N_FIELD: {
        Node *r = n->a;
        if (norm_lvalue(n)->kind == N_ID) return check_expr(n);
        if (r->kind == N_ID && !st_lookup(r->name)) {
            const char *c = r->name, *f = n->name;
            if (is(c, "Integer") && is(f, "MAX_VALUE")) return make_int(n, 2147483647LL, 0);
            if (is(c, "Integer") && is(f, "MIN_VALUE")) return make_int(n, -2147483648LL, 0);
            if (is(c, "Long") && is(f, "MAX_VALUE")) return make_int(n, 9223372036854775807LL, 1);
            if (is(c, "Long") && is(f, "MIN_VALUE")) return make_int(n, -9223372036854775807LL - 1, 1);
            if (is(c, "Short") && is(f, "MAX_VALUE")) return make_int(n, 32767, 0);
            if (is(c, "Short") && is(f, "MIN_VALUE")) return make_int(n, -32768, 0);
            if (is(c, "Byte") && is(f, "MAX_VALUE")) return make_int(n, 127, 0);
            if (is(c, "Byte") && is(f, "MIN_VALUE")) return make_int(n, -128, 0);
            if ((is(c, "Double") || is(c, "Float")) && is(f, "MAX_VALUE")) return make_float(n, is(c, "Double") ? 1.7976931348623157e308 : 3.4028234663852886e38);
            if ((is(c, "Double") || is(c, "Float")) && is(f, "MIN_VALUE")) return make_float(n, is(c, "Double") ? 4.9e-324 : 1.401298464324817e-45);
            if (is(c, "Math") && is(f, "PI")) return make_float(n, 3.14159265358979323846);
            if (is(c, "Math") && is(f, "E")) return make_float(n, 2.71828182845904523536);
            if (is(c, "System") && (is(f, "out") || is(f, "in") || is(f, "err"))) {
                err(n->line, "System.%s cannot be used as a value here", f);
                return n->type = T_NONE;
            }
            if (is(c, "Integer") || is(c, "Long") || is(c, "Double") || is(c, "Math") || is(c, "Float") || is(c, "Short") || is(c, "Byte"))
                err(n->line, "unknown field '%s.%s'", c, f);
            else report_unknown(c, r->line);
            return n->type = T_NONE;
        }
        Type t = check_expr(r);
        if (t == T_NONE) return n->type = T_NONE;
        if (r->dims > 0 && is(n->name, "length")) { n->dims = 0; return n->type = T_INT; }
        if (t == T_STRING && r->dims == 0 && is(n->name, "length")) { err(n->line, "use length() with parentheses for a string"); return n->type = T_NONE; }
        err(n->line, "type '%s' has no field '%s'", tn(r), n->name);
        return n->type = T_NONE;
    }

    case N_NEWARR: {
        int d = 0;
        for (Node *s = n->a; s; s = s->next) {
            d++;
            if (s->kind == N_INT && s->flag == -1) continue;
            Type t = check_expr(s);
            if (t != T_NONE && (!is_integral(t) || s->dims)) err(n->line, "array size must be an integer, got '%s'", tn(s));
        }
        n->dims = d;
        if (n->a && n->a->kind == N_INT && n->a->flag == -1) err(n->line, "array creation needs the first dimension size");
        return n->type;
    }

    case N_ARRLIT: {
        /* new int[]{1, 2, 3}: check elements against the element type */
        for (Node *e = n->a; e; e = e->next) {
            if (e->kind == N_ARRLIT) {
                if (n->dims < 2) err(e->line, "too many braces in array initializer");
                else { e->type = n->type; e->dims = n->dims - 1; check_expr(e); }
                continue;
            }
            Type t = check_expr(e);
            if (n->dims != 1 && t != T_NONE) { err(e->line, "array initializer needs nested { } for a %d-dimensional array", n->dims); continue; }
            if (!assignable(n->type, 0, t, e->dims, e, 0))
                err(e->line, "invalid assignment: cannot store '%s' in a '%s' array", tn(e), type_name(n->type));
        }
        return n->type;
    }

    case N_NEWOBJ:
        for (Node *a = n->b; a; a = a->next)
            if (!(a->kind == N_FIELD && a->a && a->a->kind == N_ID && is(a->a->name, "System"))) check_expr(a);
        if (n->type == T_SCANNER) return T_SCANNER;
        err(n->line, "creating objects of class '%s' is not supported", n->name);
        return n->type = T_NONE;

    default:
        return T_NONE;
    }
}

/* ------------------------------------------------------------- statements */

/* body of if/while/for: a lone statement gets its own scope; a block opens one itself */
static void check_body(Node *n) {
    if (!n) return;
    if (n->kind == N_BLOCK) { check_stmts(n); return; }
    st_enter();
    check_stmts(n);
    st_exit();
}

static void check_init_list(Node *list, Type t, int dims, const char *name) {
    for (Node *e = list; e; e = e->next) {
        if (e->kind == N_ARRLIT) {
            if (dims < 2) err(e->line, "too many braces in the initializer of '%s'", name);
            else check_init_list(e->a, t, dims - 1, name);
            continue;
        }
        Type et = check_expr(e);
        if (dims > 1 && !(t == T_CHAR && et == T_STRING)) {
            if (et == T_NONE) continue;
            if (e->dims == dims - 1 && et == t) continue;      /* int[][] m = { row1, row2 } */
            if (!java() && e->dims == 0) {                     /* C: int m[2][2] = {0}; / {1, 2, 3, 4} (filled row by row) */
                if (!assignable(t, 0, et, 0, e, 0))
                    err(e->line, "invalid assignment: cannot store '%s' in '%s' array '%s'", tn(e), type_name(t), name);
                continue;
            }
            err(e->line, "the initializer of '%s' needs nested { } for each row", name);
            continue;
        }
        if (!assignable(t, dims - 1, et, e->dims, e, 0))
            err(e->line, "invalid assignment: cannot store '%s' in '%s' array '%s'", tn(e), type_name(t), name);
    }
}

static void check_decl(Node *n) {
    /* char *s / char s[] = "..." are strings in C and C++ */
    if (n->ptr) {
        if (n->type == T_CHAR) { n->type = T_STRING; n->dims += n->ptr - 1; }
        else if (!n->flag) err(n->line, "pointer variables are not supported ('%s')", n->name);
        n->ptr = 0;
    }
    if (n->type == T_CHAR && !java() && n->a && n->dims > 0 &&
        (!n->b || n->b->kind == N_STR || (n->b->kind == N_ARRLIT && n->b->a && n->b->a->kind == N_STR))) {
        n->type = T_STRING; n->dims--;
        /* drop the last size: it was the string's capacity */
        if (n->a->next == NULL) n->a = NULL;
        else { Node *p = n->a; while (p->next->next) p = p->next; p->next = NULL; }
    }
    int auto_typed = 0;
    if (n->type == T_AUTO) {
        auto_typed = 1;
        if (!n->b || n->b->kind == N_ARRLIT) { err(n->line, "'auto' variable '%s' needs an initializer", n->name); n->type = T_NONE; }
        else {
            Type t = check_expr(n->b);
            n->type = t == T_VOID ? T_NONE : t; n->dims = n->b->dims;
            if (n->type == T_NONE) n->type = T_INT;
        }
    }
    if (n->type == T_VOID) err(n->line, "variable '%s' cannot have type void", n->name);

    Symbol *old = st_lookup_current(n->name);
    if (old) {
        if (old->is_func) err(n->line, "'%s' is already a function name (line %d)", n->name, old->line);
        else err(n->line, "redeclaration of '%s' (first declared at line %d)", n->name, old->line);
    }
    /* array sizes */
    for (Node *s = n->a; s; s = s->next) {
        if (s->kind == N_INT && s->flag == -1) continue;
        Type t = check_expr(s);
        if (t != T_NONE && (!is_integral(t) || s->dims)) err(n->line, "array size must be an integer, got '%s'", tn(s));
    }
    Symbol *sym = NULL;
    if (!old) {
        sym = st_insert(n->name, n->type, n->line);
        sym->dims = n->dims;
        sym->is_const = n->is_const;
        sym->func = n;          /* declaration node (sizeof uses it) */
        if (strict() || n->type != T_FLOAT) n->f32 = 0;
        sym->f32 = n->f32;
        n->sym = sym;
    }
    if (n->is_const && !n->b && !n->flag && !java() && st_level() >= 0 && n->dims == 0)
        err(n->line, "const variable '%s' must be initialized", n->name);

    if (!n->b) {
        if (n->dims > 0 && n->a && n->a->kind == N_INT && n->a->flag == -1)
            err(n->line, "array '%s' needs a size or an initializer", n->name);
        return;
    }
    if (n->b->kind == N_ARRLIT && !n->b->flag) {
        if (n->dims == 0) {
            /* int x = {5};  (C) */
            if (list_len(n->b->a) != 1) err(n->line, "a scalar '%s' cannot be initialised with a list", n->name);
            else {
                Node *e = n->b->a;
                Type t = check_expr(e);
                if (!assignable(n->type, 0, t, e->dims, e, 0))
                    err(n->line, "invalid assignment: cannot assign '%s' to '%s' variable '%s'", tn(e), type_name(n->type), n->name);
            }
            return;
        }
        if (n->type == T_STRING && n->b->a && n->b->a->kind == N_STR && n->dims == 0) return;
        check_init_list(n->b->a, n->type, n->dims, n->name);
        return;
    }
    if (auto_typed) return;          /* the initializer was already checked */
    /* scalar (or whole-array) initializer: same rules as an assignment */
    if (sym) {
        Node tmp; memset(&tmp, 0, sizeof tmp);
        tmp.kind = N_ASSIGN; tmp.line = n->line; tmp.name = n->name; tmp.a = n->b;
        check_assign(&tmp, 1);
        n->b = tmp.a;
    } else check_expr(n->b);
}

static int case_value(Node *e, long long *v, const char **s) {
    *s = NULL;
    if (e->kind == N_INT || e->kind == N_CHAR || e->kind == N_BOOL) { *v = e->ival; return 1; }
    if (e->kind == N_STR) { *s = e->name; return 1; }
    if (e->kind == N_ID && e->sym && e->sym->is_const && e->sym->func && e->sym->func->b) {
        Node *i = e->sym->func->b;
        if (i->kind == N_INT || i->kind == N_CHAR) { *v = i->ival; return 1; }
    }
    return 0;
}

static void check_switch(Node *n) {
    Type t = check_expr(n->a);
    if (t != T_NONE) {
        if (n->a->dims || t == T_FLOAT || t == T_VOID || t == T_SCANNER || (t == T_BOOL && java()) || (t == T_STRING && lang_mode == LANG_C))
            err(n->line, "switch needs an integer, char%s value, got '%s'", java() || lang_mode == LANG_MC ? " or String" : "", tn(n->a));
    }
    int ndefault = 0;
    /* duplicate case labels */
    for (Node *c = n->b; c; c = c->next) {
        if (c->kind != N_CASE) continue;
        if (!c->a) { if (++ndefault > 1) err(c->line, "multiple default labels in one switch"); continue; }
    }
    st_enter();
    switch_depth++;
    check_stmts(n->b);
    switch_depth--;
    st_exit();
    for (Node *c = n->b; c; c = c->next) {
        if (c->kind != N_CASE || !c->a) continue;
        long long v; const char *s;
        if (!case_value(c->a, &v, &s)) {
            if (c->a->type != T_NONE) err(c->line, "case label must be a constant");
            continue;
        }
        for (Node *d = n->b; d != c; d = d->next) {
            if (d->kind != N_CASE || !d->a) continue;
            long long v2; const char *s2;
            if (!case_value(d->a, &v2, &s2)) continue;
            if ((s && s2 && !strcmp(s, s2)) || (!s && !s2 && v == v2)) {
                err(c->line, "duplicate case value");
                break;
            }
        }
    }
}

static void check_func(Node *f) {
    Node *saved_func = cur_func;
    int saved_loop = loop_depth, saved_switch = switch_depth;
    cur_func = f; loop_depth = switch_depth = 0;
    int saved_labels = nlabels_; nlabels_ = 0;
    if (f->ptr && f->type == T_CHAR) { f->type = T_STRING; f->dims += f->ptr - 1; }
    st_enter();
    for (Node *p = f->a; p; p = p->next) {
        if (p->ptr && p->type == T_CHAR) { p->type = T_STRING; p->dims += p->ptr - 1; p->ptr = 0; }
        else if (p->type == T_CHAR && p->dims > 0 && !java()) { p->type = T_STRING; p->dims--; }   /* void f(char s[]) */
        if (p->ptr) { err(p->line, "pointer parameters are not supported ('%s')", p->name); p->ptr = 0; }
        if (p->type == T_VOID || p->type == T_AUTO) { err(p->line, "parameter '%s' has an invalid type", p->name); p->type = T_NONE; }
        if (is(p->name, "_")) continue;
        if (st_lookup_current(p->name)) err(p->line, "redeclaration of parameter '%s'", p->name);
        Symbol *s = st_insert(p->name, p->type, p->line);
        s->dims = p->dims; s->is_const = p->is_const; s->func = p;
        if (p->type != T_FLOAT) p->f32 = 0;
        s->f32 = p->f32;
        p->sym = s;
    }
    if (f->b) check_stmts(f->b->a);    /* parameters and the body share one scope */
    st_exit();
    cur_func = saved_func; loop_depth = saved_loop; switch_depth = saved_switch;
    nlabels_ = saved_labels;
}

/* cin >> x / scanf / getline: the target must be a variable; its type picks how input is parsed */
static void check_read(Node *n) {
    if (!n->a) return;                       /* cin.ignore() */
    Node *t = norm_lvalue(n->a);
    if (t->kind == N_UNOP && is(t->op, "&")) t = n->a = t->a;
    if (!is_lvalue(t)) { check_expr(t); err(n->line, "input must be stored in a variable"); return; }
    Type tt = check_expr(t);
    if (tt == T_NONE) return;
    if (t->dims > 0) { err(n->line, "cannot read a whole array; read its elements in a loop"); return; }
    Symbol *s = lvalue_symbol(t);
    if (s && s->is_const) err(n->line, "cannot read into const variable '%s'", s->name);
    const char *k = n->op;
    if (k && (is(k, "line") || is(k, "fline"))) {
        if (tt != T_STRING) err(n->line, "getline needs a string variable, got '%s'", tn(t));
        return;
    }
    if (k && is(k, "rawchar")) return;
    if (k && is(k, "char") && tt == T_CHAR) return;
    n->op = tt == T_INT ? "int" : tt == T_LONG ? "long" : tt == T_FLOAT ? (t->f32 && !strict() ? "float32" : "float") : tt == T_CHAR ? "char" :
            tt == T_STRING ? "word" : tt == T_BOOL ? "bool" : "int";
    if (tt == T_SCANNER || tt == T_VOID) err(n->line, "cannot read into '%s'", tn(t));
}

static void check_stmts(Node *n) {
    for (; n; n = n->next) {
        switch (n->kind) {
        case N_DECL:
            check_decl(n);
            break;
        case N_ASSIGN:
            check_assign(n, 0);
            break;
        case N_EXPRSTMT:
            check_expr(n->a);
            break;
        case N_PRINT:
        case N_WRITE:
            check_expr(n->a);
            if (n->a->dims > 0 && n->a->type != T_NONE)
                err(n->line, "cannot print a whole array '%s' (print its elements in a loop)", tn(n->a));
            else if (n->a->type == T_VOID) err(n->line, "cannot print the result of a void function");
            else if (n->a->type == T_SCANNER) err(n->line, "cannot print a Scanner");
            break;
        case N_READ:
            check_read(n);
            break;
        case N_MANIP:
            if (n->a) { Type t = check_expr(n->a); if (t != T_NONE && !is_num(t)) err(n->line, "%s needs a number", n->name); }
            break;
        case N_RETURN:
            n->fn = cur_func;
            if (n->a) {
                Type t = check_expr(n->a);
                if (cur_func && t != T_NONE) java_float_check(cur_func->f32, cur_func->type, cur_func->dims, n->a, 0, n->line, "return value");
                if (cur_func && cur_func->type == T_VOID && t != T_NONE)
                    err(n->line, "void function '%s' cannot return a value", cur_func->name);
                else if (cur_func && t != T_NONE && cur_func->type != T_NONE &&
                         !assignable(cur_func->type, cur_func->dims, t, n->a->dims, n->a, 0))
                    err(n->line, "invalid return: function '%s' returns '%s', got '%s'", cur_func->name,
                        type_name_dims(cur_func->type, cur_func->dims), tn(n->a));
            } else if (cur_func && cur_func->type != T_VOID && java() && !is(cur_func->name, "main"))
                err(n->line, "missing return value in function '%s'", cur_func->name);
            break;
        case N_IF:
            check_condition(n->a, "if");
            check_body(n->b);
            check_body(n->c);
            break;
        case N_WHILE:
            check_condition(n->a, "while");
            loop_depth++;
            check_body(n->b);
            loop_depth--;
            break;
        case N_DOWHILE:
            loop_depth++;
            check_body(n->b);
            loop_depth--;
            check_condition(n->a, "do-while");
            break;
        case N_FOR:
            st_enter();
            check_stmts(n->a);
            if (n->b) check_condition(n->b, "for");
            loop_depth++;
            check_body(n->d);
            loop_depth--;
            check_stmts(n->c);
            st_exit();
            break;
        case N_FOREACH: {
            st_enter();
            if (n->a->kind == N_ARRLIT && !n->a->flag) {      /* for (int x : {1, 2, 3}) */
                Type et = n->type;
                if (et == T_AUTO) { et = n->a->a ? check_expr(n->a->a) : T_INT; if (et == T_NONE) et = T_INT; }
                n->a->type = et; n->a->dims = 1; n->a->flag = 1;
                n->a->f32 = n->f32;
            }
            Type ct = check_expr(n->a);
            Type et = T_NONE; int ed = 0;
            if (ct != T_NONE) {
                if (n->a->dims > 0) { et = ct; ed = n->a->dims - 1; }
                else if (ct == T_STRING) et = T_CHAR;
                else err(n->line, "for-each needs an array or a string, got '%s'", tn(n->a));
            }
            if (n->type == T_AUTO) { n->type = et == T_NONE ? T_INT : et; n->dims = ed; }
            else if (et != T_NONE && !assignable(n->type, n->dims, et, ed, NULL, 0))
                err(n->line, "for-each variable '%s' has type '%s' but the elements are '%s'", n->name,
                    type_name_dims(n->type, n->dims), type_name_dims(et, ed));
            Symbol *s = st_insert(n->name, n->type, n->line);
            s->dims = n->dims;
            if (n->type != T_FLOAT || strict()) n->f32 = 0;
            s->f32 = n->f32;
            n->sym = s;
            loop_depth++;
            check_body(n->b);
            loop_depth--;
            st_exit();
            break;
        }
        case N_SWITCH:
            check_switch(n);
            break;
        case N_CASE:
            if (n->a) check_expr(n->a);
            break;
        case N_BREAK:
            if (n->name) {
                int found = 0;
                for (int i = 0; i < nlabels_; i++) if (is(labels[i].name, n->name)) found = 1;
                if (!found) err(n->line, "undefined label '%s' in break", n->name);
            } else if (loop_depth == 0 && switch_depth == 0) err(n->line, "break statement not within a loop or switch");
            break;
        case N_CONTINUE:
            if (n->name) {
                int found = -1;
                for (int i = 0; i < nlabels_; i++) if (is(labels[i].name, n->name)) found = i;
                if (found < 0) err(n->line, "undefined label '%s' in continue", n->name);
                else if (!labels[found].is_loop) err(n->line, "label '%s' is not a loop, cannot continue it", n->name);
            } else if (loop_depth == 0) err(n->line, "continue statement not within a loop");
            break;
        case N_LABELED: {
            for (int i = 0; i < nlabels_; i++) if (is(labels[i].name, n->name)) err(n->line, "label '%s' is already in use", n->name);
            NodeKind k = n->a ? n->a->kind : N_BLOCK;
            if (nlabels_ < 128) {
                labels[nlabels_].name = n->name;
                labels[nlabels_].is_loop = k == N_WHILE || k == N_FOR || k == N_DOWHILE || k == N_FOREACH;
            }
            nlabels_++;
            check_stmts(n->a);
            nlabels_--;
            break;
        }
        case N_BLOCK:
            st_enter();
            check_stmts(n->a);
            st_exit();
            break;
        case N_FUNC:
            if (cur_func) err(n->line, "function '%s' cannot be defined inside another function", n->name);
            check_func(n);
            break;
        default:
            break;
        }
    }
}

int semantic_check(Node *root) {
    errors = 0;
    nfuncs = 0;
    loop_depth = switch_depth = 0;
    nlabels_ = 0;
    cur_func = NULL;
    st_init();
    if (!root) return 0;
    collect_funcs(root->a);
    /* functions are global names: visible from everywhere (C prototypes / Java methods) */
    for (int i = 0; i < nfuncs; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (is(funcs[j]->name, funcs[i]->name)) dup = 1;
        if (dup) continue;
        Symbol *s = st_insert(funcs[i]->name, funcs[i]->type, funcs[i]->line);
        s->is_func = 1; s->dims = funcs[i]->dims; s->func = funcs[i];
    }
    /* overloads with identical parameter types are redefinitions */
    for (int i = 0; i < nfuncs; i++)
        for (int j = 0; j < i; j++) {
            if (!is(funcs[i]->name, funcs[j]->name) || list_len(funcs[i]->a) != list_len(funcs[j]->a)) continue;
            int same = 1;
            for (Node *p = funcs[i]->a, *q = funcs[j]->a; p && q; p = p->next, q = q->next)
                if (p->type != q->type || p->dims != q->dims) same = 0;
            if (same) { err(funcs[i]->line, "redefinition of function '%s' (first defined at line %d)", funcs[i]->name, funcs[j]->line); break; }
        }
    check_stmts(root->a);
    return errors;
}
