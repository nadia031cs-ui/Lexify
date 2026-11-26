/* Lexify parser (Bison)
 * Grammar of the mini language (Section 5 of the project manual) extended so
 * that ordinary C, C++ and Java programs - from Hello World up to loops - parse:
 * every type, declaration form, operator (arithmetic, relational, logical,
 * bitwise, shift, assignment, ++/--, ?:, casts, sizeof), if/else, switch,
 * while, do-while, for, for-each, break/continue, functions, arrays and the
 * usual I/O (printf/scanf, cout/cin, System.out/Scanner). */
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

/* apply "mods type" to every declarator of a declaration */
static Node *apply_type(int is_const, TypeSpec ts, Node *decls) {
    for (Node *n = decls; n; n = n->next) {
        n->type = ts.t;
        n->dims += ts.dims;
        n->is_const = is_const;
        n->f32 = ts.t == T_FLOAT && ts.size == 4;
    }
    return decls;
}

static Node *mk_func(TypeSpec ret, int ptr, char *name, Node *params, Node *body, int line) {
    if (!body) return NULL;                       /* prototype: nothing to do */
    Node *f = mk_node(N_FUNC, line);
    f->name = name; f->type = ret.t; f->dims = ret.dims; f->ptr = ptr; f->f32 = ret.t == T_FLOAT && ret.size == 4;
    f->a = params; f->b = body;
    return f;
}

/* for-init / for-step expression lists: each item becomes a statement */
static Node *exprs_to_stmts(Node *list, int line) {
    Node *out = NULL;
    while (list) { Node *nx = list->next; list->next = NULL; out = list_append(out, mk_exprstmt(list, line)); list = nx; }
    return out;
}

static char *concat(char *a, char *b) {
    char *r = malloc(strlen(a) + strlen(b) + 1);
    strcpy(r, a); strcat(r, b); free(a); free(b);
    return r;
}

static Node *mk_incdec(const char *op, int prefix, Node *target, int line) {
    Node *n = mk_node(N_INCDEC, line); n->op = op; n->flag = prefix; n->a = target; return n;
}
%}

%define parse.error verbose
%locations

%union {
    long long ival;
    double    fval;
    char     *sval;
    Type      type;
    TypeSpec  ts;
    int       num;
    Node     *node;
}

%token <ival> INT_T "int" LONG_T "long" CHAR_T "char" FLOAT_T "float" BOOL_T "bool"
%token STRING_T "string"
%token SCANNER_T "Scanner" AUTO "auto" VOID "void"
%token <sval> MOD "modifier"
%token CLASS "class" NEW "new" IF "if" ELSE "else" WHILE "while" DO "do" FOR "for"
%token SWITCH "switch" CASE "case" DEFAULT "default" BREAK "break" CONTINUE "continue"
%token RETURN "return" PRINT "print" SIZEOF "sizeof" STATIC_CAST "static_cast"
%token TRUE_LIT "true" FALSE_LIT "false"
%token <ival> FCAST "functional cast"
%token <sval> ID "identifier"
%token <sval> STRING_LIT "string literal"
%token <ival> INT_LIT "integer literal" LONG_LIT "long literal" CHAR_LIT "character literal"
%token <fval> FLOAT_LIT "floating-point literal" F32_LIT "float literal"
%token INC "++" DEC "--"
%token ADD_ASSIGN "+=" SUB_ASSIGN "-=" MUL_ASSIGN "*=" DIV_ASSIGN "/=" MOD_ASSIGN "%="
%token AND_ASSIGN "&=" OR_ASSIGN "|=" XOR_ASSIGN "^=" SHL_ASSIGN "<<=" SHR_ASSIGN ">>=" USHR_ASSIGN ">>>="
%token AND "&&" OR "||" EQ "==" NEQ "!=" LE "<=" GE ">="
%token SHL "<<" SHR ">>" USHR ">>>" ELLIPSIS "..." ARROW "->"

