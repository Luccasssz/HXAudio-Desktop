#pragma once
// Experimental additive missing-fundamental generator. These are synthesis
// design bands, not an assumed/corrected response of the user's loudspeaker.
class SmallSpeakerBassProcessor {
public:
    void Configure(double rate, UINT32 channels) {
        constexpr double q=0.7071067811865476;
        m_extractHp.Configure(MakeHighPass(rate,40,q),channels);
        m_extractLp1.Configure(MakeLowPass(rate,110,q),channels);
        m_extractLp2.Configure(MakeLowPass(rate,110,q),channels);
        m_wetHp.Configure(MakeHighPass(rate,120,q),channels);
        m_wetHp2.Configure(MakeHighPass(rate,120,q),channels);
        m_wetHp3.Configure(MakeHighPass(rate,120,q),channels);
        m_wetLp1.Configure(MakeLowPass(rate,400,q),channels);
        m_wetLp2.Configure(MakeLowPass(rate,400,q),channels);
        m_envelope.assign(channels,0);
        m_attack=std::exp(-1/(rate*0.010)); m_release=std::exp(-1/(rate*0.120));
    }
    float Process(float input, UINT32 channel, double amount) noexcept {
        const float dry=std::isfinite(input)?input:0;
        float low=m_extractHp.Process(dry,channel);
        low=m_extractLp1.Process(low,channel); low=m_extractLp2.Process(low,channel);
        const double magnitude=std::abs(static_cast<double>(low));
        double& envelope=m_envelope[channel];
        const double coefficient=magnitude>envelope?m_attack:m_release;
        envelope=coefficient*envelope+(1-coefficient)*magnitude;
        // Level-adaptive excitation avoids requiring loud input to excite cubic
        // and quartic terms. The absolute floor avoids boosting near-silence.
        const double x=std::tanh(low/(1.5*envelope+0.03));
        const double x2=x*x;
        const double h2=2*x2, h3=4*x2*x-3*x, h4=8*x2*x2-8*x2;
        float wet=static_cast<float>(envelope*(0.25*h2+0.55*h3+0.20*h4));
        wet=m_wetHp.Process(wet,channel); wet=m_wetHp2.Process(wet,channel); wet=m_wetHp3.Process(wet,channel);
        wet=m_wetLp1.Process(wet,channel); wet=m_wetLp2.Process(wet,channel);
        // Original bass is retained at every strength. Energy-tracking wet gain
        // is bounded by strength; the existing Bridge guards remain downstream.
        return static_cast<float>(dry+0.25*std::tanh(8.0*std::clamp(amount,0.0,1.0)*wet/0.25));
    }
private:
    Biquad m_extractHp,m_extractLp1,m_extractLp2,m_wetHp,m_wetHp2,m_wetHp3,m_wetLp1,m_wetLp2;
    std::vector<double> m_envelope;
    double m_attack=0,m_release=0;
};
