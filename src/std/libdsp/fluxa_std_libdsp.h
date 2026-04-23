#ifndef FLUXA_STD_LIBDSP_H
#define FLUXA_STD_LIBDSP_H

/*
 * std.libdsp — DSP and Radar Math for Fluxa-lang
 *
 * Pure C99, zero external deps. Uses std.libv float arr as storage.
 * FFT: Cooley-Tukey in-place, power-of-2 sizes, interleaved re/im layout.
 *
 * Interleaved layout: [re0, im0, re1, im1, ...re(N-1), im(N-1)]
 * So a 1024-point complex FFT needs float arr[2048].
 *
 * API:
 *   dsp.fft(signal)                       in-place forward FFT
 *   dsp.ifft(signal)                      in-place inverse FFT
 *   dsp.window(signal, name)              apply window (hann/hamming/blackman/rect)
 *   dsp.power(psd, signal)                power spectrum: psd[i] = re²+im²
 *   dsp.magnitude(mag, signal)            magnitude: mag[i] = sqrt(re²+im²)
 *   dsp.phase(ph, signal)                 phase: ph[i] = atan2(im, re)
 *   dsp.fir(signal, h)                    FIR filter (convolution)
 *   dsp.iir(signal, b, a)                 IIR filter (direct form II)
 *   dsp.matched_filter(signal, tmpl)      matched filter cross-correlation
 *   dsp.stft(out, signal, win_size, hop)  Short-time Fourier Transform
 *   dsp.range_doppler(rd, signal, nrng, ndop)  range-Doppler map
 *   dsp.cfar(detections, rd, guard, ref, threshold) CFAR detector
 *   dsp.peak(signal)                      → index of max magnitude
 *   dsp.snr(signal, noise_floor)          → float SNR in dB
 *   dsp.normalize(signal)                 normalize to max magnitude = 1.0
 *   dsp.zeros(signal)                     zero imaginary parts (real→complex)
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../scope.h"
#include "../../err.h"

#ifdef FLUXA_LIBDSP_FFTW
#  include <fftw3.h>
/* FFTW helper: run plan on interleaved Value array */
static inline void dsp_fftw_run(Value *data, int N, int inverse) {
    fftw_complex *in  = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * (size_t)N);
    fftw_complex *out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * (size_t)N);
    for (int i = 0; i < N; i++) {
        in[i][0] = (data[2*i].type   == VAL_FLOAT) ? data[2*i].as.real   : 0.0;
        in[i][1] = (data[2*i+1].type == VAL_FLOAT) ? data[2*i+1].as.real : 0.0;
    }
    int sign = inverse ? FFTW_BACKWARD : FFTW_FORWARD;
    fftw_plan p = fftw_plan_dft_1d(N, in, out, sign, FFTW_ESTIMATE);
    fftw_execute(p);
    fftw_destroy_plan(p);
    double scale = inverse ? 1.0 / N : 1.0;
    for (int i = 0; i < N; i++) {
        data[2*i].type     = VAL_FLOAT; data[2*i].as.real     = out[i][0] * scale;
        data[2*i+1].type   = VAL_FLOAT; data[2*i+1].as.real   = out[i][1] * scale;
    }
    fftw_free(in); fftw_free(out);
}
#endif /* FLUXA_LIBDSP_FFTW */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value dsp_nil(void)       { Value v; v.type = VAL_NIL;   return v; }
static inline Value dsp_float(double d) { Value v; v.type = VAL_FLOAT; v.as.real    = d; return v; }
static inline Value dsp_int(long n)     { Value v; v.type = VAL_INT;   v.as.integer = n; return v; }

static inline double dsp_get(const Value *data, int i) {
    if (data[i].type == VAL_FLOAT) return data[i].as.real;
    if (data[i].type == VAL_INT)   return (double)data[i].as.integer;
    return 0.0;
}
static inline void dsp_set(Value *data, int i, double v) {
    data[i].type = VAL_FLOAT; data[i].as.real = v;
}

