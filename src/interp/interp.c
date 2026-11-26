/* Lexify TAC interpreter
 *   - pre-parses the TAC text into instructions (variables become slots, labels become indexes)
 *   - executes them; print / write send the program output to stdout, read takes stdin
 *   - output follows the source language: cout prints bool as 1/0 and 2.5 as 2.5,
 *     Java prints true/false and 10.0, printf uses its format.
 * Runtime errors: division/modulo by zero, array index out of bounds, runaway loops,
 * too deep recursion, too much output. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "interp/interp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef enum { V_NONE, V_INT, V_LONG, V_FLT, V_BOOL, V_CHAR, V_STR, V_ARR, V_F32 } VK;
#define IS_F(v) ((v).k == V_FLT || (v).k == V_F32)
typedef struct Arr Arr;
typedef struct { VK k; union { long long i; double f; const char *s; Arr *a; }; } Val;
struct Arr { int n; Val *e; };

typedef enum { O_NONE, O_VAR, O_CONST } OK;
typedef struct { OK k; int slot; Val v; } Opd;

typedef enum {
    I_ASSIGN, I_CAST, I_BIN, I_NOT, I_NEG, I_BNOT, I_IFFALSE, I_GOTO, I_LABEL, I_PRINT, I_WRITE, I_READ,
    I_MANIP, I_RETURN, I_HALT, I_PARAM, I_CALL, I_FUNC, I_END, I_FORMAL, I_GLOBAL, I_NEWARR, I_LOAD, I_STORE,
    I_REFPARAM
} IK;

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE, OP_AND, OP_OR,
       OP_BAND, OP_BOR, OP_XOR, OP_SHL, OP_SHR, OP_USHR };

typedef struct {
    IK k; int dst; Opd a, b; int op; int label; char *name; char *spec; int nargs; VK cast;
    Opd sizes[8]; int nsizes; int func; int line;
} Ins;

#define MAX_STEPS   50000000L
#define MAX_DEPTH   10000
#define MAX_OUTPUT  (1L << 20)

static int   lang;          /* 0 mini, 1 C, 2 C++, 3 Java (same order as Lang in ast.h) */
static Ins  *code; static int ncode, capcode;
static char **names; static int nnames, capnames;
static int  *label_pc; static int nlabels;
static Val  *vars;
static char *is_global;

typedef struct { char *name; int start, end; int *locals; int nlocals; } Func;
static Func *funcs; static int nfuncs;

/* an argument; a C++ reference argument also remembers where to copy the value back */
typedef struct { Val v; int ref; int slot; Arr *arr; long long idx; } Arg;
typedef struct { int param_slot; Arg target; } RefBack;
typedef struct { int ret_pc, dst, func; Val *saved; Arg *args; int nargs, argi; RefBack *refs; int nrefs; } Frame;
static Frame *frames; static int nframes, capframes;
static Arg  *pargs; static int npargs, cappargs;

static long out_bytes;
static int  cpp_fixed, cpp_prec = 6, cpp_boolalpha, cpp_width, cpp_left;
static char cpp_fill = ' ';

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}
static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) { fprintf(stderr, "out of memory\n"); exit(2); } return p; }

/* portable helpers (MinGW on Windows has no strndup / strcasecmp / getline) */
static char *x_strndup(const char *s, size_t n) {
    size_t l = 0; while (l < n && s[l]) l++;
    char *p = malloc(l + 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    memcpy(p, s, l); p[l] = 0;
    return p;
}
static int x_strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}
static char *x_readline(FILE *f) {          /* one whole line (any length), NULL at end of file */
    size_t cap = 256, n = 0; char *b = malloc(cap); int c;
    if (!b) return NULL;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 >= cap) { cap *= 2; b = xrealloc(b, cap); }
        b[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0 && c == EOF) { free(b); return NULL; }
    b[n] = 0;
    return b;
}

static int slot_of(const char *name) {
    for (int i = 0; i < nnames; i++) if (strcmp(names[i], name) == 0) return i;
    if (nnames == capnames) { capnames = capnames ? capnames * 2 : 64; names = xrealloc(names, capnames * sizeof *names); }
    names[nnames] = xstrdup(name);
    return nnames++;
}

static Val mk_int(long long i) { Val v; v.k = V_INT; v.i = i; return v; }
static Val mk_long(long long i) { Val v; v.k = V_LONG; v.i = i; return v; }
static Val mk_flt(double f) { Val v; v.k = V_FLT; v.f = f; return v; }
static Val mk_f32(double f) { Val v; v.k = V_F32; v.f = (float)f; return v; }
static Val mk_bool(int b) { Val v; v.k = V_BOOL; v.i = b != 0; return v; }
static Val mk_char(long long c) { Val v; v.k = V_CHAR; v.i = c; return v; }
static Val mk_str(const char *s) { Val v; v.k = V_STR; v.s = s; return v; }

/* "\n" escapes of a TAC string / char literal -> real characters */
static char *unescape(const char *s, size_t n) {
    char *o = malloc(n * 4 + 1); size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) { o[k++] = s[i]; continue; }
        char c = s[++i];
        switch (c) {
        case 'n': o[k++] = '\n'; break; case 't': o[k++] = '\t'; break; case 'r': o[k++] = '\r'; break;
        case 'a': o[k++] = '\a'; break; case 'b': o[k++] = '\b'; break; case 'f': o[k++] = '\f'; break;
        case 'v': o[k++] = '\v'; break; case '\\': o[k++] = '\\'; break; case '"': o[k++] = '"'; break;
        case '\'': o[k++] = '\''; break; case '?': o[k++] = '?'; break;
        case 'x': { unsigned v = 0; while (i + 1 < n && isxdigit((unsigned char)s[i + 1])) { i++; v = v * 16 + (isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10)); } o[k++] = (char)v; break; }
        case 'u': { unsigned v = 0; int d = 0; while (d < 4 && i + 1 < n && isxdigit((unsigned char)s[i + 1])) { i++; d++; v = v * 16 + (isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10)); }
                    if (v < 0x80) o[k++] = (char)v;
                    else if (v < 0x800) { o[k++] = (char)(0xC0 | (v >> 6)); o[k++] = (char)(0x80 | (v & 0x3F)); }
                    else { o[k++] = (char)(0xE0 | (v >> 12)); o[k++] = (char)(0x80 | ((v >> 6) & 0x3F)); o[k++] = (char)(0x80 | (v & 0x3F)); }
                    break; }
        default:
            if (c >= '0' && c <= '7') {
                unsigned v = (unsigned)(c - '0'); int d = 1;
                while (d < 3 && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '7') { i++; d++; v = v * 8 + (unsigned)(s[i] - '0'); }
                o[k++] = (char)v;
            } else o[k++] = c;
        }
    }
    o[k] = 0;
    return o;
}

/* value of a char literal body ('A', '\n', '\101', 'é') */
static long long char_body(const char *s, size_t n) {
    if (n == 0) return 0;
    if (s[0] != '\\') return (unsigned char)s[0];
    if (n >= 2 && s[1] == 'u') { long long v = 0; for (size_t i = 2; i < n; i++) v = v * 16 + (isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10)); return v; }
    char *u = unescape(s, n);
    long long v = (unsigned char)u[0];
    free(u);
    return v;
}

