# Fluxa — Makefile (Sprint 9.b)
#
# Targets principais:
#   make build              → binário nativo (Linux/macOS)
#   make build-rp2040       → cross-compile para Raspberry Pi Pico (RP2040)
#   make build-esp32        → cross-compile para ESP32 (Xtensa lx6)
#   make build-embedded     → alias: rp2040 + esp32
#   make test-runner        → suite PASS/FAIL automatizada
#   make bench              → benchmarks de performance
#   make examples           → roda todos os exemplos
#   make clean              → remove binários

# ── Compilador nativo ─────────────────────────────────────────────────────────
CC      = gcc

# libffi: portável via pkg-config (Linux x86/ARM, macOS)
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
LDFLAGS = $(FFI_LDFLAGS) -ldl -lm -lpthread

SRCS    = src/main.c \
          src/lexer.c \
          src/parser.c \
          src/scope.c \
          src/resolver.c \
          src/bytecode.c \
          src/builtins.c \
          src/block.c \
          src/ffi.c \
          src/runtime.c \
          src/handover.c \
          src/ipc_server.c

TARGET  = fluxa

# ── Flags comuns para targets embedded ───────────────────────────────────────
#
# FLUXA_IPC_NONE     → desativa todo o IPC unix socket (ipc_server.c não compila)
# FLUXA_HAS_FFI=0    → desativa libffi (não existe em bare-metal)
# FLUXA_EMBEDDED=1   → flag genérica para guards futuros no código
#
# O que é excluído no embedded (via preprocessador):
#   - ipc_discover_pid, ipc_server_bind, ipc_client_connect (unix socket)
#   - glob(), pthread (não existem em bare-metal)
#   - fluxa observe / set / logs / status (CLI IPC commands)
#   - Handover via memória paralela → usa HANDOVER_MODE_FLASH
#
EMBEDDED_CFLAGS = -std=c99 -Wall -Wextra -O2 \
                  -Isrc -Ivendor \
                  -DFLUXA_IPC_NONE=1 \
                  -DFLUXA_HAS_FFI=0 \
                  -DFLUXA_EMBEDDED=1

# Fontes embedded: sem ipc_server.c (unix socket), sem ffi.c (libffi)
SRCS_EMBEDDED = src/lexer.c \
                src/parser.c \
                src/scope.c \
                src/resolver.c \
                src/bytecode.c \
                src/builtins.c \
                src/block.c \
                src/runtime.c \
                src/handover.c

# ── RP2040 / Raspberry Pi Pico ────────────────────────────────────────────────
#
# Toolchain: arm-none-eabi-gcc (parte do pico-sdk ou standalone)
# Instalar: sudo apt install gcc-arm-none-eabi  (Ubuntu/Debian)
#           brew install --cask gcc-arm-embedded  (macOS)
#
# O binário gerado (.elf) deve ser convertido para .uf2 via elf2uf2
# ou integrado ao pico-sdk CMake build system.
#
# Flags específicas do RP2040 (Cortex-M0+):
#   -mcpu=cortex-m0plus   → core do RP2040
#   -mthumb               → conjunto de instruções Thumb-2
#   -mfloat-abi=soft      → sem FPU hardware no M0+
#   -nostdlib             → sem libc padrão (fornecer sua própria)
#   --specs=nosys.specs   → stubs de syscall (sem OS)
#
# NOTA: para integração completa com pico-sdk, use CMakeLists.txt.
# Este target gera um .o linkável para verificar que o código compila
# para o target — o link final é feito pelo pico-sdk.
#
CC_RP2040    = arm-none-eabi-gcc
TARGET_RP2040 = fluxa-rp2040

RP2040_CFLAGS = $(EMBEDDED_CFLAGS) \
                -mcpu=cortex-m0plus \
                -mthumb \
                -mfloat-abi=soft \
                -DFLUXA_TARGET_RP2040=1