%precedence LOWER_THAN_ELSE
%precedence ELSE
%right '=' ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN AND_ASSIGN OR_ASSIGN XOR_ASSIGN SHL_ASSIGN SHR_ASSIGN USHR_ASSIGN
%right '?' ':'
%left OR
%left AND
%left '|'
%left '^'
%left '&'
%left EQ NEQ
%left '<' '>' LE GE
%left SHL SHR USHR
%left '+' '-'
%left '*' '/' '%'
%right '!' '~' UMINUS INC DEC CAST SIZEOF NEW
%left '[' '.' '('

%type <node> program stmt_list stmt block decl_stmt declarators declarator func_def class_def func_body
%type <node> params_opt param_list param for_init for_cond for_step args_opt args expr_list expr
%type <node> initializer init_list init_list_opt switch_body switch_item new_dims
%type <ts>   base_type type_spec
%type <num>  mods_opt ptr_opt arr_empty
%type <node> arr_dims_opt
%type <sval> string_lit

%%

program     : stmt_list                 { ast_root = mk_program($1); $$ = ast_root; }
            ;

stmt_list   : /* empty */               { $$ = NULL; }
            | stmt_list stmt            { $$ = list_append($1, $2); }
            ;

stmt        : decl_stmt ';'             { $$ = $1; }
            | expr ';'                  { $$ = mk_exprstmt($1, @1.first_line); }
            | PRINT expr ';'            { $$ = mk_print($2, @1.first_line); }
            | RETURN ';'                { $$ = mk_return(NULL, @1.first_line); }
            | RETURN expr ';'           { $$ = mk_return($2, @1.first_line); }
            | BREAK ';'                 { $$ = mk_node(N_BREAK, @1.first_line); }
            | CONTINUE ';'              { $$ = mk_node(N_CONTINUE, @1.first_line); }
            | BREAK ID ';'              { $$ = mk_node(N_BREAK, @1.first_line); $$->name = $2; }
            | CONTINUE ID ';'           { $$ = mk_node(N_CONTINUE, @1.first_line); $$->name = $2; }
            | ID ':' stmt               { $$ = mk_node(N_LABELED, @1.first_line); $$->name = $1; $$->a = $3; }
            | IF '(' expr ')' stmt %prec LOWER_THAN_ELSE
                                        { $$ = mk_if($3, $5, NULL, @1.first_line); }
            | IF '(' expr ')' stmt ELSE stmt
                                        { $$ = mk_if($3, $5, $7, @1.first_line); }
            | WHILE '(' expr ')' stmt   { $$ = mk_while($3, $5, @1.first_line); }
            | DO stmt WHILE '(' expr ')' ';'
                                        { $$ = mk_node(N_DOWHILE, @1.first_line); $$->a = $5; $$->b = $2; }
            | FOR '(' for_init ';' for_cond ';' for_step ')' stmt
                                        { $$ = mk_node(N_FOR, @1.first_line);
                                          $$->a = $3; $$->b = $5; $$->c = $7; $$->d = $9; }
            | FOR '(' mods_opt type_spec ptr_opt ID ':' '{' init_list_opt '}' ')' stmt
                                        { $$ = mk_node(N_FOREACH, @1.first_line);           /* for (int n : {1, 2, 3}) */
                                          $$->type = $4.t; $$->dims = $4.dims; $$->name = $6;
                                          $$->a = mk_node(N_ARRLIT, @8.first_line); $$->a->a = $9; $$->b = $12;
                                          $$->is_ref = $5 >= 1000; $$->f32 = $4.t == T_FLOAT && $4.size == 4; }
            | FOR '(' mods_opt type_spec ptr_opt ID ':' expr ')' stmt
                                        { $$ = mk_node(N_FOREACH, @1.first_line);
                                          $$->type = $4.t; $$->dims = $4.dims; $$->name = $6; $$->a = $8; $$->b = $10;
                                          $$->f32 = $4.t == T_FLOAT && $4.size == 4;
                                          $$->is_ref = $5 >= 1000; }
            | SWITCH '(' expr ')' '{' switch_body '}'
                                        { $$ = mk_node(N_SWITCH, @1.first_line); $$->a = $3; $$->b = $6; }
            | func_def                  { $$ = $1; }
            | class_def                 { $$ = $1; }
            | block                     { $$ = $1; }
            | ';'                       { $$ = NULL; }
            | error ';'                 { yyerrok; $$ = NULL; }
            ;

