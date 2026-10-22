// Offline verification of the bass engine. No JUCE, no input files: every test signal is
// synthesised here and measured with a windowed DFT at the frequencies of interest.
//
//   bassifier_eval
//       Runs all checks and exits non-zero if any fails.
//
//   bassifier_eval --render in.wav out.wav [freq=100 intensity=0 original=1 gain=0 decay=0.3 ratio=0.8 response=20]
//       Processes a file (PCM 16/24-bit or float32, mono or stereo) for listening. Output is float32.
//
//   bassifier_eval --sine out.wav [hz=50 db=-12 sr=48000] [same parameters]
//       Writes a before/after demo: 3 s of the dry sine, then 3 s processed.

#include "../Source/dsp/BassEngine.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace bassifier;

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 512;
constexpr double kSettleSec = 0.5, kMeasureSec = 2.0;

using Params = BassEngine::Params;
using Channels = std::vector<std::vector<float>>;

//==============================================================================
// WAV I/O (mono/stereo PCM 16/24/32 and float32 in, float32 out)

struct Audio
{
    double sampleRate = 48000.0;
    Channels channels;
    int numSamples() const { return channels.empty() ? 0 : (int) channels[0].size(); }
};

bool writeWav (const std::string& path, const Audio& audio)
{
    std::ofstream out (path, std::ios::binary);
    if (! out)
        return false;

    const uint32_t numCh = (uint32_t) audio.channels.size();
    const uint32_t dataBytes = (uint32_t) audio.numSamples() * numCh * 4;
    const auto u32 = [&] (uint32_t v) { out.write ((const char*) &v, 4); };
    const auto u16 = [&] (uint16_t v) { out.write ((const char*) &v, 2); };

    out.write ("RIFF", 4); u32 (36 + dataBytes); out.write ("WAVEfmt ", 8);
    u32 (16); u16 (3); u16 ((uint16_t) numCh); u32 ((uint32_t) audio.sampleRate);
    u32 ((uint32_t) audio.sampleRate * numCh * 4); u16 ((uint16_t) (numCh * 4)); u16 (32);
    out.write ("data", 4); u32 (dataBytes);

    for (int n = 0; n < audio.numSamples(); ++n)
        for (uint32_t ch = 0; ch < numCh; ++ch)
            out.write ((const char*) &audio.channels[ch][(size_t) n], 4);

    return (bool) out;
}

bool readWav (const std::string& path, Audio& audio)
{
    std::ifstream in (path, std::ios::binary);
    std::vector<char> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
        return false;

    const auto u16 = [&] (size_t p) { uint16_t v; std::memcpy (&v, bytes.data() + p, 2); return v; };
    const auto u32 = [&] (size_t p) { uint32_t v; std::memcpy (&v, bytes.data() + p, 4); return v; };

    uint16_t format = 0, numCh = 0, bits = 0;

    for (size_t pos = 12; pos + 8 <= bytes.size();)
    {
        const std::string id (bytes.data() + pos, 4);
        const size_t size = u32 (pos + 4), body = pos + 8;

        if (id == "fmt ")
        {
            format = u16 (body);
            numCh = u16 (body + 2);
            audio.sampleRate = u32 (body + 4);
            bits = u16 (body + 14);
            if (format == 0xFFFE && size >= 26)
                format = u16 (body + 24);
        }
        else if (id == "data" && numCh > 0)
        {
            const size_t bytesPer = bits / 8;
            const size_t frames = std::min (size, bytes.size() - body) / (bytesPer * numCh);
            audio.channels.assign (numCh, std::vector<float> (frames));

            for (size_t n = 0; n < frames; ++n)
                for (size_t ch = 0; ch < numCh; ++ch)
                {
                    const char* p = bytes.data() + body + (n * numCh + ch) * bytesPer;
                    float v = 0.0f;

                    if (format == 3 && bits == 32)      std::memcpy (&v, p, 4);
                    else if (bits == 16)                { int16_t s; std::memcpy (&s, p, 2); v = s / 32768.0f; }
                    else if (bits == 24)                { int32_t s = ((int32_t) (int8_t) p[2] << 16) | ((uint8_t) p[1] << 8) | (uint8_t) p[0]; v = s / 8388608.0f; }
                    else if (bits == 32)                { int32_t s; std::memcpy (&s, p, 4); v = (float) (s / 2147483648.0); }
                    else                                return false;

                    audio.channels[ch][n] = v;
                }
            return true;
        }

        pos = body + size + (size & 1);
    }

    return false;
}

