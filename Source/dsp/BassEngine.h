#pragma once

#include "Biquad.h"

#include <atomic>

namespace bassifier
{

/**
    Psychoacoustic bass enhancer after the recursive generator of US 5,930,373 FIG. 4.
    No latency.

    The input is split at `freqHz` with a 4th-order Linkwitz-Riley crossover. The low half,
    summed to mono, feeds a recursive harmonic generator: the low band is multiplied by a
    one-sample-delayed copy of its own output, so every pass around the loop turns the Nth
    harmonic into the (N+1)th. The loop's output is high-passed at `freqHz` and multiplied by
    an upward compressor's control signal, which is also applied inside the loop: this is
    what gives the harmonics the patent's "residue expansion ratio" (about 1.34 dB/dB for a
    dominant 2nd harmonic, 1.74 for a 3rd) instead of the multiplier's raw 2 and 3.

    Every constant that bounds the loop is commented where it is declared.
*/
class BassEngine
{
public:
    static constexpr int kMaxChannels = 2;

    struct Params
    {
        float freqHz = 100.0f;        // "Freq": 32..256 Hz, crossover & generator cutoff (f2 = f3)
        float intensityDb = 0.0f;     // "Intensity": -24..+24 dB, gain on the generated harmonics
        bool  originalBass = true;    // "Original Bass In/Out"
        float outputGainDb = 0.0f;    // "Gain": -inf (<= -60 treat as mute)..0 dB
        // Advanced (not shown in the basic UI):
        float decay = 0.3f;           // loop gain g (0.05..0.6). Patent: 0.3
        float ratio = 0.8f;           // upward-compressor ratio r (0.5..1.0). Patent: 0.8; 1.0 = no compression
        float responseMs = 20.0f;     // compressor envelope release (1..200 ms); attack is capped, see kEnvAttackMaxMs
    };

    /** Per-block absolute peaks, linear. */
    struct Meters
    {
        float originalBassPeak = 0.0f;   // the mono low band that feeds the generator
        float harmonicsPeak = 0.0f;      // the harmonics as added to the output (after Intensity)
        float outputPeak = 0.0f;         // max over channels, after the output gain
        bool clipped = false;            // outputPeak > 1
    };

    // Timing
    static constexpr float kRampMs = 20.0f;             // Intensity / Gain / Original Bass ramps
    static constexpr float kFreqSmoothMs = 30.0f;       // per-block one-pole on Freq before the coefficients
    static constexpr float kControlSmoothMs = 2.0f;     // smoothing of the compressor control signal (patent's CS)
    static constexpr float kEnvAttackMaxMs = 2.0f;      // envelope attack never slower than this: see the comments in BassEngine.cpp
    static constexpr float kLoopPeakReleaseMs = 50.0f;  // release of the low-band peak that caps the loop gain

    // Operating point. The loop multiplies by its input, so the harmonic series it makes is
    // as rich as the input is hot: the patent's circuit has an operating level, which the
    // fixed constants below set. Both are plain constants, no envelope anywhere in them.
    static constexpr float kLoopDrive = 14.0f;          // fixed gain into the generator only (not the Original Bass path)
    static constexpr float kMakeupDb = -11.0f;          // fixed gain after the generator, before Intensity

    // Compressor
    static constexpr float kEref = 0.1f;                // envelope at which the control signal is 1 (-20 dBFS)
    static constexpr float kEnvFloor = 1.0e-6f;         // -120 dBFS: keeps pow() off zero
    static constexpr float kControlMax = 7.943f;        // +18 dB: silence does not wind the loop up
    static constexpr float kControlMin = 1.0f / 7.943f; // -18 dB: loud input does not kill it

    // Loop bounds
    static constexpr float kMaxLoopGain = 0.8f;         // cap on CS * g * |lf|: per-circulation gain of the loop
    static constexpr float kMaxFeedback = 4.0f;         // |fb| clamp; = kMaxLoopGain / (1 - kMaxLoopGain)

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /** Cheap, no allocation: meant to be called from the audio thread before process(). */
    void setParams (const Params& p) noexcept;

    /** In place, 1 or 2 channels. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** Peaks of the last processed block. Safe to read from another thread. */
    Meters getMeters() const noexcept;

    int getLatencySamples() const noexcept { return 0; }

private:
    /** Linear ramp to a target over a fixed number of samples. */
    struct Ramp
    {
        float value = 0.0f, target = 0.0f, step = 0.0f;
        int remaining = 0;

        void setTarget (float t, int samples) noexcept
        {
            if (t == target)
                return;
            target = t;
            remaining = samples > 1 ? samples : 1;
            step = (t - value) / (float) remaining;
        }

        void snap() noexcept { value = target; remaining = 0; }

        float next() noexcept
        {
            if (remaining > 0)
            {
                value += step;
                if (--remaining == 0)
                    value = target;
            }
            return value;
        }
    };

    void updateFilters (int numSamples) noexcept;
    void resetLoop() noexcept;
    float generate (float lf) noexcept;

    double sr = 48000.0;
    Params params;
    int rampSamples = 960;

    // Crossover, one per channel
    LinkwitzRiley4 crossover[kMaxChannels];
    float freqTarget = 100.0f, freqSmoothed = 100.0f, freqApplied = 0.0f;

    // Generator (FIG. 4 numbering in the comments of generate())
    Biquad fbFilter;            // FB-HPF (45), 2nd-order Butterworth, cutoff f1
    ChebyshevIIHighPass<8> outFilter;   // OUT-HPF (50), cutoff f3 = freqHz, 40 dB stopband
    ChebyshevIIHighPass<8> sideFilter;  // the compressor's own copy: see generate()
    static constexpr double kOutStopbandDb = 40.0;
    float loopGain = 0.3f;      // g (46)
    float ratioMinusOne = -0.2f;
    float fb = 0.0f;            // the one-sample delay (51)
    float env = kEnvFloor;      // compressor envelope E
    float control = 1.0f;       // smoothed control signal CS
    float lfPeak = 0.0f;        // peak of the low band, for the loop-gain cap
    float envAttack = 0.0f, envRelease = 0.0f, controlCoef = 0.0f, lfPeakDecay = 0.0f;

    Ramp intensity, outputGain, bassGain;
    float makeupGain = 1.0f;

    std::atomic<float> meterBass { 0.0f }, meterHarmonics { 0.0f }, meterOutput { 0.0f };
    std::atomic<bool> meterClipped { false };
};

} // namespace bassifier
