/* Lexify AST - node definitions (manual 4.3)
 * One AST is shared by the mini language (.mc) and the C / C++ / Java front ends. */
#ifndef LEXIFY_AST_H
#define LEXIFY_AST_H

/* T_INT .. T_NONE keep their original values (used by the mini-language tests) */
typedef enum {
    T_INT, T_FLOAT, T_BOOL, T_NONE,
    T_CHAR, T_LONG, T_STRING, T_VOID, T_SCANNER, T_AUTO
} Type;

/* a type as written in a declaration: base type + array dimensions ([] count) */
typedef struct { Type t; int dims; int size; } TypeSpec;   /* size = sizeof the base type in C */

/* source language: decides printing rules (cout / System.out) and how strict the type checks are */
typedef enum { LANG_MC, LANG_C, LANG_CPP, LANG_JAVA } Lang;
extern Lang lang_mode;
const char *lang_name(Lang l);

typedef enum {
    N_PROGRAM, N_BLOCK, N_DECL, N_ASSIGN, N_PRINT, N_IF, N_WHILE,
    N_BINOP, N_UNOP, N_ID, N_INT, N_FLOAT, N_BOOL, N_STR, N_RETURN,
    /* C / C++ / Java extensions */
    N_CHAR,      /* 'a' */
    N_DOWHILE,   /* a = cond, b = body */
    N_FOR,       /* a = init list, b = cond, c = step list, d = body */
    N_FOREACH,   /* for (T x : coll): name/type = loop variable, a = collection, b = body */
    N_BREAK, N_CONTINUE,
    N_SWITCH,    /* a = expr, b = flat list of statements and N_CASE labels */
    N_CASE,      /* a = value (NULL = default) */
    N_EXPRSTMT,  /* a = expression evaluated for its side effects */
    N_WRITE,     /* cout << a / System.out.print(a): a = expr */
    N_READ,      /* cin >> x / scanf / getline: a = lvalue, op = kind ("int", "line" ...) */
    N_MANIP,     /* cout manipulator: name = fixed/setprecision/setw..., a = argument */
    N_INCDEC,    /* op = "++"/"--", flag = 1 prefix / 0 postfix, a = lvalue */
    N_COND,      /* a ? b : c */
    N_CAST,      /* (type) a */
    N_CALL,      /* name(args): a = receiver (obj.name(...)) or NULL, b = argument list */
    N_INDEX,     /* a[b] */
    N_FIELD,     /* a.name  (arr.length, Math.PI, Integer.MAX_VALUE) */
    N_NEWARR,    /* new T[a][b]...: a = list of size expressions (NULL entries = []) */
    N_ARRLIT,    /* { e1, e2, ... }: a = element list */
    N_NEWOBJ,    /* new Scanner(System.in) */
    N_FUNC,      /* function: name, type/dims = return type, a = params (N_DECL), b = body (N_BLOCK) */
    N_SIZEOF,    /* sizeof(type) / sizeof(expr): a = expr or NULL, type/dims when a type */
    N_LABELED    /* label: stmt   (Java labeled loops / blocks): name = label, a = statement */
} NodeKind;

/* N_CALL.flag after semantic analysis */
enum { CALL_UNRESOLVED, CALL_USER, CALL_BUILTIN, CALL_READ, CALL_NOP };

typedef struct Node {
    NodeKind     kind;
    int          line;
    const char  *op;      /* operator / builtin name / read kind */
    char        *name;    /* identifier, function name, string text */
    Type         type;    /* declared type, later: expression type */
    int          dims;    /* array dimensions of the type */
    int          ptr;     /* N_DECL: number of '*' (char* is a string) */
    int          flag;    /* see uses above */
    int          is_const;
    int          is_ref;  /* C++ reference: int &x (parameters, for-each variables) */
    int          f32;     /* 32-bit float (C 'float', Java 'float', 2.5f) as opposed to double */
    long long    ival;    /* N_INT, N_CHAR, N_BOOL (0/1) */
    double       fval;    /* N_FLOAT */
    struct Node *a, *b, *c, *d;   /* children */
    struct Node *next;            /* next item in a list */
    struct Symbol *sym;           /* resolved symbol (filled by the semantic pass) */
    struct Node *fn;              /* N_CALL: called user function / N_RETURN: enclosing function */
} Node;

Node *mk_node(NodeKind k, int line);
Node *mk_program(Node *stmts);
Node *mk_block(Node *stmts, int line);
Node *mk_decl(Type t, char *name, int line);
Node *mk_assign(char *name, Node *expr, int line);
Node *mk_assign_expr(Node *target, Node *expr, int line);              /* target = expr */
Node *mk_compound(const char *op, Node *target, Node *expr, int line);  /* target op= expr */
Node *mk_print(Node *expr, int line);
Node *mk_if(Node *cond, Node *then_s, Node *else_s, int line);
Node *mk_while(Node *cond, Node *body, int line);
Node *mk_binop(const char *op, Node *l, Node *r, int line);
Node *mk_unop(const char *op, Node *e, int line);
Node *mk_id(char *name, int line);
Node *mk_int(long long v, int line);
Node *mk_long(long long v, int line);
Node *mk_char(long long v, int line);
Node *mk_float(double v, int line);
Node *mk_bool(int v, int line);
Node *mk_str(char *text, int line);              /* string literal (raw text, escapes kept) */
Node *mk_return(Node *expr, int line);           /* expr may be NULL */
Node *mk_printf(char *fmt, Node *args, int line);/* expands to a chain of N_PRINT */
Node *mk_call(Node *recv, char *name, Node *args, int line);
Node *mk_exprstmt(Node *e, int line);            /* recognises cout/cin/printf/scanf/println ... */
Node *mk_scanf(const char *fmt, Node *args, int line);   /* one N_READ per % conversion */
void  add_class_name(const char *name);         /* Java classes in the file (Helper.f() calls) */
int   is_class_name(const char *name);

Node *list_append(Node *list, Node *item);   /* safe when item == NULL */
int   list_len(const Node *list);
const char *type_name(Type t);
const char *type_name_dims(Type t, int dims); /* "int[]" ... (static buffer) */
void  ast_print(const Node *n, int depth);   /* prints a statement chain */

#endif
