/* Lexify lexer (Flex) - Section 4.1 / 5 of the project manual
 * Recognises the mini language plus the C, C++ and Java tokens used from
 * "Hello World" up to loops: every keyword, literal, operator and separator. */
%option noyywrap yylineno

%{
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "parser.tab.h"

/* flex defines these further down; the macro expander below uses them */
static int input(void);
static void yyunput(int c, char *buf_ptr);

int lexical_errors = 0;

/* portable strndup (MinGW on Windows has none) */
static char *lx_strndup(const char *s, size_t n) {
    size_t l = 0; while (l < n && s[l]) l++;
    char *p = malloc(l + 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    memcpy(p, s, l); p[l] = 0;
    return p;
}

/* which language the source looks like (main.c picks the mode from these) */
int lex_saw_c = 0, lex_saw_cpp = 0, lex_saw_java = 0;

static void lex_error(const char *msg, const char *text) {
    fprintf(stderr, "Lexical error at line %d: %s '%s'\n", yylineno, msg, text);
    lexical_errors++;
}

/* value of one (possibly escaped) character starting at s; *len = characters used */
static long long esc_value(const char *s, int *len) {
    if (s[0] != '\\') { *len = 1; return (unsigned char)s[0]; }
    *len = 2;
    switch (s[1]) {
    case 'n': return '\n';  case 't': return '\t';  case 'r': return '\r';
    case 'a': return '\a';  case 'b': return '\b';  case 'f': return '\f';
    case 'v': return '\v';  case '\\': return '\\'; case '\'': return '\'';
    case '"': return '"';   case '?': return '?';
    case 'x': { long long v = 0; int i = 2; while (isxdigit((unsigned char)s[i])) { v = v * 16 + (isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10)); i++; } *len = i; return v; }
    case 'u': { long long v = 0; int i = 2; while (i < 6 && isxdigit((unsigned char)s[i])) { v = v * 16 + (isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10)); i++; } *len = i; return v; }
    default:
        if (s[1] >= '0' && s[1] <= '7') {
            long long v = 0; int i = 1;
            while (i < 4 && s[i] >= '0' && s[i] <= '7') { v = v * 8 + (s[i] - '0'); i++; }
            *len = i; return v;
        }
        return (unsigned char)s[1];
    }
}

/* integer literal: decimal, 0x hex, 0b binary, 0 octal, Java '_' separators, u/l suffixes */
static int int_token(const char *t) {
    char buf[128]; int n = 0, has_l = 0;
    for (const char *p = t; *p && n < 120; p++) {
        if (*p == '_') continue;
        if (*p == 'l' || *p == 'L') { has_l = 1; continue; }
        if (*p == 'u' || *p == 'U') continue;
        buf[n++] = *p;
    }
    buf[n] = 0;
    unsigned long long v;
    if (buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) v = strtoull(buf + 2, NULL, 2);
    else v = strtoull(buf, NULL, 0);          /* handles 0x.. and 0.. (octal) */
    yylval.ival = (long long)v;
    if (has_l || v > 2147483647ULL) return LONG_LIT;
    return INT_LIT;
}

/* ---- #define NAME value (object-like macros are expanded) ---- */
typedef struct Macro { char *name, *body; char *params[16]; int nparams; int is_func; struct Macro *next; } Macro;
static Macro *macros;
static long macro_budget = 100000;
static Macro *find_macro(const char *n) { for (Macro *m = macros; m; m = m->next) if (!strcmp(m->name, n)) return m; return NULL; }

