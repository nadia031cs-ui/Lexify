/* Lexify driver: lexer -> parser -> AST -> semantic -> TAC (-> run)
 *
 * Usage: lexify <source> [--ast] [--symtab] [--run] [--lang c|cpp|java|mc]
 *
 * The language decides the type rules and how cout / System.out print values.
 * It is taken from --lang, else from the file extension (.c .cpp .cc .java),
 * and the source itself wins when it clearly is C++ (cout, #include <iostream>)
 * or Java (System.out, String, Scanner). A plain .mc file is the mini language. */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include "ast/ast.h"
#include "semantic/semantic.h"
#include "symbol_table/symbol_table.h"
#include "tac/tac.h"
#include "interp/interp.h"

extern FILE *yyin;
extern int yyparse(void);
extern int lexical_errors;
extern int syntax_errors;
extern int lex_saw_c, lex_saw_cpp, lex_saw_java;
extern Node *ast_root;

static int same(const char *a, const char *b) {     /* case-insensitive equality, portable */
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int lang_from_name(const char *s, Lang *out) {
    if (same(s, "c")) { *out = LANG_C; return 1; }
    if (same(s, "cpp") || same(s, "c++") || same(s, "cc") || same(s, "cxx")) { *out = LANG_CPP; return 1; }
    if (same(s, "java")) { *out = LANG_JAVA; return 1; }
    if (same(s, "mc") || same(s, "mini")) { *out = LANG_MC; return 1; }
    return 0;
}

int main(int argc, char **argv) {
    int show_ast = 0, show_symtab = 0, run = 0, have_lang = 0;
    Lang chosen = LANG_MC;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ast") == 0) show_ast = 1;
        else if (strcmp(argv[i], "--symtab") == 0) show_symtab = 1;
        else if (strcmp(argv[i], "--run") == 0) run = 1;
        else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            if (!lang_from_name(argv[++i], &chosen)) { fprintf(stderr, "Unknown language '%s' (use c, cpp, java or mc)\n", argv[i]); return 2; }
            have_lang = 1;
        }
        else if (strncmp(argv[i], "--lang=", 7) == 0) {
            if (!lang_from_name(argv[i] + 7, &chosen)) { fprintf(stderr, "Unknown language '%s' (use c, cpp, java or mc)\n", argv[i] + 7); return 2; }
            have_lang = 1;
        }
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "Usage: %s <source> [--ast] [--symtab] [--run] [--lang c|cpp|java|mc]\n", argv[0]);
        return 2;
    }
    if (!have_lang) {
        const char *dot = strrchr(path, '.');
        if (dot && lang_from_name(dot + 1, &chosen) && chosen != LANG_MC) have_lang = 1;
        else chosen = LANG_MC;
    }
    yyin = fopen(path, "r");
    if (!yyin) {
        perror(path);
        return 2;
    }
    yyparse();
    fclose(yyin);

    /* what the source looks like beats the page/extension it came from */
    if (lex_saw_java) lang_mode = LANG_JAVA;
    else if (lex_saw_cpp) lang_mode = LANG_CPP;
    else if (have_lang) lang_mode = chosen;
    else if (lex_saw_c) lang_mode = LANG_C;
    else lang_mode = LANG_MC;

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

    /* --run: generate the TAC, execute it and print the program's output (like an online compiler) */
    if (run) {
        char tmp_path[1024] = "";
        FILE *tmp = tmpfile();
        if (!tmp) {                 /* Windows: tmpfile() may need admin rights; use %TEMP% instead */
            const char *dir = getenv("TEMP");
            if (!dir) dir = getenv("TMP");
            if (!dir) dir = ".";
            snprintf(tmp_path, sizeof tmp_path, "%s/lexify_%ld_%d.tac", dir, (long)time(NULL), rand());
            tmp = fopen(tmp_path, "w+");
        }
        if (!tmp) { perror("temporary file"); return 2; }
        tac_generate(ast_root, tmp);
        rewind(tmp);
        int rc = interp_run(tmp, (int)lang_mode);
        fclose(tmp);
        if (tmp_path[0]) remove(tmp_path);
        return rc ? 3 : 0;
    }

    /* Final phase: Three Address Code goes to stdout (redirect with > file.tac) */
    tac_generate(ast_root, stdout);
    fprintf(stderr, "Compilation successful (%s): TAC generated.\n", lang_name(lang_mode));
    return 0;
}