//==============================================================================
// Signals and measurement

float dbToGain (double db) { return (float) std::pow (10.0, db / 20.0); }
double gainToDb (double g) { return 20.0 * std::log10 (std::max (g, 1.0e-12)); }

std::vector<float> sine (double sr, double hz, double db, double seconds)
{
    std::vector<float> x ((size_t) (sr * seconds));
    const float a = dbToGain (db);
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = a * (float) std::sin (2.0 * kPi * hz * (double) n / sr);
    return x;
}

std::vector<float> square (double sr, double hz, double db, double seconds)
{
    std::vector<float> x ((size_t) (sr * seconds));
    const float a = dbToGain (db);
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = std::fmod (hz * (double) n / sr, 1.0) < 0.5 ? a : -a;
    return x;
}

std::vector<float> noise (double sr, double db, double seconds, uint32_t seed)
{
    std::vector<float> x ((size_t) (sr * seconds));
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    const float a = dbToGain (db);
    for (auto& v : x)
        v = a * dist (rng);
    return x;
}

std::vector<float> silence (double sr, double seconds) { return std::vector<float> ((size_t) (sr * seconds), 0.0f); }

std::vector<float> concat (std::vector<float> a, const std::vector<float>& b)
{
    a.insert (a.end(), b.begin(), b.end());
    return a;
}

/** Runs the engine over the channels in kBlock-sized blocks, setting the params before each. */
Channels run (const Params& p, double sr, Channels in)
{
    BassEngine engine;
    engine.prepare (sr, kBlock);
    engine.setParams (p);

    const int numCh = (int) in.size(), total = (int) in[0].size();
    float* ptrs[BassEngine::kMaxChannels];

    for (int pos = 0; pos < total; pos += kBlock)
    {
        for (int ch = 0; ch < numCh; ++ch)
            ptrs[ch] = in[(size_t) ch].data() + pos;
        engine.setParams (p);
        engine.process (ptrs, numCh, std::min (kBlock, total - pos));
    }

    return in;
}

/** Amplitude (1.0 = 0 dBFS) of the component at hz, Hann-windowed DFT over [start, start + len). */
double toneDb (const std::vector<float>& x, double sr, double hz, double startSec = kSettleSec, double lenSec = kMeasureSec)
{
    const size_t start = (size_t) (startSec * sr);
    const size_t len = std::min ((size_t) (lenSec * sr), x.size() - start);
    const std::complex<double> step = std::polar (1.0, -2.0 * kPi * hz / sr);
    std::complex<double> phasor (1.0, 0.0), sum (0.0, 0.0);
    double wsum = 0.0;

    for (size_t n = 0; n < len; ++n)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) n / (double) len);
        sum += w * (double) x[start + n] * phasor;
        wsum += w;
        phasor *= step;
    }

    return gainToDb (2.0 * std::abs (sum) / wsum);
}

double peakDb (const std::vector<float>& x, double startSec, double endSec, double sr)
{
    double peak = 0.0;
    for (size_t n = (size_t) (startSec * sr); n < std::min (x.size(), (size_t) (endSec * sr)); ++n)
        peak = std::max (peak, (double) std::abs (x[n]));
    return gainToDb (peak);
}

bool allFinite (const Channels& chs)
{
    for (const auto& c : chs)
        for (float v : c)
            if (! std::isfinite (v))
                return false;
    return true;
}

/** Least-squares slope of y against x. */
double slope (const std::vector<double>& x, const std::vector<double>& y)
{
    double mx = 0.0, my = 0.0;
    for (size_t i = 0; i < x.size(); ++i) { mx += x[i]; my += y[i]; }
    mx /= (double) x.size(); my /= (double) y.size();
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < x.size(); ++i) { num += (x[i] - mx) * (y[i] - my); den += (x[i] - mx) * (x[i] - mx); }
    return num / den;
}

//==============================================================================
// Checks

struct Report
{
    int failures = 0;

