#pragma once
#include <fstream>

// Compiled into the shipping Bridge: exercises its actual DSP classes.
inline int RunVirtualBassRegressionTests() {
    std::ofstream log("bridge-virtual-bass.log");
    constexpr double pi = 3.14159265358979323846;
    const auto check = [&](bool pass, const char* name) {
        if (!pass) log << "FAIL " << name << '\n';
        return pass;
    };
    size_t scenarios = 0;
    for (UINT32 rate : {32000u, 44100u, 48000u, 96000u, 192000u})
    for (auto mode : {VirtualBassMode::Harmonics234, VirtualBassMode::Harmonics357, VirtualBassMode::Custom})
    for (double amount : {0.15, 0.5, 1.0, 2.0})
    for (double amplitude : {0.98, 2.0, 8.0}) {
        DspSettings settings; settings.virtualBassEnabled = true;
        settings.virtualBassAmount = amount; settings.virtualBassMode = mode;
        settings.virtualBassMultipliers = {2.5, 3.75, 6.5}; settings.preampDb = -6;
        DspProcessor stereo, left, right, chunked;
        stereo.Configure(rate, 2, settings); left.Configure(rate, 1, settings);
        right.Configure(rate, 1, settings); chunked.Configure(rate, 2, settings);
        const UINT32 count = rate / 2;
        std::vector<float> a(count*2), l(count), r(count);
        for (UINT32 i=0; i<count; ++i) {
            const double t = static_cast<double>(i)/rate;
            // Bass plus smoothly gated kick transient and unrelated right channel.
            const double envelope = std::pow(std::sin(pi*std::min(1.0,t/0.02)),2) * std::exp(-t*12);
            l[i]=static_cast<float>(amplitude*(0.75*std::sin(2*pi*50*t)+0.25*envelope*std::sin(2*pi*73*t)));
            r[i]=static_cast<float>(-amplitude*0.6*std::sin(2*pi*37*t));
            a[i*2]=l[i]; a[i*2+1]=r[i];
        }
        auto b=a; stereo.Process(a.data(),count); left.Process(l.data(),count); right.Process(r.data(),count);
        for (UINT32 i=0; i<count;) { UINT32 n=std::min<UINT32>(127,count-i); chunked.Process(b.data()+2*i,n); i+=n; }
        double rawPeak=0, protectedPeak=0, maxStep=0;
        HXAudioDsp::BridgePeakStats stats;
        float previous[2]{};
        for (UINT32 i=0; i<count; ++i) for (UINT32 c=0; c<2; ++c) {
            const auto n=2*i+c; const float raw=a[n];
            if (!check(std::isfinite(raw),"finite DSP")) return 1;
            if (!check(std::abs(raw-(c?r[i]:l[i]))<1e-6f,"channel isolation")) return 2;
            if (!check(raw==b[n],"block continuity")) return 3;
            rawPeak=std::max(rawPeak,std::abs(static_cast<double>(raw)));
            const float v=HXAudioDsp::BridgePeakGuard(raw,stats);
            protectedPeak=std::max(protectedPeak,std::abs(static_cast<double>(v)));
            maxStep=std::max(maxStep,std::abs(static_cast<double>(v-previous[c]))); previous[c]=v;
        }
        log << "rate="<<rate<<" mode="<<int(mode)<<" amount="<<amount<<" amplitude="<<amplitude
            <<" rawPeak="<<rawPeak<<" peak="<<protectedPeak<<" maxStep="<<maxStep
            <<" over="<<stats.overRange<<" soft="<<stats.protectedSamples<<" hard="<<stats.hardClips<<'\n';
        if (!check(rawPeak<8.0 && protectedPeak<1.0 && stats.hardClips==0 && stats.nonFinite==0,"peak bound")) return 4;
        if (!check(maxStep<0.15,"low-frequency waveform continuity")) return 5;
        if (amount==0.15 && amplitude==0.98 && !check(stats.overRange==0 && stats.protectedSamples==0,"conservative default headroom")) return 6;
        ++scenarios;
    }
    // Silence, impulse recovery, non-finite source rejection, stereo crosstalk.
    DspSettings settings; settings.virtualBassEnabled=true; settings.virtualBassAmount=0.15;
    DspProcessor dsp; dsp.Configure(48000,2,settings);
    std::vector<float> silence(96000,0); dsp.Process(silence.data(),48000);
    if (!check(std::all_of(silence.begin(),silence.end(),[](float v){return v==0;}),"silence")) return 7;
    silence[0]=1; silence[2]=std::numeric_limits<float>::infinity(); silence[4]=std::numeric_limits<float>::quiet_NaN();
    dsp.Process(silence.data(),48000);
    for (size_t i=0;i<silence.size();++i)
        if (!check(std::isfinite(silence[i]) && (i%2==0 || silence[i]==0),"invalid input recovery/isolation")) return 8;
    if (!check(std::abs(silence[silence.size()-2])<1e-5,"impulse tail recovery")) return 9;
    // Generator must still produce audible-band harmonics, including at default strength.
    for (auto mode : {VirtualBassMode::Harmonics234,VirtualBassMode::Harmonics357})
    for (double amount : {0.15,1.0}) {
        settings.virtualBassMode=mode; settings.virtualBassAmount=amount;
        dsp.Configure(48000,1,settings); std::vector<float> tone(96000);
        for (size_t i=0;i<tone.size();++i) tone[i]=static_cast<float>(0.5*std::sin(2*pi*50*i/48000));
        dsp.Process(tone.data(),static_cast<UINT32>(tone.size()));
        double harmonicEnergy=0;
        for (int harmonic : {2,3,4,5,7}) {
            double re=0,im=0;
            for (size_t i=48000;i<tone.size();++i) { const double phase=2*pi*50*harmonic*i/48000; re+=tone[i]*std::cos(phase); im+=tone[i]*std::sin(phase); }
            const double a=2*std::hypot(re,im)/48000; harmonicEnergy+=a*a;
            log<<"mode="<<int(mode)<<" amount="<<amount<<" harmonic="<<harmonic<<" amplitude="<<a<<'\n';
        }
        if (!check(harmonicEnergy>1e-7,"harmonic generation")) return 10;
    }
    // Music-like transient: bass kick, midrange chord and treble, then +12 dB PEQ.
    // This deliberately engages protection and checks the exact Bridge order.
    for (UINT32 rate : {44100u,48000u,96000u}) {
        DspSettings music; music.virtualBassEnabled=true; music.virtualBassAmount=0.15;
        music.preampDb=-6; music.peqBands.push_back({true,PeqFilterType::Peak,1000,12,0.9});
        dsp.Configure(rate,2,music); std::vector<float> samples(rate*2);
        for (UINT32 i=0;i<rate;++i) {
            const double t=static_cast<double>(i)/rate;
            const double kick=std::exp(-std::fmod(t,0.25)*30)*std::sin(2*pi*55*t);
            const double chord=0.3*std::sin(2*pi*997*t)+0.15*std::sin(2*pi*1501*t);
            samples[2*i]=static_cast<float>(0.5*kick+chord+0.1*std::sin(2*pi*8000*t));
            samples[2*i+1]=static_cast<float>(0.4*kick-chord);
        }
        dsp.Process(samples.data(),rate); HXAudioDsp::BridgePeakStats musicStats;
        for (auto& v:samples) {
            v=HXAudioDsp::BridgePeakGuard(v,musicStats);
            if (!check(std::isfinite(v) && std::abs(v)<1,"music transient protected")) return 14;
        }
        if (!check(musicStats.protectedSamples>0 && musicStats.hardClips==0 && musicStats.nonFinite==0,"music protection counters")) return 15;
        log<<"music rate="<<rate<<" over="<<musicStats.overRange<<" soft="<<musicStats.protectedSamples<<" hard="<<musicStats.hardClips<<'\n';
    }
    // Guard is identity below its knee and smooth/monotonic above it.
    HXAudioDsp::BridgePeakStats stats;
    float previous=HXAudioDsp::BridgePeakGuard(-2.0f,stats);
    for (int i=-20000;i<=20000;++i) {
        const float input=i*0.0001f, value=HXAudioDsp::BridgePeakGuard(input,stats);
        if (!check(value>=previous && value-previous<0.00011f,"guard continuity/monotonicity")) return 11;
        if (std::abs(input)<=0.85f && !check(input==value,"guard unity region")) return 12;
        previous=value;
    }
    const auto overBefore=stats.overRange;
    HXAudioDsp::BridgePeakGuard(std::numeric_limits<float>::infinity(),stats);
    HXAudioDsp::BridgePeakGuard(std::numeric_limits<float>::quiet_NaN(),stats);
    if (!check(stats.nonFinite==2 && stats.hardClips==0 && overBefore>0,"diagnostic counters")) return 13;
    log<<"PASS "<<scenarios<<" stress scenarios plus harmonics, continuity, silence, invalid input and counters. Offline DSP only; no real audio validation.\n";
    return 0;
}