/* parse one operand starting at *p; advances *p past it */
static Opd parse_opd(char **p) {
    Opd o; memset(&o, 0, sizeof o);
    char *s = *p;
    while (*s == ' ') s++;
    if (*s == '"') {                               /* string literal */
        char *e = s + 1;
        while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
        o.k = O_CONST; o.v = mk_str(unescape(s + 1, (size_t)(e - s - 1)));
        *p = *e ? e + 1 : e;
        return o;
    }
    if (*s == '\'') {                              /* char literal */
        char *e = s + 1;
        while (*e && *e != '\'') { if (*e == '\\' && e[1]) e++; e++; }
        o.k = O_CONST; o.v = mk_char(char_body(s + 1, (size_t)(e - s - 1)));
        *p = *e ? e + 1 : e;
        return o;
    }
    char *e = s;
    while (*e && *e != ' ' && *e != ',' && *e != '[' && *e != ']') e++;
    char save = *e; *e = 0;
    if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0) {
        o.k = O_CONST; o.v = mk_bool(s[0] == 't');
    } else if (isdigit((unsigned char)s[0]) || (s[0] == '-' && (isdigit((unsigned char)s[1]) || s[1] == '.')) || s[0] == '.') {
        o.k = O_CONST;
        size_t L = strlen(s);
        if (L && s[L - 1] == 'f') o.v = mk_f32(atof(s));
        else if (strpbrk(s, ".eEn") && !(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) o.v = mk_flt(atof(s));
        else {
            long long v = strtoll(s, NULL, 10);
            if ((L && (s[L - 1] == 'L')) || v > 2147483647LL || v < -2147483648LL) o.v = mk_long(v);
            else o.v = mk_int(v);
        }
    } else if (*s) {
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
    in->k = k; in->line = line; in->dst = -1; in->func = -1;
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

static int op_code(const char *o) {
    static const char *ops[] = { "+", "-", "*", "/", "%", "<", ">", "<=", ">=", "==", "!=", "&&", "||",
                                 "&", "|", "^", "<<", ">>", ">>>", NULL };
    for (int i = 0; ops[i]; i++) if (strcmp(o, ops[i]) == 0) return i;
    return -1;
}

static char *word(char **p) {              /* next space-separated word */
    char *s = *p;
    while (*s == ' ') s++;
    char *e = s;
    while (*e && *e != ' ' && *e != ',' && *e != ':') e++;
    char *w = x_strndup(s, (size_t)(e - s));
    *p = e;
    return w;
}

static VK kind_of(const char *k) {
    if (!strcmp(k, "long"))   return V_LONG;
    if (!strcmp(k, "float"))  return V_FLT;
    if (!strcmp(k, "float32")) return V_F32;
    if (!strcmp(k, "char"))   return V_CHAR;
    if (!strcmp(k, "bool"))   return V_BOOL;
    if (!strcmp(k, "string")) return V_STR;
    return V_INT;
}

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
    if (strncmp(s, "function ", 9) == 0) {
        Ins *in = new_ins(I_FUNC, line);
        char *p = s + 9; in->name = word(&p);
        return 1;
    }
    if (strncmp(s, "end ", 4) == 0) { Ins *in = new_ins(I_END, line); char *p = s + 4; in->name = word(&p); return 1; }
    if (strcmp(s, "halt") == 0) { new_ins(I_HALT, line); return 1; }
    if (strncmp(s, "global ", 7) == 0) { Ins *in = new_ins(I_GLOBAL, line); char *p = s + 7; char *w = word(&p); in->dst = slot_of(w); free(w); return 1; }
    if (strncmp(s, "formal ", 7) == 0) { Ins *in = new_ins(I_FORMAL, line); char *p = s + 7; char *w = word(&p); in->dst = slot_of(w); free(w); return 1; }
    if (strncmp(s, "param ", 6) == 0) { Ins *in = new_ins(I_PARAM, line); char *p = s + 6; in->a = parse_opd(&p); return 1; }
    if (strncmp(s, "refparam ", 9) == 0) {          /* refparam x   or   refparam a[i] */
        Ins *in = new_ins(I_REFPARAM, line); char *p = s + 9;
        in->a = parse_opd(&p);
        if (*p == '[') { p++; in->b = parse_opd(&p); in->nargs = 1; }
        return 1;
    }
    if (strncmp(s, "call ", 5) == 0) {
        Ins *in = new_ins(I_CALL, line); char *p = s + 5;
        in->name = word(&p); while (*p == ',' || *p == ' ') p++; in->nargs = atoi(p);
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
    if (strncmp(s, "print ", 6) == 0 || strncmp(s, "write ", 6) == 0) {
        char *p = s + 6; Ins *in = new_ins(s[0] == 'p' ? I_PRINT : I_WRITE, line);
        in->a = parse_opd(&p);
        while (*p == ' ') p++;
        if (*p == ',') { p++; Opd sp = parse_opd(&p); if (sp.k == O_CONST && sp.v.k == V_STR) in->spec = (char *)sp.v.s; }
        return 1;
    }
    if (strncmp(s, "manip ", 6) == 0) {
        char *p = s + 6; Ins *in = new_ins(I_MANIP, line);
        in->name = word(&p);
        while (*p == ' ') p++;
        if (*p) in->a = parse_opd(&p);
        return 1;
    }
    if (strncmp(s, "read ", 5) == 0) { Ins *in = new_ins(I_READ, line); char *p = s + 5; in->name = word(&p); return 1; }
    if (strcmp(s, "return") == 0 || strncmp(s, "return ", 7) == 0) {
        Ins *in = new_ins(I_RETURN, line);
        if (s[6]) { char *p = s + 7; in->a = parse_opd(&p); }
        return 1;
    }
    /* dst = ...   or   dst[i] = ... */
    char *eq = strstr(s, " = ");
    if (!eq) return 0;
    *eq = 0;
    char *p = eq + 3;
    while (*p == ' ') p++;
    char *br = strchr(s, '[');
    if (br) {                                   /* a[i] = v */
        Ins *in = new_ins(I_STORE, line);
        *br = 0;
        in->dst = slot_of(s);
        char *ip = br + 1;
        in->b = parse_opd(&ip);
        in->a = parse_opd(&p);
        return 1;
    }
    int dst = slot_of(s);
    Ins *in;
    if (p[0] == '(' && strchr(p, ')')) {
        char *close = strchr(p, ')');
        *close = 0;
        in = new_ins(I_CAST, line); in->cast = kind_of(p + 1);
        p = close + 1; in->a = parse_opd(&p);
    }
    else if (strncmp(p, "! ", 2) == 0)  { in = new_ins(I_NOT, line);  p += 2; in->a = parse_opd(&p); }
    else if (strncmp(p, "- ", 2) == 0)  { in = new_ins(I_NEG, line);  p += 2; in->a = parse_opd(&p); }
    else if (strncmp(p, "~ ", 2) == 0)  { in = new_ins(I_BNOT, line); p += 2; in->a = parse_opd(&p); }
    else if (strncmp(p, "call ", 5) == 0) {
        in = new_ins(I_CALL, line); p += 5;
        in->name = word(&p); while (*p == ',' || *p == ' ') p++; in->nargs = atoi(p);
    }
    else if (strncmp(p, "read ", 5) == 0) { in = new_ins(I_READ, line); p += 5; in->name = word(&p); }
    else if (strncmp(p, "newarray ", 9) == 0) {
        in = new_ins(I_NEWARR, line); p += 9;
        char *k = word(&p); in->cast = kind_of(k); free(k);
        while (*p == ' ') p++;
        while (*p && in->nsizes < 8) { in->sizes[in->nsizes++] = parse_opd(&p); while (*p == ' ') p++; }
    }
    else {
        Opd a = parse_opd(&p);
        if (*p == '[') {                        /* x = a[i] */
            p++;
            in = new_ins(I_LOAD, line); in->a = a; in->b = parse_opd(&p);
        } else {
            while (*p == ' ') p++;
            if (!*p) { in = new_ins(I_ASSIGN, line); in->a = a; }
            else {
                in = new_ins(I_BIN, line); in->a = a;
                char opbuf[8]; int n = 0;
                while (*p && *p != ' ' && n < 4) opbuf[n++] = *p++;
                opbuf[n] = 0;
                in->op = op_code(opbuf);
                if (in->op < 0) return 0;
                in->b = parse_opd(&p);
            }
        }
    }
    in->dst = dst;
    return 1;
}

/* ---- output ----------------------------------------------------------- */

static int out_overflow;
static void out_str(const char *s) {
    if (out_overflow) return;
    size_t n = strlen(s);
    out_bytes += (long)n;
    if (out_bytes > MAX_OUTPUT) { out_overflow = 1; return; }
    fwrite(s, 1, n, stdout);
}
static void out_char(int c) { char b[2] = { (char)c, 0 }; if (c == 0) { if (!out_overflow) { out_bytes++; fputc(0, stdout); } return; } out_str(b); }

/* a char value as text (Java chars above 127 become UTF-8) */
static void char_text(long long c, char *buf) {
    unsigned v = (unsigned)c;
    if (lang == 3 && v >= 0x80 && v <= 0xFFFF) {
        if (v < 0x800) { buf[0] = (char)(0xC0 | (v >> 6)); buf[1] = (char)(0x80 | (v & 0x3F)); buf[2] = 0; }
        else { buf[0] = (char)(0xE0 | (v >> 12)); buf[1] = (char)(0x80 | ((v >> 6) & 0x3F)); buf[2] = (char)(0x80 | (v & 0x3F)); buf[3] = 0; }
    } else { buf[0] = (char)c; buf[1] = 0; }
}

/* Java's Double.toString: shortest digits that round-trip, 1.0E10 style for big/small numbers */
static void java_double(double d, char *buf, int is_float) {
    if (isnan(d)) { strcpy(buf, "NaN"); return; }
    if (isinf(d)) { strcpy(buf, d > 0 ? "Infinity" : "-Infinity"); return; }
    if (d == 0) { strcpy(buf, signbit(d) ? "-0.0" : "0.0"); return; }
    char e[64];
    int p;
    for (p = 1; p <= 17; p++) {
        snprintf(e, sizeof e, "%.*e", p - 1, d);
        if (is_float ? (float)strtod(e, NULL) == (float)d : strtod(e, NULL) == d) break;
    }
    /* e = [-]D.DDDDe[+-]XX */
    char digits[32]; int nd = 0, neg = 0;
    const char *q = e;
    if (*q == '-') { neg = 1; q++; }
    while (*q && *q != 'e') { if (isdigit((unsigned char)*q)) digits[nd++] = *q; q++; }
    int ex = atoi(q + 1);
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = 0;
    char *o = buf;
    if (neg) *o++ = '-';
    double ad = fabs(d);
    if (ad >= 1e-3 && ad < 1e7) {
        if (ex >= 0) {
            for (int i = 0; i <= ex; i++) *o++ = i < nd ? digits[i] : '0';
            *o++ = '.';
            if (nd > ex + 1) for (int i = ex + 1; i < nd; i++) *o++ = digits[i];
            else *o++ = '0';
        } else {
            *o++ = '0'; *o++ = '.';
            for (int i = 0; i < -ex - 1; i++) *o++ = '0';
            for (int i = 0; i < nd; i++) *o++ = digits[i];
        }
        *o = 0;
    } else {
        *o++ = digits[0]; *o++ = '.';
        if (nd > 1) for (int i = 1; i < nd; i++) *o++ = digits[i];
        else *o++ = '0';
        sprintf(o, "E%d", ex);
    }
}

static double as_f(Val v) { return IS_F(v) ? v.f : (v.k == V_STR || v.k == V_ARR || v.k == V_NONE) ? 0.0 : (double)v.i; }
static long long as_i(Val v) {
    if (IS_F(v)) { if (isnan(v.f)) return 0; if (v.f >= 9.2e18) return INT64_MAX; if (v.f <= -9.2e18) return INT64_MIN; return (long long)v.f; }
    if (v.k == V_STR || v.k == V_ARR || v.k == V_NONE) return 0;
    return v.i;
}
static int truthy(Val v) {
    switch (v.k) { case V_FLT: case V_F32: return v.f != 0.0; case V_STR: case V_ARR: return 1; case V_NONE: return 0; default: return v.i != 0; }
}

/* the value as text, following the rules of the language (string concatenation, cout, println) */
static void val_text(Val v, char *buf, size_t n, int for_concat) {
    switch (v.k) {
    case V_NONE: snprintf(buf, n, "0"); break;
    case V_INT: case V_LONG: snprintf(buf, n, "%lld", v.i); break;
    case V_CHAR: char_text(v.i, buf); break;
    case V_BOOL:
        if (lang == 3 || lang == 0 || (lang == 2 && cpp_boolalpha && !for_concat)) snprintf(buf, n, "%s", v.i ? "true" : "false");
        else snprintf(buf, n, "%d", v.i ? 1 : 0);
        break;
    case V_FLT: case V_F32:
        if (lang == 3) java_double(v.f, buf, v.k == V_F32);
        else if (lang == 2 && !for_concat) {
            if (cpp_fixed) snprintf(buf, n, "%.*f", cpp_prec, v.f);
            else snprintf(buf, n, "%.*g", cpp_prec ? cpp_prec : 1, v.f);
        } else if (lang == 2) snprintf(buf, n, "%f", v.f);
        else snprintf(buf, n, "%g", v.f);
        break;
    case V_STR: snprintf(buf, n, "%s", v.s); break;
    case V_ARR: snprintf(buf, n, "[array@%lx]", (unsigned long)(uintptr_t)v.a & 0xfffff); break;
    }
}

static char *val_string(Val v, int for_concat) {
    if (v.k == V_STR) return (char *)v.s;
    char buf[512];
    val_text(v, buf, sizeof buf, for_concat);
    return xstrdup(buf);
}

/* printf-style conversion of one value with a specifier such as %d, %-5s, %.2f, %c */
static void format_one(Val v, const char *spec, char *buf, size_t n) {
    char fmt[64]; size_t k = 0;
    size_t sl = strlen(spec);
    char conv = spec[sl - 1];
    for (size_t i = 0; i + 1 < sl && k < 40; i++)
        if (!strchr("lhzLjtq", spec[i])) fmt[k++] = spec[i];
    switch (conv) {
    case 'd': case 'i':
        fmt[k++] = 'l'; fmt[k++] = 'l'; fmt[k++] = 'd'; fmt[k] = 0;
        snprintf(buf, n, fmt, as_i(v)); break;
    case 'u': case 'x': case 'X': case 'o': {
        fmt[k++] = 'l'; fmt[k++] = 'l'; fmt[k++] = conv; fmt[k] = 0;
        unsigned long long u = (v.k == V_LONG) ? (unsigned long long)as_i(v) : (unsigned long long)(uint32_t)as_i(v);
        snprintf(buf, n, fmt, u); break;
    }
    case 'c': {
        fmt[k++] = 's'; fmt[k] = 0;
        char c[8]; char_text(as_i(v), c);
        snprintf(buf, n, fmt, c); break;
    }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        fmt[k++] = conv; fmt[k] = 0;
        snprintf(buf, n, fmt, as_f(v)); break;
    case 'b': case 'B':
        fmt[k++] = 's'; fmt[k] = 0;
        snprintf(buf, n, fmt, truthy(v) ? (conv == 'B' ? "TRUE" : "true") : (conv == 'B' ? "FALSE" : "false")); break;
    case 's': default: {
        fmt[k++] = 's'; fmt[k] = 0;
        char *t = val_string(v, 1);
        if (IS_F(v) && lang == 1) { static char tb[64]; snprintf(tb, sizeof tb, "%g", v.f); t = tb; }
        snprintf(buf, n, fmt, t);
        if (conv == 'S') for (char *c = buf; *c; c++) *c = (char)toupper((unsigned char)*c);
    }
    }
}

static void print_plain(Val v) {         /* mini language: print x; */
    char buf[512];
    if (IS_F(v)) snprintf(buf, sizeof buf, "%g", v.f);
    else if (v.k == V_BOOL) snprintf(buf, sizeof buf, "%s", v.i ? "true" : "false");
    else if (v.k == V_STR) { out_str(v.s); return; }
    else val_text(v, buf, sizeof buf, 0);
    out_str(buf);
}

static void write_val(Val v) {           /* cout << v  /  System.out.print(v) */
    char sbuf[512];
    const char *text;
    if (v.k == V_STR) text = v.s;
    else { val_text(v, sbuf, sizeof sbuf, 0); text = sbuf; }
    if (cpp_width > 0) {
        int len = (int)strlen(text);
        if (len < cpp_width) {
            if (cpp_left) { out_str(text); for (int i = len; i < cpp_width; i++) out_char(cpp_fill); }
            else { for (int i = len; i < cpp_width; i++) out_char(cpp_fill); out_str(text); }
            cpp_width = 0;
            return;
        }
        cpp_width = 0;
    }
    if (v.k == V_CHAR && v.i == 0) { out_char(0); return; }
    out_str(text);
}

/* ---- runtime errors --------------------------------------------------- */

static int rt_line;
static int rt_error(const char *msg) {
    fflush(stdout);
    if (lang == 3) fprintf(stderr, "Exception in thread \"main\" %s\n", msg);
    else fprintf(stderr, "Runtime error: %s\n", msg);
    (void)rt_line;
    return 1;
}

/* ---- input (stdin) ---------------------------------------------------- */

static int in_fail, read_count, read_eof;
static void skip_ws(void) { int c; while ((c = getchar()) != EOF && isspace(c)) ; if (c != EOF) ungetc(c, stdin); }
static char *read_word(void) {
    skip_ws();
    size_t cap = 64, n = 0; char *b = malloc(cap); int c;
    while ((c = getchar()) != EOF && !isspace(c)) { if (n + 1 >= cap) { cap *= 2; b = xrealloc(b, cap); } b[n++] = (char)c; }
    if (c != EOF) ungetc(c, stdin);
    b[n] = 0;
    if (n == 0) in_fail = 1;
    return b;
}
static char *read_line(int keep_nl) {
    size_t cap = 128, n = 0; char *b = malloc(cap); int c;
    while ((c = getchar()) != EOF && c != '\n') { if (n + 2 >= cap) { cap *= 2; b = xrealloc(b, cap); } b[n++] = (char)c; }
    if (n && b[n - 1] == '\r') n--;
    if (c == '\n' && keep_nl) b[n++] = '\n';
    if (c == EOF && n == 0) in_fail = 1;
    b[n] = 0;
    return b;
}

static Val do_read_raw(const char *kind);
static Val do_read(const char *kind) {
    if (!strcmp(kind, "mark")) { read_count = 0; read_eof = 0; return mk_int(0); }
    if (!strcmp(kind, "ok")) return mk_bool(!in_fail);
    if (!strcmp(kind, "scanfret")) return mk_int(read_count ? read_count : (in_fail || feof(stdin)) ? -1 : 0);
    int was = in_fail;
    Val v = do_read_raw(kind);
    if (!in_fail && !was && strncmp(kind, "lit", 3) && strncmp(kind, "wslit", 5) && strcmp(kind, "ignore1") && strcmp(kind, "ignoreline") && strcmp(kind, "hasnext") && strcmp(kind, "eof")) read_count++;
    if (!was && in_fail && feof(stdin)) read_eof = 1;
    return v;
}
static Val do_read_raw(const char *kind) {
    if (!strcmp(kind, "int") || !strcmp(kind, "long")) {
        if (in_fail) return !strcmp(kind, "int") ? mk_int(0) : mk_long(0);
        skip_ws();
        char buf[64]; int n = 0, c = getchar();
        if (c == '-' || c == '+') { buf[n++] = (char)c; c = getchar(); }
        while (c != EOF && isdigit(c) && n < 60) { buf[n++] = (char)c; c = getchar(); }
        if (c != EOF) ungetc(c, stdin);
        buf[n] = 0;
        if (n == 0 || (n == 1 && !isdigit((unsigned char)buf[0]))) in_fail = 1;
        long long v = strtoll(buf, NULL, 10);
        return !strcmp(kind, "int") ? mk_int((int32_t)v) : mk_long(v);
    }
    if (!strcmp(kind, "float") || !strcmp(kind, "float32")) {
        int f32 = kind[5] == '3';
        if (in_fail) return f32 ? mk_f32(0) : mk_flt(0);
        char *w = read_word();
        char *end; double d = strtod(w, &end);
        if (end == w) in_fail = 1;
        return f32 ? mk_f32(d) : mk_flt(d);
    }
    if (!strcmp(kind, "char")) { skip_ws(); int c = getchar(); if (c == EOF) { in_fail = 1; c = 0; } return mk_char(c); }
    if (!strcmp(kind, "rawchar")) { int c = getchar(); return lang == 3 ? mk_char(c == EOF ? 0 : c) : mk_int(c == EOF ? -1 : c); }
    if (!strcmp(kind, "word")) return mk_str(read_word());
    if (!strcmp(kind, "line")) return mk_str(read_line(0));
    if (!strcmp(kind, "fline")) return mk_str(read_line(1));
    if (!strcmp(kind, "bool")) { char *w = read_word(); return mk_bool(!strcmp(w, "true") || !strcmp(w, "1") || !strcmp(w, "TRUE")); }
    if (!strncmp(kind, "lit", 3) || !strncmp(kind, "wslit", 5)) {   /* scanf format literal: must match */
        if (in_fail) return mk_int(0);
        if (kind[0] == 'w') skip_ws();
        int want = atoi(kind + (kind[0] == 'w' ? 5 : 3));
        int c = getchar();
        if (c != want) { if (c != EOF) ungetc(c, stdin); in_fail = 1; }
        return mk_int(0);
    }
    if (!strcmp(kind, "ignore1")) { getchar(); return mk_int(0); }
    if (!strcmp(kind, "ignoreline")) { int c; while ((c = getchar()) != EOF && c != '\n') ; return mk_int(0); }
    if (!strcmp(kind, "hasnext")) { skip_ws(); int c = getchar(); if (c != EOF) ungetc(c, stdin); return mk_bool(c != EOF); }
    if (!strcmp(kind, "eof")) { int c = getchar(); if (c != EOF) ungetc(c, stdin); return mk_bool(c == EOF || in_fail); }
    return mk_int(0);
}

/* ---- arithmetic -------------------------------------------------------- */

static Val wrap32(long long x) { return mk_int((int32_t)(uint32_t)(unsigned long long)x); }
static Val num_result(long long x, int is_long) { return is_long ? mk_long(x) : wrap32(x); }

static char *concat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 1);
    if (!r) { fprintf(stderr, "out of memory\n"); exit(2); }
    memcpy(r, a, la); memcpy(r + la, b, lb + 1);
    return r;
}

