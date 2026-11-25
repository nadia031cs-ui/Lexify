/* Lexify lexer (Flex) - Section 4.1 / 5 of the project manual */
%option noyywrap nounput noinput yylineno

%{
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "parser.tab.h"

int lexical_errors = 0;

/* value of a C character literal body: a  or  \n  \t ... */
static int char_value(const char *s) {
    if (s[0] != '\\') return (unsigned char)s[0];
    switch (s[1]) {
    case 'n': return '\n';  case 't': return '\t';  case 'r': return '\r';
    case '0': return 0;     default:  return (unsigned char)s[1];
    }
}

/* give every token its line number for the parser (@n.first_line) */
#define YY_USER_ACTION yylloc.first_line = yylloc.last_line = yylineno;
%}

DIGIT   [0-9]
ID      [A-Za-z_][A-Za-z0-9_]*

%x BLOCK_COMMENT

%%

"int"       { return INT; }
"char"      { return INT; }     /* C compatibility: char is stored as an int (use %c to print it) */
"float"     { return FLOAT; }
"double"    { return FLOAT; }   /* C compatibility: double is treated as float */
"bool"      { return BOOL; }
"boolean"   { return BOOL; }     /* Java */
"long"      { return INT; }      /* C / C++ / Java: stored as int */
"void"      { return VOID; }
"return"    { return RETURN; }
"for"       { return FOR; }
"printf"    { return PRINTF; }
"if"        { return IF; }
"else"      { return ELSE; }
"while"     { return WHILE; }
"print"     { return PRINT; }
"class"     { return CLASS; }    /* Java: class Main { ... } */
"cout"      { return COUT; }     /* C++ */
"endl"      { return ENDL; }     /* C++ */
"public"|"private"|"protected"|"static"|"final" { /* Java modifiers: ignored */ }
"true"      { return TRUE_LIT; }
"false"     { return FALSE_LIT; }

{DIGIT}+"."{DIGIT}+[fFdD]?  { yylval.fval = atof(yytext); return FLOAT_LIT; }   /* 5.7, 5.7f (C++/Java) */
{DIGIT}+[fFdD]        { yylval.fval = atof(yytext); return FLOAT_LIT; }   /* 2f, 2d (Java) */
{DIGIT}+[lL]?         { yylval.ival = atoi(yytext); return INT_LIT; }     /* 10, 10L */
{ID}                  { yylval.sval = strdup(yytext); return ID; }

\"([^\"\\\n]|\\.)*\"   { char *q = malloc(yyleng); memcpy(q, yytext + 1, yyleng - 2); q[yyleng - 2] = 0;
                          yylval.sval = q; return STRING_LIT; }
\"([^\"\\\n]|\\.)*     { fprintf(stderr, "Lexical error at line %d: unterminated string literal\n", yylineno);
                          lexical_errors++; }

'([^'\\\n]|\\.)'   { yylval.ival = char_value(yytext + 1); return CHAR_LIT; }

"#"[ \t]*"include"[^\n]*   { /* C preprocessor line: ignored */ }
"#"[ \t]*"define"[^\n]*    { /* C preprocessor line: ignored */ }

 /* ---- C++ ---- */
"using"[ \t]+"namespace"[ \t]+{ID}[ \t]*";"   { /* using namespace std;  ignored */ }
"std"[ \t]*"::"                                { /* std::cout, std::endl -> cout, endl */ }

 /* ---- Java ---- */
"import"[ \t][^\n;]*";"                        { /* import java.util.*;  ignored */ }
"System"[ \t]*"."[ \t]*"out"[ \t]*"."[ \t]*"println"  { return PRINTLN; }
"System"[ \t]*"."[ \t]*"out"[ \t]*"."[ \t]*"printf"   { return PRINTF; }
"System"[ \t]*"."[ \t]*"out"[ \t]*"."[ \t]*"print"    { return JPRINT; }

"<<"  { return SHL; }   /* C++: cout << x */
"++"  { return INC; }
"--"  { return DEC; }
"+="  { return ADD_ASSIGN; }
"-="  { return SUB_ASSIGN; }
"*="  { return MUL_ASSIGN; }
"/="  { return DIV_ASSIGN; }
"%="  { return MOD_ASSIGN; }
"&&"  { return AND; }
"||"  { return OR; }
"=="  { return EQ; }
"!="  { return NEQ; }
"<="  { return LE; }
">="  { return GE; }
[<>=!+\-*/%]  { return yytext[0]; }
[{}();,\[\]] { return yytext[0]; }

"//".*                  { /* single-line comment: discarded */ }
"/*"                    { BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>"*/"     { BEGIN(INITIAL); }
<BLOCK_COMMENT>.|\n     { /* discarded */ }
<BLOCK_COMMENT><<EOF>>  { fprintf(stderr, "Lexical error at line %d: unterminated block comment\n", yylineno);
                          lexical_errors++; BEGIN(INITIAL); }

[ \t\r\n]+  { /* whitespace: discarded */ }

.  { fprintf(stderr, "Lexical error at line %d: invalid token '%s'\n", yylineno, yytext);
     lexical_errors++; }

%%
