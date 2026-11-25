/* Lexify AST - construction and printing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast/ast.h"

static Node *new_node(NodeKind k, int line) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "out of memory\n"); exit(2); }
    n->kind = k; n->line = line; n->type = T_NONE;
    return n;
}

Node *mk_program(Node *s)  { Node *n = new_node(N_PROGRAM, 1); n->a = s; return n; }
Node *mk_block(Node *s, int l) { Node *n = new_node(N_BLOCK, l); n->a = s; return n; }
Node *mk_decl(Type t, char *name, int l) { Node *n = new_node(N_DECL, l); n->type = t; n->name = name; return n; }
Node *mk_assign(char *name, Node *e, int l) { Node *n = new_node(N_ASSIGN, l); n->name = name; n->a = e; return n; }
Node *mk_print(Node *e, int l) { Node *n = new_node(N_PRINT, l); n->a = e; return n; }
Node *mk_if(Node *c, Node *t, Node *e, int l) { Node *n = new_node(N_IF, l); n->a = c; n->b = t; n->c = e; return n; }
Node *mk_while(Node *c, Node *b, int l) { Node *n = new_node(N_WHILE, l); n->a = c; n->b = b; return n; }
Node *mk_binop(const char *op, Node *a, Node *b, int l) { Node *n = new_node(N_BINOP, l); n->op = op; n->a = a; n->b = b; return n; }
Node *mk_unop(const char *op, Node *e, int l) { Node *n = new_node(N_UNOP, l); n->op = op; n->a = e; return n; }
Node *mk_id(char *name, int l) { Node *n = new_node(N_ID, l); n->name = name; return n; }
Node *mk_int(int v, int l) { Node *n = new_node(N_INT, l); n->ival = v; return n; }
Node *mk_float(double v, int l) { Node *n = new_node(N_FLOAT, l); n->fval = v; return n; }
Node *mk_bool(int v, int l) { Node *n = new_node(N_BOOL, l); n->ival = v; return n; }

Node *mk_str(char *t, int l) { Node *n = new_node(N_STR, l); n->name = t; return n; }
Node *mk_return(Node *e, int l) { Node *n = new_node(N_RETURN, l); n->a = e; return n; }

/* printf("a %d b\n", x)  ->  print "a "; print x; print " b\n";
 * Literal text becomes string prints, each %d/%f/%c/%s/%i/%u/%ld.. takes the next argument. */
Node *mk_printf(char *fmt, Node *args, int l) {
    Node *out = NULL;
    size_t len = strlen(fmt), bi = 0;
    char *buf = malloc(len + 1);
    if (!buf) { fprintf(stderr, "out of memory\n"); exit(2); }
    for (size_t i = 0; i < len; i++) {
        if (fmt[i] == '%' && i + 1 < len && fmt[i + 1] == '%') { buf[bi++] = '%'; i++; continue; }
        if (fmt[i] == '%' && args) {
            size_t j = i + 1;
            while (j < len && strchr("0123456789.-+ #lhz", fmt[j])) j++;   /* flags / width / length */
            if (j < len && strchr("dioufFeEgGcsx", fmt[j])) {
                if (bi) { buf[bi] = 0; out = list_append(out, mk_print(mk_str(strdup(buf), l), l)); bi = 0; }
                Node *a = args; args = args->next; a->next = NULL;
                Node *pr = mk_print(a, l);
                pr->name = malloc(j - i + 2);                 /* the specifier, e.g. "%.2f" */
                memcpy(pr->name, fmt + i, j - i + 1); pr->name[j - i + 1] = 0;
                out = list_append(out, pr);
                i = j;
                continue;
            }
        }
        buf[bi++] = fmt[i];
    }
    if (bi) { buf[bi] = 0; out = list_append(out, mk_print(mk_str(strdup(buf), l), l)); }
    while (args) {                    /* extra arguments without a specifier: still printed */
        Node *a = args; args = args->next; a->next = NULL;
        out = list_append(out, mk_print(a, l));
    }
    free(buf);
    return out;
}

/* One print without a newline.  Spec "%C" = C++ cout style (bool -> 1/0),
 * "%J" = Java style (bool -> true/false, double -> 10.0), "%c" = a char literal. */