block       : '{' stmt_list '}'         { $$ = mk_block($2, @1.first_line); }
            ;

switch_body : /* empty */               { $$ = NULL; }
            | switch_body switch_item   { $$ = list_append($1, $2); }
            ;

switch_item : CASE expr ':'             { $$ = mk_node(N_CASE, @1.first_line); $$->a = $2; }
            | DEFAULT ':'               { $$ = mk_node(N_CASE, @1.first_line); }
            | stmt                      { $$ = $1; }
            ;

/* Java:  public class Main { ... }   -- its members become top-level items */
class_def   : mods_opt CLASS ID '{' stmt_list '}'   { add_class_name($3); $$ = $5; }
            ;

/* int main() { ... }   int add(int a, int b) { ... }   public static void main(String[] args) { ... } */
func_def    : mods_opt type_spec ptr_opt ID '(' params_opt ')' func_body
                                        { $$ = mk_func($2, $3 % 1000, $4, $6, $8, @4.first_line); }
            | mods_opt VOID ptr_opt ID '(' params_opt ')' func_body
                                        { TypeSpec v = { T_VOID, 0, 0 }; $$ = mk_func(v, $3 % 1000, $4, $6, $8, @4.first_line); }
            ;

func_body   : block                     { $$ = $1; }
            | ';'                       { $$ = NULL; }
            ;

params_opt  : /* empty */               { $$ = NULL; }
            | VOID                      { $$ = NULL; }
            | param_list                { $$ = $1; }
            ;

param_list  : param                     { $$ = $1; }
            | param_list ',' param      { $$ = list_append($1, $3); }
            ;

param       : mods_opt type_spec ptr_opt ID arr_dims_opt
                                        { $$ = mk_decl($2.t, $4, @4.first_line); $$->dims = $2.dims + list_len($5);
                                          $$->ptr = $3 % 1000; $$->is_ref = $3 >= 1000; $$->is_const = $1; $$->flag = 1;
                                          $$->f32 = $2.t == T_FLOAT && $2.size == 4; }
            | mods_opt type_spec ptr_opt
                                        { $$ = mk_decl($2.t, strdup("_"), @2.first_line); $$->dims = $2.dims; $$->ptr = $3 % 1000; $$->flag = 1; }
            | mods_opt type_spec ELLIPSIS ID
                                        { $$ = mk_decl($2.t, $4, @4.first_line); $$->dims = $2.dims + 1; $$->flag = 1; }
            ;

/* int a;   int a = 5;   int a, b = 2, c;   int a[5] = {1,2};   int[] a = new int[n];   char *s = "hi"; */
decl_stmt   : mods_opt type_spec declarators    { $$ = apply_type($1, $2, $3); }
            ;

mods_opt    : /* empty */               { $$ = 0; }
            | mods_opt MOD              { $$ = $1 | (!strcmp($2, "const") || !strcmp($2, "final") || !strcmp($2, "constexpr")); free($2); }
            ;

type_spec   : base_type                 { $$ = $1; }
            | type_spec '[' ']'         { $$ = $1; $$.dims++; }
            ;

base_type   : INT_T                     { $$.t = T_INT;     $$.dims = 0; $$.size = (int)$1; }
            | LONG_T                    { $$.t = T_LONG;    $$.dims = 0; $$.size = (int)$1; }
            | CHAR_T                    { $$.t = T_CHAR;    $$.dims = 0; $$.size = (int)$1; }
            | FLOAT_T                   { $$.t = T_FLOAT;   $$.dims = 0; $$.size = (int)$1; }
            | BOOL_T                    { $$.t = T_BOOL;    $$.dims = 0; $$.size = (int)$1; }
            | STRING_T                  { $$.t = T_STRING;  $$.dims = 0; $$.size = 32; }
            | SCANNER_T                 { $$.t = T_SCANNER; $$.dims = 0; $$.size = 8; }
            | AUTO                      { $$.t = T_AUTO;    $$.dims = 0; $$.size = 0; }
            ;

declarators : declarator                { $$ = $1; }
            | declarators ',' declarator{ $$ = list_append($1, $3); }
            ;

