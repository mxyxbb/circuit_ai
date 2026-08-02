#pragma once
#include <vector>
#include <cmath>
#include <cstddef>

// ─────────────────────────────────────────────────────────────────────────────
// Self-contained FFT / spectrum utility for the scope's frequency-domain view.
//
// Frontend-only, header-only, zero external dependencies (no Eigen, no FFTW).
// Implements an iterative radix-2 Cooley–Tukey FFT with zero-padding to the next
// power of two, a selectable analysis window, optional DC removal, and one-sided
// amplitude-spectrum normalisation (a pure sinusoid of amplitude A that lands in
// a single bin reads back as ≈ A regardless of the chosen window).
//
// Time-domain input may be lightly non-uniform (the simulator pushes at a fixed
// dt·sample_ratio, but we resample onto a uniform grid via linear interpolation
// to stay robust against any jitter). The sample rate is derived from the span
// of the analysis window.
//
// Two user-facing knobs (Options):
//   • f0   — fundamental frequency. When > 0 the analysis window is snapped to
//            the most recent whole number of f0 periods (coherent sampling), so
//            the fundamental and its harmonics land exactly on FFT bins and
//            spectral leakage vanishes. 0 = use the whole supplied span.
//   • nfft — FFT size (points). When > 0 the window is resampled to exactly
//            nfft points (rounded up to a power of two) with no zero-padding, so
//            coherence is preserved. 0 = auto: with f0 set, the window is
//            resampled to the next power of two above the native sample count
//            (again no padding — padding would move the harmonics off the bin
//            grid and under-read their amplitude); without f0 the native count
//            is used and zero-padded to the next power of two.
// ─────────────────────────────────────────────────────────────────────────────
namespace fft {

enum class Window { Rectangular = 0, Hann = 1, Hamming = 2, Blackman = 3 };

struct Options {
    Window win      = Window::Hann;
    bool   removeDc = true;
    double f0       = 0.0;   // fundamental (Hz); 0 = full span
    int    nfft     = 0;     // FFT size; 0 = auto (native count, zero-padded)
};

struct Spectrum {
    std::vector<double> freq;   // Hz, one-sided [0 .. fs/2]
    std::vector<double> mag;    // linear amplitude (same units as the signal)
    double fs = 0.0;            // sample rate used (Hz)
    double df = 0.0;            // bin spacing (Hz)
    int    n  = 0;              // number of time-domain samples analysed (Ntime)
    int    nfft = 0;            // transform length actually used
    double winDur  = 0.0;       // analysis-window duration (s)
    int    periods = 0;         // whole f0 periods in the window (0 if f0 unset)
    // Fundamental: bin with the largest magnitude excluding DC (index 0).
    double peakFreq = 0.0;
    double peakMag  = 0.0;

