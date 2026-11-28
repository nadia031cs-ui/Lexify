@echo off
rem Lexify - Windows build. Regenerates the parser and lexer every time,
rem so every laptop builds from the CURRENT src (never from an old build folder).
rem Needs: gcc (MinGW), and Bison 3.x + Flex  (winflexbison: win_bison / win_flex, or bison / flex).
setlocal
cd /d "%~dp0"

set "BISON="
where win_bison >nul 2>nul && set "BISON=win_bison"
if not defined BISON (where bison >nul 2>nul && set "BISON=bison")
set "FLEX="
where win_flex >nul 2>nul && set "FLEX=win_flex"
if not defined FLEX (where flex >nul 2>nul && set "FLEX=flex")
if not defined BISON goto nobison
if not defined FLEX goto noflex
where gcc >nul 2>nul || goto nogcc

if not exist build mkdir build
if exist build\parser.tab.c del build\parser.tab.c
if exist build\parser.tab.h del build\parser.tab.h
if exist build\lex.yy.c del build\lex.yy.c
if exist lexify.exe del lexify.exe

echo [1/3] %BISON%  src\parser\parser.y
%BISON% -d -o build\parser.tab.c src\parser\parser.y
if errorlevel 1 goto bisonfail

echo [2/3] %FLEX%  src\lexer\lexer.l
%FLEX% -o build\lex.yy.c src\lexer\lexer.l
if errorlevel 1 goto flexfail

echo [3/3] gcc
gcc -Isrc -Ibuild build\lex.yy.c build\parser.tab.c src\main.c src\ast\ast.c src\symbol_table\symbol_table.c src\semantic\semantic.c src\tac\tac.c src\interp\interp.c -o lexify.exe -lm
if not exist lexify.exe goto gccfail

echo.
echo BUILD OK  (lexify.exe)
lexify.exe tests\valid\c_style.mc --run
exit /b 0

:nobison
echo BUILD FAILED: bison not found. Install winflexbison (gives win_bison / win_flex) and add it to PATH.
exit /b 1
:noflex
echo BUILD FAILED: flex not found. Install winflexbison (gives win_bison / win_flex) and add it to PATH.
exit /b 1
:nogcc
echo BUILD FAILED: gcc not found. Install MinGW-w64 and add its bin folder to PATH.
exit /b 1
:bisonfail
echo BUILD FAILED at bison. parser.y needs Bison 3.x  (check:  %BISON% --version).
echo Old GnuWin32 Bison 2.4 cannot build it: install winflexbison and use win_bison.
exit /b 1
:flexfail
echo BUILD FAILED at flex.
exit /b 1
:gccfail
echo BUILD FAILED at gcc (see the errors above).
exit /b 1