static void add_define(const char *text) {
    const char *p = strstr(text, "define") + 6;
    while (*p == ' ' || *p == '\t') p++;
    const char *ns = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (p == ns) return;
    char *name = lx_strndup(ns, (size_t)(p - ns));
    Macro *fm = NULL;
    if (*p == '(') {                            /* function-like macro: #define MAX(a, b) ((a) > (b) ? (a) : (b)) */
        fm = calloc(1, sizeof *fm);
        fm->is_func = 1;
        p++;
        while (*p && *p != ')') {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            const char *ps = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            if (p > ps && fm->nparams < 16) fm->params[fm->nparams++] = lx_strndup(ps, (size_t)(p - ps));
            while (*p == ' ' || *p == '\t') p++;
            if (*p != ',' && *p != ')') { if (*p) p++; }
        }
        if (*p == ')') p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    char *body = strdup(p);
    char *c = strstr(body, "//"); if (c) *c = 0;
    c = strstr(body, "/*"); if (c) *c = 0;
    size_t bl = strlen(body);
    while (bl && (body[bl - 1] == ' ' || body[bl - 1] == '\t' || body[bl - 1] == '\r')) body[--bl] = 0;
    if (!fm && !strcmp(name, body)) return;
    Macro *m = fm ? fm : calloc(1, sizeof *m);
    m->name = name; m->body = body; m->next = macros; macros = m;
}

/* MAX(x+1, y): read the arguments from the input and build the expansion */
static char *expand_call(Macro *m) {
    int c;
    char ws[256]; int nws = 0;
    while ((c = input()) == ' ' || c == '\t') if (nws < 255) ws[nws++] = (char)c;
    if (c != '(') {                             /* used without arguments: leave it alone */
        if (c != EOF && c != 0) unput(c);
        while (nws) unput(ws[--nws]);
        return NULL;
    }
    char *args[16]; int nargs = 0;
    size_t cap = 256, len = 0; char *cur = malloc(cap);
    int depth = 0;
    while ((c = input()) != EOF && c != 0) {
        if (c == '"' || c == '\'') {          /* copy string / char literals untouched */
            int q = c;
            if (len + 2 >= cap) { cap *= 2; cur = realloc(cur, cap); }
            cur[len++] = (char)c;
            while ((c = input()) != EOF && c != 0 && c != q) {
                if (len + 3 >= cap) { cap *= 2; cur = realloc(cur, cap); }
                cur[len++] = (char)c;
                if (c == '\\') { c = input(); cur[len++] = (char)c; }
            }
            if (c == q) cur[len++] = (char)c;
            continue;
        }
        if (c == '(') depth++;
        if (c == ')' && depth-- == 0) break;
        if (c == ',' && depth == 0) {
            cur[len] = 0; if (nargs < 16) args[nargs++] = cur;
            cap = 256; len = 0; cur = malloc(cap);
            continue;
        }
        if (len + 2 >= cap) { cap *= 2; cur = realloc(cur, cap); }
        cur[len++] = (char)(c == '\n' ? ' ' : c);
    }
    cur[len] = 0;
    if (nargs < 16 && (len > 0 || nargs > 0 || m->nparams > 0)) args[nargs++] = cur;
    /* substitute parameter names (whole identifiers only) */
    size_t ocap = strlen(m->body) * 2 + 64, ol = 0;
    char *o = malloc(ocap);
    for (const char *b = m->body; *b; ) {
        if (isalpha((unsigned char)*b) || *b == '_') {
            const char *s0 = b;
            while (isalnum((unsigned char)*b) || *b == '_') b++;
            size_t wl = (size_t)(b - s0);
            const char *rep = NULL;
            for (int i = 0; i < m->nparams; i++)
                if (strlen(m->params[i]) == wl && !strncmp(m->params[i], s0, wl)) { rep = i < nargs ? args[i] : ""; break; }
            const char *src = rep ? rep : s0;
            size_t sl = rep ? strlen(rep) : wl;
            while (ol + sl + 4 >= ocap) { ocap *= 2; o = realloc(o, ocap); }
            memcpy(o + ol, src, sl); ol += sl;       /* raw text, exactly like the C preprocessor */
        } else {
            if (ol + 2 >= ocap) { ocap *= 2; o = realloc(o, ocap); }
            o[ol++] = *b++;
        }
    }
    o[ol] = 0;
    return o;
}

/* ---- typedef long long ll;  ->  'll' becomes a type keyword ---- */
typedef struct Alias { char *name; int tok; struct Alias *next; } Alias;
static Alias *aliases;
static void add_typedef(const char *text) {
    char buf[512]; snprintf(buf, sizeof buf, "%s", text + 7);
    char *semi = strchr(buf, ';'); if (semi) *semi = 0;
    char *end = buf + strlen(buf);
    while (end > buf && !(isalnum((unsigned char)end[-1]) || end[-1] == '_')) end--;
    char *start = end;
    while (start > buf && (isalnum((unsigned char)start[-1]) || start[-1] == '_')) start--;
    if (start == end) return;
    char *name = lx_strndup(start, (size_t)(end - start));
    *start = 0;
    int tok = INT_T;
    if (strstr(buf, "string")) tok = STRING_T;
    else if (strstr(buf, "double") || strstr(buf, "float")) tok = FLOAT_T;
    else if (strstr(buf, "long")) tok = LONG_T;
    else if (strstr(buf, "char")) tok = CHAR_T;
    else if (strstr(buf, "bool")) tok = BOOL_T;
    Alias *a = calloc(1, sizeof *a);
    a->name = name; a->tok = tok; a->next = aliases; aliases = a;
}

/* give every token its line number for the parser (@n.first_line) */
#define YY_USER_ACTION yylloc.first_line = yylloc.last_line = yylineno;
%}