    void check (bool ok, const char* fmt, ...)
    {
        va_list args;
        va_start (args, fmt);
        std::printf ("  [%s] ", ok ? "PASS" : "FAIL");
        std::vprintf (fmt, args);
        std::printf ("\n");
        va_end (args);
        if (! ok)
            ++failures;
    }
};

Params generatorOnly (float freq = 120.0f)
{
    Params p;
    p.freqHz = freq;
    p.originalBass = false;
    p.intensityDb = 0.0f;
    return p;
}

/** Magnitude of the LR4 high-pass at f for cutoff fc: what the crossover lets through of a tone below it. */
double lr4HighPassDb (double f, double fc)
{
    const double r4 = std::pow (f / fc, 4.0);
    return gainToDb (r4 / (1.0 + r4));
}

void checkHarmonics (Report& rep, double sr)
{
    std::printf ("\n1. Harmonic generation @ %.0f Hz (freq = 120, Original Bass out, Intensity 0 dB, input -12 dBFS)\n", sr);
    const double inDb = -12.0;

    for (double f0 : { 50.0, 80.0, 100.0 })
    {
        const Params p = generatorOnly();
        const auto out = run (p, sr, { sine (sr, f0, inDb, kSettleSec + kMeasureSec) })[0];

        const double fundDb = toneDb (out, sr, f0) - inDb;
        const double crossoverLeak = lr4HighPassDb (f0, p.freqHz);

        std::printf ("  %3.0f Hz: fundamental %+6.1f dB (LR4 crossover alone leaks %+.1f dB); harmonics re input:", f0, fundDb, crossoverLeak);
        std::vector<double> harm (11, -200.0);
        for (int k = 2; k <= 10; ++k)
        {
            harm[(size_t) k] = toneDb (out, sr, k * f0) - inDb;
            std::printf (" %d:%+.1f", k, harm[(size_t) k]);
        }
        std::printf ("\n");

        // The crossover's own high-pass passes the fundamental too (LR4 is -30.7 dB at 50/120 Hz
        // and -9.8 dB at 100/120), so a fixed -40 dB target is not meaningful: what is checked is
        // that the generator adds no more than 3 dB on top of that leak (its OUT-HPF stopband is
        // -40 dB, times the compressor's control signal).
        rep.check (fundDb <= crossoverLeak + 3.0, "%3.0f Hz fundamental %+.1f dB: generator adds %.1f dB to the crossover's %+.1f dB leak (<= 3 dB)",
                   f0, fundDb, fundDb - crossoverLeak, crossoverLeak);

        // Three consecutive harmonics above the cutoff, each within 30 dB of the strongest (the
        // patent's "at least three consecutive harmonics"). Each pass round the loop multiplies
        // by CS * g * kLoopDrive * A / 2, so this is what kLoopDrive is set for.
        const int first = (int) std::ceil (p.freqHz / f0 - 1.0e-9);
        double strongest = -200.0;
        for (int k = first; k < first + 3; ++k)
            strongest = std::max (strongest, harm[(size_t) k]);
        bool within = true;
        for (int k = first; k < first + 3; ++k)
            within = within && harm[(size_t) k] >= strongest - 30.0;
        rep.check (within, "%3.0f Hz harmonics %d..%d at %+.1f / %+.1f / %+.1f dB: each within 30 dB of the strongest", f0, first, first + 2,
                   harm[(size_t) first], harm[(size_t) first + 1], harm[(size_t) first + 2]);
    }

    // The operating point: richness against input level (the loop multiplies by its input,
    // so the series gets steeper as the level drops, and the loop-gain cap pins it at the top).
    std::printf ("  info: per-order decay vs level, decay 0.3 (first harmonic above the cutoff, +1, +2, re input; pinned = loop-gain cap active)\n");
    for (double f0 : { 80.0, 50.0 })
        for (double level : { -30.0, -18.0, -6.0, 0.0 })
        {
            const Params p = generatorOnly();
            const auto out = run (p, sr, { sine (sr, f0, level, kSettleSec + kMeasureSec) })[0];
            const int first = (int) std::ceil (p.freqHz / f0 - 1.0e-9);
            const double a = toneDb (out, sr, first * f0) - level, b = toneDb (out, sr, (first + 1) * f0) - level, c = toneDb (out, sr, (first + 2) * f0) - level;
            std::printf ("  info: %2.0f Hz at %3.0f dBFS: %+6.1f / %+6.1f / %+6.1f dB, per order %4.1f, %4.1f dB (harmonic %d at %.1f dBFS)\n",
                         f0, level, a, b, c, a - b, b - c, first, a + level);
        }
}

