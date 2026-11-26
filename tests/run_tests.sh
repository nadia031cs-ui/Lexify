#!/bin/sh
# Runs the test suite against ./lexify.
#  valid/            must exit 0; if X.tac sits next to X.mc, stdout must match it exactly
#  lexical_errors/   must exit 1 and print "Lexical error"
#  syntax_errors/    must exit 1 and print "Syntax error"
#  semantic_errors/  must exit 1 and print "Semantic error" (enabled by tests/.semantic_ready)
#  programs/         C, C++ and Java programs: `--run` output must equal the recorded .out
cd "$(dirname "$0")/.." || exit 2
BIN=./lexify
pass=0; fail=0; skip=0

check() {  # dir  expected_exit  expected_text
    for f in tests/$1/*.mc; do
        out=$($BIN "$f" 2>&1); code=$?
        if [ "$code" -eq "$2" ] && { [ -z "$3" ] || printf "%s\n" "$out" | grep -q "$3"; }; then
            echo "PASS  $f"; pass=$((pass+1))
        else
            echo "FAIL  $f"; printf "%s\n" "$out" | sed 's/^/      /'; fail=$((fail+1))
        fi
    done
}

# valid programs: exit 0, and TAC (stdout) must equal the expected .tac file
for f in tests/valid/*.mc; do
    exp="${f%.mc}.tac"
    out=$($BIN "$f" 2>/dev/null); code=$?
    if [ "$code" -ne 0 ]; then
        echo "FAIL  $f (exit $code)"; $BIN "$f" 2>&1 | sed 's/^/      /'; fail=$((fail+1))
    elif [ -f "$exp" ] && [ "$out" != "$(cat "$exp")" ]; then
        echo "FAIL  $f (TAC differs from $exp)"
        printf "%s\n" "$out" | diff - "$exp" | sed 's/^/      /'; fail=$((fail+1))
    else
        echo "PASS  $f"; pass=$((pass+1))
    fi
done
check lexical_errors 1 "Lexical error"
check syntax_errors 1 "Syntax error"
for f in tests/semantic_errors/*.mc; do
    if [ -f tests/.semantic_ready ]; then
        out=$($BIN "$f" 2>&1); code=$?
        if [ "$code" -eq 1 ] && printf "%s\n" "$out" | grep -q "Semantic error"; then
            echo "PASS  $f"; pass=$((pass+1))
        else echo "FAIL  $f"; fail=$((fail+1)); fi
    else
        echo "SKIP  $f (semantic phase not implemented; create tests/.semantic_ready to enable)"; skip=$((skip+1))
    fi
done

# real C / C++ / Java programs: `lexify X --run` (stdin from X.in) must print exactly X.out,
# which was recorded from gcc / g++ / javac+java
for f in tests/programs/c/*.c tests/programs/cpp/*.cpp tests/programs/java/*.java; do
    [ -f "$f" ] || continue
    base="${f%.*}"
    inp="$base.in"; [ -f "$inp" ] || inp=/dev/null
    $BIN "$f" --run < "$inp" > /tmp/lexify_test_out.$$ 2> /tmp/lexify_test_err.$$; code=$?
    if [ "$code" -eq 0 ] && cmp -s /tmp/lexify_test_out.$$ "$base.out"; then
        echo "PASS  $f"; pass=$((pass+1))
    else
        echo "FAIL  $f (exit $code)"; sed 's/^/      /' /tmp/lexify_test_err.$$
        diff /tmp/lexify_test_out.$$ "$base.out" | head -20 | sed 's/^/      /'; fail=$((fail+1))
    fi
done
rm -f /tmp/lexify_test_out.$$ /tmp/lexify_test_err.$$
echo; echo "passed=$pass failed=$fail skipped=$skip"
[ "$fail" -eq 0 ]