static int binop(int op, Val a, Val b, Val *r) {
    if (op == OP_AND) { *r = mk_bool(truthy(a) && truthy(b)); return 0; }
    if (op == OP_OR)  { *r = mk_bool(truthy(a) || truthy(b)); return 0; }
    if (op == OP_ADD && (a.k == V_STR || b.k == V_STR)) {
        *r = mk_str(concat(val_string(a, 1), val_string(b, 1)));
        return 0;
    }
    if (op >= OP_LT && op <= OP_NE) {
        int c;
        if (a.k == V_STR && b.k == V_STR) {
            int s = strcmp(a.s, b.s);
            c = op == OP_LT ? s < 0 : op == OP_GT ? s > 0 : op == OP_LE ? s <= 0 : op == OP_GE ? s >= 0 : op == OP_EQ ? s == 0 : s != 0;
        } else if (a.k == V_STR || b.k == V_STR) {
            /* string vs char (C++ s == 'a') */
            Val s = a.k == V_STR ? a : b, ch = a.k == V_STR ? b : a;
            int eq = ch.k == V_CHAR && strlen(s.s) == 1 && (unsigned char)s.s[0] == (unsigned char)ch.i;
            c = op == OP_EQ ? eq : op == OP_NE ? !eq : 0;
        } else if (a.k == V_ARR || b.k == V_ARR) {
            int eq = a.k == b.k && a.a == b.a;
            c = op == OP_EQ ? eq : op == OP_NE ? !eq : 0;
        } else if (IS_F(a) || IS_F(b)) {
            double x = as_f(a), y = as_f(b);
            c = op == OP_LT ? x < y : op == OP_GT ? x > y : op == OP_LE ? x <= y : op == OP_GE ? x >= y : op == OP_EQ ? x == y : x != y;
        } else {
            long long x = as_i(a), y = as_i(b);
            c = op == OP_LT ? x < y : op == OP_GT ? x > y : op == OP_LE ? x <= y : op == OP_GE ? x >= y : op == OP_EQ ? x == y : x != y;
        }
        *r = mk_bool(c);
        return 0;
    }
    if (a.k == V_BOOL && b.k == V_BOOL && (op == OP_BAND || op == OP_BOR || op == OP_XOR)) {
        *r = mk_bool(op == OP_BAND ? (a.i & b.i) : op == OP_BOR ? (a.i | b.i) : (a.i ^ b.i));
        return 0;
    }
    if (IS_F(a) || IS_F(b)) {
        double x = as_f(a), y = as_f(b), z = 0;
        switch (op) {
        case OP_ADD: z = x + y; break; case OP_SUB: z = x - y; break;
        case OP_MUL: z = x * y; break; case OP_DIV: z = x / y; break;
        case OP_MOD: z = fmod(x, y); break;
        default: return rt_error("bitwise operator used on a floating-point value");
        }
        /* float op float (or int) stays a 32-bit float; anything with a double is a double */
        *r = (a.k == V_FLT || b.k == V_FLT) ? mk_flt(z) : mk_f32(z);
        return 0;
    }
    int is_long = a.k == V_LONG || b.k == V_LONG;
    long long x = as_i(a), y = as_i(b);
    unsigned long long ux = (unsigned long long)x, uy = (unsigned long long)y;
    switch (op) {
    case OP_ADD: *r = num_result((long long)(ux + uy), is_long); break;
    case OP_SUB: *r = num_result((long long)(ux - uy), is_long); break;
    case OP_MUL: *r = num_result((long long)(ux * uy), is_long); break;
    case OP_DIV:
        if (y == 0) return rt_error(lang == 3 ? "java.lang.ArithmeticException: / by zero" : "division by zero");
        if (y == -1) *r = num_result((long long)(0ULL - ux), is_long);
        else *r = num_result(x / y, is_long);
        break;
    case OP_MOD:
        if (y == 0) return rt_error(lang == 3 ? "java.lang.ArithmeticException: / by zero" : "modulo by zero");
        *r = num_result(y == -1 ? 0 : x % y, is_long);
        break;
    case OP_BAND: *r = num_result(x & y, is_long); break;
    case OP_BOR:  *r = num_result(x | y, is_long); break;
    case OP_XOR:  *r = num_result(x ^ y, is_long); break;
    case OP_SHL:  *r = is_long || a.k == V_LONG ? mk_long((long long)(ux << (y & 63))) : wrap32((long long)((uint32_t)x << (y & 31))); break;
    case OP_SHR:  *r = a.k == V_LONG ? mk_long(x >> (y & 63)) : wrap32((int32_t)x >> (y & 31)); break;
    case OP_USHR: *r = a.k == V_LONG ? mk_long((long long)(ux >> (y & 63))) : wrap32((long long)((uint32_t)x >> (y & 31))); break;
    default: *r = mk_int(0);
    }
    if (a.k == V_LONG && (op == OP_SHL)) *r = mk_long(r->i);
    return 0;
}

