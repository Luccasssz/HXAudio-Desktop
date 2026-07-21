#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace HXAudioDsp
{
    constexpr double kMinimumGainDb = -30.0;
    constexpr double kMaximumGainDb = 30.0;
    constexpr double kMinimumPeqGainDb = -40.0;
    constexpr double kMaximumPeqGainDb = 40.0;
    constexpr double kStaticHeadroomSafetyDb = 0.25;

    struct FrequencyProbe
    {
        double frequencyHz = 0.0;
        double q = 1.0;
    };

    inline double ClampGainDb(double gainDb)
    {
        return std::isfinite(gainDb)
            ? std::clamp(gainDb, kMinimumGainDb, kMaximumGainDb) : 0.0;
    }

    inline double ClampPeqGainDb(double gainDb)
    {
        return std::isfinite(gainDb)
            ? std::clamp(gainDb, kMinimumPeqGainDb,
                kMaximumPeqGainDb) : 0.0;
    }

    // This is a fixed configuration-time bound for the virtual-bass
    // waveshaper.  It is deliberately independent of the audio samples, so it
    // cannot pump or breathe like a level detector, compressor or limiter.
    inline double VirtualBassPeakGainDb(bool enabled, double amount,
        unsigned int mode)
    {
        if (!enabled) return 0.0;
        const double boundedAmount = std::clamp(amount, 0.0, 2.0);
        if (boundedAmount <= 0.0) return 0.0;

        const double drive = 1.0 + 7.5 * boundedAmount;
        // 2/3/4 uses terms whose weighted absolute bound is 1.71.  The odd
        // 3/5/7 Chebyshev sum is bounded by one.  A custom cosine transfer can
        // differ from its silence value by at most two.
        const double transferBound = mode == 1 ? 1.0 : mode == 2 ? 2.0 : 1.71;
        const double peakGain = 1.0 +
            (2.5 * boundedAmount * transferBound / drive);
        return 20.0 * std::log10(peakGain);
    }

    template<typename CoefficientAt>
    double CascadeGainDbAtOmega(std::size_t coefficientCount,
        const CoefficientAt& coefficientAt, double omega)
    {
        const double cosine1 = std::cos(omega);
        const double sine1 = std::sin(omega);
        const double cosine2 = std::cos(2.0 * omega);
        const double sine2 = std::sin(2.0 * omega);
        double product = 1.0;
        double accumulatedDb = 0.0;

        for (std::size_t index = 0; index < coefficientCount; ++index)
        {
            const auto& coefficients = coefficientAt(index);
            const double numeratorReal = coefficients.b0 +
                coefficients.b1 * cosine1 + coefficients.b2 * cosine2;
            const double numeratorImaginary = -coefficients.b1 * sine1 -
                coefficients.b2 * sine2;
            const double denominatorReal = 1.0 +
                coefficients.a1 * cosine1 + coefficients.a2 * cosine2;
            const double denominatorImaginary = -coefficients.a1 * sine1 -
                coefficients.a2 * sine2;
            const double numeratorPower = numeratorReal * numeratorReal +
                numeratorImaginary * numeratorImaginary;
            const double denominatorPower = denominatorReal * denominatorReal +
                denominatorImaginary * denominatorImaginary;
            if (!(denominatorPower > std::numeric_limits<double>::min()) ||
                !std::isfinite(denominatorPower))
                return std::numeric_limits<double>::infinity();
            if (!(numeratorPower > std::numeric_limits<double>::min()))
                return -std::numeric_limits<double>::infinity();

            const double ratio = numeratorPower / denominatorPower;
            if (!(ratio > 0.0) || !std::isfinite(ratio))
                return ratio > 0.0 ? std::numeric_limits<double>::infinity() :
                    -std::numeric_limits<double>::infinity();
            if (ratio > 1.0e100 || ratio < 1.0e-100)
            {
                accumulatedDb += 10.0 * std::log10(ratio);
                continue;
            }
            product *= ratio;
            if (product > 1.0e100)
            {
                accumulatedDb += 1000.0;
                product *= 1.0e-100;
            }
            else if (product < 1.0e-100)
            {
                accumulatedDb -= 1000.0;
                product *= 1.0e100;
            }
        }
        return accumulatedDb + 10.0 * std::log10(product);
    }

    // Finds the maximum gain of the complete configured biquad cascade, then
    // turns that value into one fixed attenuation.  Linear and logarithmic
    // sweeps are combined with exact band-center/near-band probes so narrow-Q
    // filters and high sample-rate endpoints are both covered without any
    // sample-dependent processing in the real-time path.
    template<typename CoefficientAt>
    double CalculateStaticHeadroomDb(double sampleRate, double preampDb,
        std::size_t coefficientCount, const CoefficientAt& coefficientAt,
        const FrequencyProbe* probes, std::size_t probeCount,
        double additionalPeakGainDb)
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0) return 0.0;
        constexpr double pi = 3.14159265358979323846;
        const double nyquist = sampleRate * 0.5;
        double maximumDb = ClampGainDb(preampDb) +
            std::max(0.0, additionalPeakGainDb);

        const auto inspectFrequency = [&](double frequencyHz)
        {
            if (!std::isfinite(frequencyHz) || frequencyHz < 0.0 ||
                frequencyHz > nyquist) return;
            const double omega = pi * frequencyHz / nyquist;
            const double cascadeDb = CascadeGainDbAtOmega(
                coefficientCount, coefficientAt, omega);
            if (std::isfinite(cascadeDb))
            {
                maximumDb = std::max(maximumDb, ClampGainDb(preampDb) +
                    std::max(0.0, additionalPeakGainDb) + cascadeDb);
            }
        };

        inspectFrequency(0.0);
        inspectFrequency(nyquist);

        constexpr std::size_t linearSteps = 32768;
        for (std::size_t step = 1; step < linearSteps; ++step)
            inspectFrequency(nyquist * static_cast<double>(step) / linearSteps);

        constexpr std::size_t logarithmicSteps = 8192;
        const double minimumLogFrequency = std::max(0.001, nyquist * 1.0e-8);
        const double logSpan = std::log(nyquist / minimumLogFrequency);
        for (std::size_t step = 0; step <= logarithmicSteps; ++step)
        {
            inspectFrequency(minimumLogFrequency * std::exp(logSpan *
                static_cast<double>(step) / logarithmicSteps));
        }

        for (std::size_t index = 0; index < probeCount; ++index)
        {
            const double center = probes[index].frequencyHz;
            if (!std::isfinite(center) || center <= 0.0 || center >= nyquist)
                continue;
            inspectFrequency(center);
            const double q = std::max(0.01, probes[index].q);
            const double octaveRadius = std::min(4.0, 1.0 / q);
            for (double scale : { 0.25, 0.5, 1.0 })
            {
                const double factor = std::pow(2.0, octaveRadius * scale);
                inspectFrequency(center / factor);
                inspectFrequency(center * factor);
            }
        }

        return maximumDb > 0.0 ? maximumDb + kStaticHeadroomSafetyDb : 0.0;
    }
}
