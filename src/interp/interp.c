/* Lexify TAC interpreter
 *   - pre-parses the TAC text into instructions (variables become slots, labels become indexes)
 *   - executes them; `print` writes the program output to stdout
 * Runtime errors: division/modulo by zero, runaway loops (step limit). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "interp/interp.h"

typedef enum { V_INT, V_FLT, V_BOOL } VK;
typedef struct { VK k; long long i; double f; } Val;

typedef enum { O_NONE, O_VAR, O_CONST, O_STR } OK;
typedef struct { OK k; int slot; Val v; char *s; } Opd;

typedef enum { I_ASSIGN, I_CAST, I_BIN, I_NOT, I_NEG, I_IFFALSE, I_GOTO, I_LABEL, I_PRINT, I_RETURN } IK;
typedef struct { IK k; int dst; Opd a, b; char op[3]; int label; char *spec; int line; } Ins;

#define MAX_STEPS 20000000L

static Ins  *code; static int ncode, capcode;
static char **names; static int nnames, capnames;
static int  *label_pc; static int nlabels;
static Val  *vars; static char *isset;

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

static int slot_of(const char *name) {
    for (int i = 0; i < nnames; i++) if (strcmp(names[i], name) == 0) return i;
    if (nnames == capnames) { capnames = capnames ? capnames * 2 : 64; names = xrealloc(names, capnames * sizeof *names); }
    names[nnames] = strdup(name);
    return nnames++;
}

static Val mk_int(long long i) { Val v; v.k = V_INT; v.i = i; v.f = 0; return v; }

/* parse one operand starting at *p; advances *p past it */
static Opd parse_opd(char **p) {
    Opd o; memset(&o, 0, sizeof o);
    char *s = *p;
    while (*s == ' ') s++;
    if (*s == '"') {                               /* string literal (print only) */
        char *e = s + 1;
        while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
        o.k = O_STR; o.s = malloc((size_t)(e - s)); memcpy(o.s, s + 1, (size_t)(e - s - 1)); o.s[e - s - 1] = 0;
        *p = *e ? e + 1 : e;
        return o;
    }
    char *e = s;
    while (*e && *e != ' ' && *e != ',') e++;
    char save = *e; *e = 0;
    if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0) {
        o.k = O_CONST; o.v.k = V_BOOL; o.v.i = (s[0] == 't');
    } else if (isdigit((unsigned char)s[0]) || (s[0] == '-' && (isdigit((unsigned char)s[1]) || s[1] == '.'))) {
        o.k = O_CONST;
        if (strpbrk(s, ".eEn")) { o.v.k = V_FLT; o.v.f = atof(s); }
        else { o.v.k = V_INT; o.v.i = atoll(s); }
    } else {
        o.k = O_VAR; o.slot = slot_of(s);
    }
    *e = save;
    *p = e;
    return o;
}

static Ins *new_ins(IK k, int line) {
    if (ncode == capcode) { capcode = capcode ? capcode * 2 : 256; code = xrealloc(code, capcode * sizeof *code); }
    Ins *in = &code[ncode++];
    memset(in, 0, sizeof *in);
    in->k = k; in->line = line;
    return in;
}

static void set_label(int n, int pc) {
    if (n >= nlabels) {
        int old = nlabels; nlabels = n + 16;
        label_pc = xrealloc(label_pc, nlabels * sizeof *label_pc);
        for (int i = old; i < nlabels; i++) label_pc[i] = -1;
    }
    label_pc[n] = pc;
}

static int parse_label_ref(const char *s) { while (*s == ' ') s++; return (*s == 'L') ? atoi(s + 1) : -1; }

