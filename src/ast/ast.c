/* Lexify AST - construction and printing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast/ast.h"

Lang lang_mode = LANG_MC;

static char *class_names[64];
static int nclass_names;
void add_class_name(const char *name) { if (nclass_names < 64) class_names[nclass_names++] = strdup(name); }
int is_class_name(const char *name) {
    for (int i = 0; i < nclass_names; i++) if (strcmp(class_names[i], name) == 0) return 1;
    return 0;
}

const char *lang_name(Lang l) {
    switch (l) { case LANG_C: return "C"; case LANG_CPP: return "C++"; case LANG_JAVA: return "Java"; default: return "mini"; }
}

static char *xstrdup(const char *s) {
    char *p = strdup(s ? s : "");
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

Node *mk_node(NodeKind k, int line) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "out of memory\n"); exit(2); }
    n->kind = k; n->line = line; n->type = T_NONE;
    return n;
}
#define new_node mk_node

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
Node *mk_int(long long v, int l) { Node *n = new_node(N_INT, l); n->ival = v; return n; }
Node *mk_long(long long v, int l) { Node *n = new_node(N_INT, l); n->ival = v; n->flag = 1; return n; }
Node *mk_char(long long v, int l) { Node *n = new_node(N_CHAR, l); n->ival = v; return n; }
Node *mk_float(double v, int l) { Node *n = new_node(N_FLOAT, l); n->fval = v; return n; }
Node *mk_bool(int v, int l) { Node *n = new_node(N_BOOL, l); n->ival = v; return n; }
Node *mk_str(char *t, int l) { Node *n = new_node(N_STR, l); n->name = t; return n; }
Node *mk_return(Node *e, int l) { Node *n = new_node(N_RETURN, l); n->a = e; return n; }

Node *mk_call(Node *recv, char *name, Node *args, int l) {
    Node *n = new_node(N_CALL, l); n->a = recv; n->name = name; n->b = args; return n;
}

/* target = e   (simple variables keep the original N_ASSIGN form used by the TAC tests) */
Node *mk_assign_expr(Node *target, Node *e, int l) {
    if (target && target->kind == N_ID) return mk_assign(xstrdup(target->name), e, l);
    Node *n = new_node(N_ASSIGN, l); n->c = target; n->a = e; return n;
}

static Node *copy_id(const Node *id) { return mk_id(xstrdup(id->name), id->line); }

/* target op= e  ->  target = target op e  (an indexed target keeps 'op' so its index is evaluated once) */
Node *mk_compound(const char *op, Node *target, Node *e, int l) {
    if (target && target->kind == N_ID) {
        Node *n = mk_assign(xstrdup(target->name), mk_binop(op, copy_id(target), e, l), l);
        n->flag = 2;                     /* compound: Java allows the implicit narrowing */
        return n;
    }
    Node *n = new_node(N_ASSIGN, l); n->c = target; n->a = e; n->op = op; n->flag = 2; return n;
}

/* printf("a %d b\n", x)  ->  print "a "; print x, "%d"; print " b\n";
 * Literal text becomes string prints, each %d/%f/%c/%s/%i/%u/%ld.. takes the next argument. */
