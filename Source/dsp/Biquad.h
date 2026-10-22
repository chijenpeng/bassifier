#pragma once

#include <algorithm>
#include <cmath>

namespace bassifier
{

/**
    Second-order section (RBJ cookbook), transposed direct form II.

    Coefficients and states are double: the crossover goes down to 32 Hz and up to 192 kHz,
    where a float state loses the low end to rounding (the poles sit within 1e-3 of the unit
    circle). The audio in and out is float.
*/
class Biquad
{
public:
    static constexpr double kButterworthQ = 0.7071067811865476;

    void setLowPass (double sampleRate, double freq, double q = kButterworthQ) noexcept { set (sampleRate, freq, q, false); }
    void setHighPass (double sampleRate, double freq, double q = kButterworthQ) noexcept { set (sampleRate, freq, q, true); }

    /** Any analog-prototype section bilinear-transformed by the caller; a0 need not be 1. */
    void setCoefficients (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
        a1 = na1 / na0; a2 = na2 / na0;
    }

    void reset() noexcept { z1 = z2 = 0.0; }

    float process (float x) noexcept
    {
        const double in = x;
        const double y = b0 * in + z1;
        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;
        return (float) y;
    }

private:
    void set (double sampleRate, double freq, double q, bool highPass) noexcept
    {
        const double pi = 3.14159265358979323846;
        const double f = freq < sampleRate * 0.45 ? freq : sampleRate * 0.45;
        const double w = 2.0 * pi * f / sampleRate;
        const double cosw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;

        const double edge = highPass ? (1.0 + cosw) : (1.0 - cosw);
        b0 = edge * 0.5 / a0;
        b1 = (highPass ? -edge : edge) / a0;
        b2 = b0;
        a1 = -2.0 * cosw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
};

/**
    Inverse Chebyshev (type II) high-pass: flat passband, equiripple stopband at -stopbandDb.
    This is the shape of the patent's elliptic OUT-HPF (FIG. 8: a -40 dB floor that reaches up
    to about 0.7 of the cutoff), and what lets the compressor see the first harmonic above the
    cutoff rather than the fundamental below it. A Butterworth of the same order is 25 dB
    worse there. The closed-form design needs no elliptic integrals: the poles are the
    reciprocals of a Chebyshev I filter's, the zeros sit at j / cos (theta_k).
*/
template <int Order>
class ChebyshevIIHighPass
{
    static_assert (Order >= 2 && Order % 2 == 0, "even order only");

public:
    static constexpr int kSections = Order / 2;

    /** `cutoff` is the -3 dB point; the stopband starts at cutoff / stopbandRatio (stopbandDb). */
    void set (double sampleRate, double cutoff, double stopbandDb) noexcept
    {
        const double pi = 3.14159265358979323846;
        const double a = std::asinh (1.0 / epsilon (stopbandDb)) / Order;
        const double stopEdge = 2.0 * pi * std::min (cutoff, sampleRate * 0.45) / stopbandRatio (stopbandDb);
        const double c = 2.0 * sampleRate;

        for (int k = 0; k < kSections; ++k)
        {
            const double theta = (2.0 * k + 1.0) * pi / (2.0 * Order);
            // Chebyshev I pole s (normalised passband 1); the type II pole is 1 / s.
            const double sr = -std::sinh (a) * std::sin (theta), si = std::cosh (a) * std::cos (theta);
            const double sMag = std::sqrt (sr * sr + si * si);
            const double q = sMag / (2.0 * std::sinh (a) * std::sin (theta));

            // Low-pass -> high-pass (s -> stopEdge / s): pole radius stopEdge * |s|, zero at stopEdge * cos (theta).
            const double w0 = c * std::tan (stopEdge * sMag / c);
            const double wz = c * std::tan (stopEdge * std::cos (theta) / c);

            sections[k].setCoefficients (c * c + wz * wz, 2.0 * (wz * wz - c * c), c * c + wz * wz,
                                         c * c + w0 * c / q + w0 * w0, 2.0 * (w0 * w0 - c * c), c * c - w0 * c / q + w0 * w0);
        }
    }

    /** cutoff (-3 dB) divided by the stopband edge: 1.41 for a 6th order at 40 dB. */
    static double stopbandRatio (double stopbandDb) noexcept
    {
        return std::cosh (std::acosh (1.0 / epsilon (stopbandDb)) / Order);
    }

    void reset() noexcept
    {
        for (auto& s : sections)
            s.reset();
    }

    float process (float x) noexcept
    {
        for (auto& s : sections)
            x = s.process (x);
        return x;
    }

private:
    /** The Chebyshev ripple factor for a stopband this deep. */
    static double epsilon (double stopbandDb) noexcept
    {
        return 1.0 / std::sqrt (std::pow (10.0, stopbandDb / 10.0) - 1.0);
    }

    Biquad sections[kSections];
};

/**
    Fourth-order Linkwitz-Riley crossover: each side is two cascaded 2nd-order Butterworth
    sections. Low and high sum to a 2nd-order all-pass, so the split is flat in magnitude,
    which is what makes "Original Bass in, Intensity at minimum" transparent.
*/
class LinkwitzRiley4
{
public:
    void set (double sampleRate, double freq) noexcept
    {
        for (auto& s : lp) s.setLowPass (sampleRate, freq);
        for (auto& s : hp) s.setHighPass (sampleRate, freq);
    }

    void reset() noexcept
    {
        for (auto& s : lp) s.reset();
        for (auto& s : hp) s.reset();
    }

    void process (float x, float& low, float& high) noexcept
    {
        low = lp[1].process (lp[0].process (x));
        high = hp[1].process (hp[0].process (x));
    }

private:
    Biquad lp[2], hp[2];
};

} // namespace bassifier
