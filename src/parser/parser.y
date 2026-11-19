/* Lexify parser (Bison) - grammar from Section 5 of the project manual.
 * This is the syntax-only skeleton: actions to build the AST go in the
 * rules below (see src/ast/). */
%code requires {
#include "ast/ast.h"
}

%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast/ast.h"

int  yylex(void);
void yyerror(const char *msg);
extern int yylineno;
int   syntax_errors = 0;
Node *ast_root = NULL;
%}

%define parse.error verbose
%locations

%union {
    int    ival;
    double fval;
    char  *sval;
    Type   type;
    Node  *node;
}

%token INT FLOAT BOOL IF ELSE WHILE PRINT TRUE_LIT FALSE_LIT
%token VOID RETURN FOR PRINTF
%token INC DEC ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%token <sval> STRING_LIT
%token <sval> ID
%token <ival> INT_LIT
%token <fval> FLOAT_LIT
%token AND OR EQ NEQ LE GE

%left OR
%left AND
%left EQ NEQ
%left '<' '>' LE GE
%left '+' '-'
%left '*' '/' '%'
%right '!' UMINUS
%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE

%type <node> program stmt_list stmt block decl_stmt declarators declarator simple
%type <node> func_def for_init for_cond for_step print_args expr_list expr
%type <type> type

%%

program     : stmt_list                 { ast_root = mk_program($1); $$ = ast_root; }
            ;

stmt_list   : /* empty */               { $$ = NULL; }
            | stmt_list stmt            { $$ = list_append($1, $2); }
            ;

stmt        : decl_stmt ';'             { $$ = $1; }
            | simple ';'                { $$ = $1; }
            | PRINT expr ';'            { $$ = mk_print($2, @1.first_line); }
            | PRINTF '(' print_args ')' ';'
                                        { $$ = $3; }
            | RETURN ';'                { $$ = mk_return(NULL, @1.first_line); }
            | RETURN expr ';'           { $$ = mk_return($2, @1.first_line); }
            | IF '(' expr ')' stmt %prec LOWER_THAN_ELSE
                                        { $$ = mk_if($3, $5, NULL, @1.first_line); }
            | IF '(' expr ')' stmt ELSE stmt
                                        { $$ = mk_if($3, $5, $7, @1.first_line); }
            | WHILE '(' expr ')' stmt   { $$ = mk_while($3, $5, @1.first_line); }
            | FOR '(' for_init ';' for_cond ';' for_step ')' stmt
                                        { /* for(i;c;s) b  ==>  { i; while (c) { b; s; } } */
                                          Node *body = mk_block(list_append($9, $7), @1.first_line);
                                          $$ = mk_block(list_append($3, mk_while($5, body, @1.first_line)),
                                                        @1.first_line); }
            | func_def                  { $$ = $1; }
            | block                     { $$ = $1; }
            | ';'                       { $$ = NULL; }
            | error ';'                 { yyerrok; $$ = NULL; }
            ;

/* int main() { ... }  -- C-style entry point, compiled as a plain block */
func_def    : type ID '(' ')' block         { $$ = $5; }
            | type ID '(' VOID ')' block    { $$ = $6; }
            | VOID ID '(' ')' block         { $$ = $5; }
            | VOID ID '(' VOID ')' block    { $$ = $6; }
            ;

block       : '{' stmt_list '}'         { $$ = mk_block($2, @1.first_line); }
            ;

/* int a;   int a = 5;   int a, b = 2, c;   */
decl_stmt   : type declarators          { for (Node *n = $2; n; n = n->next) if (n->kind == N_DECL) n->type = $1;
                                          $$ = $2; }
            ;

declarators : declarator                { $$ = $1; }
            | declarators ',' declarator{ $$ = list_append($1, $3); }
            ;

declarator  : ID                        { $$ = mk_decl(T_NONE, $1, @1.first_line); }
            | ID '=' expr               { $$ = list_append(mk_decl(T_NONE, $1, @1.first_line),
                                                           mk_assign(strdup($1), $3, @1.first_line)); }
            ;

