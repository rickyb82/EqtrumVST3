#include "EqMath.h"

#include <algorithm>

namespace eqtrum
{
namespace eqmath
{
namespace
{
constexpr double pi = 3.14159265358979323846;

struct Prep
{
    double cosw = 1.0;
    double sinw = 0.0;
    double alpha = 0.0;
};

Prep prepare (double fs, double freq, double q) noexcept
{
    const double safeFs = (std::isfinite (fs) && fs > 1000.0) ? fs : 44100.0;
    const double f = juce::jlimit (5.0, safeFs * 0.49, std::isfinite (freq) ? freq : 1000.0);
    const double qq = juce::jlimit (0.025, 100.0, std::isfinite (q) ? q : 0.707);
    const double w0 = 2.0 * pi * f / safeFs;

    Prep p;
    p.cosw = std::cos (w0);
    p.sinw = std::sin (w0);
    p.alpha = p.sinw / (2.0 * qq);
    return p;
}

double safeGainDb (double g) noexcept
{
    return std::isfinite (g) ? juce::jlimit (-48.0, 48.0, g) : 0.0;
}

Coeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    if (! std::isfinite (a0) || std::abs (a0) < 1.0e-12)
        return {};

    Coeffs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;

    if (! (std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2)
           && std::isfinite (c.a1) && std::isfinite (c.a2)))
        return {};

    return c;
}
} // namespace

Coeffs highPass (double fs, double freq, double q) noexcept
{
    const auto p = prepare (fs, freq, q);
    return normalise ((1.0 + p.cosw) * 0.5, -(1.0 + p.cosw), (1.0 + p.cosw) * 0.5,
                      1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha);
}

Coeffs lowPass (double fs, double freq, double q) noexcept
{
    const auto p = prepare (fs, freq, q);
    return normalise ((1.0 - p.cosw) * 0.5, 1.0 - p.cosw, (1.0 - p.cosw) * 0.5,
                      1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha);
}

Coeffs bandPass (double fs, double freq, double q) noexcept
{
    const auto p = prepare (fs, freq, q);
    return normalise (p.alpha, 0.0, -p.alpha, 1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha);
}

Coeffs bell (double fs, double freq, double q, double gainDb) noexcept
{
    const auto p = prepare (fs, freq, q);
    const double a = std::pow (10.0, safeGainDb (gainDb) / 40.0);
    return normalise (1.0 + p.alpha * a, -2.0 * p.cosw, 1.0 - p.alpha * a,
                      1.0 + p.alpha / a, -2.0 * p.cosw, 1.0 - p.alpha / a);
}

Coeffs lowShelf (double fs, double freq, double q, double gainDb) noexcept
{
    const auto p = prepare (fs, freq, q);
    const double a = std::pow (10.0, safeGainDb (gainDb) / 40.0);
    const double beta = 2.0 * std::sqrt (a) * p.alpha;
    return normalise (a * ((a + 1.0) - (a - 1.0) * p.cosw + beta),
                      2.0 * a * ((a - 1.0) - (a + 1.0) * p.cosw),
                      a * ((a + 1.0) - (a - 1.0) * p.cosw - beta),
                      (a + 1.0) + (a - 1.0) * p.cosw + beta,
                      -2.0 * ((a - 1.0) + (a + 1.0) * p.cosw),
                      (a + 1.0) + (a - 1.0) * p.cosw - beta);
}

Coeffs highShelf (double fs, double freq, double q, double gainDb) noexcept
{
    const auto p = prepare (fs, freq, q);
    const double a = std::pow (10.0, safeGainDb (gainDb) / 40.0);
    const double beta = 2.0 * std::sqrt (a) * p.alpha;
    return normalise (a * ((a + 1.0) + (a - 1.0) * p.cosw + beta),
                      -2.0 * a * ((a - 1.0) + (a + 1.0) * p.cosw),
                      a * ((a + 1.0) + (a - 1.0) * p.cosw - beta),
                      (a + 1.0) - (a - 1.0) * p.cosw + beta,
                      2.0 * ((a - 1.0) - (a + 1.0) * p.cosw),
                      (a + 1.0) - (a - 1.0) * p.cosw - beta);
}

BandCoeffs designBand (const BandSpec& spec, double fs) noexcept
{
    BandCoeffs r;
    r.active = spec.on;

    const double blend = juce::jlimit (0.0, 1.0, std::isfinite (spec.slopeBlend) ? spec.slopeBlend : 0.0);
    const double q = std::isfinite (spec.q) ? spec.q : 0.707;

    switch (spec.type)
    {
        case BandType::highPass:
        case BandType::lowPass:
        {
            // 24 dB/oct = two cascaded stages with 4th-order Butterworth pole Qs,
            // scaled by the user Q relative to 0.7071 (Q = 0.7071 -> exact Butterworth).
            const double k = q / 0.70710678;
            const double q1 = q * (1.0 - blend) + 0.54119610 * k * blend;
            const double q2 = 1.30656296 * k;
            const bool hp = spec.type == BandType::highPass;
            r.stage1 = hp ? highPass (fs, spec.freq, q1) : lowPass (fs, spec.freq, q1);
            r.stage2 = hp ? highPass (fs, spec.freq, q2) : lowPass (fs, spec.freq, q2);
            r.twoStages = blend > 0.5;
            break;
        }

        case BandType::lowShelf:
            r.stage1 = lowShelf (fs, spec.freq, q, spec.gainDb);
            break;

        case BandType::highShelf:
            r.stage1 = highShelf (fs, spec.freq, q, spec.gainDb);
            break;

        case BandType::bell:
            r.stage1 = bell (fs, spec.freq, q, spec.gainDb);
            break;
    }

    return r;
}

double magnitudeSquared (const Coeffs& c, double omega) noexcept
{
    const double c1 = std::cos (omega);
    const double s1 = std::sin (omega);
    const double c2 = std::cos (2.0 * omega);
    const double s2 = std::sin (2.0 * omega);

    const double nr = c.b0 + c.b1 * c1 + c.b2 * c2;
    const double ni = -(c.b1 * s1 + c.b2 * s2);
    const double dr = 1.0 + c.a1 * c1 + c.a2 * c2;
    const double di = -(c.a1 * s1 + c.a2 * s2);
    const double den = dr * dr + di * di;

    if (! std::isfinite (den) || den < 1.0e-30)
        return 1.0;

    const double m = (nr * nr + ni * ni) / den;
    return std::isfinite (m) ? m : 1.0;
}

double bandMagnitude (const BandCoeffs& c, double omega) noexcept
{
    if (! c.active)
        return 1.0;

    double m = magnitudeSquared (c.stage1, omega);

    if (c.twoStages)
        m *= magnitudeSquared (c.stage2, omega);

    return std::sqrt (std::max (0.0, m));
}
} // namespace eqmath
} // namespace eqtrum
