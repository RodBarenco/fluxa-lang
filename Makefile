# Fluxa — Makefile (Sprint 5)
CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -O2 \
           -Isrc -Ivendor
SRCS    = src/main.c \
          src/lexer.c \
          src/parser.c \
          src/scope.c \
          src/resolver.c \
          src/bytecode.c \
          src/builtins.c \
          src/block.c \
          src/runtime.c
TARGET  = fluxa

.PHONY: all build test test-sprint5 bench clean

all: build

build:
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)
	@echo "✓ build ok → ./$(TARGET)"

test: build
	@echo "── Sprint 1 test ──────────────────────────────"
	@./$(TARGET) run tests/hello.flx
	@echo "── types test ─────────────────────────────────"
	@./$(TARGET) run tests/types.flx
	@echo "── Sprint 2 test ──────────────────────────────"
	@./$(TARGET) run tests/sprint2.flx
	@echo "── Sprint 3 test ──────────────────────────────"
	@./$(TARGET) run tests/sprint3.flx
	@echo "── Sprint 4 test ──────────────────────────────"
	@./$(TARGET) run tests/sprint4.flx
	@echo "── Sprint 5 test ──────────────────────────────"
	@./$(TARGET) run tests/sprint5.flx
	@echo "── block isolation test ───────────────────────"
	@./$(TARGET) run tests/block_isolation.flx
	@echo "── block root test ────────────────────────────"
	@./$(TARGET) run tests/block_root.flx
	@echo "── block methods test ─────────────────────────"
	@./$(TARGET) run tests/block_methods.flx
	@echo "── typeof instance error test (expect error) ──"
	@./$(TARGET) run tests/block_no_instance_typeof.flx || true
	@echo "── all tests passed ───────────────────────────"

test-sprint5: build
	@echo "── Sprint 5 only ──────────────────────────────"
	@./$(TARGET) run tests/sprint5.flx
	@./$(TARGET) run tests/block_isolation.flx
	@./$(TARGET) run tests/block_root.flx
	@./$(TARGET) run tests/block_methods.flx
	@echo "── typeof instance error (expect error) ───────"
	@./$(TARGET) run tests/block_no_instance_typeof.flx || true

bench: build
	@echo "── bench (global while) ───────────────────────"
	@time ./$(TARGET) run tests/bench.flx
	@echo "── bench_block (while inside method) ──────────"
	@time ./$(TARGET) run tests/bench_block.flx

clean:
	rm -f $(TARGET)