DIGIT   [0-9]
ID      ([A-Za-z_][A-Za-z0-9_$]*|"$"[A-Za-z0-9_$]+)
W       [ \t]+
NOTID   [^A-Za-z0-9_$]
US      (("unsigned"|"signed"){W})
EXP     [eE][+-]?{DIGIT}+

%x BLOCK_COMMENT

%%

 /* ---------- preprocessor / imports / namespaces (no tokens) ---------- */
^[ \t]*"#"[ \t]*"define"[^\n]*      { lex_saw_c = 1; add_define(yytext); }
^[ \t]*"#"[ \t]*"include"[^\n]*     { lex_saw_c = 1;
                                      if (strstr(yytext, "iostream") || strstr(yytext, "bits/stdc++") || strstr(yytext, "iomanip") ||
                                          strstr(yytext, "<string>") || strstr(yytext, "cmath") || strstr(yytext, "cstdio"))
                                          lex_saw_cpp = 1; }
^[ \t]*"#"[^\n]*                    { /* other preprocessor lines (#pragma, #ifndef ...) ignored */ }
"using"{W}"namespace"{W}{ID}[ \t]*";"   { lex_saw_cpp = 1; }
"using"{W}{ID}[ \t]*"::"[^;\n]*";"      { lex_saw_cpp = 1; }
"import"{W}[A-Za-z0-9_.*$ \t]+";"       { lex_saw_java = 1; }
"package"{W}[A-Za-z0-9_. \t]+";"        { lex_saw_java = 1; }
"typedef"[^;{}]*";"                     { add_typedef(yytext); }
("std::")?("greater"|"less")[ \t]*"<"[^>;\n]*">"[ \t]*"("[ \t]*")"  { lex_saw_cpp = 1; yylval.sval = strdup(yytext[0] == 'g' || strstr(yytext, "greater") ? "__greater__" : "__less__"); return ID; }
{ID}[ \t]*"::"                          { if (!strncmp(yytext, "std", 3)) lex_saw_cpp = 1; /* std::cout -> cout */ }
"@"{ID}                                 { lex_saw_java = 1; /* @Override ... */ }
"throws"{W}{ID}([ \t]*","[ \t]*{ID})*  { lex_saw_java = 1; }
("extends"|"implements"){W}{ID}([ \t]*","[ \t]*{ID})*  { lex_saw_java = 1; }

 /* ---------- multi-word C types ---------- */
{US}"char"/{NOTID}                      { yylval.ival = 1; return CHAR_T; }
{US}?"long"{W}"long"({W}"int")?/{NOTID} { yylval.ival = 8; return LONG_T; }
"long"{W}"double"/{NOTID}               { yylval.ival = 16; return FLOAT_T; }
{US}?"long"({W}"int")?/{NOTID}          { yylval.ival = 8; return LONG_T; }
{US}?"short"({W}"int")?/{NOTID}         { yylval.ival = 2; return INT_T; }
("unsigned"|"signed")({W}"int")?/{NOTID} { yylval.ival = 4; return INT_T; }

 /* ---------- C++ functional casts: int(x), double(x) ... ---------- */
("int"|"short")/[ \t]*"("    { yylval.ival = T_INT; return FCAST; }
"long"/[ \t]*"("             { yylval.ival = T_LONG; return FCAST; }
"double"/[ \t]*"("           { yylval.ival = T_FLOAT; return FCAST; }
"float"/[ \t]*"("            { yylval.ival = T_FLOAT | 0x100; return FCAST; }   /* 0x100: 32-bit float */
"char"/[ \t]*"("             { yylval.ival = T_CHAR; return FCAST; }
"bool"/[ \t]*"("             { yylval.ival = T_BOOL; return FCAST; }
"string"/[ \t]*"("           { lex_saw_cpp = 1; yylval.ival = T_STRING; return FCAST; }

 /* ---------- keywords ---------- */
