#pragma once

#include "Params.h"

#include <cmath>

namespace eqtrum
{
// Normalised biquad coefficients (a0 == 1). Default = identity.
struct Coeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct BandSpec
{
    BandType type = BandType::bell;
    bool on = false;
    double freq = 1000.0;
    double gainDb = 0.0;
    double q = 0.707;
    double slopeBlend = 0.0;   // HP/LP only: 0 = 12 dB/oct, 1 = 24 dB/oct
};

struct BandCoeffs
{
    Coeffs stage1;
    Coeffs stage2;
    bool twoStages = false;
    bool active = false;
};

namespace eqmath
{
Coeffs highPass (double fs, double freq, double q) noexcept;
Coeffs lowPass (double fs, double freq, double q) noexcept;
Coeffs bandPass (double fs, double freq, double q) noexcept;   // constant 0 dB peak gain
Coeffs bell (double fs, double freq, double q, double gainDb) noexcept;
Coeffs lowShelf (double fs, double freq, double q, double gainDb) noexcept;
Coeffs highShelf (double fs, double freq, double q, double gainDb) noexcept;

BandCoeffs designBand (const BandSpec& spec, double fs) noexcept;
double magnitudeSquared (const Coeffs& c, double omega) noexcept;   // omega = 2*pi*f/fs
double bandMagnitude (const BandCoeffs& c, double omega) noexcept;  // linear magnitude, 1 when inactive
} // namespace eqmath

// Transposed direct form II biquad with double precision state.
struct Biquad
{
    Coeffs c;
    double z1 = 0.0;
    double z2 = 0.0;

    inline float process (float x) noexcept
    {
        const double in = static_cast<double> (x);
        const double y = c.b0 * in + z1;
        z1 = c.b1 * in - c.a1 * y + z2;
        z2 = c.b2 * in - c.a2 * y;
        return static_cast<float> (y);
    }

    void reset() noexcept
    {
        z1 = 0.0;
        z2 = 0.0;
    }

    bool isFinite() const noexcept { return std::isfinite (z1) && std::isfinite (z2); }
};
} // namespace eqtrum
