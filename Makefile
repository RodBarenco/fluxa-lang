# ─────────────────────────────────────────────────────────────────────────────
# Fluxa — Build System
# v0.10 | C99 | targets: native Linux/macOS, RP2040, ESP32, Cortex-M
# ─────────────────────────────────────────────────────────────────────────────
#
# QUICK START
#   make                   Build native binary → ./fluxa
#   make test-runner       Run full test suite (PASS/FAIL report)
#   make test-suite2       Run Suite 2 — edge cases & integration (70 cases)
#   make test-all          Run everything: unit + suite2 + integration
#   make bench             Performance benchmarks
#   make examples          Run all example programs
#   make clean             Remove all build artifacts
#
# EMBEDDED TARGETS (require cross-compilers — see notes in each section)
#   make build-rp2040      Cross-compile for Raspberry Pi Pico (RP2040 / Cortex-M0+)
#   make build-esp32       Cross-compile for ESP32 (Xtensa lx6)
#   make build-cortex-m    Cross-compile for generic ARM Cortex-M (configurable)
#   make build-embedded    Attempt all embedded targets (skips missing toolchains)
#
# DEVELOPMENT
#   make build-asan        Build with AddressSanitizer — for crash investigation
#
# ─────────────────────────────────────────────────────────────────────────────


# ── Native compiler ───────────────────────────────────────────────────────────

CC = gcc

# libffi: detected via pkg-config (Linux x86/ARM64, macOS with Homebrew).
# If pkg-config or libffi is unavailable, FFI support is disabled and the
# build falls back to a pure-C runtime without C interop (no import c / danger).
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

# All source files for the native build (includes IPC server and FFI)
SRCS = src/main.c       \
       src/lexer.c      \
       src/parser.c     \
       src/scope.c      \
       src/resolver.c   \
       src/bytecode.c   \
       src/builtins.c   \
       src/block.c      \
       src/ffi.c        \
       src/runtime.c    \
       src/handover.c   \
       src/ipc_server.c

TARGET = fluxa


# ── Embedded build flags ──────────────────────────────────────────────────────
#
# These flags are shared across all embedded targets (RP2040, ESP32, Cortex-M).
# They strip out subsystems that have no equivalent on bare-metal hardware:
#
#   FLUXA_IPC_NONE=1   Disables the unix-socket IPC server entirely.
#                      Removes: ipc_server_bind, ipc_client_connect,
#                               observe/set/logs/status CLI commands.
#
#   FLUXA_HAS_FFI=0    Disables libffi. No dynamic C interop on bare-metal.
#
#   FLUXA_EMBEDDED=1   Generic guard for any future embedded-only code paths.
#
# What is excluded on embedded (via preprocessor guards):
#   - Unix socket IPC          (no OS filesystem or sockets)
#   - pthread                  (no POSIX threads on bare-metal)
#   - glob()                   (no filesystem on bare-metal)
#   - fluxa observe/set/logs/status CLI commands
#   - HANDOVER_MODE_MEMORY     (replaced by HANDOVER_MODE_FLASH on RP2040 —
#                               two runtimes in parallel don't fit in 264 KB SRAM)
#
EMBEDDED_CFLAGS = -std=c99 -Wall -Wextra -O2 \
                  -Isrc -Ivendor               \
                  -DFLUXA_IPC_NONE=1           \
                  -DFLUXA_HAS_FFI=0            \
                  -DFLUXA_EMBEDDED=1

# Embedded source list: no ipc_server.c (unix socket), no ffi.c (libffi)
SRCS_EMBEDDED = src/lexer.c    \
                src/parser.c   \
                src/scope.c    \
                src/resolver.c \
                src/bytecode.c \
                src/builtins.c \
                src/block.c    \
                src/runtime.c  \
                src/handover.c


# ── RP2040 / Raspberry Pi Pico ────────────────────────────────────────────────
#
# Toolchain: arm-none-eabi-gcc
#   Ubuntu/Debian:  sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi
#   macOS:          brew install --cask gcc-arm-embedded
#   Arch Linux:     sudo pacman -S arm-none-eabi-gcc
#
# The RP2040 uses an ARM Cortex-M0+ core:
#   -mcpu=cortex-m0plus   Target the M0+ core
#   -mthumb               Use the Thumb-2 compact instruction set
#   -mfloat-abi=soft      No hardware FPU on Cortex-M0+; software float only
#
# This target compiles source files to .o object files only.
# The final link is handled by the pico-sdk CMake build system:
#   1. Copy the generated .o files into your pico-sdk project
#   2. Add them to CMakeLists.txt via target_link_libraries()
#   3. Run cmake + make to produce .elf → convert to .uf2 via elf2uf2
#   4. Drag the .uf2 onto the RP2040 in BOOTSEL mode to flash it
#
CC_RP2040     = arm-none-eabi-gcc
TARGET_RP2040 = fluxa-rp2040