double measureSlope (double sr, double f0, int harmonic, float ratio, double topDb = -6.0, std::vector<double>* levelsOut = nullptr)
{
    std::vector<double> ins, outs;
    for (double inDb = -42.0; inDb <= topDb + 1.0e-9; inDb += 6.0)
    {
        Params p = generatorOnly();
        p.ratio = ratio;
        const auto out = run (p, sr, { sine (sr, f0, inDb, kSettleSec + kMeasureSec) })[0];
        ins.push_back (inDb);
        outs.push_back (toneDb (out, sr, harmonic * f0));
    }
    if (levelsOut != nullptr)
        *levelsOut = outs;
    return slope (ins, outs);
}

void checkLoudness (Report& rep, double sr)
{
    std::printf ("\n2. Loudness matching @ %.0f Hz (freq = 120, input -42..-6 dBFS in 6 dB steps)\n", sr);

    struct Case { double f0; int harmonic; double target, tol; };
    for (const Case& c : { Case { 80.0, 2, 1.34, 0.2 }, Case { 50.0, 3, 1.74, 0.35 } })
    {
        std::vector<double> levels;
        const double s = measureSlope (sr, c.f0, c.harmonic, 0.8f, -6.0, &levels);
        const double sLow = measureSlope (sr, c.f0, c.harmonic, 0.8f, -12.0);

        // With r = 1 there is no compression, so the loop gain is g * kLoopDrive * A = 4.2 A and
        // the loop-gain cap takes over above about -14 dBFS: the pure multiplier expansion only
        // exists below that, so it is measured over -42..-18 dBFS (full range printed as well).
        const double s1 = measureSlope (sr, c.f0, c.harmonic, 1.0f, -18.0);
        const double s1Full = measureSlope (sr, c.f0, c.harmonic, 1.0f, -6.0);

        std::printf ("  %2.0f Hz -> %d%s harmonic (%3.0f Hz) levels r=0.8:", c.f0, c.harmonic, c.harmonic == 2 ? "nd" : "rd", c.harmonic * c.f0);
        for (double l : levels)
            std::printf (" %.1f", l);
        std::printf (" dBFS\n");

        rep.check (std::abs (s - c.target) <= c.tol, "%2.0f Hz: slope of %3.0f Hz = %.2f dB/dB with r = 0.8 over -42..-6 (target %.2f +- %.2f; %.2f over -42..-12, before the cap)",
                   c.f0, c.harmonic * c.f0, s, c.target, c.tol, sLow);
        rep.check (std::abs (s1 - c.harmonic) <= 0.25, "%2.0f Hz: slope of %3.0f Hz = %.2f dB/dB with r = 1.0 over -42..-18 (expect ~%d: pure multiplier expansion; %.2f over -42..-6, cap pins the top)",
                   c.f0, c.harmonic * c.f0, s1, c.harmonic, s1Full);
    }
}

void checkTransparency (Report& rep, double sr)
{
    std::printf ("\n3. Transparency @ %.0f Hz (freq = 100, Original Bass in, Intensity -24 dB, Gain 0 dB)\n", sr);
    Params p;
    p.intensityDb = -24.0f;
    const double inDb = -6.0;
    bool ok = true;
    std::printf ("  deviation:");

    for (double hz : { 30.0, 60.0, 120.0, 250.0, 1000.0, 5000.0, 10000.0 })
    {
        const auto out = run (p, sr, { sine (sr, hz, inDb, kSettleSec + kMeasureSec) })[0];
        const double dev = toneDb (out, sr, hz) - inDb;
        std::printf (" %.0f:%+.2f", hz, dev);
        ok = ok && std::abs (dev) <= 0.5;
    }
    std::printf (" dB\n");
    rep.check (ok, "all tones pass within +-0.5 dB");
}