declarator  : ptr_opt ID arr_dims_opt   { $$ = mk_decl(T_NONE, $2, @2.first_line); $$->ptr = $1 % 1000;
                                          $$->dims = list_len($3); $$->a = $3; }
            | ptr_opt ID arr_dims_opt '=' initializer
                                        { $$ = mk_decl(T_NONE, $2, @2.first_line); $$->ptr = $1 % 1000;
                                          $$->dims = list_len($3); $$->a = $3; $$->b = $5; }
            | ptr_opt ID arr_dims_opt '{' init_list_opt '}'          /* C++11: int x{5}; int a[3]{}; */
                                        { $$ = mk_decl(T_NONE, $2, @2.first_line); $$->ptr = $1 % 1000;
                                          $$->dims = list_len($3); $$->a = $3;
                                          $$->b = mk_node(N_ARRLIT, @4.first_line); $$->b->a = $5; }
            ;

ptr_opt     : /* empty */               { $$ = 0; }
            | ptr_opt '*'               { $$ = $1 + 1; }
            | ptr_opt '&'               { $$ = $1 + 1000; }       /* reference */
            ;

/* [5][3]  or  []  : a list of size expressions; an empty [] is an N_INT with flag = -1 */
arr_dims_opt: /* empty */               { $$ = NULL; }
            | arr_dims_opt '[' expr ']' { $$ = list_append($1, $3); }
            | arr_dims_opt '[' ']'      { Node *e = mk_int(-1, yylineno); e->flag = -1; $$ = list_append($1, e); }
            ;

initializer : expr                      { $$ = $1; }
            | '{' init_list_opt '}'     { $$ = mk_node(N_ARRLIT, @1.first_line); $$->a = $2; }
            ;

init_list_opt: /* empty */              { $$ = NULL; }
            | init_list                 { $$ = $1; }
            | init_list ','             { $$ = $1; }
            ;

init_list   : initializer               { $$ = $1; }
            | init_list ',' initializer { $$ = list_append($1, $3); }
            ;

for_init    : /* empty */               { $$ = NULL; }
            | decl_stmt                 { $$ = $1; }
            | expr_list                 { $$ = exprs_to_stmts($1, @1.first_line); }
            ;

for_cond    : /* empty */               { $$ = mk_bool(1, yylineno); }
            | expr                      { $$ = $1; }
            ;

for_step    : /* empty */               { $$ = NULL; }
            | expr_list                 { $$ = exprs_to_stmts($1, @1.first_line); }
            ;

expr_list   : expr                      { $$ = $1; }
            | expr_list ',' expr        { $$ = list_append($1, $3); }
            ;

args_opt    : /* empty */               { $$ = NULL; }
            | args                      { $$ = $1; }
            ;

args        : expr                      { $$ = $1; }
            | args ',' expr             { $$ = list_append($1, $3); }
            ;

string_lit  : STRING_LIT                { $$ = $1; }
            | string_lit STRING_LIT     { $$ = concat($1, $2); }    /* "abc" "def" */
            ;

new_dims    : '[' expr ']'              { $$ = $2; }
            | new_dims '[' expr ']'     { $$ = list_append($1, $3); }
            | new_dims '[' ']'          { Node *e = mk_int(-1, yylineno); e->flag = -1; $$ = list_append($1, e); }
            ;

arr_empty   : '[' ']'                   { $$ = 1; }
            | arr_empty '[' ']'         { $$ = $1 + 1; }
            ;