static int parse_line(char *s, int line) {
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ')) s[--len] = 0;
    if (!*s) return 1;

    if (s[0] == 'L' && isdigit((unsigned char)s[1]) && s[len - 1] == ':') {          /* Ln: */
        Ins *in = new_ins(I_LABEL, line);
        in->label = atoi(s + 1);
        set_label(in->label, ncode - 1);
        return 1;
    }
    if (strncmp(s, "goto ", 5) == 0) { Ins *in = new_ins(I_GOTO, line); in->label = parse_label_ref(s + 5); return 1; }
    if (strncmp(s, "ifFalse ", 8) == 0) {
        char *p = s + 8; Ins *in = new_ins(I_IFFALSE, line);
        in->a = parse_opd(&p);
        while (*p == ' ') p++;
        if (strncmp(p, "goto", 4) != 0) return 0;
        in->label = parse_label_ref(p + 4);
        return 1;
    }
    if (strncmp(s, "print ", 6) == 0) {
        char *p = s + 6; Ins *in = new_ins(I_PRINT, line);
        in->a = parse_opd(&p);
        while (*p == ' ') p++;
        if (*p == ',') { p++; Opd sp = parse_opd(&p); in->spec = sp.s; }
        return 1;
    }
    if (strcmp(s, "return") == 0 || strncmp(s, "return ", 7) == 0) {
        Ins *in = new_ins(I_RETURN, line);
        if (s[6]) { char *p = s + 7; in->a = parse_opd(&p); }
        return 1;
    }
    /* dst = ... */
    char *eq = strstr(s, " = ");
    if (!eq) return 0;
    *eq = 0;
    int dst = slot_of(s);
    char *p = eq + 3;
    while (*p == ' ') p++;
    Ins *in;
    if (strncmp(p, "(float) ", 8) == 0) { in = new_ins(I_CAST, line); p += 8; in->a = parse_opd(&p); }
    else if (strncmp(p, "! ", 2) == 0)  { in = new_ins(I_NOT, line);  p += 2; in->a = parse_opd(&p); }
    else if (strncmp(p, "- ", 2) == 0)  { in = new_ins(I_NEG, line);  p += 2; in->a = parse_opd(&p); }
    else {
        Opd a = parse_opd(&p);
        while (*p == ' ') p++;
        if (!*p) { in = new_ins(I_ASSIGN, line); in->a = a; }
        else {
            in = new_ins(I_BIN, line); in->a = a;
            int n = 0;
            while (*p && *p != ' ' && n < 2) in->op[n++] = *p++;
            in->op[n] = 0;
            in->b = parse_opd(&p);
        }
    }
    in->dst = dst;
    return 1;
}

/* ---- values ---------------------------------------------------------- */

static int is_float(Val v) { return v.k == V_FLT; }
static double as_f(Val v) { return v.k == V_FLT ? v.f : (double)v.i; }
static long long as_i(Val v) { return v.k == V_FLT ? (long long)v.f : v.i; }
static int truthy(Val v) { return v.k == V_FLT ? v.f != 0.0 : v.i != 0; }
static Val wrap32(long long x) { return mk_int((int32_t)(uint32_t)(unsigned long long)x); }

static Val eval(const Opd *o) {
    if (o->k == O_CONST) return o->v;
    if (o->k == O_VAR)   return isset[o->slot] ? vars[o->slot] : mk_int(0);
    return mk_int(0);
}

static void put_string(const char *s) {
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': putchar('\n'); break;  case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break;  case '0': break;
            case 'a': putchar('\a'); break;  case '\\': putchar('\\'); break;
            case '"': putchar('"'); break;   case '\'': putchar('\''); break;
            default:  putchar(*s);
            }
        } else putchar(*s);
    }
}

static void print_val(Val v, const char *spec) {
    if (!spec) {
        if (v.k == V_FLT) printf("%g", v.f);
        else if (v.k == V_BOOL) fputs(v.i ? "true" : "false", stdout);
        else printf("%lld", v.i);
        return;
    }
    char fmt[64]; size_t n = 0;
    size_t sl = strlen(spec);
    char conv = spec[sl - 1];
    for (size_t i = 0; i + 1 < sl && n < 40; i++)
        if (spec[i] != 'l' && spec[i] != 'h' && spec[i] != 'z') fmt[n++] = spec[i];
    if (strchr("diux", conv)) { fmt[n++] = 'l'; fmt[n++] = 'l'; }
    fmt[n++] = conv; fmt[n] = 0;
    switch (conv) {
    case 'd': case 'i': printf(fmt, as_i(v)); break;
    case 'u': case 'x': printf(fmt, (unsigned long long)(uint32_t)as_i(v)); break;
    case 'c': printf(fmt, (int)as_i(v)); break;
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': printf(fmt, as_f(v)); break;
    default:  print_val(v, NULL);
    }
}

static int rt_error(const Ins *in, const char *msg) {
    fflush(stdout);
    fprintf(stderr, "Runtime error: %s\n", msg);
    (void)in;
    return 1;
}

