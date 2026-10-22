#include "BassEngine.h"

#include <algorithm>
#include <cmath>

namespace bassifier
{

namespace
{
    constexpr float kAntiDenormal = 1.0e-12f;   // -240 dBFS of DC keeps every filter state off the denormal range

    float dbToGain (float db) noexcept { return std::exp (db * 0.11512925f); }

    float finiteOr (float value, float fallback) noexcept
    {
        return std::isfinite (value) ? value : fallback;
    }

    float onePoleCoef (double sampleRate, float ms) noexcept
    {
        return 1.0f - std::exp (-1000.0f / (std::max (0.01f, ms) * (float) sampleRate));
    }
}

void BassEngine::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sr = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
    rampSamples = std::max (1, (int) (kRampMs * 0.001 * sr));
    controlCoef = onePoleCoef (sr, kControlSmoothMs);
    lfPeakDecay = 1.0f - onePoleCoef (sr, kLoopPeakReleaseMs);

    makeupGain = dbToGain (kMakeupDb);
    freqApplied = 0.0f;   // forces a coefficient update on the first block
    setParams (params);
    freqSmoothed = freqTarget;
    intensity.snap();
    outputGain.snap();
    bassGain.snap();
    reset();
}

void BassEngine::reset() noexcept
{
    for (auto& c : crossover)
        c.reset();
    resetLoop();
    meterBass.store (0.0f, std::memory_order_relaxed);
    meterHarmonics.store (0.0f, std::memory_order_relaxed);
    meterOutput.store (0.0f, std::memory_order_relaxed);
    meterClipped.store (false, std::memory_order_relaxed);
}

void BassEngine::resetLoop() noexcept
{
    fbFilter.reset();
    outFilter.reset();
    sideFilter.reset();
    fb = 0.0f;
    env = kEnvFloor;
    control = 1.0f;
    lfPeak = 0.0f;
}

void BassEngine::setParams (const Params& p) noexcept
{
    params = p;

    freqTarget = std::clamp (finiteOr (p.freqHz, 100.0f), 20.0f, 1000.0f);
    loopGain = std::clamp (finiteOr (p.decay, 0.3f), 0.0f, 0.6f);
    ratioMinusOne = std::clamp (finiteOr (p.ratio, 0.8f), 0.5f, 1.0f) - 1.0f;

    // Response is the release. The attack is capped so that a note's onset pulls the control
    // signal down before the loop has had time to run away on the high gain left over from
    // the silence before it.
    const float response = std::clamp (finiteOr (p.responseMs, 20.0f), 1.0f, 200.0f);
    envRelease = onePoleCoef (sr, response);
    envAttack = onePoleCoef (sr, std::min (response, kEnvAttackMaxMs));

    intensity.setTarget (dbToGain (std::clamp (finiteOr (p.intensityDb, 0.0f), -24.0f, 24.0f)), rampSamples);
    const float outputDb = finiteOr (p.outputGainDb, 0.0f);
    outputGain.setTarget (outputDb <= -60.0f ? 0.0f : dbToGain (std::min (outputDb, 0.0f)), rampSamples);
    bassGain.setTarget (p.originalBass ? 1.0f : 0.0f, rampSamples);
}

void BassEngine::updateFilters (int numSamples) noexcept
{
    // One-pole on Freq, stepped once per block, so a knob sweep does not click. Snap once
    // it is close so the coefficients stop being recomputed.
    const float coef = 1.0f - std::exp (-(float) numSamples / (kFreqSmoothMs * 0.001f * (float) sr));
    freqSmoothed += coef * (freqTarget - freqSmoothed);
    if (std::abs (freqSmoothed - freqTarget) < 0.001f * freqTarget)
        freqSmoothed = freqTarget;

    if (freqSmoothed == freqApplied)
        return;
    freqApplied = freqSmoothed;

    for (auto& c : crossover)
        c.set (sr, freqApplied);

    // f1: the feedback high-pass has to pass the fundamentals (down to the bottom of the low
    // band) but stop the DC and difference tones the multiplier makes. The patent's example
    // is f1 = 40 Hz for a 120 Hz band; a third of Freq keeps that proportion, 20 Hz is the
    // floor, and it never rises above half of Freq.
    const double f1 = std::min (std::max (freqApplied / 3.0, 20.0), freqApplied / 2.0);
    fbFilter.setHighPass (sr, f1);
    outFilter.set (sr, freqApplied, kOutStopbandDb);
    sideFilter.set (sr, freqApplied, kOutStopbandDb);
}