expr        : expr '=' expr             { $$ = mk_assign_expr($1, $3, @2.first_line); }
            | expr ADD_ASSIGN expr      { $$ = mk_compound("+",  $1, $3, @2.first_line); }
            | expr SUB_ASSIGN expr      { $$ = mk_compound("-",  $1, $3, @2.first_line); }
            | expr MUL_ASSIGN expr      { $$ = mk_compound("*",  $1, $3, @2.first_line); }
            | expr DIV_ASSIGN expr      { $$ = mk_compound("/",  $1, $3, @2.first_line); }
            | expr MOD_ASSIGN expr      { $$ = mk_compound("%",  $1, $3, @2.first_line); }
            | expr AND_ASSIGN expr      { $$ = mk_compound("&",  $1, $3, @2.first_line); }
            | expr OR_ASSIGN expr       { $$ = mk_compound("|",  $1, $3, @2.first_line); }
            | expr XOR_ASSIGN expr      { $$ = mk_compound("^",  $1, $3, @2.first_line); }
            | expr SHL_ASSIGN expr      { $$ = mk_compound("<<", $1, $3, @2.first_line); }
            | expr SHR_ASSIGN expr      { $$ = mk_compound(">>", $1, $3, @2.first_line); }
            | expr USHR_ASSIGN expr     { $$ = mk_compound(">>>", $1, $3, @2.first_line); }
            | expr '?' expr ':' expr    { $$ = mk_node(N_COND, @2.first_line); $$->a = $1; $$->b = $3; $$->c = $5; }
            | expr OR expr              { $$ = mk_binop("||", $1, $3, @2.first_line); }
            | expr AND expr             { $$ = mk_binop("&&", $1, $3, @2.first_line); }
            | expr '|' expr             { $$ = mk_binop("|",  $1, $3, @2.first_line); }
            | expr '^' expr             { $$ = mk_binop("^",  $1, $3, @2.first_line); }
            | expr '&' expr             { $$ = mk_binop("&",  $1, $3, @2.first_line); }
            | expr EQ expr              { $$ = mk_binop("==", $1, $3, @2.first_line); }
            | expr NEQ expr             { $$ = mk_binop("!=", $1, $3, @2.first_line); }
            | expr '<' expr             { $$ = mk_binop("<",  $1, $3, @2.first_line); }
            | expr '>' expr             { $$ = mk_binop(">",  $1, $3, @2.first_line); }
            | expr LE expr              { $$ = mk_binop("<=", $1, $3, @2.first_line); }
            | expr GE expr              { $$ = mk_binop(">=", $1, $3, @2.first_line); }
            | expr SHL expr             { $$ = mk_binop("<<", $1, $3, @2.first_line); }
            | expr SHR expr             { $$ = mk_binop(">>", $1, $3, @2.first_line); }
            | expr USHR expr            { $$ = mk_binop(">>>", $1, $3, @2.first_line); }
            | expr '+' expr             { $$ = mk_binop("+",  $1, $3, @2.first_line); }
            | expr '-' expr             { $$ = mk_binop("-",  $1, $3, @2.first_line); }
            | expr '*' expr             { $$ = mk_binop("*",  $1, $3, @2.first_line); }
            | expr '/' expr             { $$ = mk_binop("/",  $1, $3, @2.first_line); }
            | expr '%' expr             { $$ = mk_binop("%",  $1, $3, @2.first_line); }
            | '!' expr                  { $$ = mk_unop("!", $2, @1.first_line); }
            | '~' expr                  { $$ = mk_unop("~", $2, @1.first_line); }
            | '-' expr %prec UMINUS     { if ($2->kind == N_INT)        { $2->ival = -$2->ival; $$ = $2; }
                                          else if ($2->kind == N_FLOAT) { $2->fval = -$2->fval; $$ = $2; }
                                          else $$ = mk_unop("-", $2, @1.first_line); }
            | '+' expr %prec UMINUS     { $$ = mk_unop("+", $2, @1.first_line); }
            | '&' expr %prec UMINUS     { $$ = mk_unop("&", $2, @1.first_line); }
            | INC expr                  { $$ = mk_incdec("++", 1, $2, @1.first_line); }
            | DEC expr                  { $$ = mk_incdec("--", 1, $2, @1.first_line); }
            | expr INC                  { $$ = mk_incdec("++", 0, $1, @2.first_line); }
            | expr DEC                  { $$ = mk_incdec("--", 0, $1, @2.first_line); }
            | '(' type_spec ptr_opt ')' expr %prec CAST
                                        { $$ = mk_node(N_CAST, @1.first_line); $$->type = $2.t; $$->dims = $2.dims;
                                          if ($2.t == T_CHAR && $3 % 1000) $$->type = T_STRING; $$->a = $5;
                                          $$->f32 = $2.t == T_FLOAT && $2.size == 4; }
            | FCAST '(' expr ',' expr ')'                     /* string(5, '*') */
                                        { $$ = mk_call(NULL, strdup("string"), list_append($3, $5), @1.first_line); $$->flag = -2; }
            | FCAST '(' ')'             { $$ = mk_node(N_CAST, @1.first_line); $$->type = (Type)($1 & 0xFF);
                                          $$->a = (($1 & 0xFF) == T_STRING) ? mk_str(strdup(""), @1.first_line) : mk_int(0, @1.first_line); }
            | FCAST '(' expr ')'         { $$ = mk_node(N_CAST, @1.first_line); $$->type = (Type)($1 & 0xFF); $$->f32 = ($1 & 0x100) != 0; $$->a = $3; }
            | STATIC_CAST '<' type_spec '>' '(' expr ')'
                                        { $$ = mk_node(N_CAST, @1.first_line); $$->type = $3.t; $$->a = $6; $$->f32 = $3.t == T_FLOAT && $3.size == 4; }
            | SIZEOF '(' type_spec ptr_opt ')' %prec '.'
                                        { $$ = mk_node(N_SIZEOF, @1.first_line); $$->type = $3.t; $$->ptr = $4;
                                          $$->ival = $4 % 1000 ? 8 : ($3.dims ? 8 : $3.size); }
            | SIZEOF expr               { $$ = mk_node(N_SIZEOF, @1.first_line); $$->a = $2; }
            | NEW base_type new_dims %prec NEW
                                        { $$ = mk_node(N_NEWARR, @1.first_line); $$->type = $2.t; $$->a = $3; $$->dims = list_len($3);
                                          $$->f32 = $2.t == T_FLOAT && $2.size == 4; }
            | NEW base_type arr_empty '{' init_list_opt '}'
                                        { $$ = mk_node(N_ARRLIT, @1.first_line); $$->type = $2.t; $$->dims = $3; $$->a = $5; $$->flag = 1;
                                          $$->f32 = $2.t == T_FLOAT && $2.size == 4; }
            | NEW SCANNER_T '(' args_opt ')'
                                        { $$ = mk_node(N_NEWOBJ, @1.first_line); $$->name = strdup("Scanner"); $$->type = T_SCANNER; $$->b = $4; }
            | NEW ID '(' args_opt ')'   { $$ = mk_node(N_NEWOBJ, @1.first_line); $$->name = $2; $$->b = $4; }
            | NEW STRING_T '(' args_opt ')'
                                        { $$ = mk_call(NULL, strdup("String"), $4, @1.first_line); $$->flag = -1; }
            | expr '[' expr ']'         { $$ = mk_node(N_INDEX, @2.first_line); $$->a = $1; $$->b = $3; }
            | expr '.' ID               { $$ = mk_node(N_FIELD, @2.first_line); $$->a = $1; $$->name = $3; }
            | expr '.' ID '(' args_opt ')'
                                        { $$ = mk_call($1, $3, $5, @3.first_line); }
            | expr '.' PRINT '(' args_opt ')'
                                        { $$ = mk_call($1, strdup("print"), $5, @3.first_line); }  /* System.out.print */
            | ID '(' args_opt ')'       { $$ = mk_call(NULL, $1, $3, @1.first_line); }
            | '(' expr ')'              { $$ = $2; }
            | ID                        { $$ = mk_id($1, @1.first_line); }
            | INT_LIT                   { $$ = mk_int($1, @1.first_line); }
            | LONG_LIT                  { $$ = mk_long($1, @1.first_line); }
            | CHAR_LIT                  { $$ = mk_char($1, @1.first_line); }
            | FLOAT_LIT                 { $$ = mk_float($1, @1.first_line); }
            | F32_LIT                   { $$ = mk_float($1, @1.first_line); $$->f32 = 1; }
            | string_lit                { $$ = mk_str($1, @1.first_line); }
            | TRUE_LIT                  { $$ = mk_bool(1, @1.first_line); }
            | FALSE_LIT                 { $$ = mk_bool(0, @1.first_line); }
            ;

%%

void yyerror(const char *msg) {
    const char *m = msg;
    if (strncmp(m, "syntax error, ", 14) == 0) m += 14;
    fprintf(stderr, "Syntax error at line %d: %s\n", yylineno, m);
    syntax_errors++;
}
