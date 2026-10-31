#pragma once
#include <fstream>
#include <filesystem>
inline double SpeakerToneAmplitude(const std::vector<float>& a,double hz,double rate) {
    double re=0,im=0; const size_t start=a.size()/2;
    for(size_t i=start;i<a.size();++i) {double p=6.28318530717958647692*hz*i/rate; re+=a[i]*std::cos(p); im+=a[i]*std::sin(p);}
    return 2*std::hypot(re,im)/(a.size()-start);
}
inline void SpeakerWriteWave(const char* path,const std::vector<float>& a, double gain) {
    std::ofstream out(path,std::ios::binary);
    auto u16=[&](uint16_t n){char b[2]={char(n),char(n>>8)};out.write(b,2);};
    auto u32=[&](uint32_t n){char b[4]={char(n),char(n>>8),char(n>>16),char(n>>24)};out.write(b,4);};
    out.write("RIFF",4);u32(36+static_cast<uint32_t>(a.size()*2));out.write("WAVEfmt ",8);u32(16);u16(1);u16(1);u32(48000);u32(96000);u16(2);u16(16);out.write("data",4);u32(static_cast<uint32_t>(a.size()*2));
    for(float v:a)u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v*gain,-1.0,1.0)*32767))));
}
inline int RunSmallSpeakerTests() {
    using namespace HXAudioBridgeDsp;
    std::ofstream log("bridge-small-speaker.log"),csv("bridge-small-speaker-spectrum.csv");
    csv<<"rate,hz,input,strength,mode,h1,h2,h3,h4,target120to350,totalRms,thd2to10,above700harmonics,guardSamples\n";
    double minRatio=1e30,minFundamental=1e30,maxHighRatio=0,minMatchedRatio=1e30;size_t cases=0;
    for(UINT32 rate:{32000u,44100u,48000u,96000u})
    for(double hz:{40.,50.,60.,80.,100.})
    for(double amplitude:{0.15,0.5,0.98})
    for(double amount:{0.15,0.3,0.5,1.0}) {
        double target[2]{},total[2]{};
        for(int mode=0;mode<2;++mode) {
            Parameters p;p.enabled=true;p.amount=amount;p.smallSpeaker=mode!=0;p.gainDb=-6;
            Processor d;d.Configure(rate,1,p);std::vector<float> a(rate);
            for(size_t i=0;i<a.size();++i)a[i]=static_cast<float>(amplitude*std::sin(6.283185307179586*hz*i/rate));
            d.Process(a.data(),rate); HXAudioDsp::BridgePeakStats st;
            for(auto& v:a){if(!std::isfinite(v))return 1;v=HXAudioDsp::BridgePeakGuard(v,st);}
            if(st.hardClips||st.nonFinite)return 2;
            double thd=0,high=0;double h[11]{};
            for(int n=1;n<=10;++n) {
                h[n]=SpeakerToneAmplitude(a,hz*n,rate);
                if(n>=2)thd+=h[n]*h[n];
                if(n>=2&&hz*n>=120&&hz*n<=350)target[mode]+=h[n]*h[n];
                if(hz*n>700)high+=h[n]*h[n];
            }
            for(size_t i=a.size()/2;i<a.size();++i)total[mode]+=double(a[i])*a[i];
            total[mode]/=a.size()-a.size()/2;
            if(mode){minFundamental=std::min(minFundamental,h[1]/(amplitude*std::pow(10.,-6./20)));maxHighRatio=std::max(maxHighRatio,high/std::max(1e-20,target[mode]));}
            csv<<rate<<','<<hz<<','<<amplitude<<','<<amount<<','<<mode<<','<<h[1]<<','<<h[2]<<','<<h[3]<<','<<h[4]<<','<<target[mode]<<','<<std::sqrt(total[mode])<<','<<std::sqrt(thd)/std::max(1e-12,h[1])<<','<<high<<','<<st.protectedSamples<<'\n';
        }
        const double ratio=target[1]/std::max(1e-20,target[0]);minRatio=std::min(minRatio,ratio);
        if(amplitude==0.5&&amount==0.5)minMatchedRatio=std::min(minMatchedRatio,(target[1]/total[1])/(target[0]/total[0]));
        if(rate==48000&&amplitude==0.5&&amount==0.5)log<<"tone="<<hz<<" targetEnergyRatio="<<ratio<<" normalizedRatio="<<(target[1]/total[1])/(target[0]/total[0])<<'\n';
        ++cases;
    }
    log<<"cases="<<cases<<" minTargetEnergyRatio="<<minRatio<<" minFundamentalRetention="<<minFundamental<<" minMatchedRatioAt50="<<minMatchedRatio<<" maxAbove700ToTarget="<<maxHighRatio<<'\n';
    // Produce deterministic synthetic music and sequential bass tones, raw and
    // equal-RMS A/B. Normalization only attenuates; no extra limiter is used.
    std::filesystem::create_directories("speaker-ab");
    for(bool music:{false,true}) {
        std::vector<float> input(48000*10);
        constexpr double notes[]={40,50,60,80,100};
        for(size_t i=0;i<input.size();++i){double t=double(i)/48000;double f=notes[static_cast<int>(t/2)%5];double local=std::fmod(t,2.0);double fade=std::min({1.0,local/0.03,(2-local)/0.03});double bass=0.5*std::sin(6.283185307179586*f*t);
            if(music){double beat=std::fmod(t,0.5);bass=0.45*std::exp(-beat*7)*std::sin(6.283185307179586*f*t);bass+=0.10*std::sin(6.283185307179586*440*t)+0.06*std::sin(6.283185307179586*660*t)+0.04*std::exp(-beat*50)*std::sin(6.283185307179586*4000*t);}
            input[i]=static_cast<float>(fade*bass);
        }
        std::vector<float> a[2];double rms[2]{},peak[2]{};
        for(int m=0;m<2;++m){Parameters p;p.enabled=true;p.amount=0.5;p.smallSpeaker=m!=0;Processor d;d.Configure(48000,1,p);a[m]=input;d.Process(a[m].data(),static_cast<UINT32>(input.size()));HXAudioDsp::BridgePeakStats st;for(auto& v:a[m]){v=HXAudioDsp::BridgePeakGuard(v,st);rms[m]+=double(v)*v;peak[m]=std::max(peak[m],std::abs(double(v)));}rms[m]=std::sqrt(rms[m]/a[m].size());}
        double bandFraction[2]{};
        for(int m=0;m<2;++m){Biquad hp[2],lp[2];for(int k=0;k<2;++k){hp[k].Configure(MakeHighPass(48000,120,0.7071067811865476),1);lp[k].Configure(MakeLowPass(48000,350,0.7071067811865476),1);}double e=0;for(float sample:a[m]){for(int k=0;k<2;++k){sample=hp[k].Process(sample,0);sample=lp[k].Process(sample,0);}e+=double(sample)*sample;}bandFraction[m]=e/a[m].size()/(rms[m]*rms[m]);}
        log<<(music?"music":"tones")<<" matchedBandFractionRatio="<<bandFraction[1]/bandFraction[0]<<'\n';
        double harmonicFraction[2]{};
        for(int m=0;m<2;++m){double e=0;for(int note=0;note<5;++note){std::vector<float> segment(a[m].begin()+note*96000,a[m].begin()+(note+1)*96000);for(int h=2;h<=8;++h)if(notes[note]*h>=120&&notes[note]*h<=350){double v=SpeakerToneAmplitude(segment,notes[note]*h,48000);e+=v*v;}}harmonicFraction[m]=e/(rms[m]*rms[m]);}
        log<<(music?"music":"tones")<<" matchedNoteHarmonicRatio="<<harmonicFraction[1]/harmonicFraction[0]<<'\n';
        if(harmonicFraction[1]/harmonicFraction[0]<1.2)return 4;
        double targetRms=std::min(rms[0],rms[1]);
        for(int m=0;m<2;++m){std::string base=std::string("speaker-ab/")+(music?"music":"tones")+(m?"-B-small-speaker":"-A-original");SpeakerWriteWave((base+"-raw.wav").c_str(),a[m],1);SpeakerWriteWave((base+"-matched.wav").c_str(),a[m],targetRms/rms[m]);}
        log<<(music?"music":"tones")<<" rawRmsA="<<rms[0]<<" rawRmsB="<<rms[1]<<" matchedRms="<<targetRms<<" peakA="<<peak[0]<<" peakB="<<peak[1]<<'\n';
    }
    if(minMatchedRatio<1.1||minRatio<1.1||minFundamental<0.8||maxHighRatio>0.05){log<<"FAIL spectral acceptance\n";return 3;}
    log<<"PASS stronger target harmonics, bass retention, bounded upper harmonics; synthetic results only, real listening unverified.\n";return 0;
}
