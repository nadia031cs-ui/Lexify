# Lexify - single-command build (Section 8 of the manual)
CC      = gcc
CFLAGS  = -Wall -Wextra -g -Isrc -Ibuild
BUILD   = build
TARGET  = lexify

all: $(TARGET)

SRCS    = src/main.c src/ast/ast.c src/symbol_table/symbol_table.c src/semantic/semantic.c src/tac/tac.c src/interp/interp.c

$(TARGET): $(BUILD)/lex.yy.c $(BUILD)/parser.tab.c $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/parser.tab.c $(BUILD)/parser.tab.h: src/parser/parser.y | $(BUILD)
	bison -d -o $(BUILD)/parser.tab.c $<

$(BUILD)/lex.yy.c: src/lexer/lexer.l $(BUILD)/parser.tab.h | $(BUILD)
	flex -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

test: $(TARGET)
	sh tests/run_tests.sh

clean:
	rm -rf $(BUILD) $(TARGET) $(TARGET).exe

.PHONY: all test clean