static Node *print_item(Node *e, const char *spec, int l) {
    e->next = NULL;
    Node *pr = mk_print(e, l);
    if (e->kind == N_STR) return pr;                       /* text: printed as it is */
    if (e->kind == N_INT && e->op && strcmp(e->op, "char") == 0) spec = "%c";
    pr->name = strdup(spec);
    return pr;
}

/* cout << a << " text " << endl;   ->  print a, "%C"; print " text "; print "\n" */
Node *mk_cout(Node *items, int l) {
    Node *out = NULL;
    while (items) {
        Node *e = items; items = items->next;
        out = list_append(out, print_item(e, "%C", l));
    }
    return out;
}

/* Java string concatenation is left-associative: "x" + a + b  ==  ("x" + a) + b.
 * A '+' whose operands contain a string is concatenation; otherwise it is arithmetic. */
static int has_string(const Node *e) {
    if (!e) return 0;
    if (e->kind == N_STR) return 1;
    return e->kind == N_BINOP && strcmp(e->op, "+") == 0 && (has_string(e->a) || has_string(e->b));
}

static Node *concat_parts(Node *e, int l) {
    if (e->kind == N_BINOP && strcmp(e->op, "+") == 0 && has_string(e)) {
        Node *left = concat_parts(e->a, l);
        return list_append(left, concat_parts(e->b, l));
    }
    return print_item(e, "%J", l);
}

/* System.out.println("Largest = " + largest);  ->  print "Largest = "; print largest, "%J"; print "\n" */
Node *mk_jprint(Node *e, int newline, int l) {
    Node *out = e ? concat_parts(e, l) : NULL;
    if (newline) out = list_append(out, mk_print(mk_str(strdup("\\n"), l), l));
    return out;
}

Node *list_append(Node *list, Node *item) {
    if (!item) return list;
    if (!list) return item;
    Node *p = list;
    while (p->next) p = p->next;
    p->next = item;
    return list;
}

const char *type_name(Type t) {
    switch (t) { case T_INT: return "int"; case T_FLOAT: return "float"; case T_BOOL: return "bool"; default: return "none"; }
}

static void indent(int d) { for (int i = 0; i < d; i++) printf("  "); }

static void print_one(const Node *n, int d) {
    indent(d);
    switch (n->kind) {
    case N_PROGRAM: printf("Program\n"); ast_print(n->a, d + 1); break;
    case N_BLOCK:   printf("Block\n");   ast_print(n->a, d + 1); break;
    case N_DECL:    printf("Decl %s %s (line %d)\n", type_name(n->type), n->name, n->line); break;
    case N_ASSIGN:  printf("Assign %s (line %d)\n", n->name, n->line); print_one(n->a, d + 1); break;
    case N_PRINT:   printf("Print%s%s (line %d)\n", n->name ? " " : "", n->name ? n->name : "", n->line); print_one(n->a, d + 1); break;
    case N_IF:
        printf("If (line %d)\n", n->line);
        indent(d + 1); printf("Cond:\n");  print_one(n->a, d + 2);
        indent(d + 1); printf("Then:\n");  ast_print(n->b, d + 2);
        if (n->c) { indent(d + 1); printf("Else:\n"); ast_print(n->c, d + 2); }
        break;
    case N_WHILE:
        printf("While (line %d)\n", n->line);
        indent(d + 1); printf("Cond:\n"); print_one(n->a, d + 2);
        indent(d + 1); printf("Body:\n"); ast_print(n->b, d + 2);
        break;
    case N_BINOP: printf("BinOp '%s'\n", n->op); print_one(n->a, d + 1); print_one(n->b, d + 1); break;
    case N_UNOP:  printf("UnOp '%s'\n", n->op);  print_one(n->a, d + 1); break;
    case N_ID:    printf("Id %s\n", n->name); break;
    case N_INT:   printf("Int %d\n", n->ival); break;
    case N_FLOAT: printf("Float %g\n", n->fval); break;
    case N_BOOL:  printf("Bool %s\n", n->ival ? "true" : "false"); break;
    case N_STR:   printf("String \"%s\"\n", n->name); break;
    case N_RETURN: printf("Return (line %d)\n", n->line); if (n->a) print_one(n->a, d + 1); break;
    }
}

void ast_print(const Node *n, int depth) {
    for (; n; n = n->next) print_one(n, depth);
}