/* assignment-like statements (also usable in a for header) */
simple      : ID '=' expr               { $$ = mk_assign($1, $3, @1.first_line); }
            | ID ADD_ASSIGN expr        { $$ = mk_assign($1, mk_binop("+", mk_id(strdup($1), @1.first_line), $3, @2.first_line), @1.first_line); }
            | ID SUB_ASSIGN expr        { $$ = mk_assign($1, mk_binop("-", mk_id(strdup($1), @1.first_line), $3, @2.first_line), @1.first_line); }
            | ID MUL_ASSIGN expr        { $$ = mk_assign($1, mk_binop("*", mk_id(strdup($1), @1.first_line), $3, @2.first_line), @1.first_line); }
            | ID DIV_ASSIGN expr        { $$ = mk_assign($1, mk_binop("/", mk_id(strdup($1), @1.first_line), $3, @2.first_line), @1.first_line); }
            | ID MOD_ASSIGN expr        { $$ = mk_assign($1, mk_binop("%", mk_id(strdup($1), @1.first_line), $3, @2.first_line), @1.first_line); }
            | ID INC                    { $$ = mk_assign($1, mk_binop("+", mk_id(strdup($1), @1.first_line), mk_int(1, @1.first_line), @2.first_line), @1.first_line); }
            | ID DEC                    { $$ = mk_assign($1, mk_binop("-", mk_id(strdup($1), @1.first_line), mk_int(1, @1.first_line), @2.first_line), @1.first_line); }
            | INC ID                    { $$ = mk_assign($2, mk_binop("+", mk_id(strdup($2), @2.first_line), mk_int(1, @1.first_line), @1.first_line), @2.first_line); }
            | DEC ID                    { $$ = mk_assign($2, mk_binop("-", mk_id(strdup($2), @2.first_line), mk_int(1, @1.first_line), @1.first_line), @2.first_line); }
            ;

for_init    : /* empty */               { $$ = NULL; }
            | decl_stmt                 { $$ = $1; }
            | simple                    { $$ = $1; }
            ;

for_cond    : /* empty */               { $$ = mk_bool(1, yylineno); }
            | expr                      { $$ = $1; }
            ;

for_step    : /* empty */               { $$ = NULL; }
            | simple                    { $$ = $1; }
            ;

/* printf("fmt", e1, e2, ...) */
print_args  : STRING_LIT                { $$ = mk_printf($1, NULL, @1.first_line); }
            | STRING_LIT ',' expr_list  { $$ = mk_printf($1, $3, @1.first_line); }
            ;

expr_list   : expr                      { $$ = $1; }
            | expr_list ',' expr        { $$ = list_append($1, $3); }
            ;

type        : INT                       { $$ = T_INT; }
            | FLOAT                     { $$ = T_FLOAT; }
            | BOOL                      { $$ = T_BOOL; }
            ;

expr        : expr OR expr              { $$ = mk_binop("||", $1, $3, @2.first_line); }
            | expr AND expr             { $$ = mk_binop("&&", $1, $3, @2.first_line); }
            | expr EQ expr              { $$ = mk_binop("==", $1, $3, @2.first_line); }
            | expr NEQ expr             { $$ = mk_binop("!=", $1, $3, @2.first_line); }
            | expr '<' expr             { $$ = mk_binop("<",  $1, $3, @2.first_line); }
            | expr '>' expr             { $$ = mk_binop(">",  $1, $3, @2.first_line); }
            | expr LE expr              { $$ = mk_binop("<=", $1, $3, @2.first_line); }
            | expr GE expr              { $$ = mk_binop(">=", $1, $3, @2.first_line); }
            | expr '+' expr             { $$ = mk_binop("+",  $1, $3, @2.first_line); }
            | expr '-' expr             { $$ = mk_binop("-",  $1, $3, @2.first_line); }
            | expr '*' expr             { $$ = mk_binop("*",  $1, $3, @2.first_line); }
            | expr '/' expr             { $$ = mk_binop("/",  $1, $3, @2.first_line); }
            | expr '%' expr             { $$ = mk_binop("%",  $1, $3, @2.first_line); }
            | '!' expr                  { $$ = mk_unop("!", $2, @1.first_line); }
            | '-' expr %prec UMINUS     { if ($2->kind == N_INT)        { $2->ival = -$2->ival; $$ = $2; }
                                          else if ($2->kind == N_FLOAT) { $2->fval = -$2->fval; $$ = $2; }
                                          else $$ = mk_unop("-", $2, @1.first_line); }
            | '(' expr ')'              { $$ = $2; }
            | ID                        { $$ = mk_id($1, @1.first_line); }
            | INT_LIT                   { $$ = mk_int($1, @1.first_line); }
            | FLOAT_LIT                 { $$ = mk_float($1, @1.first_line); }
            | TRUE_LIT                  { $$ = mk_bool(1, @1.first_line); }
            | FALSE_LIT                 { $$ = mk_bool(0, @1.first_line); }
            ;

%%

void yyerror(const char *msg) {
    fprintf(stderr, "Syntax error at line %d: %s\n", yylineno, msg);
    syntax_errors++;
}
