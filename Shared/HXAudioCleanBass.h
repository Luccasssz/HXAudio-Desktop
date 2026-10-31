#pragma once
// Experimental low-intermodulation virtual bass. Three overlapping narrow
// extraction bands independently excite harmonic generators, reducing
// cross-products between distant low notes compared with a single broadband
// nonlinear generator. The existing small-speaker algorithm is unchanged.
class CleanSmallSpeakerBassProcessor {
public:
    void Configure(double rate, UINT32 channels) {
        constexpr double q = 0.7071067811865476;
        constexpr double hp[] = {32.0, 52.0, 78.0};
        constexpr double lp[] = {58.0, 88.0, 120.0};
        for (size_t i = 0; i < m_bands.size(); ++i) {
            auto& band = m_bands[i];
            band.highPass.Configure(MakeHighPass(rate, hp[i], q), channels);
            band.lowPass1.Configure(MakeLowPass(rate, lp[i], q), channels);
            band.lowPass2.Configure(MakeLowPass(rate, lp[i], q), channels);
            band.envelope.assign(channels, 0.0);
        }
        m_wetHighPass1.Configure(MakeHighPass(rate, 110.0, q), channels);
        m_wetHighPass2.Configure(MakeHighPass(rate, 110.0, q), channels);
        m_wetLowPass1.Configure(MakeLowPass(rate, 350.0, q), channels);
        m_wetLowPass2.Configure(MakeLowPass(rate, 350.0, q), channels);
        m_attack = std::exp(-1.0 / (rate * 0.018));
        m_release = std::exp(-1.0 / (rate * 0.160));
    }

    float Process(float input, UINT32 channel, double amount) noexcept {
        const double dry = std::isfinite(input) ? input : 0.0;
        double wet = 0.0;
        for (auto& band : m_bands) {
            double low = band.highPass.Process(static_cast<float>(dry), channel);
            low = band.lowPass1.Process(static_cast<float>(low), channel);
            low = band.lowPass2.Process(static_cast<float>(low), channel);
            double& envelope = band.envelope[channel];
            const double magnitude = std::abs(low);
            const double coefficient = magnitude > envelope ? m_attack : m_release;
            envelope = coefficient * envelope + (1.0 - coefficient) * magnitude;
            // Normalized, smooth excitation is bounded. Each narrow band
            // generates its own harmonics, not cross-terms with other bands.
            const double x = std::tanh(low / (1.7 * envelope + 0.04));
            const double x2 = x * x;
            const double h2 = 2.0 * x2;
            const double h3 = 4.0 * x2 * x - 3.0 * x;
            const double h4 = 8.0 * x2 * x2 - 8.0 * x2;
            wet += 0.58 * envelope * (0.15 * h2 + 0.75 * h3 + 0.10 * h4);
        }
        // Remove DC, fundamental leakage and overly bright upper partials.
        float filtered = m_wetHighPass1.Process(static_cast<float>(wet), channel);
        filtered = m_wetHighPass2.Process(filtered, channel);
        filtered = m_wetLowPass1.Process(filtered, channel);
        filtered = m_wetLowPass2.Process(filtered, channel);
        // Bounded additive wet contribution, without deliberate dry-bass loss.
        const double strength = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.0;
        const double added = 0.24 * std::tanh((7.0 * strength / 0.24) * filtered);
        return static_cast<float>(dry + added);
    }

private:
    struct Band {
        Biquad highPass, lowPass1, lowPass2;
        std::vector<double> envelope;
    };
    std::array<Band, 3> m_bands{};
    Biquad m_wetHighPass1, m_wetHighPass2, m_wetLowPass1, m_wetLowPass2;
    double m_attack = 0.0, m_release = 0.0;
};