    bool empty() const { return freq.empty(); }
};

namespace detail {

constexpr double kPi = 3.14159265358979323846;

// In-place iterative radix-2 FFT (n must be a power of two).
inline void fftRadix2(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / (double)len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cwr = 1.0, cwi = 0.0;              // running twiddle factor
            const size_t half = len >> 1;
            for (size_t k = 0; k < half; ++k) {
                const double vr = re[i + k + half] * cwr - im[i + k + half] * cwi;
                const double vi = re[i + k + half] * cwi + im[i + k + half] * cwr;
                const double ur = re[i + k], ui = im[i + k];
                re[i + k]        = ur + vr;  im[i + k]        = ui + vi;
                re[i + k + half] = ur - vr;  im[i + k + half] = ui - vi;
                const double nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
}

inline double windowSample(Window w, size_t i, size_t n) {
    if (n <= 1) return 1.0;
    const double x = (double)i / (double)(n - 1);           // 0..1
    switch (w) {
        case Window::Hann:     return 0.5 - 0.5 * std::cos(2.0 * kPi * x);
        case Window::Hamming:  return 0.54 - 0.46 * std::cos(2.0 * kPi * x);
        case Window::Blackman: return 0.42 - 0.5 * std::cos(2.0 * kPi * x)
                                          + 0.08 * std::cos(4.0 * kPi * x);
        case Window::Rectangular:
        default:               return 1.0;
    }
}

} // namespace detail

// Compute a one-sided amplitude spectrum from time/value sample arrays.
// t must be non-decreasing. Returns an empty Spectrum when there is too little
// data (fewer than 4 samples) or a degenerate time span.
inline Spectrum compute(const std::vector<double>& t,
                        const std::vector<double>& y,
                        const Options& opt) {
    Spectrum out;
    const int n = (int)std::min(t.size(), y.size());
    if (n < 4) return out;

    const double t0 = t.front();
    const double t1 = t[n - 1];
    const double fullDur = t1 - t0;
    if (fullDur <= 0.0) return out;

    // ── Analysis window [wStart, wEnd] ──────────────────────────────────────
    // With a user fundamental, snap to the most recent whole number of periods
    // (coherent sampling): the window ends at the latest sample and spans P·T0.
    double wStart = t0, wEnd = t1;
    int    periods = 0;
    if (opt.f0 > 0.0) {
        const double period = 1.0 / opt.f0;
        int P = (int)std::floor(fullDur / period);
        if (P >= 1) {
            periods = P;
            wEnd    = t1;
            wStart  = t1 - (double)P * period;
            if (wStart < t0) wStart = t0;
        }
    }
    const double dur = wEnd - wStart;
    if (dur <= 0.0) return out;

    // ── Number of uniform time samples (Ntime) fed to the transform ─────────
    int Ntime;
    if (opt.nfft > 0) {
        Ntime = 1;
        while (Ntime < opt.nfft) Ntime <<= 1;   // round up to power of two
    } else {
        Ntime = 0;
        for (double tv : t) if (tv >= wStart && tv <= wEnd) ++Ntime;
        if (Ntime < 4) Ntime = n;
        if (opt.f0 > 0.0) {
            // Coherent mode: resample to the NEXT power of two instead of
            // zero-padding after windowing. With padding the bin grid becomes
            // fs/nfft != 1/dur, so the harmonics k·f0 = k·P/dur no longer land
            // on bins — the Dirichlet kernel is then sampled off-peak and the
            // harmonic amplitude reads low (measured ~14% low for a 5-period
            // window padded 50000→65536). Resampling keeps df = 1/dur exactly,
            // putting every harmonic dead on a bin.
            int p2 = 1;
            while (p2 < Ntime) p2 <<= 1;
            Ntime = p2;
        }
    }
    if (Ntime < 4) Ntime = 4;

    // Uniform grid spans [wStart, wEnd) with spacing dur/Ntime (the endpoint is
    // excluded so that one period maps to a whole number of samples — the right
    // convention for periodic signals and coherent sampling).
    const double dtG = dur / (double)Ntime;
    const double fs  = 1.0 / dtG;               // = Ntime / dur

    // Resample onto the uniform grid via linear interpolation. t is sorted, so
    // advance a single cursor rather than searching per sample.
    std::vector<double> u(Ntime);
    int idx = 0;
    for (int k = 0; k < Ntime; ++k) {
        const double tk = wStart + (double)k * dtG;
        while (idx + 1 < n && t[idx + 1] < tk) ++idx;
        if (idx + 1 >= n) {
            u[k] = y[n - 1];
        } else {
            const double span = t[idx + 1] - t[idx];
            const double f = span > 0.0 ? (tk - t[idx]) / span : 0.0;
            u[k] = y[idx] + (y[idx + 1] - y[idx]) * f;
        }
    }

    if (opt.removeDc) {
        double mean = 0.0;
        for (double v : u) mean += v;
        mean /= (double)Ntime;
        for (double& v : u) v -= mean;
    }

    // Apply window and accumulate coherent gain (sum of window weights).
    double sumW = 0.0;
    std::vector<double> re(Ntime), im(Ntime, 0.0);
    for (int k = 0; k < Ntime; ++k) {
        const double w = detail::windowSample(opt.win, (size_t)k, (size_t)Ntime);
        sumW += w;
        re[k] = u[k] * w;
    }
    if (sumW <= 0.0) sumW = (double)Ntime;

    // Transform length: when nfft is requested Ntime is already a power of two
    // (no padding, so coherence is preserved); otherwise zero-pad to next pow2.
    size_t nfft = 1;
    while (nfft < (size_t)Ntime) nfft <<= 1;
    re.resize(nfft, 0.0);
    im.resize(nfft, 0.0);

    detail::fftRadix2(re, im);

    const size_t half = nfft / 2;
    out.fs      = fs;
    out.df      = fs / (double)nfft;
    out.n       = Ntime;
    out.nfft    = (int)nfft;
    out.winDur  = dur;
    out.periods = periods;
    out.freq.resize(half + 1);
    out.mag.resize(half + 1);
    for (size_t k = 0; k <= half; ++k) {
        const double m = std::sqrt(re[k] * re[k] + im[k] * im[k]) / sumW;
        // One-sided amplitude: interior bins carry both ±freq halves.
        const double amp = (k == 0 || k == half) ? m : 2.0 * m;
        out.freq[k] = (double)k * out.df;
        out.mag[k]  = amp;
        if (k > 0 && amp > out.peakMag) {
            out.peakMag  = amp;
            out.peakFreq = out.freq[k];
        }
    }
    return out;
}

} // namespace fft