/* Get arr data pointer and size */
static inline Value *dsp_arr(const Value *v, ErrStack *err, int *had_error,
                               int line, const char *fn, int *sz) {
    char errbuf[280];
    if (v->type != VAL_ARR || !v->as.arr.data) {
        snprintf(errbuf, sizeof(errbuf), "libdsp.%s: expected float arr", fn);
        errstack_push(err, ERR_FLUXA, errbuf, "libdsp", line);
        *had_error = 1; return NULL;
    }
    *sz = v->as.arr.size;
    return v->as.arr.data;
}

/* ── Cooley-Tukey FFT (iterative, in-place) ──────────────────────── */
/*
 * Interleaved layout: data[2*k] = real part, data[2*k+1] = imag part.
 * N = number of complex samples = arr.size / 2.
 * N must be a power of 2.
 */
static int dsp_is_pow2(int n) { return n > 0 && (n & (n-1)) == 0; }

static void dsp_fft_core(Value *data, int N, int inverse) {
    /* Bit-reversal permutation */
    int j = 0;
    for (int i = 1; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double tr = dsp_get(data, 2*i),   ti = dsp_get(data, 2*i+1);
            double qr = dsp_get(data, 2*j),   qi = dsp_get(data, 2*j+1);
            dsp_set(data, 2*i,   qr); dsp_set(data, 2*i+1, qi);
            dsp_set(data, 2*j,   tr); dsp_set(data, 2*j+1, ti);
        }
    }
    /* Cooley-Tukey butterfly */
    for (int len = 2; len <= N; len <<= 1) {
        double ang = 2.0 * M_PI / len * (inverse ? 1 : -1);
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < N; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len/2; k++) {
                double ur = dsp_get(data, 2*(i+k)),     ui = dsp_get(data, 2*(i+k)+1);
                double vr = dsp_get(data, 2*(i+k+len/2)), vi = dsp_get(data, 2*(i+k+len/2)+1);
                double tr = cr*vr - ci*vi,  ti = cr*vi + ci*vr;
                dsp_set(data, 2*(i+k),        ur+tr); dsp_set(data, 2*(i+k)+1,        ui+ti);
                dsp_set(data, 2*(i+k+len/2),  ur-tr); dsp_set(data, 2*(i+k+len/2)+1,  ui-ti);
                double ncr = cr*wr - ci*wi;
                ci = cr*wi + ci*wr; cr = ncr;
            }
        }
    }
    /* Normalize IFFT */
    if (inverse) {
        for (int i = 0; i < N; i++) {
            dsp_set(data, 2*i,   dsp_get(data, 2*i)   / N);
            dsp_set(data, 2*i+1, dsp_get(data, 2*i+1) / N);
        }
    }
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_libdsp_call(const char *fn_name,
                                           const Value *args, int argc,
                                           ErrStack *err, int *had_error,
                                           int line,
                                           const FluxaConfig *cfg) {
    /* Select backend from [libs.libdsp] backend = "fftw"|"native" (default: native) */
    int use_fftw = 0;
    (void)use_fftw; /* suppresses unused warning when FFTW not compiled in */
#ifdef FLUXA_LIBDSP_FFTW
    if (cfg && strncmp(cfg->libdsp_backend, "fftw", 4) == 0) use_fftw = 1;
#else
    (void)cfg;
#endif
    char errbuf[280];

#define DSP_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "libdsp.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "libdsp", line); \
    *had_error = 1; return dsp_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "libdsp.%s: expected %d arg(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "libdsp", line); \
        *had_error = 1; return dsp_nil(); \
    } \
} while(0)

#define GET_ARR(idx, dptr, sz) \
    int sz = 0; \
    Value *(dptr) = dsp_arr(&args[(idx)], err, had_error, line, fn_name, &sz); \
    if (!(dptr)) return dsp_nil();

#define GET_FLOAT(idx, var) \
    double (var) = 0.0; \
    if (args[(idx)].type == VAL_FLOAT) (var) = args[(idx)].as.real; \
    else if (args[(idx)].type == VAL_INT) (var) = (double)args[(idx)].as.integer; \
    else DSP_ERR("expected float argument");

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT) DSP_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        DSP_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

    /* ── dsp.fft(signal) ─────────────────────────────── */
    if (!strcmp(fn_name, "fft")) {
        NEED(1); GET_ARR(0, d, n);
        if (n < 2 || n % 2 != 0) DSP_ERR("fft: array size must be even (interleaved re/im)");
        int N = n / 2;
#ifdef FLUXA_LIBDSP_FFTW
        if (use_fftw) { dsp_fftw_run(d, N, 0); return dsp_nil(); }
#endif
        if (!dsp_is_pow2(N)) DSP_ERR("fft: number of complex samples must be power of 2");
        dsp_fft_core(d, N, 0);
        return dsp_nil();
    }

    /* ── dsp.ifft(signal) ────────────────────────────── */
    if (!strcmp(fn_name, "ifft")) {
        NEED(1); GET_ARR(0, d, n);
        if (n < 2 || n % 2 != 0) DSP_ERR("ifft: array size must be even");
        int N = n / 2;
#ifdef FLUXA_LIBDSP_FFTW
        if (use_fftw) { dsp_fftw_run(d, N, 1); return dsp_nil(); }
#endif
        if (!dsp_is_pow2(N)) DSP_ERR("ifft: number of complex samples must be power of 2");
        dsp_fft_core(d, N, 1);
        return dsp_nil();
    }

    /* ── dsp.zeros(signal) — fill imaginary parts with 0 ── */
    if (!strcmp(fn_name, "zeros")) {
        NEED(1); GET_ARR(0, d, n);
        if (n % 2 != 0) DSP_ERR("zeros: array size must be even (interleaved re/im)");
        for (int i = 0; i < n/2; i++) {
            /* real part: keep as-is if set, imaginary = 0 */
            dsp_set(d, 2*i+1, 0.0);
        }
        return dsp_nil();
    }

    /* ── dsp.window(signal, type) ───────────────────────── */
    if (!strcmp(fn_name, "window")) {
        NEED(2); GET_ARR(0, d, n); GET_STR(1, wtype);
        if (n < 2 || n % 2 != 0) DSP_ERR("window: array size must be even");
        int N = n / 2; /* number of complex samples */
        for (int i = 0; i < N; i++) {
            double w = 1.0;
            if (!strcmp(wtype, "hann")) {
                w = 0.5 * (1.0 - cos(2.0 * M_PI * i / (N - 1)));
            } else if (!strcmp(wtype, "hamming")) {
                w = 0.54 - 0.46 * cos(2.0 * M_PI * i / (N - 1));
            } else if (!strcmp(wtype, "blackman")) {
                w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (N - 1))
                         + 0.08 * cos(4.0 * M_PI * i / (N - 1));
            } else if (!strcmp(wtype, "rect")) {
                w = 1.0;
            } else {
                DSP_ERR("window: unknown type — use hann, hamming, blackman, rect");
            }
            dsp_set(d, 2*i,   dsp_get(d, 2*i)   * w);
            dsp_set(d, 2*i+1, dsp_get(d, 2*i+1) * w);
        }
        return dsp_nil();
    }

    /* ── dsp.power(psd, signal) ─────────────────────────── */
    if (!strcmp(fn_name, "power")) {
        NEED(2); GET_ARR(0, psd, np); GET_ARR(1, sig, ns);
        if (ns % 2 != 0) DSP_ERR("power: signal size must be even");
        int N = ns / 2;
        if (np < N) DSP_ERR("power: psd array too small (needs N elements)");
        for (int i = 0; i < N; i++) {
            double re = dsp_get(sig, 2*i), im = dsp_get(sig, 2*i+1);
            dsp_set(psd, i, re*re + im*im);
        }
        return dsp_nil();
    }

    /* ── dsp.magnitude(mag, signal) ─────────────────────── */
    if (!strcmp(fn_name, "magnitude")) {
        NEED(2); GET_ARR(0, mag, nm); GET_ARR(1, sig, ns);
        if (ns % 2 != 0) DSP_ERR("magnitude: signal size must be even");
        int N = ns / 2;
        if (nm < N) DSP_ERR("magnitude: output array too small");
        for (int i = 0; i < N; i++) {
            double re = dsp_get(sig, 2*i), im = dsp_get(sig, 2*i+1);
            dsp_set(mag, i, sqrt(re*re + im*im));
        }
        return dsp_nil();
    }

    /* ── dsp.phase(ph, signal) ──────────────────────────── */
    if (!strcmp(fn_name, "phase")) {
        NEED(2); GET_ARR(0, ph, nph); GET_ARR(1, sig, ns);
        if (ns % 2 != 0) DSP_ERR("phase: signal size must be even");
        int N = ns / 2;
        if (nph < N) DSP_ERR("phase: output array too small");
        for (int i = 0; i < N; i++) {
            double re = dsp_get(sig, 2*i), im = dsp_get(sig, 2*i+1);
            dsp_set(ph, i, atan2(im, re));
        }
        return dsp_nil();
    }

    /* ── dsp.fir(signal, h) ─────────────────────────────── */
    /* FIR filter — linear convolution (real-valued, operates on re parts) */
    if (!strcmp(fn_name, "fir")) {
        NEED(2); GET_ARR(0, sig, ns); GET_ARR(1, h, nh);
        /* Work on interleaved real parts only */
        int N = (ns % 2 == 0) ? ns/2 : ns; /* support both complex and real */
        int is_complex = (ns % 2 == 0 && ns > nh);
        int stride = is_complex ? 2 : 1;
        double *tmp = (double *)calloc((size_t)N, sizeof(double));
        for (int i = 0; i < N; i++) {
            double acc = 0.0;
            for (int k = 0; k < nh && k <= i; k++) {
                acc += dsp_get(h, k) * dsp_get(sig, stride * (i-k));
            }
            tmp[i] = acc;
        }
        for (int i = 0; i < N; i++) dsp_set(sig, stride*i, tmp[i]);
        free(tmp);
        return dsp_nil();
    }

    /* ── dsp.iir(signal, b, a) ──────────────────────────── */
    /* IIR filter — direct form II transposed (real-valued) */
    if (!strcmp(fn_name, "iir")) {
        NEED(3); GET_ARR(0, sig, ns); GET_ARR(1, b, nb); GET_ARR(2, a, na);
        int N = ns;
        double *w = (double *)calloc((size_t)(nb > na ? nb : na), sizeof(double));
        for (int i = 0; i < N; i++) {
            double x = dsp_get(sig, i);
            double y = dsp_get(b, 0) * x + w[0];
            for (int k = 1; k < nb || k < na; k++) {
                double bk = (k < nb) ? dsp_get(b, k) : 0.0;
                double ak = (k < na) ? dsp_get(a, k) : 0.0;
                double wk = (k < (nb > na ? nb : na) - 1) ? w[k] : 0.0;
                w[k-1] = bk * x - ak * y + wk;
            }
            dsp_set(sig, i, y);
        }
        free(w);
        return dsp_nil();
    }

    /* ── dsp.matched_filter(signal, tmpl) ──────────────── */
    /* Cross-correlation: out[i] = sum_k signal[i+k] * tmpl[k] */
    if (!strcmp(fn_name, "matched_filter")) {
        NEED(2); GET_ARR(0, sig, ns); GET_ARR(1, tmpl, nt);
        if (nt > ns) DSP_ERR("matched_filter: template longer than signal");
        double *tmp = (double *)calloc((size_t)ns, sizeof(double));
        for (int i = 0; i < ns - nt + 1; i++) {
            double acc = 0.0;
            for (int k = 0; k < nt; k++) {
                acc += dsp_get(sig, i+k) * dsp_get(tmpl, k);
            }
            tmp[i] = acc;
        }
        for (int i = 0; i < ns; i++) dsp_set(sig, i, tmp[i]);
        free(tmp);
        return dsp_nil();
    }

    /* ── dsp.stft(out, signal, win_size, hop) ───────────── */
    /* Short-time Fourier Transform.
     * out: float arr of size (num_frames * win_size * 2) — interleaved complex
     * signal: real-valued input (just real parts, not interleaved)
     * win_size: FFT size (power of 2)
     * hop: hop size in samples */
    if (!strcmp(fn_name, "stft")) {
        NEED(4); GET_ARR(0, out, no); GET_ARR(1, sig, ns);
        GET_INT(2, win_size); GET_INT(3, hop);
        if (!dsp_is_pow2((int)win_size)) DSP_ERR("stft: win_size must be power of 2");
        if (hop < 1) DSP_ERR("stft: hop must be >= 1");
        int W = (int)win_size;
        int num_frames = (ns - W) / (int)hop + 1;
        if (no < num_frames * W * 2)
            DSP_ERR("stft: output array too small (need num_frames * win_size * 2)");
        /* Hann window */
        double *win = (double *)malloc((size_t)W * sizeof(double));
        for (int i = 0; i < W; i++)
            win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (W - 1)));
        /* Frame buffer */
        Value *frame = (Value *)calloc((size_t)(W*2), sizeof(Value));
        for (int f = 0; f < num_frames; f++) {
            int start = f * (int)hop;
            /* Load windowed frame as interleaved complex */
            for (int i = 0; i < W; i++) {
                frame[2*i].type     = VAL_FLOAT;
                frame[2*i].as.real   = (start+i < ns) ? dsp_get(sig, start+i) * win[i] : 0.0;
                frame[2*i+1].type   = VAL_FLOAT;
                frame[2*i+1].as.real = 0.0;
            }
            dsp_fft_core(frame, W, 0);
            /* Copy to output */
            for (int i = 0; i < W*2; i++)
                dsp_set(out, f * W * 2 + i, dsp_get(frame, i));
        }
        free(win); free(frame);
        return dsp_nil();
    }

    /* ── dsp.range_doppler(rd, signal, nrng, ndop) ──────── */
    /* Range-Doppler map: 2D FFT over a pulse matrix.
     * signal: flat array of nrng*ndop*2 interleaved complex samples
     * rd:     output flat array, same size
     * Process: FFT along range (rows), then FFT along Doppler (cols) */
    if (!strcmp(fn_name, "range_doppler")) {
        NEED(4); GET_ARR(0, rd, nrd); GET_ARR(1, sig, ns);
        GET_INT(2, nrng); GET_INT(3, ndop);
        if (nrng < 1 || ndop < 1) DSP_ERR("range_doppler: dimensions must be >= 1");
        if (!dsp_is_pow2((int)nrng) || !dsp_is_pow2((int)ndop))
            DSP_ERR("range_doppler: nrng and ndop must be powers of 2");
        long expected = nrng * ndop * 2;
        if (ns < expected || nrd < (int)expected)
            DSP_ERR("range_doppler: signal/rd too small for nrng*ndop*2");
        /* Copy signal → rd */
        for (int i = 0; i < (int)expected; i++) dsp_set(rd, i, dsp_get(sig, i));
        /* FFT along range (each Doppler row) */
        Value *row = (Value *)calloc((size_t)(nrng * 2), sizeof(Value));
        for (int d = 0; d < (int)ndop; d++) {
            for (int r = 0; r < (int)nrng; r++) {
                row[2*r].type = VAL_FLOAT;
                row[2*r].as.real = dsp_get(rd, (d * nrng + r) * 2);
                row[2*r+1].type = VAL_FLOAT;
                row[2*r+1].as.real = dsp_get(rd, (d * nrng + r) * 2 + 1);
            }
            dsp_fft_core(row, (int)nrng, 0);
            for (int r = 0; r < (int)nrng; r++) {
                dsp_set(rd, (d * nrng + r) * 2,     dsp_get(row, 2*r));
                dsp_set(rd, (d * nrng + r) * 2 + 1, dsp_get(row, 2*r+1));
            }
        }
        free(row);
        /* FFT along Doppler (each range column) */
        Value *col = (Value *)calloc((size_t)(ndop * 2), sizeof(Value));
        for (int r = 0; r < (int)nrng; r++) {
            for (int d = 0; d < (int)ndop; d++) {
                col[2*d].type = VAL_FLOAT;
                col[2*d].as.real = dsp_get(rd, (d * nrng + r) * 2);
                col[2*d+1].type = VAL_FLOAT;
                col[2*d+1].as.real = dsp_get(rd, (d * nrng + r) * 2 + 1);
            }
            dsp_fft_core(col, (int)ndop, 0);
            for (int d = 0; d < (int)ndop; d++) {
                dsp_set(rd, (d * nrng + r) * 2,     dsp_get(col, 2*d));
                dsp_set(rd, (d * nrng + r) * 2 + 1, dsp_get(col, 2*d+1));
            }
        }
        free(col);
        return dsp_nil();
    }

    /* ── dsp.cfar(detections, rd, guard, ref, threshold) ── */
    /* Cell-Averaging CFAR detector on power map (real-valued).
     * detections: int arr of same size — 1 where detected, 0 otherwise
     * rd: power map (output of dsp.power on range_doppler result)
     * guard: guard cells on each side
     * ref: reference cells on each side
     * threshold: CFAR threshold multiplier */
    if (!strcmp(fn_name, "cfar")) {
        NEED(5); GET_ARR(0, det, nd); GET_ARR(1, rd, nr);
        GET_INT(2, guard); GET_INT(3, ref); GET_FLOAT(4, threshold);
        if (nd < nr) DSP_ERR("cfar: detections array too small");
        int G = (int)guard, R = (int)ref;
        int win = G + R;
        for (int i = 0; i < nr; i++) {
            double noise = 0.0;
            int count = 0;
            for (int k = i - win; k <= i + win; k++) {
                if (k < 0 || k >= nr) continue;
                int dist = abs(k - i);
                if (dist > G && dist <= win) { noise += dsp_get(rd, k); count++; }
            }
            double noise_avg = (count > 0) ? noise / count : 1e-10;
            double cell = dsp_get(rd, i);
            det[i].type      = VAL_INT;
            det[i].as.integer = (cell > threshold * noise_avg) ? 1 : 0;
        }
        return dsp_nil();
    }

    /* ── dsp.peak(signal) → index of max magnitude ──────── */
    if (!strcmp(fn_name, "peak")) {
        NEED(1); GET_ARR(0, d, n);
        if (n < 2) DSP_ERR("peak: array too small");
        int is_complex = (n % 2 == 0);
        int N = is_complex ? n/2 : n;
        int best = 0;
        double best_mag = -1.0;
        for (int i = 0; i < N; i++) {
            double mag;
            if (is_complex) {
                double re = dsp_get(d, 2*i), im = dsp_get(d, 2*i+1);
                mag = re*re + im*im;
            } else {
                double v = dsp_get(d, i);
                mag = v < 0 ? -v : v;
            }
            if (mag > best_mag) { best_mag = mag; best = i; }
        }
        return dsp_int(best);
    }

    /* ── dsp.snr(signal, noise_floor) → float SNR in dB ─── */
    if (!strcmp(fn_name, "snr")) {
        NEED(2); GET_ARR(0, d, n); GET_FLOAT(1, noise_floor);
        if (n < 2) DSP_ERR("snr: array too small");
        int is_complex = (n % 2 == 0);
        int N = is_complex ? n/2 : n;
        double peak_pow = 0.0;
        for (int i = 0; i < N; i++) {
            double pow;
            if (is_complex) {
                double re = dsp_get(d, 2*i), im = dsp_get(d, 2*i+1);
                pow = re*re + im*im;
            } else {
                double v = dsp_get(d, i);
                pow = v*v;
            }
            if (pow > peak_pow) peak_pow = pow;
        }
        if (noise_floor <= 0.0) DSP_ERR("snr: noise_floor must be > 0");
        return dsp_float(10.0 * log10(peak_pow / noise_floor));
    }

    /* ── dsp.normalize(signal) — max magnitude = 1.0 ────── */
    if (!strcmp(fn_name, "normalize")) {
        NEED(1); GET_ARR(0, d, n);
        if (n < 1) DSP_ERR("normalize: empty array");
        double max_mag = 0.0;
        for (int i = 0; i < n; i++) {
            double v = dsp_get(d, i);
            double av = v < 0 ? -v : v;
            if (av > max_mag) max_mag = av;
        }
        if (max_mag < 1e-15) DSP_ERR("normalize: zero signal");
        for (int i = 0; i < n; i++) dsp_set(d, i, dsp_get(d, i) / max_mag);
        return dsp_nil();
    }

#undef DSP_ERR
#undef NEED
#undef GET_ARR
#undef GET_FLOAT
#undef GET_INT
#undef GET_STR

    snprintf(errbuf, sizeof(errbuf), "libdsp.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "libdsp", line);
    *had_error = 1;
    return dsp_nil();
}

/* ── Lib descriptor ──────────────────────────────────────────────── */
FLUXA_LIB_EXPORT(
    name      = "libdsp",
    toml_key  = "std.libdsp",
    owner     = "libdsp",
    call      = fluxa_std_libdsp_call,
    rt_aware  = 0,
    cfg_aware = 1
)

#endif /* FLUXA_STD_LIBDSP_H */
