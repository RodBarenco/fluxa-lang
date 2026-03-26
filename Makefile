# Fluxa — Makefile (Sprint 5)
CC      = gcc
# libffi: portable via pkg-config (Linux x86/ARM, macOS)
HAVE_FFI := $(shell pkg-config --exists libffi 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAVE_FFI),1)
    FFI_CFLAGS  := $(shell pkg-config --cflags libffi) -DFLUXA_HAS_FFI=1
    FFI_LDFLAGS := $(shell pkg-config --libs libffi)
else
    FFI_CFLAGS  := -DFLUXA_HAS_FFI=0
    FFI_LDFLAGS :=
endif

CFLAGS  = -std=c99 -Wall -Wextra -pedantic -O2 \
           -Isrc -Ivendor $(FFI_CFLAGS)
LDFLAGS = $(FFI_LDFLAGS) -ldl -lm
SRCS    = src/main.c \
          src/lexer.c \
          src/parser.c \
          src/scope.c \
          src/resolver.c \
          src/bytecode.c \
          src/builtins.c \
          src/block.c \
          src/ffi.c \
          src/runtime.c
TARGET  = fluxa

.PHONY: all build test test-sprint5 bench clean

all: build

build:
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
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
	@echo "── Sprint 6 test ──────────────────────────────"
	@./$(TARGET) run tests/sprint6.flx
	@echo "── danger isolation test ──────────────────────"
	@./$(TARGET) run tests/danger_basic.flx
	@echo "── danger err stack test ──────────────────────"
	@./$(TARGET) run tests/danger_err_stack.flx
	@echo "── danger clean test ──────────────────────────"
	@./$(TARGET) run tests/danger_clean.flx
	@echo "── danger after test ──────────────────────────"
	@./$(TARGET) run tests/danger_after.flx
	@echo "── arr heap test ──────────────────────────────"
	@./$(TARGET) run tests/arr_heap.flx
	@echo "── Sprint 6.b test ─────────────────────────────"
	@./$(TARGET) run tests/sprint6b.flx
	@echo "── arr default test ───────────────────────────"
	@./$(TARGET) run tests/arr_default.flx
	@echo "── arr default types test ─────────────────────"
	@./$(TARGET) run tests/arr_default_types.flx
	@echo "── ffi libm test ──────────────────────────────"
	@./$(TARGET) run tests/ffi_libm.flx
	@echo "── ffi void test ──────────────────────────────"
	@./$(TARGET) run tests/ffi_void.flx
	@echo "── ffi portability test ───────────────────────"
	@./$(TARGET) run tests/ffi_portability.flx
	@echo "── err nil check test ─────────────────────────"
	@./$(TARGET) run tests/err_nil_check.flx
	@echo "── arr param test ─────────────────────────────"
	@./$(TARGET) run tests/arr_param.flx
	@echo "── arr block field test ───────────────────────"
	@./$(TARGET) run tests/arr_block_field.flx
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