void checkStability (Report& rep)
{
    const double sr = 48000.0;
    std::printf ("\n4. Stability (0 dBFS 40 Hz square + 0 dBFS noise, freq = 256, decay = 0.6, ratio = 0.5, Intensity +24 dB)\n");

    Params p;
    p.freqHz = 256.0f;
    p.decay = 0.6f;
    p.ratio = 0.5f;
    p.intensityDb = 24.0f;

    auto in = square (sr, 40.0, 0.0, 5.0);
    const auto nz = noise (sr, 0.0, 5.0, 1234);
    for (size_t n = 0; n < in.size(); ++n)
        in[n] += nz[n];
    in = concat (in, silence (sr, 1.5));

    const auto out = run (p, sr, { in, in });
    const double peak = peakDb (out[0], 0.0, 5.0, sr);
    const double tail = peakDb (out[0], 6.0, 6.5, sr);
    double dc = 0.0;
    for (size_t n = (size_t) (6.0 * sr); n < out[0].size(); ++n)
        dc += out[0][n];
    dc /= 0.5 * sr;

    rep.check (allFinite (out), "no NaN / Inf");
    rep.check (peak < 24.0, "output peak %+.1f dBFS < +24 dBFS", peak);
    rep.check (tail < -90.0, "1 s into the silence the output is %.1f dBFS < -90 dBFS (DC %.1e)", tail, dc);

    Params invalid;
    invalid.freqHz = invalid.intensityDb = invalid.outputGainDb = invalid.decay = invalid.ratio = invalid.responseMs
        = std::numeric_limits<float>::quiet_NaN();
    auto recovered = run (invalid, sr, { sine (sr, 60.0, -12.0, 0.25) });
    rep.check (allFinite (recovered), "nonfinite parameters fall back to safe defaults");

    auto poisoned = sine (sr, 60.0, -12.0, 0.25);
    poisoned[100] = std::numeric_limits<float>::infinity();
    poisoned[101] = std::numeric_limits<float>::quiet_NaN();
    recovered = run (Params {}, sr, { std::move (poisoned) });
    rep.check (allFinite (recovered), "nonfinite input cannot poison recursive state");

    BassEngine invalidRateEngine;
    invalidRateEngine.prepare (std::numeric_limits<double>::quiet_NaN(), kBlock);
    auto invalidRateAudio = sine (sr, 60.0, -12.0, 0.25);
    float* invalidRateChannel = invalidRateAudio.data();
    invalidRateEngine.process (&invalidRateChannel, 1, (int) invalidRateAudio.size());
    rep.check (allFinite ({ invalidRateAudio }), "invalid sample rate falls back to 48 kHz");
}

void checkStereo (Report& rep)
{
    const double sr = 48000.0;
    std::printf ("\n5. Stereo (defaults, freq = 100)\n");
    Params p;

    auto out = run (p, sr, { sine (sr, 60.0, -12.0, kSettleSec + kMeasureSec), silence (sr, kSettleSec + kMeasureSec) });
    const double l = toneDb (out[0], sr, 120.0), r = toneDb (out[1], sr, 120.0);
    rep.check (std::abs (l - r) <= 0.1, "L-only 60 Hz: 2nd harmonic L %.2f dBFS, R %.2f dBFS (diff %.3f dB)", l, r, l - r);

    out = run (p, sr, { sine (sr, 5000.0, 0.0, kSettleSec + kMeasureSec), silence (sr, kSettleSec + kMeasureSec) });
    const double rPeak = peakDb (out[1], kSettleSec, kSettleSec + kMeasureSec, sr);
    rep.check (rPeak < -80.0, "L-only 0 dBFS 5 kHz: R peak %.1f dBFS < -80 dBFS", rPeak);
}

void checkPerformance (Report& rep)
{
    const double sr = 48000.0;
    std::printf ("\n6. Performance (60 s stereo noise @ 48 kHz, %d-sample blocks)\n", kBlock);
    Channels in { noise (sr, -6.0, 60.0, 1), noise (sr, -6.0, 60.0, 2) };
    Params p;

    const auto t0 = std::chrono::steady_clock::now();
    const auto out = run (p, sr, std::move (in));
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

    rep.check (seconds < 1.0 && allFinite (out), "%.3f s wall time (%.0fx realtime)", seconds, 60.0 / seconds);
}