Node *mk_printf(char *fmt, Node *args, int l) {
    Node *out = NULL;
    size_t len = strlen(fmt), bi = 0;
    char *buf = malloc(len + 3);
    if (!buf) { fprintf(stderr, "out of memory\n"); exit(2); }
    for (size_t i = 0; i < len; i++) {
        if (fmt[i] == '%' && i + 1 < len && fmt[i + 1] == '%') { buf[bi++] = '%'; i++; continue; }
        if (fmt[i] == '%' && i + 1 < len && fmt[i + 1] == 'n') { buf[bi++] = '\\'; buf[bi++] = 'n'; i++; continue; } /* Java %n */
        if (fmt[i] == '%' && args) {
            size_t j = i + 1;
            while (j < len && strchr("0123456789.-+ #lhzLjt',", fmt[j])) j++;   /* flags / width / length */
            if (j < len && strchr("dioufFeEgGcsxXbBaA", fmt[j])) {
                if (bi) { buf[bi] = 0; out = list_append(out, mk_print(mk_str(xstrdup(buf), l), l)); bi = 0; }
                Node *a = args; args = args->next; a->next = NULL;
                Node *pr = mk_print(a, l);
                pr->name = malloc(j - i + 2);                 /* the specifier, e.g. "%.2f" */
                if (!pr->name) { fprintf(stderr, "out of memory\n"); exit(2); }
                size_t k = 0;
                for (size_t q = i; q <= j; q++) if (fmt[q] != ',' && fmt[q] != '\'') pr->name[k++] = fmt[q];
                pr->name[k] = 0;
                out = list_append(out, pr);
                i = j;
                continue;
            }
        }
        buf[bi++] = fmt[i];
    }
    if (bi) { buf[bi] = 0; out = list_append(out, mk_print(mk_str(xstrdup(buf), l), l)); }
    while (args) {                    /* extra arguments without a specifier: still printed */
        Node *a = args; args = args->next; a->next = NULL;
        out = list_append(out, mk_print(a, l));
    }
    free(buf);
    if (!out) out = mk_node(N_BLOCK, l);   /* printf("") */
    return out;
}

/* ---- statement-level recognition of the I/O idioms of C, C++ and Java ---- */

static int is_id(const Node *n, const char *name) { return n && n->kind == N_ID && strcmp(n->name, name) == 0; }

static Node *mk_write(Node *e, int l) { Node *n = new_node(N_WRITE, l); n->a = e; return n; }
static Node *mk_newline(int l) { return mk_write(mk_str(xstrdup("\\n"), l), l); }
static Node *mk_read(Node *target, const char *kind, int l) {
    Node *n = new_node(N_READ, l);
    if (target && target->kind == N_UNOP && strcmp(target->op, "&") == 0) target = target->a;  /* scanf("%d", &x) */
    n->a = target; n->op = kind; return n;
}

/* System.out / System.err */
static int is_system_out(const Node *r) {
    return r && r->kind == N_FIELD && is_id(r->a, "System") && (strcmp(r->name, "out") == 0 || strcmp(r->name, "err") == 0);
}

static const char *manip_names[] = { "fixed", "scientific", "boolalpha", "noboolalpha", "left", "right",
                                     "showpoint", "noshowpoint", "defaultfloat", "flush", NULL };

static Node *cout_item(Node *it, int l) {
    if (is_id(it, "endl")) return mk_newline(l);
    if (it->kind == N_ID)
        for (int i = 0; manip_names[i]; i++)
            if (strcmp(it->name, manip_names[i]) == 0) { Node *m = new_node(N_MANIP, l); m->name = it->name; return m; }
    if (it->kind == N_CALL && !it->a &&
        (!strcmp(it->name, "setprecision") || !strcmp(it->name, "setw") || !strcmp(it->name, "setfill"))) {
        Node *m = new_node(N_MANIP, l); m->name = it->name; m->a = it->b; if (m->a) m->a->next = NULL; return m;
    }
    return mk_write(it, l);
}

/* collect the right operands of a left-leaning chain  ((s op a) op b) op c  ->  a, b, c  */
static Node *chain_items(Node *e, const char *op, const char *head1, const char *head2, const char *head3) {
    Node *spine[4096]; int n = 0;
    Node *p = e;
    while (p && p->kind == N_BINOP && strcmp(p->op, op) == 0 && n < 4096) { spine[n++] = p; p = p->a; }
    if (!(is_id(p, head1) || (head2 && is_id(p, head2)) || (head3 && is_id(p, head3))) || n == 0) return NULL;
    Node *items = NULL;
    for (int i = n - 1; i >= 0; i--) { Node *it = spine[i]->b; it->next = NULL; items = list_append(items, it); }
    return items;
}

