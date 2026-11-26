/* Lexify symbol table - stack of scopes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "symbol_table/symbol_table.h"

typedef struct Scope {
    Symbol *head;
    struct Scope *parent;
} Scope;

static Scope  *cur = NULL;
static int     level = -1;
static Symbol *all_head = NULL, *all_tail = NULL;

void st_init(void) {
    cur = NULL; level = -1;
    all_head = all_tail = NULL;
    st_enter();                       /* global scope */
}

void st_enter(void) {
    Scope *s = calloc(1, sizeof(Scope));
    if (!s) { fprintf(stderr, "out of memory\n"); exit(2); }
    s->parent = cur;
    cur = s;
    level++;
}

void st_exit(void) {
    if (!cur || !cur->parent) return;     /* never close the global scope */
    for (Symbol *p = cur->head; p; p = p->next_in_scope) p->active = 0;
    Scope *old = cur;
    cur = cur->parent;
    level--;
    free(old);
}

int st_level(void) { return level; }

Symbol *st_insert(const char *name, Type t, int line) {
    Symbol *s = calloc(1, sizeof(Symbol));
    if (!s) { fprintf(stderr, "out of memory\n"); exit(2); }
    s->name = strdup(name);
    s->type = t; s->scope = level; s->line = line; s->active = 1;
    s->next_in_scope = cur->head;
    cur->head = s;
    if (all_tail) all_tail->next_all = s; else all_head = s;
    all_tail = s;
    return s;
}

Symbol *st_lookup_current(const char *name) {
    for (Symbol *p = cur->head; p; p = p->next_in_scope)
        if (strcmp(p->name, name) == 0) return p;
    return NULL;
}

Symbol *st_lookup(const char *name) {
    for (Scope *sc = cur; sc; sc = sc->parent)
        for (Symbol *p = sc->head; p; p = p->next_in_scope)
            if (strcmp(p->name, name) == 0) return p;
    return NULL;
}

Symbol *st_find_inactive(const char *name) {
    for (Symbol *p = all_head; p; p = p->next_all)
        if (!p->active && strcmp(p->name, name) == 0) return p;
    return NULL;
}

void st_dump(void) {
    printf("%-12s %-10s %-6s %s\n", "Name", "Type", "Scope", "Line");
    printf("------------ ---------- ------ ----\n");
    for (Symbol *p = all_head; p; p = p->next_all) {
        char t[80];
        snprintf(t, sizeof t, "%s%s", type_name_dims(p->type, p->dims), p->is_func ? "()" : "");
        printf("%-12s %-10s %-6d %d\n", p->name, t, p->scope, p->line);
    }
}
