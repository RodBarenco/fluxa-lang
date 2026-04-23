#!/usr/bin/env bash
# tests/libs/libdsp.sh — std.libdsp test suite
# Tests FFT, windowing, power spectrum, filters, CFAR, STFT, range-Doppler.
# All tests use mathematically verifiable properties (Parseval, linearity, etc.)
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/libdsp/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/libdsp/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() {
    printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.libv="1.0"\nstd.libdsp="1.0"\n' \
        > "$P/fluxa.toml"
}
run() { toml; cat > "$P/main.flx"; timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.libdsp ───────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std libdsp
float arr s[8] = libdsp.vec(4)
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. FFT of DC signal → energy at bin 0 only
# DC signal: all real parts = 1.0, all imaginary = 0
# After FFT: bin 0 = N (real), all others = 0
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[8] = libv.vec(8)
s[0] = 1.0
s[2] = 1.0
s[4] = 1.0
s[6] = 1.0
libdsp.fft(s)
print(s[0])
print(s[1])
print(s[2])
FLX
)
echo "$out" | grep -q "^4$" && pass "fft_dc_bin0_is_N" || fail "fft_dc_bin0_is_N" "4" "$out"
echo "$out" | grep -q "^0$" && pass "fft_dc_bin1_is_0" || fail "fft_dc_bin1_is_0" "0" "$out"

# 3. FFT → IFFT roundtrip (Parseval: signal recovers)
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[16] = libv.vec(16)
s[0] = 3.0
s[2] = 1.0
s[4] = 4.0
s[6] = 1.0
libdsp.fft(s)
libdsp.ifft(s)
print(s[0])
print(s[2])
print(s[4])
FLX
)
echo "$out" | grep -q "^3$" && pass "fft_ifft_roundtrip_re0" || fail "fft_ifft_roundtrip_re0" "3" "$out"
echo "$out" | grep -q "^1$" && pass "fft_ifft_roundtrip_re1" || fail "fft_ifft_roundtrip_re1" "1" "$out"

# 4. Power spectrum — DC signal: all power at bin 0
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[8] = libv.vec(8)
s[0] = 2.0
s[2] = 2.0
s[4] = 2.0
s[6] = 2.0
libdsp.fft(s)
float arr p[4] = libv.vec(4)
libdsp.power(p, s)
print(p[0])
print(p[1])
FLX
)
echo "$out" | grep -q "^64$" && pass "power_dc_bin0" || fail "power_dc_bin0" "64 (N²×A²=4²×4)" "$out"
echo "$out" | grep -q "^0$" && pass "power_dc_bin1_zero" || fail "power_dc_bin1_zero" "0" "$out"

# 5. Magnitude of known complex value
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[4] = libv.vec(4)
s[0] = 3.0
s[1] = 4.0
float arr m[2] = libv.vec(2)
libdsp.magnitude(m, s)
print(m[0])
FLX
)
echo "$out" | grep -q "^5$" && pass "magnitude_3_4_is_5" || fail "magnitude_3_4_is_5" "5" "$out"

# 6. Phase of known complex value (3+4j → atan2(4,3) ≈ 0.927)
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[4] = libv.vec(4)
s[0] = 3.0
s[1] = 4.0
float arr ph[2] = libv.vec(2)
libdsp.phase(ph, s)
print(ph[0])
FLX
)
# atan2(4,3) ≈ 0.927, print truncates to 0.9...
echo "$out" | grep -qE "^0\.[89]" && pass "phase_3_4_correct" || fail "phase_3_4_correct" "~0.927" "$out"

# 7. Window — hann window zeros endpoints
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[8] = libv.vec(8)
s[0] = 1.0
s[2] = 1.0
s[4] = 1.0
s[6] = 1.0
libdsp.window(s, "hann")
print(s[0])
FLX
)
echo "$out" | grep -q "^0$" && pass "hann_zeros_first_sample" || fail "hann_zeros_first_sample" "0" "$out"

# 8. Hamming window — non-zero at endpoints (0.08)
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[8] = libv.vec(8)
s[0] = 1.0
s[2] = 1.0
s[4] = 1.0
s[6] = 1.0
libdsp.window(s, "hamming")
print(s[0])
FLX
)
echo "$out" | grep -qE "^0\.[01]" && pass "hamming_nonzero_first" || fail "hamming_nonzero_first" "~0.08" "$out"

# 9. rect window — no change
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[8] = libv.vec(8)
s[0] = 2.0
s[2] = 3.0
libdsp.window(s, "rect")
print(s[0])
print(s[2])
FLX
)
echo "$out" | grep -q "^2$" && pass "rect_window_no_change_0" || fail "rect_window_no_change_0" "2" "$out"
echo "$out" | grep -q "^3$" && pass "rect_window_no_change_2" || fail "rect_window_no_change_2" "3" "$out"