/* scanf("%d %f", &a, &b) -> one N_READ per conversion */
Node *mk_scanf(const char *fmt, Node *args, int l) {
    Node *out = NULL;
    int skip_ws = 1;
    for (size_t i = 0; fmt[i]; i++) {
        if (fmt[i] == ' ' || fmt[i] == '\t' || (fmt[i] == '\\' && (fmt[i + 1] == 'n' || fmt[i + 1] == 't'))) { skip_ws = 1; if (fmt[i] == '\\') i++; continue; }
        if (fmt[i] != '%' || fmt[i + 1] == '%') {
            /* a literal character in the format must match the input: scanf("%d,%d") */
            char kind[16];
            int ch = (unsigned char)fmt[i];
            if (fmt[i] == '%') i++;
            snprintf(kind, sizeof kind, "%slit%d", skip_ws ? "ws" : "", ch);
            out = list_append(out, mk_read(NULL, strdup(kind), l));
            skip_ws = 0;
            continue;
        }
        size_t j = i + 1;
        int suppress = 0, longs = 0;
        if (fmt[j] == '*') { suppress = 1; j++; }
        while (fmt[j] && strchr("0123456789hlLqjzt", fmt[j])) { if (fmt[j] == 'l' || fmt[j] == 'L' || fmt[j] == 'q') longs++; j++; }
        char conv = fmt[j];
        if (!conv) break;
        i = j;
        if (conv == '%') continue;
        const char *kind = "int";
        if (strchr("di", conv)) kind = longs ? "long" : "int";
        else if (strchr("uxo", conv)) kind = longs ? "long" : "int";
        else if (strchr("feEgGaA", conv)) kind = "float";
        else if (conv == 'c') kind = skip_ws ? "char" : "rawchar";
        else if (conv == 's' || conv == '[') kind = "word";
        skip_ws = 0;
        if (suppress || !args) { if (!suppress) continue; out = list_append(out, mk_read(NULL, kind, l)); continue; }
        Node *a = args; args = args->next; a->next = NULL;
        out = list_append(out, mk_read(a, kind, l));
    }
    if (!out) out = mk_node(N_BLOCK, l);
    return out;
}

static Node *arg_n(Node *args, int i) { while (args && i--) args = args->next; return args; }

Node *mk_exprstmt(Node *e, int l) {
    if (!e) return NULL;
    Node *items;

    /* cout << a << b << endl;   (also cerr / clog) */
    if ((items = chain_items(e, "<<", "cout", "cerr", "clog")) != NULL) {
        Node *out = NULL;
        for (Node *it = items, *nx; it; it = nx) { nx = it->next; it->next = NULL; out = list_append(out, cout_item(it, l)); }
        return out;
    }
    /* cin >> a >> b; */
    if ((items = chain_items(e, ">>", "cin", NULL, NULL)) != NULL) {
        Node *out = NULL;
        for (Node *it = items, *nx; it; it = nx) { nx = it->next; it->next = NULL; out = list_append(out, mk_read(it, NULL, l)); }
        return out;
    }
    if (e->kind == N_CALL) {
        const char *f = e->name;
        Node *args = e->b;
        Node *recv = e->a;
        if (!recv && !strcmp(f, "printf") && args && args->kind == N_STR) {
            Node *rest = args->next; args->next = NULL; return mk_printf(args->name, rest, l);
        }
        if (is_system_out(recv)) {
            if ((!strcmp(f, "printf") || !strcmp(f, "format")) && args && args->kind == N_STR) {
                Node *rest = args->next; args->next = NULL; return mk_printf(args->name, rest, l);
            }
            if (!strcmp(f, "println")) {
                if (!args) return mk_newline(l);
                args->next = NULL;
                return list_append(mk_write(args, l), mk_newline(l));
            }
            if (!strcmp(f, "print") && args) { args->next = NULL; return mk_write(args, l); }
            if (!strcmp(f, "flush")) return NULL;
        }
        if (!recv && !strcmp(f, "scanf") && args && args->kind == N_STR) return mk_scanf(args->name, args->next, l);
        if (!recv && !strcmp(f, "puts") && args) { args->next = NULL; return list_append(mk_write(args, l), mk_newline(l)); }
        if (!recv && !strcmp(f, "putchar") && args) {
            Node *c = new_node(N_CAST, l); c->type = T_CHAR; c->a = args; args->next = NULL; return mk_write(c, l);
        }
        if (!recv && !strcmp(f, "getline") && is_id(args, "cin") && args->next) return mk_read(args->next, "line", l);
        if (!recv && !strcmp(f, "gets") && args) { args->next = NULL; return mk_read(args, "line", l); }
        if (!recv && !strcmp(f, "fgets") && args) { args->next = NULL; return mk_read(args, "fline", l); }
        if (is_id(recv, "cin")) {
            if (!strcmp(f, "getline") && args) { args->next = NULL; return mk_read(args, "line", l); }
            if (!strcmp(f, "get") && args) { args->next = NULL; return mk_read(args, "rawchar", l); }
            if (!strcmp(f, "ignore")) return mk_read(NULL, arg_n(args, 1) ? "ignoreline" : "ignore1", l);
        }
    }
    /* i++;  ++i;  i--;  --i;  ->  i = i + 1  (same TAC as the original grammar) */
    if (e->kind == N_INCDEC && e->a && e->a->kind == N_ID) {
        Node *n = mk_assign(xstrdup(e->a->name),
                            mk_binop(e->op[0] == '+' ? "+" : "-", copy_id(e->a), mk_int(1, l), l), l);
        n->flag = 2;
        return n;
    }
    if (e->kind == N_ASSIGN) return e;
    Node *n = new_node(N_EXPRSTMT, l);
    n->a = e;
    return n;
}