# ── ESP32 (Xtensa lx6) ────────────────────────────────────────────────────────
#
# Toolchain: xtensa-esp32-elf-gcc (parte do esp-idf)
# Instalar: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
#           ou: pip install espidf
#
# ESP32 tem FPU — usa hard float.
# Para ESP32-S2/S3 (RISC-V): usar riscv32-esp-elf-gcc com -march=rv32imc
#
CC_ESP32    = xtensa-esp32-elf-gcc
TARGET_ESP32 = fluxa-esp32

ESP32_CFLAGS = $(EMBEDDED_CFLAGS) \
               -mlongcalls \
               -DFLUXA_TARGET_ESP32=1

# ── ARM Cortex-M genérico (STM32, nRF52, etc.) ───────────────────────────────
#
# Mesmo toolchain do RP2040 mas flags de CPU configuráveis.
# Exemplos:
#   STM32F4 (Cortex-M4 com FPU):  -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
#   nRF52840 (Cortex-M4F):        -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
#   STM32G0 (Cortex-M0+):         -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft
#
CC_CORTEXM    = arm-none-eabi-gcc
TARGET_CORTEXM = fluxa-cortex-m

CORTEXM_CFLAGS = $(EMBEDDED_CFLAGS) \
                 -mcpu=cortex-m4 \
                 -mthumb \
                 -mfpu=fpv4-sp-d16 \
                 -mfloat-abi=hard \
                 -DFLUXA_TARGET_CORTEX_M=1

# ═════════════════════════════════════════════════════════════════════════════
.PHONY: all build build-rp2040 build-esp32 build-cortex-m build-embedded \
        test test-sprint5 test-sprint8 test-sprint9 test-sprint9b \
        test-runner bench examples clean \
        test-integration test-integration-s1 test-integration-s2 test-all \
        check-toolchain-rp2040 check-toolchain-esp32

all: build

# ── Build nativo ──────────────────────────────────────────────────────────────
build:
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@echo "✓ build ok → ./$(TARGET)"

# ── Build RP2040 ──────────────────────────────────────────────────────────────
build-rp2040: check-toolchain-rp2040
	@echo "── cross-compile: RP2040 (Cortex-M0+) ──────────────────────────────"
	$(CC_RP2040) $(RP2040_CFLAGS) -c $(SRCS_EMBEDDED)
	@echo "✓ rp2040 objects ok (link via pico-sdk CMake)"
	@echo "  dica: copie os .o para seu projeto pico-sdk e adicione ao CMakeLists.txt"

check-toolchain-rp2040:
	@which $(CC_RP2040) > /dev/null 2>&1 || \
	  (echo "✗ $(CC_RP2040) não encontrado." && \
	   echo "  Ubuntu/Debian: sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi" && \
	   echo "  macOS:         brew install --cask gcc-arm-embedded" && \
	   exit 1)

# ── Build ESP32 ───────────────────────────────────────────────────────────────
build-esp32: check-toolchain-esp32
	@echo "── cross-compile: ESP32 (Xtensa lx6) ──────────────────────────────"
	$(CC_ESP32) $(ESP32_CFLAGS) -c $(SRCS_EMBEDDED)
	@echo "✓ esp32 objects ok (link via esp-idf CMake)"
	@echo "  dica: adicione os .o ao seu componente esp-idf"

check-toolchain-esp32:
	@which $(CC_ESP32) > /dev/null 2>&1 || \
	  (echo "✗ $(CC_ESP32) não encontrado." && \
	   echo "  Instale o esp-idf: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/" && \
	   echo "  ou: pip install espidf" && \
	   exit 1)

# ── Build Cortex-M genérico ───────────────────────────────────────────────────
# Personalize CORTEXM_CFLAGS acima para seu chip específico.
build-cortex-m: check-toolchain-rp2040
	@echo "── cross-compile: ARM Cortex-M genérico ────────────────────────────"
	$(CC_CORTEXM) $(CORTEXM_CFLAGS) -c $(SRCS_EMBEDDED)
	@echo "✓ cortex-m objects ok"