"int"       { yylval.ival = 4; return INT_T; }
"byte"      { yylval.ival = 1; return INT_T; }
"size_t"    { yylval.ival = 8; return LONG_T; }
"char"      { yylval.ival = 1; return CHAR_T; }
"float"     { yylval.ival = 4; return FLOAT_T; }
"double"    { yylval.ival = 8; return FLOAT_T; }
"bool"      { yylval.ival = 1; return BOOL_T; }
"boolean"   { lex_saw_java = 1; yylval.ival = 1; return BOOL_T; }
"string"    { lex_saw_cpp = 1; return STRING_T; }
"String"/[ \t]*"."  { lex_saw_java = 1; yylval.sval = strdup("String"); return ID; /* String.valueOf(...) */ }
 /* Java wrapper classes: a type (Integer x = 5;) or a class with static methods (Integer.parseInt) */
("Integer"|"Long"|"Double"|"Float"|"Character"|"Boolean"|"Short"|"Byte")/[ \t]*"."  { lex_saw_java = 1; yylval.sval = strdup(yytext); return ID; }
"Integer"|"Short"|"Byte"  { lex_saw_java = 1; yylval.ival = 4; return INT_T; }
"Long"      { lex_saw_java = 1; yylval.ival = 8; return LONG_T; }
"Double"    { lex_saw_java = 1; yylval.ival = 8; return FLOAT_T; }
"Float"     { lex_saw_java = 1; yylval.ival = 4; return FLOAT_T; }
"Character" { lex_saw_java = 1; yylval.ival = 1; return CHAR_T; }
"Boolean"   { lex_saw_java = 1; yylval.ival = 1; return BOOL_T; }
"StringBuilder"|"StringBuffer"  { lex_saw_java = 1; return STRING_T; }
"var"       { if (lex_saw_java) return AUTO; yylval.sval = strdup(yytext); return ID; }
"String"    { lex_saw_java = 1; return STRING_T; }
"Scanner"   { lex_saw_java = 1; return SCANNER_T; }
"auto"      { return AUTO; }
"void"      { return VOID; }
"const"|"static"|"final"|"public"|"private"|"protected"|"volatile"|"register"|"extern"|"inline"|"constexpr"|"abstract"|"synchronized"|"transient"|"strictfp"|"native"  { yylval.sval = strdup(yytext); return MOD; }
"class"     { return CLASS; }
"new"       { return NEW; }
"return"    { return RETURN; }
"if"        { return IF; }
"else"      { return ELSE; }
"while"     { return WHILE; }
"do"        { return DO; }
"for"       { return FOR; }
"switch"    { return SWITCH; }
"case"      { return CASE; }
"default"   { return DEFAULT; }
"break"     { return BREAK; }
"continue"  { return CONTINUE; }
"sizeof"    { return SIZEOF; }
"static_cast"   { lex_saw_cpp = 1; return STATIC_CAST; }
"print"     { return PRINT; }
"true"      { return TRUE_LIT; }
"false"     { return FALSE_LIT; }

 /* ---------- literals ---------- */
({DIGIT}+"."{DIGIT}*|"."{DIGIT}+){EXP}?[fF]     { yylval.fval = (float)atof(yytext); return F32_LIT; }
{DIGIT}+{EXP}?[fF]                              { yylval.fval = (float)atof(yytext); return F32_LIT; }
({DIGIT}+"."{DIGIT}*|"."{DIGIT}+){EXP}?[dDlL]?  { yylval.fval = atof(yytext); return FLOAT_LIT; }
{DIGIT}+{EXP}[dDlL]?                            { yylval.fval = atof(yytext); return FLOAT_LIT; }
{DIGIT}+[dD]                                    { yylval.fval = atof(yytext); return FLOAT_LIT; }
0[xX][0-9a-fA-F_]+[uUlL]*                       { return int_token(yytext); }
0[bB][01_]+[uUlL]*                              { return int_token(yytext); }
{DIGIT}([0-9_]*{DIGIT})?[uUlL]*                 { return int_token(yytext); }