# 10. peak() returns index of max magnitude bin
# Use size-5 (non-even) so peak treats as real-valued, not complex
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[5] = libv.vec(5)
s[3] = 10.0
int idx = libdsp.peak(s)
print(idx)
FLX
)
echo "$out" | grep -q "^3$" && pass "peak_finds_max_index" || fail "peak_finds_max_index" "3" "$out"

# 11. normalize — max element becomes 1.0
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[4] = libv.vec(4)
s[0] = 3.0
s[1] = 6.0
s[2] = 1.0
s[3] = 2.0
libdsp.normalize(s)
print(s[1])
print(s[0])
FLX
)
echo "$out" | grep -q "^1$" && pass "normalize_max_is_1" || fail "normalize_max_is_1" "1" "$out"
echo "$out" | grep -q "^0\.5$" && pass "normalize_others_scaled" || fail "normalize_others_scaled" "0.5" "$out"

# 12. FIR filter — unit impulse → returns coefficients
# Use odd-size array so FIR treats as real (stride=1)
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[5] = libv.vec(5)
float arr h[2] = libv.vec(2)
s[0] = 1.0
h[0] = 2.0
h[1] = 3.0
libdsp.fir(s, h)
print(s[0])
print(s[1])
FLX
)
echo "$out" | grep -q "^2$" && pass "fir_impulse_h0" || fail "fir_impulse_h0" "2" "$out"
echo "$out" | grep -q "^3$" && pass "fir_impulse_h1" || fail "fir_impulse_h1" "3" "$out"

# 13. CFAR — strong target detected, noise floor not detected
toml
cat > "$P/main.flx" << 'FLX'
import std libv
import std libdsp
float arr rd[16] = libv.vec(16)
int arr det[16] = libv.vec(16)
rd[8] = 100.0
libdsp.cfar(det, rd, 1, 3, 8.0)
print(det[8])
print(det[0])
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^1$" && pass "cfar_detects_target" || fail "cfar_detects_target" "1" "$out"
echo "$out" | grep -q "^0$" && pass "cfar_noise_not_detected" || fail "cfar_noise_not_detected" "0" "$out"

# 14. SNR — signal at 100, noise at 1 → 20 dB
out=$(run << 'FLX'
import std libv
import std libdsp
float arr s[4] = libv.vec(4)
s[0] = 10.0
float snr = libdsp.snr(s, 1.0)
print(snr)
FLX
)
echo "$out" | grep -q "^20$" && pass "snr_10_1_is_20dB" || fail "snr_10_1_is_20dB" "20" "$out"

# 15. FFT size validation — non-power-of-2 → error
toml
cat > "$P/main.flx" << 'FLX'
import std libv
import std libdsp
danger {
    float arr s[6] = libv.vec(6)
    libdsp.fft(s)
}
if err != nil { print("error caught") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "error caught" && pass "fft_non_pow2_error" || fail "fft_non_pow2_error" "error caught" "$out"

# 16. FFTW backend — same FFT result as native
# Only if compiled with FFTW support (FLUXA_LIBDSP_FFTW=1)
if ./fluxa version 2>/dev/null | grep -qi "fftw" ||    strings ./fluxa 2>/dev/null | grep -q "fftw_malloc"; then
    FFTW_AVAILABLE=1
else
    FFTW_AVAILABLE=0
fi

if [ "$FFTW_AVAILABLE" -eq 1 ]; then
    # Write a toml with fftw backend
    mkdir -p "$P"
    printf '[project]
name="t"
entry="main.flx"
[libs]
std.libv="1.0"
std.libdsp="1.0"
[libs.libdsp]
backend="fftw"
' > "$P/fluxa.toml"
    cat > "$P/main.flx" << 'FLX'
import std libv
import std libdsp
float arr s[8] = libv.vec(8)
s[0] = 1.0
s[2] = 1.0
s[4] = 1.0
s[6] = 1.0
libdsp.fft(s)
print(s[0])
print(s[1])
FLX
    out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
    echo "$out" | grep -q "^4$" && pass "fftw_backend_dc_bin0"         || fail "fftw_backend_dc_bin0" "4" "$out"
    echo "$out" | grep -q "^0$" && pass "fftw_backend_dc_bin1_zero"         || fail "fftw_backend_dc_bin1_zero" "0" "$out"
fi

# FFTW backend: always runs (graceful fallback to native if not compiled)
printf '[project]
name="t"
entry="main.flx"
[libs]
std.libv="1.0"
std.libdsp="1.0"
[libs.libdsp]
backend="fftw"
' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std libv
import std libdsp
float arr s[8] = libv.vec(8)
s[0] = 1.0
s[2] = 1.0
s[4] = 1.0
s[6] = 1.0
libdsp.fft(s)
print(s[0])
print(s[1])
FLX
out=$(timeout 8s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -q "^4$" && pass "fftw_backend_dc_bin0"     || fail "fftw_backend_dc_bin0" "4" "$out"
echo "$out" | grep -q "^0$" && pass "fftw_backend_dc_bin1_zero"     || fail "fftw_backend_dc_bin1_zero" "0" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.libdsp: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.libdsp: PASS" && exit 0 || exit 1