static Val cast_to(Val v, VK k) {
    switch (k) {
    case V_FLT: return mk_flt(as_f(v));
    case V_F32: return mk_f32(as_f(v));
    case V_INT:
        if (IS_F(v)) {
            if (isnan(v.f)) return mk_int(0);
            if (v.f >= 2147483647.0) return mk_int(2147483647);
            if (v.f <= -2147483648.0) return mk_int(-2147483648LL);
            return mk_int((long long)v.f);
        }
        return wrap32(as_i(v));
    case V_LONG: return mk_long(as_i(v));
    case V_CHAR: {
        long long c = as_i(v);
        return mk_char(lang == 3 ? (c & 0xFFFF) : (long long)(unsigned char)c);
    }
    case V_BOOL: return mk_bool(truthy(v));
    case V_STR: return mk_str(val_string(v, 1));
    default: return v;
    }
}

/* ---- arrays ------------------------------------------------------------ */

static Val zero_of(VK k) {
    switch (k) {
    case V_FLT: return mk_flt(0); case V_F32: return mk_f32(0); case V_BOOL: return mk_bool(0); case V_CHAR: return mk_char(0);
    case V_LONG: return mk_long(0); case V_STR: return mk_str(""); default: return mk_int(0);
    }
}

static int new_array(VK k, long long *sizes, int n, Val *out) {
    if (sizes[0] < 0) return rt_error(lang == 3 ? "java.lang.NegativeArraySizeException" : "negative array size");
    if (sizes[0] > 50000000) return rt_error("array too large");
    Arr *a = calloc(1, sizeof *a);
    a->n = (int)sizes[0];
    a->e = calloc((size_t)(a->n ? a->n : 1), sizeof(Val));
    if (!a->e) return rt_error("out of memory");
    for (int i = 0; i < a->n; i++) {
        if (n > 1) { if (new_array(k, sizes + 1, n - 1, &a->e[i])) return 1; }
        else a->e[i] = zero_of(k);
    }
    out->k = V_ARR; out->a = a;
    return 0;
}

static int bounds_error(long long i, long long n) {
    char msg[160];
    if (lang == 3) snprintf(msg, sizeof msg, "java.lang.ArrayIndexOutOfBoundsException: Index %lld out of bounds for length %lld", i, n);
    else snprintf(msg, sizeof msg, "array index %lld out of bounds (size %lld)", i, n);
    return rt_error(msg);
}

/* ---- builtins ------------------------------------------------------------ */

static char *str_upper(const char *s, int up) {
    char *r = xstrdup(s);
    for (char *p = r; *p; p++) *p = (char)(up ? toupper((unsigned char)*p) : tolower((unsigned char)*p));
    return r;
}

static long long find_str(const char *h, const char *n, long long from) {
    long long L = (long long)strlen(h);
    if (from < 0) from = 0;
    if (from > L) return -1;
    const char *p = strstr(h + from, n);
    return p ? (long long)(p - h) : -1;
}

