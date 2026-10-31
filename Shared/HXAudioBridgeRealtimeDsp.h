#pragma once
#include <atomic>
#include <type_traits>
#include "HXAudioSmallSpeakerBass.h"
#include "HXAudioCleanBass.h"

namespace HXAudioBridgeDsp {
// One producer and one consumer per queue. Neither waits for the other. Full
// queues keep their existing data; the UI retains/retries its newest value.
template<class T, size_t Capacity = 64> class SpscQueue {
    static_assert(std::is_trivially_copyable<T>::value, "POD snapshots only");
    static_assert(std::atomic<size_t>::is_always_lock_free, "Lock-free indices required");
public:
    bool Push(const T& value) noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % Capacity;
        if (next == m_tail.load(std::memory_order_acquire)) return false;
        m_values[head] = value;
        m_head.store(next, std::memory_order_release);
        return true;
    }
    bool Pop(T& value) noexcept {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return false;
        value = m_values[tail];
        m_tail.store((tail + 1) % Capacity, std::memory_order_release);
        return true;
    }
    // Bounded work even if producer runs continuously during this call.
    bool Latest(T& value) noexcept {
        bool found = false;
        for (size_t i = 0; i < Capacity; ++i) {
            T next;
            if (!Pop(next)) break;
            value = next; found = true;
        }
        return found;
    }
private:
    std::array<T, Capacity> m_values{};
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
};
struct Parameters {
    double gainDb = -6, frequencyHz = 1000, peqGainDb = 0, q = 0.9;
    double amount = 0.15;
    bool enabled = false;
    bool smallSpeaker = false;
    bool cleanBass = false; // Only audible when small-speaker mode is enabled.
    uint64_t revision = 0;
};
inline bool Valid(const Parameters& p) noexcept {
    return std::isfinite(p.gainDb) && p.gainDb >= -24 && p.gainDb <= 0 &&
        std::isfinite(p.frequencyHz) && p.frequencyHz >= 20 && p.frequencyHz <= 20000 &&
        std::isfinite(p.peqGainDb) && p.peqGainDb >= -12 && p.peqGainDb <= 12 &&
        std::isfinite(p.q) && p.q >= 0.1 && p.q <= 10 &&
        std::isfinite(p.amount) && p.amount >= 0 && p.amount <= 1;
}
inline bool SameValues(const Parameters& a, const Parameters& b) noexcept {
    return a.gainDb == b.gainDb && a.frequencyHz == b.frequencyHz &&
        a.peqGainDb == b.peqGainDb && a.q == b.q && a.amount == b.amount && a.enabled == b.enabled && a.smallSpeaker == b.smallSpeaker && a.cleanBass == b.cleanBass;
}
inline bool SamePeq(const Parameters& a, const Parameters& b) noexcept {
    return a.frequencyHz == b.frequencyHz && a.peqGainDb == b.peqGainDb && a.q == b.q;
}
struct Status {
    Parameters target;
    double gainDb = -6, amount = 0.15, enableMix = 0;
    double peqHz = 1000, peqDb = 0, peqQ = 0.9;
    bool transitioning = false;
};
struct Control {
    SpscQueue<Parameters> requests;
    SpscQueue<Status> status;
};
class Ramp {
public:
    void Reset(double value) noexcept { current = target = value; remaining = 0; }
    void Set(double value, UINT32 frames) noexcept {
        if (value == target) return;
        target = value; remaining = std::max<UINT32>(1, frames);
        step = (target - current) / remaining;
    }
    double Next() noexcept {
        if (remaining && --remaining == 0) current = target;
        else if (remaining) current += step;
        return current;
    }
    double current = 0, target = 0;
    UINT32 remaining = 0;
private:
    double step = 0;
};
// Each bank owns fixed-size (session-initialized) channel state. Replacing the
// inactive bank's coefficients never mutates the audible bank's state.
class PeqBank {
public:
    void Prepare(UINT32 channels) { z1.assign(channels, 0); z2.assign(channels, 0); }
    void Retune(const BiquadCoefficients& c) noexcept {
        coefficients = c;
        std::fill(z1.begin(), z1.end(), 0); std::fill(z2.begin(), z2.end(), 0);
    }
    float Process(float input, UINT32 channel) noexcept {
        const double value = coefficients.b0 * input + z1[channel];
        z1[channel] = coefficients.b1 * input - coefficients.a1 * value + z2[channel];
        z2[channel] = coefficients.b2 * input - coefficients.a2 * value;
        return static_cast<float>(value);
    }
private:
    BiquadCoefficients coefficients;
    std::vector<double> z1, z2;
};
class Processor {
public:
    // Session setup only. Allocations stay here, before WASAPI starts playback.
    void Configure(UINT32 rate, UINT32 channels, const Parameters& initial, Control* control = nullptr) {
        m_rate = std::max<UINT32>(rate, 8000); m_channels = std::max<UINT32>(channels, 1);
        m_rampFrames = std::max<UINT32>(1, m_rate / 50); // 20 ms
        m_fadeFrames = std::max<UINT32>(1, m_rate * 30 / 1000);
        m_control = control; m_target = Valid(initial) ? initial : Parameters{};
        m_peqTarget = m_target; m_active = 0; m_fadePosition = m_fadeFrames;
        m_gain.Reset(std::pow(10.0, m_target.gainDb / 20));
        m_amount.Reset(m_target.amount); m_enabled.Reset(m_target.enabled ? 1 : 0);
        m_speakerMix.Reset(m_target.smallSpeaker ? 1 : 0);
        m_cleanMix.Reset(m_target.cleanBass ? 1 : 0);
        m_smallSpeaker.Configure(m_rate,m_channels);
        m_cleanSpeaker.Configure(m_rate,m_channels);
        DspSettings bass; bass.virtualBassEnabled = true; bass.virtualBassAmount = m_target.amount;
        m_bass.Configure(m_rate, m_channels, bass);
        for (auto& bank : m_peq) { bank.Prepare(m_channels); bank.Retune(Coefficients(m_target)); }
    }
    void Process(float* samples, UINT32 frames) noexcept {
        if (!samples || !frames) return;
        // Block boundary: newest valid snapshot wins; no locks/allocations/IO.
        Parameters next;
        if (m_control && m_control->requests.Latest(next) && Valid(next)) {
            m_target = next;
            m_gain.Set(std::pow(10.0, next.gainDb / 20), m_rampFrames);
            m_amount.Set(next.amount, m_rampFrames);
            m_enabled.Set(next.enabled ? 1 : 0, m_rampFrames);
            m_speakerMix.Set(next.smallSpeaker ? 1 : 0,m_rampFrames);
            m_cleanMix.Set(next.cleanBass ? 1 : 0,m_rampFrames);
        }
        if (!Fading() && !SamePeq(m_target, m_peqTarget)) {
            m_peq[1-m_active].Retune(Coefficients(m_target));
            m_peqTarget = m_target; m_fadePosition = 0;
        }
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const double gain = m_gain.Next(), amount = m_amount.Next(), speakerMix=m_speakerMix.Next(), cleanMix=m_cleanMix.Next();
            // At 0%, bypass all bass processing. Keep the internal bass filters
            // warm, including while disabled, so rapid toggles never revive stale state.
            const double mix = m_enabled.Next() * std::min(1.0, amount / 0.01);
            m_bass.SetRealtimeAmount(std::max(amount, 1e-6));
            const bool fade = Fading();
            const double t = fade ? static_cast<double>(m_fadePosition + 1) / m_fadeFrames : 0;
            // Raised-cosine crossfade has zero slope at both ends.
            const double blend = fade ? 0.5 - 0.5 * std::cos(3.14159265358979323846 * t) : 0;
            for (UINT32 c = 0; c < m_channels; ++c) {
                const size_t i = static_cast<size_t>(frame) * m_channels + c;
                const float dry = std::isfinite(samples[i]) ? samples[i] : 0;
                const float legacy = m_bass.Process(dry, c);
                const float optimized=m_smallSpeaker.Process(dry,c,amount);
                const float multiband=m_cleanSpeaker.Process(dry,c,amount);
                // Hybrid preserves a fraction of the already-auditioned punch
                // while reducing broadband nonlinear cross-products. It is
                // an experimental timbre compromise, not an IMD guarantee.
                const float clean=static_cast<float>(0.40*optimized+0.60*multiband);
                const float selected=static_cast<float>(optimized+cleanMix*(clean-optimized));
                const float bass=static_cast<float>(legacy+speakerMix*(selected-legacy));
                const float input = static_cast<float>((dry + mix * (bass - dry)) * gain);
                const float a = m_peq[m_active].Process(input, c);
                samples[i] = fade ? static_cast<float>(a + blend * (m_peq[1-m_active].Process(input, c) - a)) : a;
            }
            if (fade && ++m_fadePosition == m_fadeFrames) m_active = 1 - m_active;
        }
        if (m_control) m_control->status.Push(CurrentStatus()); // Full: skip, never wait.
    }
    Status CurrentStatus() const noexcept {
        Status s; s.target=m_target; s.gainDb=20*std::log10(m_gain.current);
        s.amount=m_amount.current; s.enableMix=m_enabled.current;
        s.peqHz=m_peqTarget.frequencyHz; s.peqDb=m_peqTarget.peqGainDb; s.peqQ=m_peqTarget.q;
        s.transitioning=Fading() || m_gain.remaining || m_amount.remaining || m_enabled.remaining || m_speakerMix.remaining || m_cleanMix.remaining || !SamePeq(m_target,m_peqTarget);
        return s;
    }
private:
    bool Fading() const noexcept { return m_fadePosition < m_fadeFrames; }
    BiquadCoefficients Coefficients(const Parameters& p) const noexcept {
        return MakePeaking(m_rate, p.frequencyHz, p.q, p.peqGainDb);
    }
    UINT32 m_rate=48000, m_channels=0, m_rampFrames=960, m_fadeFrames=1440, m_fadePosition=1440;
    int m_active=0;
    Parameters m_target, m_peqTarget;
    Ramp m_gain, m_amount, m_enabled, m_speakerMix, m_cleanMix;
    SmallSpeakerBassProcessor m_smallSpeaker;
    CleanSmallSpeakerBassProcessor m_cleanSpeaker;
    VirtualBassProcessor m_bass;
    PeqBank m_peq[2];
    Control* m_control=nullptr;
};
}
