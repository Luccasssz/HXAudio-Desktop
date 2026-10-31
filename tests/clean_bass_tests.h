#pragma once
#include <fstream>
#include <filesystem>

// Compare the actual production DSP banks with level-normalized intermodulation
// diagnostics. The inputs are deterministic; this is NOT a listening verdict.
inline int RunCleanBassTests() {
    using namespace HXAudioBridgeDsp;
    std::ofstream log("bridge-clean-bass.log"), csv("bridge-clean-bass-spectrum.csv");
    csv << "f1,f2,mode,targetPower,intermodPower,imdToTarget,fullRms,softProtected\n";
    constexpr double pi = 3.14159265358979323846;
    const UINT32 rate = 48000;
    const std::array<std::array<int, 2>, 4> pairs{{{{40,94}},{{50,90}},{{54,98}},{{58,102}}}};
    size_t improved = 0;
    double oldRelative = 0.0, cleanRelative = 0.0;
    for (const auto& pair : pairs) {
        const int f1=pair[0], f2=pair[1];
        std::vector<int> target, intermod;
        for (int f : {f1,f2})
            for (int h : {2,3,4}) {
                const int tone=f*h;
                if (tone>=120 && tone<=350 && std::find(target.begin(),target.end(),tone)==target.end())
                    target.push_back(tone);
            }
        for (int tone : {f1+f2, std::abs(f1-f2), 2*f1+f2, 2*f2+f1,
                         std::abs(2*f1-f2), std::abs(2*f2-f1)}) {
            if (tone>=120 && tone<=350 &&
                std::find(target.begin(),target.end(),tone)==target.end() &&
                std::find(intermod.begin(),intermod.end(),tone)==intermod.end())
                intermod.push_back(tone);
        }
        if (target.empty() || intermod.empty()) return 1;
        double tPower[2]{}, iPower[2]{}, relative[2]{};
        for (int mode=0;mode<2;++mode) {
            Parameters p; p.enabled=true; p.smallSpeaker=true;
            p.cleanBass=(mode==1); p.amount=0.5; p.gainDb=-6;
            Processor dsp; dsp.Configure(rate,1,p);
            std::vector<float> samples(rate);
            for (UINT32 i=0;i<rate;++i) {
                const double t=static_cast<double>(i)/rate;
                samples[i]=static_cast<float>(0.25*std::sin(2*pi*f1*t)+
                                              0.25*std::sin(2*pi*f2*t));
            }
            dsp.Process(samples.data(),rate);
            HXAudioDsp::BridgePeakStats guard;
            double rms=0.0;
            for (auto& s : samples) {
                s=HXAudioDsp::BridgePeakGuard(s,guard);
                if (!std::isfinite(s) || std::abs(s)>=1.0f) return 2;
                rms+=static_cast<double>(s)*s;
            }
            rms=std::sqrt(rms/samples.size());
            for (int f : target) {
                const double h=SpeakerToneAmplitude(samples,f,rate);
                tPower[mode]+=h*h;
            }
            for (int f : intermod) {
                const double h=SpeakerToneAmplitude(samples,f,rate);
                iPower[mode]+=h*h;
            }
            relative[mode]=iPower[mode]/std::max(1e-20,tPower[mode]);
            csv << f1 << ',' << f2 << ',' << mode << ',' << tPower[mode] << ','
                << iPower[mode] << ',' << relative[mode] << ','
                << rms << ',' << guard.protectedSamples << '\n';
        }
        const double ratio=relative[1]/std::max(1e-20,relative[0]);
        if (ratio<0.90) ++improved;
        oldRelative+=relative[0]; cleanRelative+=relative[1];
        log << "twoTone=" << f1 << "+" << f2
            << " targetEnergyB/A=" << tPower[1]/std::max(1e-20,tPower[0])
            << " normalizedIMDB/A=" << ratio << '\n';
        // Reduction is meaningful only if the new mode still produces
        // enough audible target-band harmonics; avoid a trivial mute-to-pass.
        if (!(tPower[1]>tPower[0]*0.08)) return 3;
    }
    log << "pairsImproved=" << improved
        << " totalIMDNormalizedB/A=" << cleanRelative/std::max(1e-20,oldRelative) << '\n';
    if (improved<3 || cleanRelative>oldRelative*0.75) return 4;

    // Preserve the fundamental while retaining a nontrivial harmonic effect.
    for(UINT32 sampleRate:{44100u,48000u,96000u}) {
        for (int frequency:{40,50,80,100}) {
            double fundamental[2]{},target[2]{};
            for(int mode=0;mode<2;++mode) {
                Parameters p; p.enabled=true; p.smallSpeaker=true;
                p.cleanBass=mode==1; p.amount=0.5; p.gainDb=-6;
                Processor dsp; dsp.Configure(sampleRate,1,p);
                std::vector<float> samples(sampleRate);
                for(UINT32 i=0;i<sampleRate;++i) {
                    const double t=static_cast<double>(i)/sampleRate;
                    samples[i]=static_cast<float>(0.5*std::sin(2*pi*frequency*t));
                }
                dsp.Process(samples.data(),sampleRate);
                HXAudioDsp::BridgePeakStats guard;
                for(auto& s:samples) {
                    s=HXAudioDsp::BridgePeakGuard(s,guard);
                    if(!std::isfinite(s)||std::abs(s)>=1.0f) return 5;
                }
                fundamental[mode]=SpeakerToneAmplitude(samples,frequency,sampleRate);
                for(int h:{2,3,4}) if(frequency*h>=120 && frequency*h<=350) {
                    const double v=SpeakerToneAmplitude(samples,frequency*h,sampleRate);
                    target[mode]+=v*v;
                }
            }
            if(fundamental[1]<0.85*(0.5*std::pow(10.0,-6.0/20.0))) return 6;
            if(target[1]<target[0]*0.08) return 7;
            log << "fundamental=" << frequency << " at=" << sampleRate
                << " retention=" << fundamental[1]/(0.5*std::pow(10.0,-6.0/20.0))
                << " targetB/A=" << target[1]/std::max(target[0],1e-20) << '\n';
        }
    }

    // A/B files: equal-RMS by attenuation only. Process only once when listening.
    std::filesystem::create_directories("clean-bass-ab");
    std::vector<float> original(rate*4);
    for(size_t i=0;i<original.size();++i) {
        const double t=static_cast<double>(i)/rate;
        const double beat=std::fmod(t,0.48);
        const double bass=0.31*std::sin(2*pi*50*t)+0.20*std::sin(2*pi*90*t);
        const double kick=0.22*std::exp(-beat*15)*std::sin(2*pi*58*t);
        const double chord=0.12*std::sin(2*pi*440*t)+0.07*std::sin(2*pi*660*t);
        original[i]=static_cast<float>(bass+kick+chord);
    }
    std::vector<float> output[2];
    double rms[2]{};
    for(int mode=0;mode<2;++mode) {
        Parameters p; p.enabled=true; p.smallSpeaker=true; p.cleanBass=(mode==1);
        p.amount=0.5; p.gainDb=-6;
        Processor dsp; dsp.Configure(rate,1,p);
        output[mode]=original;
        dsp.Process(output[mode].data(),static_cast<UINT32>(original.size()));
        HXAudioDsp::BridgePeakStats guard;
        for(auto& x:output[mode]) {
            x=HXAudioDsp::BridgePeakGuard(x,guard);
            rms[mode]+=static_cast<double>(x)*x;
        }
        rms[mode]=std::sqrt(rms[mode]/output[mode].size());
        if(!std::isfinite(rms[mode]) || rms[mode]<=0) return 8;
    }
    const double matched=std::min(rms[0],rms[1]);
    SpeakerWriteWave("clean-bass-ab/A-current-small-speaker.wav",output[0],matched/rms[0]);
    SpeakerWriteWave("clean-bass-ab/B-clean-multiband.wav",output[1],matched/rms[1]);
    log << "AB matched RMS=" << matched
        << " raw RMS A=" << rms[0] << " B=" << rms[1] << '\n';
    log << "PASS experimental multiband clean-bass matrix and normalized IMD comparisons. "
           "Synthetic only; test on the actual speaker before adopting.\n";
    return 0;
}
