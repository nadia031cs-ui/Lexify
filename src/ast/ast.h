/* Lexify AST - node definitions (manual 4.3) */
#ifndef LEXIFY_AST_H
#define LEXIFY_AST_H

typedef enum { T_INT, T_FLOAT, T_BOOL, T_NONE } Type;

typedef enum {
    N_PROGRAM, N_BLOCK, N_DECL, N_ASSIGN, N_PRINT, N_IF, N_WHILE,
    N_BINOP, N_UNOP, N_ID, N_INT, N_FLOAT, N_BOOL, N_STR, N_RETURN
} NodeKind;

typedef struct Node {
    NodeKind     kind;
    int          line;
    const char  *op;      /* N_BINOP / N_UNOP: "+", "&&", "!" ... */
    char        *name;    /* N_DECL / N_ASSIGN / N_ID */
    Type         type;    /* N_DECL: declared type; later: expression type */
    int          ival;    /* N_INT, N_BOOL (0/1) */
    double       fval;    /* N_FLOAT */
    struct Node *a, *b, *c;   /* children: cond/lhs, then/rhs, else */
    struct Node *next;        /* next statement in a statement list */
} Node;

Node *mk_program(Node *stmts);
Node *mk_block(Node *stmts, int line);
Node *mk_decl(Type t, char *name, int line);
Node *mk_assign(char *name, Node *expr, int line);
Node *mk_print(Node *expr, int line);
Node *mk_if(Node *cond, Node *then_s, Node *else_s, int line);
Node *mk_while(Node *cond, Node *body, int line);
Node *mk_binop(const char *op, Node *l, Node *r, int line);
Node *mk_unop(const char *op, Node *e, int line);
Node *mk_id(char *name, int line);
Node *mk_int(int v, int line);
Node *mk_float(double v, int line);
Node *mk_bool(int v, int line);
Node *mk_str(char *text, int line);              /* string literal (printf text) */
Node *mk_return(Node *expr, int line);           /* expr may be NULL */
Node *mk_printf(char *fmt, Node *args, int line);/* expands to a chain of N_PRINT */
Node *mk_cout(Node *items, int line);            /* C++  cout << a << "x" << endl;    -> chain of N_PRINT */
Node *mk_jprint(Node *expr, int newline, int line); /* Java System.out.println("x = " + a); -> chain of N_PRINT */

Node *list_append(Node *list, Node *item);   /* safe when item == NULL */
const char *type_name(Type t);
void  ast_print(const Node *n, int depth);   /* prints a statement chain */

#endif