static int cmp_vals(const void *x, const void *y) {
    const Val *a = x, *b = y;
    if (a->k == V_STR && b->k == V_STR) return strcmp(a->s, b->s);
    double p = as_f(*a), q = as_f(*b);
    if (!IS_F(*a) && !IS_F(*b)) { long long i = as_i(*a), j = as_i(*b); return (i > j) - (i < j); }
    return (p > q) - (p < q);
}

static int string_oob(long long i, long long n) {
    char msg[160];
    if (lang == 3) snprintf(msg, sizeof msg, "java.lang.StringIndexOutOfBoundsException: index %lld, length %lld", i, n);
    else snprintf(msg, sizeof msg, "string index %lld out of range (length %lld)", i, n);
    return rt_error(msg);
}

static int halted, exit_code;

/* runtime printf for String.format */
static char *format_runtime(const char *fmt, Val *args, int n) {
    size_t cap = 256, len = 0; char *o = malloc(cap); int ai = 0;
    for (size_t i = 0; fmt[i]; i++) {
        char piece[1024]; piece[0] = 0;
        if (fmt[i] == '%' && fmt[i + 1] == '%') { strcpy(piece, "%"); i++; }
        else if (fmt[i] == '%' && fmt[i + 1] == 'n') { strcpy(piece, "\n"); i++; }
        else if (fmt[i] == '%') {
            size_t j = i + 1;
            while (fmt[j] && strchr("0123456789.-+ #lhz,", fmt[j])) j++;
            if (fmt[j] && strchr("dioufFeEgGcsSxXbB", fmt[j])) {
                char spec[64]; size_t k = 0;
                for (size_t q = i; q <= j && k < 60; q++) if (fmt[q] != ',') spec[k++] = fmt[q];
                spec[k] = 0;
                if (ai < n) format_one(args[ai++], spec, piece, sizeof piece);
                i = j;
            } else { piece[0] = '%'; piece[1] = 0; }
        } else { piece[0] = fmt[i]; piece[1] = 0; }
        size_t pl = strlen(piece);
        while (len + pl + 1 > cap) { cap *= 2; o = xrealloc(o, cap); }
        memcpy(o + len, piece, pl); len += pl;
    }
    o[len] = 0;
    return o;
}