RP2040_CFLAGS = $(EMBEDDED_CFLAGS) \
                -mcpu=cortex-m0plus \
                -mthumb             \
                -mfloat-abi=soft    \
                -DFLUXA_TARGET_RP2040=1


# ── ESP32 (Xtensa lx6) ────────────────────────────────────────────────────────
#
# Toolchain: xtensa-esp32-elf-gcc (bundled with esp-idf)
#   Install: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
#
# The ESP32 has a hardware FPU, so float operations use hard ABI by default.
# For ESP32-S2 / ESP32-S3 (RISC-V cores instead of Xtensa), use
# riscv32-esp-elf-gcc with -march=rv32imc instead.
#
# This target produces .o files for linking via an esp-idf component:
#   1. Copy the .o files into your esp-idf component directory
#   2. Add them to your component's CMakeLists.txt
#   3. Build with idf.py build
#
CC_ESP32     = xtensa-esp32-elf-gcc
TARGET_ESP32 = fluxa-esp32

ESP32_CFLAGS = $(EMBEDDED_CFLAGS) \
               -mlongcalls         \
               -DFLUXA_TARGET_ESP32=1


# ── Generic ARM Cortex-M ──────────────────────────────────────────────────────
#
# Uses the same arm-none-eabi-gcc toolchain as RP2040.
# The default flags target a Cortex-M4 with FPU (e.g. STM32F4, nRF52840).
# Edit CORTEXM_CFLAGS below for your specific chip:
#
#   Cortex-M0 / M0+  (STM32G0, RP2040, SAM D21):
#     -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft
#
#   Cortex-M4 with FPU (STM32F4, nRF52840, MIMXRT):
#     -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
#
#   Cortex-M7 with FPU (STM32H7, i.MX RT1060):
#     -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
#
CC_CORTEXM     = arm-none-eabi-gcc
TARGET_CORTEXM = fluxa-cortex-m

CORTEXM_CFLAGS = $(EMBEDDED_CFLAGS)  \
                 -mcpu=cortex-m4      \
                 -mthumb              \
                 -mfpu=fpv4-sp-d16    \
                 -mfloat-abi=hard     \
                 -DFLUXA_TARGET_CORTEX_M=1


# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all build                                                \
        build-rp2040 build-esp32 build-cortex-m build-embedded  \
        build-asan                                               \
        check-toolchain-rp2040 check-toolchain-esp32            \
        test test-runner                                         \
        test-sprint5 test-sprint8 test-sprint9 test-sprint9b    \
        test-suite2                                              \
        test-suite2-prst test-suite2-handover test-suite2-gc    \
        test-suite2-dyn test-suite2-block                       \
        test-suite2-types test-suite2-embedded                  \
        test-integration test-integration-s1 test-integration-s2 \
        test-all                                                 \
        bench examples clean

all: build


# ─────────────────────────────────────────────────────────────────────────────
# Native build
# ─────────────────────────────────────────────────────────────────────────────

build:
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@echo "✓ build ok → ./$(TARGET)"


# ─────────────────────────────────────────────────────────────────────────────
# Embedded builds
# ─────────────────────────────────────────────────────────────────────────────

build-rp2040: check-toolchain-rp2040
	@echo "── cross-compile: RP2040 (Cortex-M0+) ──────────────────────────────"
	$(CC_RP2040) $(RP2040_CFLAGS) -c $(SRCS_EMBEDDED)
	@echo "✓ RP2040 objects compiled — link via pico-sdk CMake"
	@echo "  Copy the .o files into your pico-sdk project and add them to"
	@echo "  CMakeLists.txt via target_link_libraries()"

check-toolchain-rp2040:
	@which $(CC_RP2040) > /dev/null 2>&1 || \
	  (echo "✗ $(CC_RP2040) not found." && \
	   echo "  Ubuntu/Debian: sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi" && \
	   echo "  macOS:         brew install --cask gcc-arm-embedded" && \
	   exit 1)

build-esp32: check-toolchain-esp32
	@echo "── cross-compile: ESP32 (Xtensa lx6) ───────────────────────────────"
	$(CC_ESP32) $(ESP32_CFLAGS) -c $(SRCS_EMBEDDED)
	@echo "✓ ESP32 objects compiled — link via esp-idf component CMake"
	@echo "  Add the .o files to your esp-idf component CMakeLists.txt"

