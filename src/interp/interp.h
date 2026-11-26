/* Lexify TAC interpreter: executes the generated Three Address Code and prints the program's output */
#ifndef LEXIFY_INTERP_H
#define LEXIFY_INTERP_H
#include <stdio.h>

/* Reads TAC text from 'tac', runs it, program output goes to stdout, input comes from stdin.
 * 'language' is a Lang value (ast.h): it decides how cout / System.out print values.
 * Returns 0 on success, 1 on a runtime error (message already printed to stderr). */
int interp_run(FILE *tac, int language);

#endif