static int builtin(const char *f, Val *a, int n, Val *r) {
    *r = mk_int(0);
    #define F(x) (!strcmp(f, x))
    #define A0 (n > 0 ? a[0] : mk_int(0))
    #define A1 (n > 1 ? a[1] : mk_int(0))
    #define A2 (n > 2 ? a[2] : mk_int(0))
    if (F("sqrt"))  { *r = mk_flt(sqrt(as_f(A0))); return 0; }
    if (F("pow"))   { *r = mk_flt(pow(as_f(A0), as_f(A1))); return 0; }
    if (F("floor")) { *r = mk_flt(floor(as_f(A0))); return 0; }
    if (F("ceil"))  { *r = mk_flt(ceil(as_f(A0))); return 0; }
    if (F("round")) { *r = mk_flt(round(as_f(A0))); return 0; }
    if (F("lround")) { double x = floor(as_f(A0) + 0.5); *r = A0.k == V_FLT ? mk_long((long long)x) : mk_int((long long)x); return 0; }
    if (F("abs") && A0.k == V_F32) { *r = mk_f32(fabs(A0.f)); return 0; }
    if (F("trunc")) { *r = mk_flt(trunc(as_f(A0))); return 0; }
    if (F("exp"))   { *r = mk_flt(exp(as_f(A0))); return 0; }
    if (F("log"))   { *r = mk_flt(log(as_f(A0))); return 0; }
    if (F("log10")) { *r = mk_flt(log10(as_f(A0))); return 0; }
    if (F("log2"))  { *r = mk_flt(log2(as_f(A0))); return 0; }
    if (F("sin"))   { *r = mk_flt(sin(as_f(A0))); return 0; }
    if (F("cos"))   { *r = mk_flt(cos(as_f(A0))); return 0; }
    if (F("tan"))   { *r = mk_flt(tan(as_f(A0))); return 0; }
    if (F("asin"))  { *r = mk_flt(asin(as_f(A0))); return 0; }
    if (F("acos"))  { *r = mk_flt(acos(as_f(A0))); return 0; }
    if (F("atan"))  { *r = mk_flt(atan(as_f(A0))); return 0; }
    if (F("sinh"))  { *r = mk_flt(sinh(as_f(A0))); return 0; }
    if (F("cosh"))  { *r = mk_flt(cosh(as_f(A0))); return 0; }
    if (F("tanh"))  { *r = mk_flt(tanh(as_f(A0))); return 0; }
    if (F("atan2")) { *r = mk_flt(atan2(as_f(A0), as_f(A1))); return 0; }
    if (F("hypot")) { *r = mk_flt(hypot(as_f(A0), as_f(A1))); return 0; }
    if (F("fmod"))  { *r = mk_flt(fmod(as_f(A0), as_f(A1))); return 0; }
    if (F("cbrt"))  { *r = mk_flt(cbrt(as_f(A0))); return 0; }
    if (F("toRadians")) { *r = mk_flt(as_f(A0) * M_PI / 180.0); return 0; }
    if (F("toDegrees")) { *r = mk_flt(as_f(A0) * 180.0 / M_PI); return 0; }
    if (F("signum")) { double x = as_f(A0); *r = mk_flt((x > 0) - (x < 0)); return 0; }
    if (F("abs")) {
        Val x = A0;
        if (x.k == V_FLT) *r = mk_flt(fabs(x.f));
        else if (x.k == V_LONG) *r = mk_long(x.i < 0 ? -x.i : x.i);
        else *r = wrap32(x.i < 0 ? -x.i : x.i);
        return 0;
    }
    if (F("max") || F("min")) {
        Val x = A0, y = A1;
        int pick_x = F("max") ? (cmp_vals(&x, &y) >= 0) : (cmp_vals(&x, &y) <= 0);
        Val p = pick_x ? x : y;
        if (x.k == V_FLT || y.k == V_FLT) *r = mk_flt(as_f(p));
        else if (IS_F(x) || IS_F(y)) *r = mk_f32(as_f(p));
        else if (x.k == V_LONG || y.k == V_LONG) *r = mk_long(as_i(p));
        else if (x.k == V_CHAR && y.k == V_CHAR) *r = p;
        else *r = mk_int(as_i(p));
        return 0;
    }
    if (F("floorDiv") || F("floorMod")) {
        long long x = as_i(A0), y = as_i(A1);
        if (y == 0) return rt_error(lang == 3 ? "java.lang.ArithmeticException: / by zero" : "division by zero");
        long long q = x / y; if ((x % y != 0) && ((x < 0) != (y < 0))) q--;
        long long v = F("floorDiv") ? q : x - q * y;
        *r = (A0.k == V_LONG || A1.k == V_LONG) ? mk_long(v) : mk_int(v);
        return 0;
    }
    if (F("random")) { *r = mk_flt((double)rand() / ((double)RAND_MAX + 1.0)); return 0; }
    if (F("rand"))   { *r = mk_int(rand()); return 0; }
    if (F("time"))   { *r = mk_long((long long)time(NULL)); return 0; }
    if (F("exit"))   { halted = 1; exit_code = (int)as_i(A0); return 0; }
    if (F("sum"))    { return binop(OP_ADD, A0, A1, r); }

    if (F("len")) {
        Val x = A0;
        if (x.k == V_ARR) *r = mk_int(x.a->n);
        else if (x.k == V_STR) *r = mk_int((long long)strlen(x.s));
        else return rt_error(lang == 3 ? "java.lang.NullPointerException" : "length of a non-array value");
        return 0;
    }
    /* strings */
    if (!strncmp(f, "String.", 7)) {
        Val s0 = A0;
        const char *s = s0.k == V_STR ? s0.s : val_string(s0, 1);
        long long L = (long long)strlen(s);
        const char *m = f + 7;
        #define M(x) (!strcmp(m, x))
        if (M("length")) { *r = mk_int(L); return 0; }
        if (M("charAt")) {
            long long i = as_i(A1);
            if (i < 0 || i >= L) return string_oob(i, L);
            *r = mk_char((unsigned char)s[i]); return 0;
        }
        if (M("equals")) { *r = mk_bool(A1.k == V_STR && !strcmp(s, A1.s)); return 0; }
        if (M("equalsIgnoreCase")) { *r = mk_bool(A1.k == V_STR && !x_strcasecmp(s, A1.s)); return 0; }
        if (M("compareTo") || M("compareToIgnoreCase")) {
            const char *t = A1.k == V_STR ? A1.s : "";
            long long lt = (long long)strlen(t), i;
            int ic = M("compareToIgnoreCase");
            for (i = 0; i < L && i < lt; i++) {
                int x = (unsigned char)s[i], y = (unsigned char)t[i];
                if (ic) { x = tolower(x); y = tolower(y); }
                if (x != y) { *r = mk_int(lang == 3 ? x - y : (x < y ? -1 : 1)); return 0; }
            }
            *r = mk_int(lang == 3 ? L - lt : (L < lt ? -1 : L > lt));
            return 0;
        }
        if (M("isEmpty")) { *r = mk_bool(L == 0); return 0; }
        if (M("toUpperCase")) { *r = mk_str(str_upper(s, 1)); return 0; }
        if (M("toLowerCase")) { *r = mk_str(str_upper(s, 0)); return 0; }
        if (M("trim")) {
            const char *b = s; while (*b && isspace((unsigned char)*b)) b++;
            const char *e = s + L; while (e > b && isspace((unsigned char)e[-1])) e--;
            *r = mk_str(x_strndup(b, (size_t)(e - b))); return 0;
        }
        if (M("substring")) {
            long long b = as_i(A1), e = n > 2 ? as_i(A2) : L;
            if (b < 0 || e > L || b > e) {
                char msg[160]; snprintf(msg, sizeof msg, "%sbegin %lld, end %lld, length %lld",
                                        lang == 3 ? "java.lang.StringIndexOutOfBoundsException: " : "substring: ", b, e, L);
                return rt_error(msg);
            }
            *r = mk_str(x_strndup(s + b, (size_t)(e - b))); return 0;
        }
        if (M("substr")) {
            long long b = n > 1 ? as_i(A1) : 0, cnt = n > 2 ? as_i(A2) : L;
            if (b < 0 || b > L) return rt_error("std::out_of_range: substr position out of range");
            if (cnt < 0 || b + cnt > L) cnt = L - b;
            *r = mk_str(x_strndup(s + b, (size_t)cnt)); return 0;
        }
        if (M("indexOf") || M("lastIndexOf") || M("contains") || M("startsWith") || M("endsWith")) {
            char nb[8]; const char *needle;
            if (A1.k == V_STR) needle = A1.s; else { char_text(as_i(A1), nb); needle = nb; }
            long long nl = (long long)strlen(needle);
            if (M("indexOf")) { *r = mk_int(find_str(s, needle, n > 2 ? as_i(A2) : 0)); return 0; }
            if (M("contains")) { *r = mk_bool(strstr(s, needle) != NULL); return 0; }
            if (M("startsWith")) { *r = mk_bool(!strncmp(s, needle, (size_t)nl)); return 0; }
            if (M("endsWith")) { *r = mk_bool(nl <= L && !strcmp(s + L - nl, needle)); return 0; }
            long long last = -1, p = -1;
            while ((p = find_str(s, needle, p + 1)) >= 0) { last = p; if (nl == 0) break; }
            *r = mk_int(last); return 0;
        }
        if (M("concat")) { *r = mk_str(concat(s, val_string(A1, 1))); return 0; }
        if (M("repeat")) {
            long long k = as_i(A1); if (k < 0) k = 0;
            char *o = malloc((size_t)(L * k + 1)); o[0] = 0;
            for (long long i = 0; i < k; i++) memcpy(o + i * L, s, (size_t)L);
            o[L * k] = 0; *r = mk_str(o); return 0;
        }
        if (M("replace")) {
            char b1[8], b2[8]; const char *from, *to;
            if (A1.k == V_STR) from = A1.s; else { char_text(as_i(A1), b1); from = b1; }
            if (A2.k == V_STR) to = A2.s; else { char_text(as_i(A2), b2); to = b2; }
            size_t fl = strlen(from), tl = strlen(to);
            if (!fl) { *r = mk_str(s); return 0; }
            size_t cap = (size_t)L * (tl + 1) + 1, k = 0; char *o = malloc(cap);
            for (const char *p = s; *p; ) {
                if (!strncmp(p, from, fl)) { memcpy(o + k, to, tl); k += tl; p += fl; }
                else o[k++] = *p++;
            }
            o[k] = 0; *r = mk_str(o); return 0;
        }
        if (M("self")) { *r = mk_str(s); return 0; }
        if (M("empty")) { *r = mk_str(""); return 0; }
        if (M("fromChars")) {
            if (A0.k != V_ARR) { *r = mk_str(""); return 0; }
            char *o = malloc((size_t)A0.a->n * 3 + 1); size_t k = 0;
            for (int i = 0; i < A0.a->n; i++) { char cb[8]; char_text(as_i(A0.a->e[i]), cb); for (char *q = cb; *q; q++) o[k++] = *q; }
            o[k] = 0; *r = mk_str(o); return 0;
        }
        if (M("pop_back")) { *r = mk_str(L ? x_strndup(s, (size_t)(L - 1)) : ""); return 0; }
        if (M("clear")) { *r = mk_str(""); return 0; }
        if (M("count")) {
            long long c = 0; char nb[8]; char_text(as_i(A1), nb);
            if (A1.k == V_STR) { for (long long p = 0; (p = find_str(s, A1.s, p)) >= 0 && *A1.s; p++) c++; }
            else for (long long i = 0; i < L; i++) if ((unsigned char)s[i] == (unsigned char)as_i(A1)) c++;
            *r = mk_int(c); return 0;
        }
        if (M("fill")) {                              /* string(n, c) */
            long long k = as_i(A0); if (k < 0) k = 0;
            char *o = malloc((size_t)k + 1); memset(o, (char)as_i(A1), (size_t)k); o[k] = 0;
            *r = mk_str(o); return 0;
        }
        if (M("append")) { *r = mk_str(concat(s, val_string(A1, 1))); return 0; }
        if (M("reverse")) {
            char *o = xstrdup(s);
            for (long long i = 0, j = L - 1; i < j; i++, j--) { char t = o[i]; o[i] = o[j]; o[j] = t; }
            *r = mk_str(o); return 0;
        }
        if (M("sort") || M("sortDesc")) {
            char *o = xstrdup(s);
            int desc = M("sortDesc");
            for (long long i = 1; i < L; i++) {          /* insertion sort: strings are short */
                char c = o[i]; long long j = i - 1;
                while (j >= 0 && (desc ? o[j] < c : o[j] > c)) { o[j + 1] = o[j]; j--; }
                o[j + 1] = c;
            }
            *r = mk_str(o); return 0;
        }
        if (M("insert")) {
            long long i = as_i(A1);
            if (i < 0 || i > L) return string_oob(i, L);
            char *ins = val_string(A2, 1);
            size_t il = strlen(ins);
            char *o = malloc((size_t)L + il + 1);
            memcpy(o, s, (size_t)i); memcpy(o + i, ins, il); memcpy(o + i + il, s + i, (size_t)(L - i) + 1);
            *r = mk_str(o); return 0;
        }
        if (M("deleteCharAt") || M("delete") || M("erase")) {
            long long b, e;
            if (M("deleteCharAt")) { b = as_i(A1); e = b + 1; if (b < 0 || b >= L) return string_oob(b, L); }
            else if (M("delete")) { b = as_i(A1); e = as_i(A2); if (e > L) e = L; if (b < 0 || b > e) return string_oob(b, L); }
            else { b = n > 1 ? as_i(A1) : 0; e = n > 2 ? b + as_i(A2) : L; if (e > L || e < b) e = L; if (b < 0 || b > L) return rt_error("std::out_of_range: erase position out of range"); }
            char *o = malloc((size_t)L + 1);
            memcpy(o, s, (size_t)b); memcpy(o + b, s + e, (size_t)(L - e) + 1);
            *r = mk_str(o); return 0;
        }
        if (M("setCharAt")) {
            long long i = as_i(A1);
            if (i < 0 || i >= L) return string_oob(i, L);
            char *o = xstrdup(s); o[i] = (char)as_i(A2);
            *r = mk_str(o); return 0;
        }
        if (M("setLength")) {
            long long k = as_i(A1); if (k < 0) k = 0;
            char *o = calloc((size_t)k + 1, 1);
            memcpy(o, s, (size_t)(k < L ? k : L));
            *r = mk_str(o); return 0;
        }
        if (M("toCharArray")) {
            long long sz = L; Val arr;
            if (new_array(V_CHAR, &sz, 1, &arr)) return 1;
            for (long long i = 0; i < L; i++) arr.a->e[i] = mk_char((unsigned char)s[i]);
            *r = arr; return 0;
        }
        #undef M
    }
    if (F("strcmp")) { int c = strcmp(A0.k == V_STR ? A0.s : "", A1.k == V_STR ? A1.s : ""); *r = mk_int((c > 0) - (c < 0)); return 0; }
    if (F("toupper")) { *r = mk_int(toupper((int)as_i(A0))); return 0; }
    if (F("tolower")) { *r = mk_int(tolower((int)as_i(A0))); return 0; }
    if (F("Char.toUpper")) { *r = mk_char(toupper((int)as_i(A0))); return 0; }
    if (F("Char.toLower")) { *r = mk_char(tolower((int)as_i(A0))); return 0; }
    if (F("Char.numeric")) {
        int c = (int)as_i(A0);
        *r = mk_int(isdigit(c) ? c - '0' : isalpha(c) ? tolower(c) - 'a' + 10 : -1); return 0;
    }
    if (F("isdigit")) { *r = mk_bool(isdigit((int)as_i(A0) & 0xFF)); return 0; }
    if (F("isalpha")) { *r = mk_bool(isalpha((int)as_i(A0) & 0xFF)); return 0; }
    if (F("isalnum")) { *r = mk_bool(isalnum((int)as_i(A0) & 0xFF)); return 0; }
    if (F("isupper")) { *r = mk_bool(isupper((int)as_i(A0) & 0xFF)); return 0; }
    if (F("islower")) { *r = mk_bool(islower((int)as_i(A0) & 0xFF)); return 0; }
    if (F("isspace")) { *r = mk_bool(isspace((int)as_i(A0) & 0xFF)); return 0; }
    if (F("ispunct")) { *r = mk_bool(ispunct((int)as_i(A0) & 0xFF)); return 0; }
    if ((F("parseInt") || F("parseLong")) && A0.k != V_STR) { *r = F("parseInt") ? wrap32(as_i(A0)) : mk_long(as_i(A0)); return 0; }
    if (F("parseDouble") && A0.k != V_STR) { *r = mk_flt(as_f(A0)); return 0; }
    if (F("parseInt") || F("parseLong")) {
        const char *s = A0.k == V_STR ? A0.s : "";
        char *end; long long v = strtoll(s, &end, n > 1 ? (int)as_i(A1) : 10);
        if (lang == 3 && (end == s || *end)) {
            char msg[300]; snprintf(msg, sizeof msg, "java.lang.NumberFormatException: For input string: \"%.200s\"", s);
            return rt_error(msg);
        }
        *r = F("parseInt") ? wrap32(v) : mk_long(v); return 0;
    }
    if (F("parseDouble")) {
        const char *s = A0.k == V_STR ? A0.s : "";
        char *end; double v = strtod(s, &end);
        if (lang == 3 && (end == s)) {
            char msg[300]; snprintf(msg, sizeof msg, "java.lang.NumberFormatException: For input string: \"%.200s\"", s);
            return rt_error(msg);
        }
        *r = mk_flt(v); return 0;
    }
    if (F("parseBool")) { *r = mk_bool(A0.k == V_STR && !x_strcasecmp(A0.s, "true")); return 0; }
    if (F("toString")) { *r = mk_str(val_string(A0, 1)); return 0; }
    if (F("toBinary") || F("toHex") || F("toOctal")) {
        unsigned long long u = A0.k == V_LONG ? (unsigned long long)A0.i : (unsigned long long)(uint32_t)as_i(A0);
        char buf[80];
        if (F("toHex")) snprintf(buf, sizeof buf, "%llx", u);
        else if (F("toOctal")) snprintf(buf, sizeof buf, "%llo", u);
        else { int k = 0; char t[80]; do { t[k++] = (char)('0' + (u & 1)); u >>= 1; } while (u); for (int i = 0; i < k; i++) buf[i] = t[k - 1 - i]; buf[k] = 0; }
        *r = mk_str(xstrdup(buf)); return 0;
    }
    if (F("format")) { *r = mk_str(format_runtime(A0.k == V_STR ? A0.s : "", a + 1, n - 1)); return 0; }
    if (F("Arrays.sort")) {
        if (A0.k != V_ARR) return rt_error("Arrays.sort needs an array");
        long long from = n > 1 ? as_i(A1) : 0, to = n > 2 ? as_i(A2) : A0.a->n;
        if (from < 0 || to > A0.a->n || from > to) return bounds_error(to, A0.a->n);
        qsort(A0.a->e + from, (size_t)(to - from), sizeof(Val), cmp_vals);
        return 0;
    }
    if (F("Arrays.count")) {
        if (A0.k != V_ARR) return rt_error("needs an array");
        long long from = as_i(A1), to = as_i(A2), c = 0;
        if (from < 0 || to > A0.a->n || from > to) return bounds_error(to, A0.a->n);
        Val want = n > 3 ? a[3] : mk_int(0);
        for (long long i = from; i < to; i++) { Val t; if (!binop(OP_EQ, A0.a->e[i], want, &t) && t.i) c++; }
        *r = mk_int(c); return 0;
    }
    if (F("Arrays.reverse") || F("Arrays.sortDesc")) {
        if (A0.k != V_ARR) return rt_error("needs an array");
        long long from = n > 1 ? as_i(A1) : 0, to = n > 2 ? as_i(A2) : A0.a->n;
        if (from < 0 || to > A0.a->n || from > to) return bounds_error(to, A0.a->n);
        if (F("Arrays.sortDesc")) qsort(A0.a->e + from, (size_t)(to - from), sizeof(Val), cmp_vals);
        for (long long i = from, j = to - 1; i < j; i++, j--) { Val t = A0.a->e[i]; A0.a->e[i] = A0.a->e[j]; A0.a->e[j] = t; }
        return 0;
    }
    if (F("Arrays.fill")) {
        if (A0.k != V_ARR) return rt_error("Arrays.fill needs an array");
        for (int i = 0; i < A0.a->n; i++) {
            Val v = A1;
            if (A0.a->n && A0.a->e[0].k == V_FLT) v = mk_flt(as_f(v));
            else if (A0.a->n && A0.a->e[0].k == V_F32) v = mk_f32(as_f(v));
            A0.a->e[i] = v;
        }
        return 0;
    }
    if (F("Arrays.copy")) {
        if (A0.k != V_ARR) return rt_error("clone of a non-array");
        Arr *c = calloc(1, sizeof *c); c->n = A0.a->n; c->e = calloc((size_t)(c->n ? c->n : 1), sizeof(Val));
        memcpy(c->e, A0.a->e, sizeof(Val) * (size_t)c->n);
        r->k = V_ARR; r->a = c; return 0;
    }
    if (F("Arrays.toString")) {
        if (A0.k != V_ARR) { *r = mk_str("null"); return 0; }
        size_t cap = 64, len = 1; char *o = malloc(cap); o[0] = '[';
        for (int i = 0; i < A0.a->n; i++) {
            char *t = val_string(A0.a->e[i], 1);
            size_t tl = strlen(t);
            while (len + tl + 4 > cap) { cap *= 2; o = xrealloc(o, cap); }
            if (i) { o[len++] = ','; o[len++] = ' '; }
            memcpy(o + len, t, tl); len += tl;
        }
        o[len++] = ']'; o[len] = 0;
        *r = mk_str(o); return 0;
    }
    #undef F
    return -1;
}