check-toolchain-esp32:
	@which $(CC_ESP32) > /dev/null 2>&1 || \
	  (echo "✗ $(CC_ESP32) not found." && \
	   echo "  Install esp-idf: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/" && \
	   exit 1)

# Generic Cortex-M — edit CORTEXM_CFLAGS above for your specific chip
build-cortex-m: check-toolchain-rp2040
	@echo "── cross-compile: ARM Cortex-M (default: M4 with FPU) ──────────────"
	$(CC_CORTEXM) $(CORTEXM_CFLAGS) -c $(SRCS_EMBEDDED)
	@echo "✓ Cortex-M objects compiled"
	@echo "  Edit CORTEXM_CFLAGS in this Makefile to match your chip"

# Attempt all embedded targets — silently skips any toolchain that is not installed
build-embedded:
	@echo "── building all embedded targets ────────────────────────────────────"
	@$(MAKE) build-rp2040 2>/dev/null && echo "  RP2040:   ✓" || echo "  RP2040:   ✗ (arm-none-eabi-gcc not found)"
	@$(MAKE) build-esp32  2>/dev/null && echo "  ESP32:    ✓" || echo "  ESP32:    ✗ (xtensa-esp32-elf-gcc not found)"
	@echo "── embedded build complete ───────────────────────────────────────────"


# ─────────────────────────────────────────────────────────────────────────────
# Development build: AddressSanitizer
# ─────────────────────────────────────────────────────────────────────────────
#
# Catches memory errors (use-after-free, buffer overflows, leaks) at runtime.
# Binary is 3-4x larger and 2-5x slower. Do NOT ship this binary.
#
# Usage:
#   make build-asan
#   ./fluxa_asan run <file.flx>

build-asan:
	$(CC) -std=c99 -Wall -Wextra -g \
	  -fsanitize=address,undefined   \
	  -Isrc -Ivendor -DFLUXA_HAS_FFI=1 \
	  $(SRCS) -o $(TARGET)_asan \
	  $(FFI_LDFLAGS) -ldl -lm -lpthread
	@echo "✓ asan build ok → ./$(TARGET)_asan  (development only — do not ship)"


# ─────────────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────────────
#
# Test hierarchy — from fastest to most thorough:
#
#   make test           Runs every .flx test file directly. No PASS/FAIL
#                       summary — you see raw output. Best for active feature
#                       development where you want to see exactly what happens.
#
#   make test-runner    Automated PASS/FAIL report with diff on failures.
#                       CI-friendly. Uses tests/run_tests.sh.
#                       Covers ~47 test cases across all sprints.
#
#   make test-suite2    Suite 2 — 70 edge-case and integration tests in 7
#                       sections: prst, handover, gc, dyn, block,
#                       types_danger, embedded.
#                       Run a single section: make test-suite2-<section>
#                       Documentation: tests/suite2/SUITE2.md
#
#   make test-integration  End-to-end Atomic Handover protocol simulation.
#                       No Docker required. Uses bash + python3 only.
#                       Scenario 1: normal IoT handover with prst preservation.
#                       Scenario 2: fault injection — kill at each step.
#
#   make test-all       Runs test-runner + test-suite2 + test-integration.
#                       The canonical "is everything green?" check.
#
# ─────────────────────────────────────────────────────────────────────────────

