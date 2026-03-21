# Fluxa — Makefile
CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -g \
           -Isrc -Ivendor

SRCS    = src/lexer.c src/parser.c src/runtime.c

TARGET  = fluxa

.PHONY: all build test clean

all: build

build:
	@echo "✓ Compilando os módulos disponíveis..."
	$(CC) $(CFLAGS) -c $(SRCS)
	@echo "✓ Lexer compilado com sucesso (.o). (Executável aguardando main.c)"

test: build
	@echo "── Testes serão ativados na conclusão da Sprint 1 ──"
#	@echo "── Sprint 1 test ──────────────────────────────"
#	@./$(TARGET) run tests/hello.flx
#	@echo "── types test ─────────────────────────────────"
#	@./$(TARGET) run tests/types.flx
#	@echo "── all tests passed ───────────────────────────"

clean:
	rm -f *.o $(TARGET)