/* ---- execution --------------------------------------------------------- */

static Val eval(const Opd *o) {
    if (o->k == O_CONST) return o->v;
    if (o->k == O_VAR) { Val v = vars[o->slot]; return v.k == V_NONE ? mk_int(0) : v; }
    return mk_int(0);
}

static Arg *push_arg(Val v) {
    if (npargs == cappargs) { cappargs = cappargs ? cappargs * 2 : 64; pargs = xrealloc(pargs, cappargs * sizeof *pargs); }
    Arg *a = &pargs[npargs++];
    memset(a, 0, sizeof *a);
    a->v = v;
    return a;
}

static int jump(int label, int *pc) {
    if (label < 0 || label >= nlabels || label_pc[label] < 0) return rt_error("bad jump target");
    *pc = label_pc[label];
    return 0;
}

static int do_return(Val v, int has_value, int *pc) {
    Frame *fr = &frames[nframes - 1];
    Func *fn = &funcs[fr->func];
    /* reference parameters: read their final values before the caller's slots come back */
    Val back[64]; int nb = fr->nrefs < 64 ? fr->nrefs : 64;
    for (int i = 0; i < nb; i++) back[i] = vars[fr->refs[i].param_slot];
    for (int i = 0; i < fn->nlocals; i++) vars[fn->locals[i]] = fr->saved[i];
    for (int i = 0; i < nb; i++) {
        Arg *t = &fr->refs[i].target;
        if (t->ref == 1) vars[t->slot] = back[i];
        else if (t->ref == 2 && t->arr && t->idx >= 0 && t->idx < t->arr->n) t->arr->e[t->idx] = back[i];
    }
    free(fr->refs);
    free(fr->saved);
    free(fr->args);
    *pc = fr->ret_pc;
    if (fr->dst >= 0) vars[fr->dst] = has_value ? v : mk_int(0);
    nframes--;
    return 0;
}