# ── Build todos os embedded ───────────────────────────────────────────────────
# Tenta cada toolchain; continua mesmo se algum não estiver instalado.
build-embedded:
	@echo "── build embedded targets ──────────────────────────────────────────"
	@$(MAKE) build-rp2040  2>/dev/null && echo "  RP2040:   ✓" || echo "  RP2040:   ✗ (toolchain não instalado)"
	@$(MAKE) build-esp32   2>/dev/null && echo "  ESP32:    ✓" || echo "  ESP32:    ✗ (toolchain não instalado)"
	@echo "── embedded build concluído ────────────────────────────────────────"

# ── Testes ───────────────────────────────────────────────────────────────────
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
	@echo "── Sprint 6.c runtime test ─────────────────────"
	@./$(TARGET) run tests/sprint6c_runtime.flx
	@echo "── Sprint 7.a test ─────────────────────────────"
	@./$(TARGET) run tests/sprint7a.flx
	@echo "── Sprint 7.a collision test ───────────────────"
	@./$(TARGET) run tests/sprint7a_collision.flx
	@echo "── Sprint 7.a explain test ─────────────────────"
	@./$(TARGET) explain tests/sprint7a.flx > /dev/null
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
	@echo "── Sprint 7.b test ─────────────────────────────"
	@./$(TARGET) run tests/sprint7b.flx
	@echo "── all tests passed ───────────────────────────"

# Automated PASS/FAIL runner — shows diff on failures, suitable for CI
test-runner: build
	@./tests/run_tests.sh ./$(TARGET)

test-sprint5: build
	@echo "── Sprint 5 only ──────────────────────────────"
	@./$(TARGET) run tests/sprint5.flx
	@./$(TARGET) run tests/block_isolation.flx
	@./$(TARGET) run tests/block_root.flx
	@./$(TARGET) run tests/block_methods.flx
	@echo "── typeof instance error (expect error) ───────"
	@./$(TARGET) run tests/block_no_instance_typeof.flx || true

test-sprint8: build
	@echo "── Sprint 8: Handover Atômico ─────────────────────────────────────────"
	@echo "── handover básico ─────────────────────────────────────────────────────"
	@./$(TARGET) run tests/sprint8_handover_basic.flx
	@echo "── handover dry run falha (espera erro) ────────────────────────────────"
	@./$(TARGET) run tests/sprint8_handover_dry_run_fail.flx
	@echo "── handover prst preservado ────────────────────────────────────────────"
	@./$(TARGET) run tests/sprint8_handover_prst.flx
	@echo "── handover versão de protocolo ────────────────────────────────────────"
	@./$(TARGET) run tests/sprint8_handover_version.flx
	@echo "── linha nos erros ─────────────────────────────────────────────────────"
	@./$(TARGET) run tests/sprint8_error_line.flx || true
	@echo "── all sprint8 tests OK ────────────────────────────────────────────────"

test-sprint9: build
	@echo "── Sprint 9: CLI & IPC ────────────────────────────────────────────────────"
	@chmod +x tests/sprint9_cli.sh tests/sprint9_ipc.sh
	@bash tests/sprint9_cli.sh --fluxa ./$(TARGET)
	@bash tests/sprint9_ipc.sh --fluxa ./$(TARGET)
	@echo "── sprint 9 tests OK ──────────────────────────────────────────────────────"

test-sprint9b: build
	@echo "── Sprint 9.b: Issue #95 + #96 ────────────────────────────────────────────"
	@chmod +x tests/sprint9b_set_in_loop.sh tests/sprint9b_explain_live.sh
	@bash tests/sprint9b_set_in_loop.sh --fluxa ./$(TARGET)
	@bash tests/sprint9b_explain_live.sh --fluxa ./$(TARGET)
	@echo "── sprint 9.b tests OK ────────────────────────────────────────────────────"

bench: build
	@echo "── bench (global while) ───────────────────────"
	@bash -c "time ./$(TARGET) run tests/bench.flx"
	@echo "── bench_block (while inside method) ──────────"
	@bash -c "time ./$(TARGET) run tests/bench_block.flx"