Node *list_append(Node *list, Node *item) {
    if (!item) return list;
    if (!list) return item;
    Node *p = list;
    while (p->next) p = p->next;
    p->next = item;
    return list;
}

int list_len(const Node *l) { int n = 0; for (; l; l = l->next) n++; return n; }

const char *type_name(Type t) {
    int j = lang_mode == LANG_JAVA;
    switch (t) {
    case T_INT: return "int"; case T_FLOAT: return lang_mode == LANG_MC ? "float" : "double";
    case T_BOOL: return j ? "boolean" : "bool";
    case T_CHAR: return "char"; case T_LONG: return lang_mode == LANG_CPP || lang_mode == LANG_C ? "long long" : "long";
    case T_STRING: return j ? "String" : lang_mode == LANG_C ? "char*" : "string";
    case T_VOID: return "void"; case T_SCANNER: return "Scanner"; case T_AUTO: return "auto";
    default: return "none";
    }
}

const char *type_name_dims(Type t, int dims) {
    static char buf[4][64];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 64, "%s", type_name(t));
    for (int i = 0; i < dims && strlen(b) < 60; i++) strcat(b, "[]");
    return b;
}

static void indent(int d) { for (int i = 0; i < d; i++) printf("  "); }

static void print_one(const Node *n, int d);
static void print_labeled(const char *label, const Node *n, int d) {
    if (!n) return;
    indent(d); printf("%s:\n", label);
    ast_print(n, d + 1);
}