{ID}        {
                Macro *m = find_macro(yytext);
                char *expanded = NULL;
                if (m && m->is_func && macro_budget > 0) {
                    char *idtext = strdup(yytext);
                    expanded = expand_call(m);
                    if (!expanded) {             /* not followed by '(' : an ordinary identifier */
                        yylval.sval = idtext;
                        return ID;
                    }
                    free(idtext);
                }
                if (expanded) {
                    macro_budget--;
                    unput(' ');
                    for (int i = (int)strlen(expanded) - 1; i >= 0; i--) unput(expanded[i]);
                    unput(' ');
                    free(expanded);
                } else if (m && !m->is_func && macro_budget-- > 0) {
                    /* push the replacement text back into the input */
                    char *body = strdup(m->body);
                    unput(' ');
                    for (int i = (int)strlen(body) - 1; i >= 0; i--) unput(body[i]);
                    unput(' ');
                    free(body);
                } else {
                    for (Alias *a = aliases; a; a = a->next) if (!strcmp(a->name, yytext)) { yylval.ival = a->tok == LONG_T ? 8 : a->tok == CHAR_T || a->tok == BOOL_T ? 1 : a->tok == FLOAT_T ? 8 : 4; return a->tok; }
                    if (!strcmp(yytext, "cout") || !strcmp(yytext, "cin") || !strcmp(yytext, "endl")) lex_saw_cpp = 1;
                    else if (!strcmp(yytext, "System")) lex_saw_java = 1;
                    else if (!strcmp(yytext, "printf") || !strcmp(yytext, "scanf")) lex_saw_c = 1;
                    yylval.sval = strdup(yytext);
                    return ID;
                }
            }

\"([^\"\\\n]|\\.)*\"   { char *q = malloc(yyleng); memcpy(q, yytext + 1, yyleng - 2); q[yyleng - 2] = 0;
                          yylval.sval = q; return STRING_LIT; }
\"([^\"\\\n]|\\.)*     { fprintf(stderr, "Lexical error at line %d: unterminated string literal\n", yylineno);
                          lexical_errors++; }

'(\\[0-7]{1,3}|\\x[0-9a-fA-F]+|\\u[0-9a-fA-F]{4}|\\.|[^'\\\n])'   { int len; yylval.ival = esc_value(yytext + 1, &len); return CHAR_LIT; }
'([^'\\\n]|\\.)+'      { lex_error("invalid character literal", yytext); }
'                      { lex_error("unterminated character literal", yytext); }

 /* ---------- operators (longest first) ---------- */
">>>="  { return USHR_ASSIGN; }
"<<="   { return SHL_ASSIGN; }
">>="   { return SHR_ASSIGN; }
">>>"   { return USHR; }
"..."   { return ELLIPSIS; }
"++"    { return INC; }
"--"    { return DEC; }
"+="    { return ADD_ASSIGN; }
"-="    { return SUB_ASSIGN; }
"*="    { return MUL_ASSIGN; }
"/="    { return DIV_ASSIGN; }
"%="    { return MOD_ASSIGN; }
"&="    { return AND_ASSIGN; }
"|="    { return OR_ASSIGN; }
"^="    { return XOR_ASSIGN; }
"&&"    { return AND; }
"||"    { return OR; }
"=="    { return EQ; }
"!="    { return NEQ; }
"<="    { return LE; }
">="    { return GE; }
"<<"    { return SHL; }
">>"    { return SHR; }
"->"    { return ARROW; }
[<>=!+\-*/%&|^~?:.]  { return yytext[0]; }
[{}()\[\];,]         { return yytext[0]; }

"//"[^\n]*              { /* single-line comment: discarded */ }
"/*"                    { BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>"*/"     { BEGIN(INITIAL); }
<BLOCK_COMMENT>[^*\n]+  { /* discarded */ }
<BLOCK_COMMENT>.|\n     { /* discarded */ }
<BLOCK_COMMENT><<EOF>>  { fprintf(stderr, "Lexical error at line %d: unterminated block comment\n", yylineno);
                          lexical_errors++; BEGIN(INITIAL); yyterminate(); }

[ \t\r\n\f\v]+  { /* whitespace: discarded */ }
"\xEF\xBB\xBF"   { /* UTF-8 byte order mark (Windows editors) */ }
"\\"\r?\n       { /* line continuation */ }

.  { lex_error("invalid token", yytext); }

%%
