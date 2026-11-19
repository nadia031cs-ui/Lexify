# Formal CFG (matches src/parser/parser.y)

```
program   -> stmt_list
stmt_list -> ε | stmt_list stmt
stmt      -> decl ';'
           | ID '=' expr ';'
           | 'print' expr ';'
           | 'if' '(' expr ')' stmt
           | 'if' '(' expr ')' stmt 'else' stmt
           | 'while' '(' expr ')' stmt
           | block
block     -> '{' stmt_list '}'
decl      -> type ID
type      -> 'int' | 'float' | 'bool'
expr      -> expr '||' expr | expr '&&' expr
           | expr ('==' | '!=') expr
           | expr ('<' | '>' | '<=' | '>=') expr
           | expr ('+' | '-') expr
           | expr ('*' | '/' | '%') expr
           | '!' expr | '(' expr ')'
           | ID | INT_LIT | FLOAT_LIT | 'true' | 'false'
```

Precedence (low to high): `||`, `&&`, `== !=`, `< > <= >=`, `+ -`, `* / %`, `!`. All binary operators are left-associative.
Dangling else is resolved by `%prec LOWER_THAN_ELSE` (else binds to the nearest if).

## C-style extensions (accepted in addition to the grammar above)
```
stmt      -> decl_stmt ';' | simple ';' | 'printf' '(' STRING [',' expr_list] ')' ';'
           | 'return' [expr] ';' | 'for' '(' [decl_stmt|simple] ';' [expr] ';' [simple] ')' stmt | ';' | func_def
func_def  -> (type|'void') ID '(' ['void'] ')' block        // int main() { ... }
decl_stmt -> type declarator (',' declarator)*              // int a = 1, b;
declarator-> ID | ID '=' expr
simple    -> ID ('='|'+='|'-='|'*='|'/='|'%=') expr | ID '++' | ID '--' | '++' ID | '--' ID
expr      -> ... | '-' expr                                  // unary minus
type      -> ... | 'double'                                  // same as float
```
`#include` / `#define` lines are ignored by the lexer. `printf` is lowered to `print` instructions.