int runChecks()
{
    Report rep;
    std::printf ("Bassifier DSP evaluation\n");

    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        checkHarmonics (rep, sr);
        checkTransparency (rep, sr);
    }
    checkLoudness (rep, 48000.0);
    checkStability (rep);
    checkStereo (rep);
    checkPerformance (rep);

    std::printf ("\n%s: %d failure(s)\n", rep.failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", rep.failures);
    return rep.failures == 0 ? 0 : 1;
}

//==============================================================================
// Command line modes

struct Options
{
    Params params;
    double hz = 50.0, db = -12.0, sr = 48000.0;

    bool parse (int argc, char** argv, int from)
    {
        for (int i = from; i < argc; ++i)
        {
            const std::string arg = argv[i];
            const auto eq = arg.find ('=');
            if (eq == std::string::npos)
            {
                std::fprintf (stderr, "expected key=value, got '%s'\n", argv[i]);
                return false;
            }
            const std::string key = arg.substr (0, eq);
            const float v = std::strtof (arg.c_str() + eq + 1, nullptr);

            if (key == "freq")           params.freqHz = v;
            else if (key == "intensity") params.intensityDb = v;
            else if (key == "original")  params.originalBass = v != 0.0f;
            else if (key == "gain")      params.outputGainDb = v;
            else if (key == "decay")     params.decay = v;
            else if (key == "ratio")     params.ratio = v;
            else if (key == "response")  params.responseMs = v;
            else if (key == "hz")        hz = v;
            else if (key == "db")        db = v;
            else if (key == "sr")        sr = v;
            else
            {
                std::fprintf (stderr, "unknown parameter '%s'\n", key.c_str());
                return false;
            }
        }
        return true;
    }
};

int renderFile (int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf (stderr, "usage: --render in.wav out.wav [key=value ...]\n");
        return 2;
    }
    Options opt;
    if (! opt.parse (argc, argv, 4))
        return 2;

    Audio audio;
    if (! readWav (argv[2], audio) || audio.channels.empty())
    {
        std::fprintf (stderr, "cannot read %s\n", argv[2]);
        return 2;
    }
    if (audio.channels.size() > (size_t) BassEngine::kMaxChannels)
        audio.channels.resize ((size_t) BassEngine::kMaxChannels);

    const auto peakOf = [] (const Channels& chs)
    {
        double peak = 0.0;
        for (const auto& c : chs)
            for (float v : c)
                peak = std::max (peak, (double) std::abs (v));
        return gainToDb (peak);
    };
    const double inPeak = peakOf (audio.channels);
    audio.channels = run (opt.params, audio.sampleRate, std::move (audio.channels));
    const double outPeak = peakOf (audio.channels);
    int over = 0;
    for (const auto& c : audio.channels)
        for (float v : c)
            over += std::abs (v) > 1.0f;

    if (! writeWav (argv[3], audio))
    {
        std::fprintf (stderr, "cannot write %s\n", argv[3]);
        return 2;
    }
    std::printf ("rendered %d samples @ %.0f Hz, %zu channel(s) -> %s: peak in %.1f dBFS, out %.1f dBFS, %d sample(s) over 0 dBFS\n",
                 audio.numSamples(), audio.sampleRate, audio.channels.size(), argv[3], inPeak, outPeak, over);
    return 0;
}

int renderSine (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: --sine out.wav [hz=50 db=-12 sr=48000 key=value ...]\n");
        return 2;
    }
    Options opt;
    if (! opt.parse (argc, argv, 3))
        return 2;

    const auto dry = sine (opt.sr, opt.hz, opt.db, 3.0);
    const auto wet = run (opt.params, opt.sr, { dry })[0];

    Audio audio;
    audio.sampleRate = opt.sr;
    audio.channels = { concat (dry, wet) };
    if (! writeWav (argv[2], audio))
    {
        std::fprintf (stderr, "cannot write %s\n", argv[2]);
        return 2;
    }
    std::printf ("wrote %s: 3 s dry %.0f Hz at %.0f dBFS, then 3 s processed\n", argv[2], opt.hz, opt.db);
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc >= 2 && std::strcmp (argv[1], "--render") == 0)
        return renderFile (argc, argv);
    if (argc >= 2 && std::strcmp (argv[1], "--sine") == 0)
        return renderSine (argc, argv);
    return runChecks();
}
