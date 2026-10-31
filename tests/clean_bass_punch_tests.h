#pragma once
#include <fstream>
#include <filesystem>

// Offline tests of the production clean-mode *wet-only* punch. Nothing here
// represents listening on the user's actual small loudspeaker.
inline int RunCleanBassPunchTests() {
    using namespace HXAudioBridgeDsp;
    std::ofstream log("bridge-bass-punch.log");
    std::ofstream csv("bridge-bass-punch-spectrum.csv");
    csv << "rate,hz,punchDb,target120to350,fundamental,rms,protected,overRange\n";
    constexpr double pi=3.14159265358979323846;
    auto check=[&](bool good,const char* reason,int code) {
        if (!good) log << "FAIL " << code << " " << reason << '\n';
        return good;
    };
    // Exercise the unchanged original and legacy output paths exactly,
    // independent of how much *inactive* clean-mode punch is requested.
    for (int selected=0;selected<2;++selected) {
        Parameters p; p.enabled=true; p.amount=0.5; p.gainDb=-6;
        p.smallSpeaker=(selected==1); p.cleanBass=false; p.punchDb=0;
        Processor baseline, boosted; baseline.Configure(48000,2,p);
        p.punchDb=6; boosted.Configure(48000,2,p);
        for (UINT32 block=0;block<600;++block) {
            std::array<float,256> a{}, b{};
            for (UINT32 i=0;i<128;++i) {
                double t=(block*128+i)/48000.0;
                a[2*i]=static_cast<float>(0.28*std::sin(2*pi*53*t));
                a[2*i+1]=static_cast<float>(0.22*std::sin(2*pi*91*t));
            }
            b=a;baseline.Process(a.data(),128);boosted.Process(b.data(),128);
            if(!check(a==b,"inactive punch changes original path",1))return 1;
        }
    }
    // Pure-tone spectrum: a clean punch increase must make the synthesized
    // bass harmonics stronger without lowering the input fundamental.
    for(UINT32 rate:{32000u,48000u,96000u}) {
        for(int hz:{40,50,60,80,100}) {
            double target[3]{},fundamental[3]{};
            for(int mode=0;mode<3;++mode) {
                Parameters p; p.enabled=true; p.smallSpeaker=true;
                p.cleanBass=true; p.amount=0.5; p.gainDb=-6;
                p.punchDb=3.0*mode;
                Processor dsp; dsp.Configure(rate,1,p);
                std::vector<float> samples(rate);
                for(UINT32 i=0;i<rate;++i)
                    samples[i]=static_cast<float>(0.4*std::sin(2*pi*hz*i/rate));
                dsp.Process(samples.data(),rate);
                HXAudioDsp::BridgePeakStats st;
                double rms=0;
                for(float& sample:samples) {
                    sample=HXAudioDsp::BridgePeakGuard(sample,st);
                    if(!check(std::isfinite(sample) && std::abs(sample)<1,"nonfinite or clipped output",2))return 2;
                    rms+=static_cast<double>(sample)*sample;
                }
                rms=std::sqrt(rms/samples.size());
                fundamental[mode]=SpeakerToneAmplitude(samples,hz,rate);
                for(int h:{2,3,4})if(h*hz>=120&&h*hz<=350) {
                    const double v=SpeakerToneAmplitude(samples,h*hz,rate);
                    target[mode]+=v*v;
                }
                csv <<rate<<','<<hz<<','<<p.punchDb<<','<<target[mode]<<','
                    <<fundamental[mode]<<','<<rms<<','<<st.protectedSamples<<','<<st.overRange<<'\n';
            }
            log<<"pureTone="<<hz<<" Hz at "<<rate
               <<" +3dB/0dB targetPower="<<target[1]/std::max(1e-20,target[0])
               <<" +6dB/0dB targetPower="<<target[2]/std::max(1e-20,target[0])<<'\n';
            if(!check(target[0]>1e-10 && target[1]>target[0]*1.35
                      && target[2]>target[1]*1.30,"punch does not increase target harmonics",3))return 3;
            for(int mode:{1,2})
                if(!check(fundamental[mode]>fundamental[0]*0.88,
                          "fundamental decreased with punch",4))return 4;
        }
    }
    // Two-note intermodulation must not grow *relative* to useful harmonic
    // power when simply scaling the existing clean mode's wet residual.
    // Under these moderate levels, downstream soft peak guard should be idle.
    for(const auto& pair:std::array<std::array<int,2>,3>{{{{50,90}},{{54,98}},{{58,102}}}}) {
        double ratios[3]{},totalRms[3]{};
        for(int mode=0;mode<3;++mode) {
            Parameters p; p.enabled=true; p.smallSpeaker=true;
            p.cleanBass=true; p.amount=0.5; p.gainDb=-9;
            p.punchDb=3.0*mode;
            Processor dsp; dsp.Configure(48000,1,p);
            std::vector<float> samples(48000);
            for(size_t i=0;i<samples.size();++i) {
                const double t=static_cast<double>(i)/48000.0;
                samples[i]=static_cast<float>(0.24*std::sin(2*pi*pair[0]*t)+
                                               0.24*std::sin(2*pi*pair[1]*t));
            }
            dsp.Process(samples.data(),static_cast<UINT32>(samples.size()));
            HXAudioDsp::BridgePeakStats st;
            for(float& sample:samples) {
                sample=HXAudioDsp::BridgePeakGuard(sample,st);
                if(!check(std::isfinite(sample),"invalid two-tone sample",5))return 5;
                totalRms[mode]+=static_cast<double>(sample)*sample;
            }
            totalRms[mode]=std::sqrt(totalRms[mode]/samples.size());
            double target=0,imd=0;
            for(int f:{pair[0],pair[1]})
                for(int h:{2,3,4})if(f*h>=120&&f*h<=350) {
                    const double mag=SpeakerToneAmplitude(samples,f*h,48000);
                    target+=mag*mag;
                }
            for(int f:{pair[0]+pair[1],2*pair[0]+pair[1],
                       2*pair[1]+pair[0],std::abs(2*pair[0]-pair[1]),
                       std::abs(2*pair[1]-pair[0])}) {
                if(f<120||f>350)continue;
                const double mag=SpeakerToneAmplitude(samples,f,48000);
                imd+=mag*mag;
            }
            ratios[mode]=imd/std::max(1e-20,target);
        }
        log<<"twoTone="<<pair[0]<<"+"<<pair[1]
           <<" IMD/target 0,+3,+6dB="<<ratios[0]<<","
           <<ratios[1]<<","<<ratios[2]<<'\n';
        if(!check(ratios[1]<=ratios[0]*1.25 && ratios[2]<=ratios[0]*1.30,
                  "new punch disproportionately raises two-tone IMD",6))return 6;
    }
    // Live updates use production SPSC transport and exactly 20ms ramp.
    {
        Control c; Parameters p; p.enabled=true;p.smallSpeaker=true;
        p.cleanBass=true;p.amount=0.5;p.punchDb=0;
        Processor dsp;dsp.Configure(48000,1,p,&c);
        p.punchDb=4;p.revision=1;c.requests.Push(p);
        for(int i=0;i<960;++i) {
            float x=0.25f*std::sin(2*pi*50*i/48000.0);
            dsp.Process(&x,1);
            if(!check(std::isfinite(x),"realtime punch sample invalid",7))return 7;
        }
        const Status st=dsp.CurrentStatus();
        if(!check(st.target.revision==1 && !st.transitioning &&
                  std::abs(st.punchDb-4)<1e-5,"20ms punch ramp did not converge",8))return 8;
        p.punchDb=7;p.revision=2;c.requests.Push(p);
        float x=0.1f;dsp.Process(&x,1);
        if(!check(dsp.CurrentStatus().target.revision==1,
                  "invalid punch snapshot incorrectly accepted",9))return 9;
    }
    // Full-scale stress has to remain finite and guarded even at maximum
    // clean-bass strength and maximum punch.
    {
        Parameters p; p.enabled=true;p.smallSpeaker=true;p.cleanBass=true;
        p.amount=1;p.gainDb=0;p.punchDb=6;
        Processor dsp;dsp.Configure(48000,2,p);
        HXAudioDsp::BridgePeakStats st;
        for(int block=0;block<750;++block) {
            std::array<float,256> a{};
            for(int i=0;i<128;++i) {
                const double t=(block*128+i)/48000.0;
                a[2*i]=static_cast<float>(0.95*std::sin(2*pi*49*t)+0.65*std::sin(2*pi*92*t));
                a[2*i+1]=static_cast<float>(0.95*std::sin(2*pi*52*t)+0.65*std::sin(2*pi*90*t));
            }
            dsp.Process(a.data(),128);
            for(float x:a) {
                float y=HXAudioDsp::BridgePeakGuard(x,st);
                if(!check(std::isfinite(y)&&std::abs(y)<1,"full-scale guard failed",10))return 10;
            }
        }
        log<<"stress protectedSamples="<<st.protectedSamples
           <<" overRange="<<st.overRange<<" hardClips="<<st.hardClips<<'\n';
        if(!check(st.hardClips==0 && st.nonFinite==0,"max stress hard clips",11))return 11;
    }
    // Generate synthetic, pre-processed, level-matched WAVs for A/B/C.
    // These contain original generated tones, no copyrighted song playback.
    std::filesystem::create_directories("punch-ab");
    constexpr UINT32 rate=48000;
    std::vector<float> input(rate*6);
    for(size_t i=0;i<input.size();++i) {
        const double t=static_cast<double>(i)/rate;
        const double beat=std::fmod(t,0.48);
        input[i]=static_cast<float>(
            0.26*std::sin(2*pi*50*t)+0.17*std::sin(2*pi*90*t)+
            0.16*std::exp(-beat*16.0)*std::sin(2*pi*58*t)+
            0.08*std::sin(2*pi*440*t)+0.06*std::sin(2*pi*660*t));
    }
    std::vector<float> outputs[3];
    double rms[3]{};
    for(int mode=0;mode<3;++mode) {
        Parameters p;p.enabled=true;p.smallSpeaker=true;p.cleanBass=true;
        p.gainDb=-6;p.amount=0.5;p.punchDb=3.0*mode;
        Processor dsp;dsp.Configure(rate,1,p);
        outputs[mode]=input;
        dsp.Process(outputs[mode].data(),static_cast<UINT32>(input.size()));
        HXAudioDsp::BridgePeakStats st;
        for(auto& x:outputs[mode]) {
            x=HXAudioDsp::BridgePeakGuard(x,st);
            rms[mode]+=static_cast<double>(x)*x;
        }
        rms[mode]=std::sqrt(rms[mode]/outputs[mode].size());
    }
    double matched=*std::min_element(rms,rms+3);
    SpeakerWriteWave("punch-ab/A-clean-0dB-matched.wav",outputs[0],matched/rms[0]);
    SpeakerWriteWave("punch-ab/B-punch-plus3dB-matched.wav",outputs[1],matched/rms[1]);
    SpeakerWriteWave("punch-ab/C-punch-plus6dB-matched.wav",outputs[2],matched/rms[2]);
    log<<"matchedRMS="<<matched<<" originalRMS="<<rms[0]
       <<" boosted3RMS="<<rms[1]<<" boosted6RMS="<<rms[2]<<'\n';
    log<<"PASS clean bass wet-only punch, baseline transparency, harmonics, IMD, ramp and clipping safety.\n";
    return 0;
}