float BassEngine::generate (float lf) noexcept
{
    // (44) multiplier + (52) adder: the fundamental times last sample's feedback, plus the
    // fundamental itself so the loop keeps being fed.
    const float y = lf * (1.0f + fb);

    const float fbRaw = loopGain * fbFilter.process (y);   // (45) FB-HPF, (46) g
    const float h = outFilter.process (y);                 // (50) OUT-HPF

    // (48) upward compressor, FIG. 5: envelope of the output, C = (E / Eref)^(r - 1), smoothed.
    // The envelope must follow the first harmonic above the cutoff, which grows with the
    // square of the input, while the fundamental's leak through OUT-HPF grows linearly: at
    // low levels even a 40 dB stopband is not enough, so the detector has a second one.
    const float mag = std::abs (sideFilter.process (h));
    env += (mag > env ? envAttack : envRelease) * (mag - env);
    env = std::max (env, kEnvFloor);
    const float c = std::clamp (std::pow (env * (1.0f / kEref), ratioMinusOne), kControlMin, kControlMax);
    control += controlCoef * (c - control);

    // The loop multiplies by lf every circulation, so its gain is CS * g * |lf|: above 1 the
    // series x + g x^2 + g^2 x^3 ... diverges. The cap only ever bites on a loud onset, before
    // the envelope has caught up; at any steady level the compressor keeps the product well
    // below it.
    lfPeak = std::max (std::abs (lf), lfPeak * lfPeakDecay);
    const float cs = std::min (control, kMaxLoopGain / (loopGain * lfPeak + 1.0e-9f));

    // (47) dynamic gain into (51) the delay. The clamp is the hard stop for whatever the cap
    // and the high-pass overshoot let through; it is +14 dB above the loop's input.
    fb = std::clamp (cs * fbRaw, -kMaxFeedback, kMaxFeedback);
    if (std::abs (fb) < 1.0e-20f)
        fb = 0.0f;

    const float output = cs * h;
    if (! std::isfinite (fb) || ! std::isfinite (output))
    {
        resetLoop();
        return 0.0f;
    }

    return output;   // (53)
}

void BassEngine::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    numChannels = std::min (numChannels, kMaxChannels);
    if (numChannels <= 0 || numSamples <= 0)
        return;

    updateFilters (numSamples);

    const float monoScale = 1.0f / (float) numChannels;
    float bassPeak = 0.0f, harmonicsPeak = 0.0f, outputPeak = 0.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        float lfCh[kMaxChannels], hfCh[kMaxChannels];
        float lf = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float x = channels[ch][n];
            if (! std::isfinite (x))   // a host's NaN must not poison the filters for good
                x = 0.0f;
            crossover[ch].process (x + kAntiDenormal, lfCh[ch], hfCh[ch]);
            lf += lfCh[ch];
        }

        // The bass is treated as mono and the harmonics are added identically to both channels.
        lf *= monoScale;
        const float harmonics = intensity.next() * makeupGain * generate (lf * kLoopDrive);
        const float bass = bassGain.next();
        const float gain = outputGain.next();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float out = (hfCh[ch] + harmonics + bass * lfCh[ch]) * gain;
            channels[ch][n] = out;
            outputPeak = std::max (outputPeak, std::abs (out));
        }

        bassPeak = std::max (bassPeak, std::abs (lf));
        harmonicsPeak = std::max (harmonicsPeak, std::abs (harmonics));
    }

    meterBass.store (bassPeak, std::memory_order_relaxed);
    meterHarmonics.store (harmonicsPeak, std::memory_order_relaxed);
    meterOutput.store (outputPeak, std::memory_order_relaxed);
    meterClipped.store (outputPeak > 1.0f, std::memory_order_relaxed);
}

BassEngine::Meters BassEngine::getMeters() const noexcept
{
    Meters m;
    m.originalBassPeak = meterBass.load (std::memory_order_relaxed);
    m.harmonicsPeak = meterHarmonics.load (std::memory_order_relaxed);
    m.outputPeak = meterOutput.load (std::memory_order_relaxed);
    m.clipped = meterClipped.load (std::memory_order_relaxed);
    return m;
}

} // namespace bassifier