static int execute(void) {
    long steps = 0;
    for (int pc = 0; pc < ncode; pc++) {
        if (++steps > MAX_STEPS) return rt_error(NULL, "execution limit exceeded (possible infinite loop)");
        Ins *in = &code[pc];
        switch (in->k) {
        case I_LABEL: break;
        case I_GOTO:
            if (in->label < 0 || in->label >= nlabels || label_pc[in->label] < 0) return rt_error(in, "bad jump target");
            pc = label_pc[in->label]; break;
        case I_IFFALSE:
            if (!truthy(eval(&in->a))) {
                if (in->label < 0 || in->label >= nlabels || label_pc[in->label] < 0) return rt_error(in, "bad jump target");
                pc = label_pc[in->label];
            }
            break;
        case I_PRINT:
            if (in->a.k == O_STR) put_string(in->a.s);
            else {
                print_val(eval(&in->a), in->spec);
                if (!in->spec) putchar('\n');      /* plain `print x;` is one line; printf has its own \n */
            }
            break;
        case I_RETURN: fflush(stdout); return 0;
        case I_ASSIGN: vars[in->dst] = eval(&in->a); isset[in->dst] = 1; break;
        case I_CAST: { Val a = eval(&in->a); Val r; r.k = V_FLT; r.f = as_f(a); r.i = 0; vars[in->dst] = r; isset[in->dst] = 1; break; }
        case I_NOT: { Val r = mk_int(!truthy(eval(&in->a))); r.k = V_BOOL; vars[in->dst] = r; isset[in->dst] = 1; break; }
        case I_NEG: {
            Val a = eval(&in->a), r;
            if (is_float(a)) { r.k = V_FLT; r.f = -a.f; r.i = 0; } else r = wrap32(-a.i);
            vars[in->dst] = r; isset[in->dst] = 1; break;
        }
        case I_BIN: {
            Val a = eval(&in->a), b = eval(&in->b), r;
            const char *op = in->op;
            int fl = is_float(a) || is_float(b);
            r = mk_int(0);
            if (!strcmp(op, "&&")) { r = mk_int(truthy(a) && truthy(b)); r.k = V_BOOL; }
            else if (!strcmp(op, "||")) { r = mk_int(truthy(a) || truthy(b)); r.k = V_BOOL; }
            else if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=") ||
                     !strcmp(op, "==") || !strcmp(op, "!=")) {
                int c;
                if (fl) {
                    double x = as_f(a), y = as_f(b);
                    c = !strcmp(op, "<") ? x < y : !strcmp(op, ">") ? x > y : !strcmp(op, "<=") ? x <= y :
                        !strcmp(op, ">=") ? x >= y : !strcmp(op, "==") ? x == y : x != y;
                } else {
                    long long x = a.i, y = b.i;
                    c = !strcmp(op, "<") ? x < y : !strcmp(op, ">") ? x > y : !strcmp(op, "<=") ? x <= y :
                        !strcmp(op, ">=") ? x >= y : !strcmp(op, "==") ? x == y : x != y;
                }
                r = mk_int(c); r.k = V_BOOL;
            } else if (fl) {
                double x = as_f(a), y = as_f(b), z = 0;
                switch (op[0]) { case '+': z = x + y; break; case '-': z = x - y; break;
                                 case '*': z = x * y; break; case '/': z = x / y; break; }
                r.k = V_FLT; r.f = z; r.i = 0;
            } else {
                long long x = a.i, y = b.i, z = 0;
                switch (op[0]) {
                case '+': z = x + y; break;
                case '-': z = x - y; break;
                case '*': z = x * y; break;
                case '/': if (y == 0) return rt_error(in, "division by zero"); z = x / y; break;
                case '%': if (y == 0) return rt_error(in, "modulo by zero");   z = x % y; break;
                }
                r = wrap32(z);
            }
            vars[in->dst] = r; isset[in->dst] = 1;
            break;
        }
        }
    }
    fflush(stdout);
    return 0;
}

int interp_run(FILE *tac) {
    char line[4096];
    int ln = 0;
    while (fgets(line, sizeof line, tac)) {
        ln++;
        if (!parse_line(line, ln)) { fprintf(stderr, "Runtime error: cannot understand TAC line %d\n", ln); return 1; }
    }
    vars  = calloc(nnames ? nnames : 1, sizeof *vars);
    isset = calloc(nnames ? nnames : 1, 1);
    if (!vars || !isset) { fprintf(stderr, "out of memory\n"); return 2; }
    return execute();
}
