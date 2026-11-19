/* Lexify driver: lexer -> parser -> AST -> semantic -> TAC */
#include <stdio.h>
#include <string.h>
#include "ast/ast.h"
#include "semantic/semantic.h"
#include "symbol_table/symbol_table.h"
#include "tac/tac.h"
#include "interp/interp.h"

extern FILE *yyin;
extern int yyparse(void);
extern int lexical_errors;
extern int syntax_errors;
extern Node *ast_root;

int main(int argc, char **argv) {
    int show_ast = 0, show_symtab = 0, run = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ast") == 0) show_ast = 1;
        else if (strcmp(argv[i], "--symtab") == 0) show_symtab = 1;
        else if (strcmp(argv[i], "--run") == 0) run = 1;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "Usage: %s <source.mc> [--ast] [--symtab] [--run]\n", argv[0]);
        return 2;
    }
    yyin = fopen(path, "r");
    if (!yyin) {
        perror(path);
        return 2;
    }
    yyparse();
    fclose(yyin);

    if (lexical_errors || syntax_errors) {
        fprintf(stderr, "Compilation failed: %d lexical error(s), %d syntax error(s)\n",
                lexical_errors, syntax_errors);
        return 1;
    }
    if (show_ast) ast_print(ast_root, 0);

    int sem_errors = semantic_check(ast_root);
    if (sem_errors) {
        fprintf(stderr, "Compilation failed: %d semantic error(s)\n", sem_errors);
        return 1;
    }
    if (show_symtab) st_dump();

    /* --run: generate the TAC, execute it and print the program's output (like an online C compiler) */
    if (run) {
        FILE *tmp = tmpfile();
        if (!tmp) { perror("tmpfile"); return 2; }
        tac_generate(ast_root, tmp);
        rewind(tmp);
        int rc = interp_run(tmp);
        fclose(tmp);
        return rc ? 3 : 0;
    }

    /* Final phase: Three Address Code goes to stdout (redirect with > file.tac) */
    tac_generate(ast_root, stdout);
    fprintf(stderr, "Compilation successful: TAC generated.\n");
    return 0;
}
