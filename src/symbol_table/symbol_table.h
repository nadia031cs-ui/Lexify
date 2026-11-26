/* Lexify symbol table with nested scopes (manual 4.4) */
#ifndef LEXIFY_SYMBOL_TABLE_H
#define LEXIFY_SYMBOL_TABLE_H

#include "ast/ast.h"

typedef struct Symbol {
    char *name;
    Type  type;
    int   dims;                  /* array dimensions (0 = scalar) */
    int   is_const;              /* const / final */
    int   f32;                   /* 32-bit float variable (or array of them) */
    int   is_func;               /* 1 = function (type = return type) */
    Node *func;                  /* the N_FUNC node of a function */
    int   scope;                 /* 0 = global, 1 = first nested block, ... */
    int   line;                  /* line where it was declared */
    int   active;                /* 0 once its scope has been closed */
    struct Symbol *next_in_scope;
    struct Symbol *next_all;     /* every symbol ever declared, in order */
} Symbol;

void    st_init(void);                          /* resets and opens global scope */
void    st_enter(void);                         /* open a nested scope */
void    st_exit(void);                          /* close it; its symbols become inactive */
int     st_level(void);                         /* current nesting level (0 = global) */
Symbol *st_insert(const char *name, Type t, int line);
Symbol *st_lookup(const char *name);            /* innermost -> outermost, active only */
Symbol *st_lookup_current(const char *name);    /* current scope only (redeclaration check) */
Symbol *st_find_inactive(const char *name);     /* declared before but its scope is closed */
void    st_dump(void);                          /* prints the whole table */

#endif