static void print_one(const Node *n, int d) {
    indent(d);
    switch (n->kind) {
    case N_PROGRAM: printf("Program\n"); ast_print(n->a, d + 1); break;
    case N_BLOCK:   printf("Block\n");   ast_print(n->a, d + 1); break;
    case N_DECL:
        printf("Decl %s %s (line %d)\n", type_name_dims(n->type, n->dims), n->name, n->line);
        if (n->a) print_labeled("Size", n->a, d + 1);
        if (n->b) print_labeled("Init", n->b, d + 1);
        break;
    case N_ASSIGN:
        if (n->name) printf("Assign%s%s %s (line %d)\n", n->op ? " " : "", n->op ? n->op : "", n->name, n->line);
        else { printf("Assign%s%s (line %d)\n", n->op ? " " : "", n->op ? n->op : "", n->line); print_labeled("Target", n->c, d + 1); }
        print_one(n->a, d + 1); break;
    case N_PRINT:   printf("Print%s%s (line %d)\n", n->name ? " " : "", n->name ? n->name : "", n->line); print_one(n->a, d + 1); break;
    case N_WRITE:   printf("Write (line %d)\n", n->line); print_one(n->a, d + 1); break;
    case N_READ:    printf("Read %s (line %d)\n", n->op ? n->op : "", n->line); if (n->a) print_one(n->a, d + 1); break;
    case N_MANIP:   printf("Manipulator %s\n", n->name); if (n->a) print_one(n->a, d + 1); break;
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
    case N_DOWHILE:
        printf("DoWhile (line %d)\n", n->line);
        indent(d + 1); printf("Body:\n"); ast_print(n->b, d + 2);
        indent(d + 1); printf("Cond:\n"); print_one(n->a, d + 2);
        break;
    case N_FOR:
        printf("For (line %d)\n", n->line);
        print_labeled("Init", n->a, d + 1);
        if (n->b) { indent(d + 1); printf("Cond:\n"); print_one(n->b, d + 2); }
        print_labeled("Step", n->c, d + 1);
        print_labeled("Body", n->d, d + 1);
        break;
    case N_FOREACH:
        printf("ForEach %s %s (line %d)\n", type_name(n->type), n->name, n->line);
        indent(d + 1); printf("In:\n"); print_one(n->a, d + 2);
        print_labeled("Body", n->b, d + 1);
        break;
    case N_SWITCH:
        printf("Switch (line %d)\n", n->line);
        indent(d + 1); printf("Expr:\n"); print_one(n->a, d + 2);
        print_labeled("Body", n->b, d + 1);
        break;
    case N_CASE:
        if (n->a) { printf("Case\n"); print_one(n->a, d + 1); } else printf("Default\n");
        break;
    case N_BREAK:    printf("Break%s%s (line %d)\n", n->name ? " " : "", n->name ? n->name : "", n->line); break;
    case N_CONTINUE: printf("Continue%s%s (line %d)\n", n->name ? " " : "", n->name ? n->name : "", n->line); break;
    case N_LABELED:  printf("Label %s (line %d)\n", n->name, n->line); ast_print(n->a, d + 1); break;
    case N_EXPRSTMT: printf("ExprStmt (line %d)\n", n->line); print_one(n->a, d + 1); break;
    case N_BINOP: printf("BinOp '%s'\n", n->op); print_one(n->a, d + 1); print_one(n->b, d + 1); break;
    case N_UNOP:  printf("UnOp '%s'\n", n->op);  print_one(n->a, d + 1); break;
    case N_INCDEC: printf("%s '%s'\n", n->flag ? "PreIncDec" : "PostIncDec", n->op); print_one(n->a, d + 1); break;
    case N_COND:
        printf("Ternary\n"); print_one(n->a, d + 1); print_one(n->b, d + 1); print_one(n->c, d + 1); break;
    case N_CAST:  printf("Cast %s\n", type_name(n->type)); print_one(n->a, d + 1); break;
    case N_CALL:
        printf("Call %s\n", n->name);
        if (n->a) print_labeled("Object", n->a, d + 1);
        if (n->b) print_labeled("Args", n->b, d + 1);
        break;
    case N_INDEX: printf("Index\n"); print_one(n->a, d + 1); print_one(n->b, d + 1); break;
    case N_FIELD: printf("Field %s\n", n->name); print_one(n->a, d + 1); break;
    case N_NEWARR: printf("NewArray %s\n", type_name_dims(n->type, n->dims)); if (n->a) ast_print(n->a, d + 1); break;
    case N_ARRLIT: printf("ArrayInit\n"); ast_print(n->a, d + 1); break;
    case N_NEWOBJ: printf("New %s\n", n->name ? n->name : "object"); break;
    case N_SIZEOF: printf("Sizeof\n"); if (n->a) print_one(n->a, d + 1); break;
    case N_FUNC:
        printf("Function %s %s (line %d)\n", type_name_dims(n->type, n->dims), n->name, n->line);
        print_labeled("Params", n->a, d + 1);
        print_labeled("Body", n->b, d + 1);
        break;
    case N_ID:    printf("Id %s\n", n->name); break;
    case N_INT:   printf("Int %lld\n", n->ival); break;
    case N_CHAR:  printf("Char %lld\n", n->ival); break;
    case N_FLOAT: printf("Float %g\n", n->fval); break;
    case N_BOOL:  printf("Bool %s\n", n->ival ? "true" : "false"); break;
    case N_STR:   printf("String \"%s\"\n", n->name); break;
    case N_RETURN: printf("Return (line %d)\n", n->line); if (n->a) print_one(n->a, d + 1); break;
    }
}

void ast_print(const Node *n, int depth) {
    for (; n; n = n->next) print_one(n, depth);
}
