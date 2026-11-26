@echo off
cd /d "%~dp0"
if exist lexify.exe del lexify.exe
gcc -Isrc -Ibuild build/lex.yy.c build/parser.tab.c src/main.c src/ast/ast.c src/symbol_table/symbol_table.c src/semantic/semantic.c src/tac/tac.c src/interp/interp.c -o lexify.exe -lm
if not exist lexify.exe (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK
lexify.exe tests\valid\c_style.mc --run