# Direct test runner — raw output, no PASS/FAIL summary
test: build
	@echo "── sprint 1: hello + types ─────────────────────────────────────────"
	@./$(TARGET) run tests/hello.flx
	@./$(TARGET) run tests/types.flx
	@echo "── sprint 2: variables, arithmetic, assignment ──────────────────────"
	@./$(TARGET) run tests/sprint2.flx
	@echo "── sprint 3: if/else, while, for ───────────────────────────────────"
	@./$(TARGET) run tests/sprint3.flx
	@echo "── sprint 4: functions, recursion, TCO ─────────────────────────────"
	@./$(TARGET) run tests/sprint4.flx
	@echo "── sprint 5: Block, typeof, isolation ──────────────────────────────"
	@./$(TARGET) run tests/sprint5.flx
	@./$(TARGET) run tests/block_isolation.flx
	@./$(TARGET) run tests/block_root.flx
	@./$(TARGET) run tests/block_methods.flx
	@echo "── sprint 5: typeof on instance must fail (expect error) ────────────"
	@./$(TARGET) run tests/block_no_instance_typeof.flx || true
	@echo "── sprint 6: danger, err stack, arr heap, free ─────────────────────"
	@./$(TARGET) run tests/sprint6.flx
	@./$(TARGET) run tests/danger_basic.flx
	@./$(TARGET) run tests/danger_err_stack.flx
	@./$(TARGET) run tests/danger_clean.flx
	@./$(TARGET) run tests/danger_after.flx
	@./$(TARGET) run tests/arr_heap.flx
	@echo "── sprint 6.b: FFI via dlopen/libffi ───────────────────────────────"
	@./$(TARGET) run tests/sprint6b.flx
	@echo "── sprint 6.c: TCO, mutual recursion ───────────────────────────────"
	@./$(TARGET) run tests/sprint6c_runtime.flx
	@echo "── sprint 7.a: prst, PrstGraph, fluxa explain ───────────────────────"
	@./$(TARGET) run tests/sprint7a.flx
	@./$(TARGET) run tests/sprint7a_collision.flx
	@./$(TARGET) explain tests/sprint7a.flx > /dev/null
	@echo "── sprint 7.a: arr default init + type enforcement ──────────────────"
	@./$(TARGET) run tests/arr_default.flx
	@./$(TARGET) run tests/arr_default_types.flx
	@echo "── sprint 7.a: FFI libm + portability ───────────────────────────────"
	@./$(TARGET) run tests/ffi_libm.flx
	@./$(TARGET) run tests/ffi_void.flx
	@./$(TARGET) run tests/ffi_portability.flx
	@echo "── sprint 7.a: err nil check, arr param, arr block field ────────────"
	@./$(TARGET) run tests/err_nil_check.flx
	@./$(TARGET) run tests/arr_param.flx
	@./$(TARGET) run tests/arr_block_field.flx
	@echo "── sprint 7.b: watcher, apply, serialization ────────────────────────"
	@./$(TARGET) run tests/sprint7b.flx
	@echo "── all direct tests passed ─────────────────────────────────────────"

# Automated PASS/FAIL runner — CI-friendly, shows diff on failures
test-runner: build
	@./tests/run_tests.sh ./$(TARGET)

# Sprint-specific targets — useful during active development of a sprint
test-sprint5: build
	@echo "── sprint 5 only ───────────────────────────────────────────────────"
	@./$(TARGET) run tests/sprint5.flx
	@./$(TARGET) run tests/block_isolation.flx
	@./$(TARGET) run tests/block_root.flx
	@./$(TARGET) run tests/block_methods.flx
	@echo "── typeof on instance (expect error) ────────────────────────────────"
	@./$(TARGET) run tests/block_no_instance_typeof.flx || true

test-sprint8: build
	@echo "── sprint 8: Atomic Handover ────────────────────────────────────────"
	@./$(TARGET) run tests/sprint8_handover_basic.flx
	@./$(TARGET) run tests/sprint8_handover_dry_run_fail.flx
	@./$(TARGET) run tests/sprint8_handover_prst.flx
	@./$(TARGET) run tests/sprint8_handover_version.flx
	@echo "── line numbers in errors (expect error with line number) ───────────"
	@./$(TARGET) run tests/sprint8_error_line.flx || true
	@echo "── all sprint 8 tests ok ────────────────────────────────────────────"

test-sprint9: build
	@echo "── sprint 9: CLI and IPC ────────────────────────────────────────────"
	@chmod +x tests/sprint9_cli.sh tests/sprint9_ipc.sh
	@bash tests/sprint9_cli.sh --fluxa ./$(TARGET)
	@bash tests/sprint9_ipc.sh --fluxa ./$(TARGET)
	@echo "── sprint 9 ok ──────────────────────────────────────────────────────"

test-sprint9b: build
	@echo "── sprint 9.b: IPC set in loop + fluxa explain live ─────────────────"
	@chmod +x tests/sprint9b_set_in_loop.sh tests/sprint9b_explain_live.sh
	@bash tests/sprint9b_set_in_loop.sh --fluxa ./$(TARGET)
	@bash tests/sprint9b_explain_live.sh --fluxa ./$(TARGET)
	@echo "── sprint 9.b ok ────────────────────────────────────────────────────"


# ── Suite 2: Edge Cases & Integration ────────────────────────────────────────
#
# 70 test cases across 7 sections. Tests the limits of the language under
# conditions that matter for real IoT deployments: type collisions across
# handovers, GC under tight memory, hundreds of Block instances, realistic
# sensor loop and state machine patterns.
#
# Full documentation: tests/suite2/SUITE2.md

