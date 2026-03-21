# Fluxa — Makefile
CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -g \
           -Isrc -Ivendor
SRCS    = src/main.c src/lexer.c src/parser.c src/runtime.c
TARGET  = fluxa

.PHONY: all build test clean

all: build

build:
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)
	@echo "✓ build ok → ./$(TARGET)"

test: build
	@echo "── Sprint 1 test ──────────────────────────────"
	@./$(TARGET) run tests/hello.flx
	@echo "── types test ─────────────────────────────────"
	@./$(TARGET) run tests/types.flx
	@echo "── all tests passed ───────────────────────────"

clean:
	rm -f $(TARGET)