examples: build
	@echo "── sort ────────────────────────────────────────"
	@./$(TARGET) run examples/sort.flx
	@echo "── stack ───────────────────────────────────────"
	@./$(TARGET) run examples/data_structures/stack.flx
	@echo "── queue ───────────────────────────────────────"
	@./$(TARGET) run examples/data_structures/queue.flx
	@echo "── linked_list ─────────────────────────────────"
	@./$(TARGET) run examples/data_structures/linked_list.flx
	@echo "── bst ─────────────────────────────────────────"
	@./$(TARGET) run examples/data_structures/bst.flx
	@echo "── tree ────────────────────────────────────────"
	@./$(TARGET) run examples/data_structures/tree.flx
	@echo "── hash_map ────────────────────────────────────"
	@./$(TARGET) run examples/data_structures/hash_map.flx
	@echo "── dynamic_programming ─────────────────────────"
	@./$(TARGET) run examples/data_structures/dynamic_programming.flx
	@echo "── 01_fizzbuzz ─────────────────────────────────"
	@./$(TARGET) run examples/problems/01_fizzbuzz.flx
	@echo "── 02_binary_search ────────────────────────────"
	@./$(TARGET) run examples/problems/02_binary_search.flx
	@echo "── 03_two_sum ──────────────────────────────────"
	@./$(TARGET) run examples/problems/03_two_sum.flx
	@echo "── 04_percolation ──────────────────────────────"
	@./$(TARGET) run examples/problems/04_percolation.flx
	@echo "── 05_sliding_window ───────────────────────────"
	@./$(TARGET) run examples/problems/05_sliding_window.flx
	@echo "── 06_maze_bfs ─────────────────────────────────────"
	@./$(TARGET) run examples/problems/06_maze_bfs.flx
	@echo "── 07_dijkstra ─────────────────────────────────────"
	@./$(TARGET) run examples/problems/07_dijkstra.flx
	@echo "── 08_pagerank ─────────────────────────────────────"
	@./$(TARGET) run examples/problems/08_pagerank.flx
	@echo "── all examples ok ─────────────────────────────"

# Integration tests — Cenários de simulação do Handover Atômico
# Requerem bash e python3. Não dependem de Docker para rodar localmente.
#
#   make test-integration      → ambos os cenários
#   make test-integration-s1   → Cenário 1 apenas (IoT Simples)
#   make test-integration-s2   → Cenário 2 apenas (Fault Injection)
#   make test-all              → unit tests (runner) + integration tests
#
test-integration: build
	@chmod +x tests/integration/run_all.sh \
	           tests/integration/scenario1/run.sh \
	           tests/integration/scenario2/run.sh
	@./tests/integration/run_all.sh --fluxa ./$(TARGET)

test-integration-s1: build
	@chmod +x tests/integration/scenario1/run.sh
	@./tests/integration/run_all.sh --fluxa ./$(TARGET) --scenario 1

test-integration-s2: build
	@chmod +x tests/integration/scenario2/run.sh
	@./tests/integration/run_all.sh --fluxa ./$(TARGET) --scenario 2

# Roda tudo: unit tests automatizados + integration tests
test-all: build
	@./tests/run_tests.sh ./$(TARGET)
	@./tests/integration/run_all.sh --fluxa ./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET_RP2040) $(TARGET_ESP32) $(TARGET_CORTEXM)
	rm -f $(TARGET)_asan $(TARGET)_debug
	rm -f *.o

# build-asan: debug build com AddressSanitizer — para investigação de crashes
# Não incluir no pacote de distribuição (binário 3-4x maior)
build-asan:
	$(CC) -std=c99 -Wall -Wextra -g -fsanitize=address,undefined \
	  -Isrc -Ivendor -DFLUXA_HAS_FFI=1 \
	  $(SRCS) -o $(TARGET)_asan \
	  $(FFI_LDFLAGS) -ldl -lm -lpthread
	@echo "✓ asan build ok → ./$(TARGET)_asan  (não distribuir)"