test-suite2: build
	@bash tests/suite2/run_suite2.sh --fluxa ./$(TARGET)

test-suite2-prst: build
	@bash tests/suite2/s2_prst.sh --fluxa ./$(TARGET)

test-suite2-handover: build
	@bash tests/suite2/s2_handover.sh --fluxa ./$(TARGET)

test-suite2-gc: build
	@bash tests/suite2/s2_gc.sh --fluxa ./$(TARGET)

test-suite2-dyn: build
	@bash tests/suite2/s2_dyn.sh --fluxa ./$(TARGET)

test-suite2-block: build
	@bash tests/suite2/s2_block.sh --fluxa ./$(TARGET)

test-suite2-types: build
	@bash tests/suite2/s2_types_danger.sh --fluxa ./$(TARGET)

test-suite2-embedded: build
	@bash tests/suite2/s2_embedded.sh --fluxa ./$(TARGET)


# ── Integration tests ─────────────────────────────────────────────────────────
#
# End-to-end simulation of the Atomic Handover protocol using real .flx programs.
# No Docker required — runs locally with bash + python3 only.
#
#   Scenario 1: Normal IoT handover (prst values preserved, transformation applied)
#   Scenario 2: Fault injection (interrupt at each handover step, corrupted snapshot)
#
# See tests/integration/ for scenario details and expected output.

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

# Full test run — unit tests + suite2 + integration scenarios
test-all: build
	@./tests/run_tests.sh ./$(TARGET)
	@bash tests/suite2/run_suite2.sh --fluxa ./$(TARGET)
	@./tests/integration/run_all.sh --fluxa ./$(TARGET)


# ─────────────────────────────────────────────────────────────────────────────
# Benchmarks
# ─────────────────────────────────────────────────────────────────────────────
#
# Two benchmarks track performance regressions across sprints:
#
#   bench (global while)
#     Tight arithmetic loop compiled to the register VM (bytecode path).
#     Baseline: ~0.15s for 10^8 iterations on a modern x86 machine.
#
#   bench_block (method while)
#     While loop inside a Block method — exercises the AST eval path,
#     scope lookup, and method dispatch.
#     Baseline: ~0.55s for 10^7 iterations.
#
# A >2x regression in bench_block usually indicates a hot-path issue in
# the while back-edge safe point or scope lookup. See Sprint 10 gc.h fix.

bench: build
	@echo "── bench: tight global while loop (bytecode VM path) ────────────────"
	@bash -c "time ./$(TARGET) run tests/bench.flx"
	@echo "── bench_block: while inside Block method (AST eval path) ───────────"
	@bash -c "time ./$(TARGET) run tests/bench_block.flx"


# ─────────────────────────────────────────────────────────────────────────────
# Examples
# ─────────────────────────────────────────────────────────────────────────────
#
# Runs all programs under examples/ to verify they execute without error.
# Useful as a smoke test after refactoring core runtime behavior.

examples: build
	@echo "── data structures ─────────────────────────────────────────────────"
	@./$(TARGET) run examples/sort.flx
	@./$(TARGET) run examples/data_structures/stack.flx
	@./$(TARGET) run examples/data_structures/queue.flx
	@./$(TARGET) run examples/data_structures/linked_list.flx
	@./$(TARGET) run examples/data_structures/bst.flx
	@./$(TARGET) run examples/data_structures/tree.flx
	@./$(TARGET) run examples/data_structures/hash_map.flx
	@./$(TARGET) run examples/data_structures/dynamic_programming.flx
	@echo "── algorithm problems ──────────────────────────────────────────────"
	@./$(TARGET) run examples/problems/01_fizzbuzz.flx
	@./$(TARGET) run examples/problems/02_binary_search.flx
	@./$(TARGET) run examples/problems/03_two_sum.flx
	@./$(TARGET) run examples/problems/04_percolation.flx
	@./$(TARGET) run examples/problems/05_sliding_window.flx
	@./$(TARGET) run examples/problems/06_maze_bfs.flx
	@./$(TARGET) run examples/problems/07_dijkstra.flx
	@./$(TARGET) run examples/problems/08_pagerank.flx
	@echo "── all examples ok ─────────────────────────────────────────────────"


# ─────────────────────────────────────────────────────────────────────────────
# Clean
# ─────────────────────────────────────────────────────────────────────────────

clean:
	rm -f $(TARGET) $(TARGET_RP2040) $(TARGET_ESP32) $(TARGET_CORTEXM)
	rm -f $(TARGET)_asan $(TARGET)_debug
	rm -f *.o