static int execute(void) {
    long steps = 0;
    for (int pc = 0; pc < ncode; pc++) {
        if (++steps > MAX_STEPS) return rt_error("execution limit exceeded (possible infinite loop)");
        if (out_overflow) return rt_error("output limit exceeded (possible infinite loop)");
        Ins *in = &code[pc];
        rt_line = in->line;
        switch (in->k) {
        case I_LABEL: case I_GLOBAL: break;
        case I_GOTO: if (jump(in->label, &pc)) return 1; break;
        case I_IFFALSE:
            if (!truthy(eval(&in->a)) && jump(in->label, &pc)) return 1;
            break;
        case I_PRINT: {
            Val v = eval(&in->a);
            if (in->a.k == O_CONST && v.k == V_STR && !in->spec) out_str(v.s);
            else if (in->spec) { char buf[1100]; format_one(v, in->spec, buf, sizeof buf); out_str(buf); }
            else { print_plain(v); out_char('\n'); }      /* plain `print x;` is one line; printf has its own \n */
            break;
        }
        case I_WRITE: write_val(eval(&in->a)); break;
        case I_MANIP: {
            const char *m = in->name;
            Val v = eval(&in->a);
            if (!strcmp(m, "fixed")) cpp_fixed = 1;
            else if (!strcmp(m, "scientific") || !strcmp(m, "defaultfloat")) cpp_fixed = 0;
            else if (!strcmp(m, "setprecision")) cpp_prec = (int)as_i(v) < 0 ? 6 : (int)as_i(v);
            else if (!strcmp(m, "setw")) cpp_width = (int)as_i(v);
            else if (!strcmp(m, "setfill")) cpp_fill = (char)as_i(v);
            else if (!strcmp(m, "boolalpha")) cpp_boolalpha = 1;
            else if (!strcmp(m, "noboolalpha")) cpp_boolalpha = 0;
            else if (!strcmp(m, "left")) cpp_left = 1;
            else if (!strcmp(m, "right")) cpp_left = 0;
            break;
        }
        case I_READ: {
            fflush(stdout);
            Val v = do_read(in->name);
            if (in->dst >= 0) vars[in->dst] = v;
            break;
        }
        case I_HALT: fflush(stdout); return 0;
        case I_FUNC: pc = funcs[in->func].end; break;      /* never fall into a function body */
        case I_END:
            if (nframes == 0) { fflush(stdout); return 0; }
            do_return(mk_int(0), 0, &pc);
            break;
        case I_RETURN:
            if (nframes == 0) { fflush(stdout); return 0; }
            do_return(eval(&in->a), in->a.k != O_NONE, &pc);
            break;
        case I_PARAM: push_arg(eval(&in->a)); break;
        case I_REFPARAM: {
            if (in->nargs == 0) {                       /* refparam x */
                Arg *a = push_arg(eval(&in->a));
                if (in->a.k == O_VAR) { a->ref = 1; a->slot = in->a.slot; }
            } else {                                    /* refparam a[i] */
                Val b = eval(&in->a);
                long long i = as_i(eval(&in->b));
                if (b.k != V_ARR) return rt_error("indexing a value that is not an array");
                if (i < 0 || i >= b.a->n) return bounds_error(i, b.a->n);
                Arg *a = push_arg(b.a->e[i]);
                a->ref = 2; a->arr = b.a; a->idx = i;
            }
            break;
        }
        case I_FORMAL: {
            Frame *fr = &frames[nframes - 1];
            if (fr->argi < fr->nargs) {
                Arg *a = &fr->args[fr->argi];
                vars[in->dst] = a->v;
                if (a->ref) {
                    fr->refs = xrealloc(fr->refs, sizeof(RefBack) * (size_t)(fr->nrefs + 1));
                    fr->refs[fr->nrefs].param_slot = in->dst;
                    fr->refs[fr->nrefs].target = *a;
                    fr->nrefs++;
                }
            } else vars[in->dst] = mk_int(0);
            fr->argi++;
            break;
        }
        case I_CALL: {
            int na = in->nargs > npargs ? npargs : in->nargs;
            Arg *argp = pargs + (npargs - na);
            if (in->func >= 0) {
                if (nframes >= MAX_DEPTH) return rt_error(lang == 3 ? "java.lang.StackOverflowError" : "stack overflow (recursion too deep)");
                if (nframes == capframes) { capframes = capframes ? capframes * 2 : 64; frames = xrealloc(frames, capframes * sizeof *frames); }
                Func *fn = &funcs[in->func];
                Frame *fr = &frames[nframes++];
                fr->ret_pc = pc; fr->dst = in->dst; fr->func = in->func;
                fr->saved = malloc(sizeof(Val) * (size_t)(fn->nlocals ? fn->nlocals : 1));
                for (int i = 0; i < fn->nlocals; i++) { fr->saved[i] = vars[fn->locals[i]]; vars[fn->locals[i]].k = V_NONE; }
                fr->args = malloc(sizeof(Arg) * (size_t)(na ? na : 1));
                memcpy(fr->args, argp, sizeof(Arg) * (size_t)na);
                fr->nargs = na; fr->argi = 0; fr->refs = NULL; fr->nrefs = 0;
                npargs -= na;
                pc = fn->start;
                break;
            }
            Val r, args[64];
            for (int i = 0; i < na && i < 64; i++) args[i] = argp[i].v;
            int rc = builtin(in->name, args, na < 64 ? na : 64, &r);
            npargs -= na;
            if (rc < 0) { char msg[300]; snprintf(msg, sizeof msg, "unknown function '%.200s'", in->name); return rt_error(msg); }
            if (rc > 0) return 1;
            if (halted) { fflush(stdout); return 0; }
            if (in->dst >= 0) vars[in->dst] = r;
            break;
        }
        case I_ASSIGN: vars[in->dst] = eval(&in->a); break;
        case I_CAST: vars[in->dst] = cast_to(eval(&in->a), in->cast); break;
        case I_NOT: vars[in->dst] = mk_bool(!truthy(eval(&in->a))); break;
        case I_BNOT: {
            Val a = eval(&in->a);
            vars[in->dst] = a.k == V_LONG ? mk_long(~a.i) : wrap32(~as_i(a));
            break;
        }
        case I_NEG: {
            Val a = eval(&in->a);
            if (IS_F(a)) { vars[in->dst] = a; vars[in->dst].f = -a.f; }
            else if (a.k == V_LONG) vars[in->dst] = mk_long((long long)(0ULL - (unsigned long long)a.i));
            else vars[in->dst] = wrap32(-as_i(a));
            break;
        }
        case I_BIN: {
            Val r;
            if (binop(in->op, eval(&in->a), eval(&in->b), &r)) return 1;
            vars[in->dst] = r;
            break;
        }
        case I_NEWARR: {
            long long sizes[8];
            for (int i = 0; i < in->nsizes; i++) sizes[i] = as_i(eval(&in->sizes[i]));
            Val v;
            if (in->nsizes == 0) { sizes[0] = 0; if (new_array(in->cast, sizes, 1, &v)) return 1; }
            else if (new_array(in->cast, sizes, in->nsizes, &v)) return 1;
            vars[in->dst] = v;
            break;
        }
        case I_LOAD: {
            Val b = eval(&in->a);
            long long i = as_i(eval(&in->b));
            if (b.k == V_STR) {
                long long L = (long long)strlen(b.s);
                if (i < 0 || i > L) return string_oob(i, L);
                vars[in->dst] = mk_char(i == L ? 0 : (unsigned char)b.s[i]);
                break;
            }
            if (b.k != V_ARR) return rt_error(lang == 3 ? "java.lang.NullPointerException: array is null" : "indexing a value that is not an array");
            if (i < 0 || i >= b.a->n) return bounds_error(i, b.a->n);
            Val e = b.a->e[i];
            vars[in->dst] = e;
            break;
        }
        case I_STORE: {
            Val b = vars[in->dst];
            long long i = as_i(eval(&in->b));
            Val v = eval(&in->a);
            if (b.k == V_STR) {               /* C / C++: s[i] = 'x' */
                long long L = (long long)strlen(b.s);
                if (i < 0 || i > L) return string_oob(i, L);
                char *c = malloc((size_t)L + 2);
                memcpy(c, b.s, (size_t)L + 1);
                if (i == L) { c[L] = (char)as_i(v); c[L + 1] = 0; }
                else c[i] = (char)as_i(v);
                if (i < L && c[i] == 0) c[i] = 0;
                vars[in->dst] = mk_str(c);
                break;
            }
            if (b.k != V_ARR) return rt_error(lang == 3 ? "java.lang.NullPointerException: array is null" : "indexing a value that is not an array");
            if (i < 0 || i >= b.a->n) return bounds_error(i, b.a->n);
            b.a->e[i] = v;
            break;
        }
        }
    }
    fflush(stdout);
    return 0;
}

static int resolve(void) {
    /* functions: name -> start/end */
    for (int pc = 0; pc < ncode; pc++) {
        if (code[pc].k != I_FUNC) continue;
        funcs = xrealloc(funcs, (size_t)(nfuncs + 1) * sizeof *funcs);
        Func *f = &funcs[nfuncs];
        memset(f, 0, sizeof *f);
        f->name = code[pc].name; f->start = pc; f->end = ncode - 1;
        for (int q = pc + 1; q < ncode; q++) if (code[q].k == I_END) { f->end = q; break; }
        code[pc].func = nfuncs;
        nfuncs++;
    }
    /* each function's local slots (saved around every call, so recursion works) */
    char *seen = calloc((size_t)(nnames ? nnames : 1), 1);
    for (int fi = 0; fi < nfuncs; fi++) {
        Func *f = &funcs[fi];
        memset(seen, 0, (size_t)(nnames ? nnames : 1));
        f->locals = malloc(sizeof(int) * (size_t)(nnames ? nnames : 1));
        for (int pc = f->start; pc <= f->end; pc++) {
            Ins *in = &code[pc];
            int s[12]; int k = 0;
            if (in->dst >= 0 && in->k != I_GLOBAL) s[k++] = in->dst;
            if (in->a.k == O_VAR) s[k++] = in->a.slot;
            if (in->b.k == O_VAR) s[k++] = in->b.slot;
            for (int i = 0; i < in->nsizes && k < 12; i++) if (in->sizes[i].k == O_VAR) s[k++] = in->sizes[i].slot;
            for (int i = 0; i < k; i++)
                if (!is_global[s[i]] && !seen[s[i]]) { seen[s[i]] = 1; f->locals[f->nlocals++] = s[i]; }
        }
    }
    free(seen);
    for (int pc = 0; pc < ncode; pc++) {
        if (code[pc].k != I_CALL) continue;
        for (int fi = 0; fi < nfuncs; fi++) if (!strcmp(funcs[fi].name, code[pc].name)) { code[pc].func = fi; break; }
    }
    return 0;
}

int interp_run(FILE *tac, int language) {
    char *line;
    int ln = 0;
    lang = language;
    while ((line = x_readline(tac)) != NULL) {
        ln++;
        if (!parse_line(line, ln)) { fprintf(stderr, "Runtime error: cannot understand TAC line %d\n", ln); free(line); return 1; }
        free(line);
    }
    vars = calloc((size_t)(nnames ? nnames : 1), sizeof *vars);
    is_global = calloc((size_t)(nnames ? nnames : 1), 1);
    if (!vars || !is_global) { fprintf(stderr, "out of memory\n"); return 2; }
    for (int pc = 0; pc < ncode; pc++) if (code[pc].k == I_GLOBAL) is_global[code[pc].dst] = 1;
    resolve();
    int rc = execute();
    fflush(stdout);
    (void)exit_code;
    return rc;
}
