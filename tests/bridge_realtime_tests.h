#pragma once
#include <fstream>
#include <thread>
#include <chrono>

inline int RunBridgeRealtimeTests() {
    using namespace HXAudioBridgeDsp;
    std::ofstream log("bridge-realtime-dsp.log");
    const auto check=[&](bool value,const char* label) { if (!value) log<<"FAIL "<<label<<'\n'; return value; };
    constexpr double pi=3.14159265358979323846;
    size_t cases=0;
    for (UINT32 rate : {32000u,44100u,48000u,96000u,192000u})
    for (double strength : {0.0,0.15,0.2,0.3,0.5,1.0}) {
        Control control; Parameters p; p.enabled=true; p.amount=strength;
        Processor dsp; dsp.Configure(rate,2,p,&control);
        std::array<float,254> block{};
        float previous[2]{}; double maxStep=0,maxRaw=0;
        HXAudioDsp::BridgePeakStats stats;
        uint64_t revision=0;
        // 2 s of bass at almost full scale, updates every ~2.6 ms at 48 kHz.
        for (UINT32 start=0;start<rate*2;start+=127) {
            if ((start/127)%3==0) {
                const int phase=(start/127)%12;
                p.amount=phase<6?strength:0; p.enabled=phase%2==0; p.smallSpeaker=phase%4==0;
                p.gainDb=phase<6?-6:-18;
                p.frequencyHz=phase<6?60:140; p.peqGainDb=phase<6?6:-6; p.q=phase<6?0.7:2;
                p.revision=++revision;
                if (!check(control.requests.Push(p),"single producer queue")) return 1;
            }
            for (size_t i=0;i<127;++i) {
                const double t=static_cast<double>(start+i)/rate;
                block[2*i]=static_cast<float>(0.98*std::sin(2*pi*50*t));
                block[2*i+1]=static_cast<float>(0.98*std::sin(2*pi*37*t+0.4));
            }
            dsp.Process(block.data(),127);
            for (size_t i=0;i<block.size();++i) {
                if (!check(std::isfinite(block[i]),"finite realtime DSP")) return 2;
                maxRaw=std::max(maxRaw,std::abs(double(block[i])));
                const float output=HXAudioDsp::BridgePeakGuard(block[i],stats);
                if (start || i>=2) maxStep=std::max(maxStep,std::abs(double(output-previous[i%2])));
                previous[i%2]=output;
            }
        }
        // Latest desired PEQ must converge despite repeated coalescing during fades.
        for (int i=0;i<200;++i) { block.fill(0); dsp.Process(block.data(),127); }
        auto status=dsp.CurrentStatus();
        log<<"rate="<<rate<<" strength="<<strength<<" maxStep="<<maxStep<<" raw="<<maxRaw<<" hard="<<stats.hardClips<<" revision="<<status.target.revision<<'\n';
        if (!check(maxStep<0.08 && maxRaw<4 && stats.nonFinite==0 && stats.hardClips==0,"smooth high-amplitude updates")) return 3;
        if (!check(status.target.revision==revision && !status.transitioning && status.peqHz==p.frequencyHz && status.amount==p.amount,"latest target convergence")) return 4;
        ++cases;
    }
    // Audition sequence requested for the slider; measure the wet harmonic
    // spectrum after each ramp settles, without changing the established wet gain.
    {
        Control c; Parameters p; p.enabled=true; p.amount=0; Processor d; d.Configure(48000,1,p,&c);
        double zeroEnergy=0;
        for (double amount : {0.0,0.15,0.3,0.5}) {
            p.amount=amount; ++p.revision; c.requests.Push(p);
            std::vector<float> tone(48000);
            for (size_t i=0;i<tone.size();++i) tone[i]=static_cast<float>(0.5*std::sin(2*pi*50*i/48000));
            d.Process(tone.data(),static_cast<UINT32>(tone.size()));
            double energy=0;
            for (int h : {2,3,4,5,7}) {
                double re=0,im=0;
                for (size_t i=24000;i<48000;++i) { const double phase=2*pi*50*h*i/48000; re+=tone[i]*std::cos(phase); im+=tone[i]*std::sin(phase); }
                const double amplitude=2*std::hypot(re,im)/24000; energy+=amplitude*amplitude;
            }
            if (amount==0) zeroEnergy=energy;
            else if (!check(energy>1e-7 && energy>zeroEnergy*100,"live wet harmonic spectrum")) return 11;
            log<<"slider amount="<<amount<<" wetHarmonicEnergy="<<energy<<'\n';
        }
    }
    // Gain ramp is exactly 20 ms, monotonic and bounded with a constant signal.
    {
        Control c; Parameters p; p.gainDb=-24; Processor d; d.Configure(48000,1,p,&c);
        p.gainDb=0; p.revision=1; c.requests.Push(p);
        float previous=0;
        for (int i=0;i<960;++i) {
            float sample=0.25f; d.Process(&sample,1);
            if (!check(sample>=previous && (i==0 || sample-previous<0.0003f),"20ms gain ramp")) return 5;
            previous=sample;
        }
        if (!check(std::abs(previous-0.25f)<1e-6 && !d.CurrentStatus().transitioning,"20ms ramp convergence")) return 6;
        p.q=std::numeric_limits<double>::quiet_NaN(); c.requests.Push(p);
        float sample=0.25f; d.Process(&sample,1);
        if (!check(d.CurrentStatus().target.revision==1 && std::isfinite(sample),"invalid snapshot rejected")) return 7;
    }
    // Optional makeup trim must be exactly transparent at 0 dB, settle without
    // a gain discontinuity, and remain independent of the bass/PEQ signal path.
    {
        Control c; Parameters p; p.gainDb=0; p.peqGainDb=0;
        p.amount=0; p.enabled=false; p.makeupDb=0;
        Processor d; d.Configure(48000,1,p,&c);
        float baseline=0.2f; d.Process(&baseline,1);
        if(!check(std::abs(baseline-0.2f)<1e-6f,"zero-makeup transparency"))return 16;
        p.makeupDb=3.0; ++p.revision; c.requests.Push(p);
        float previous=0.2f;
        for(int i=0;i<960;++i) {
            float sample=0.2f; d.Process(&sample,1);
            if(!check(std::isfinite(sample) && sample>=previous-1e-6f &&
                sample-previous<0.0002f,"makeup ramp monotonic"))return 17;
            previous=sample;
        }
        if(!check(std::abs(previous-0.2f*std::pow(10.0,3.0/20.0))<1e-5 &&
            std::abs(d.CurrentStatus().makeupDb-3.0)<1e-5 &&
            !d.CurrentStatus().transitioning,"makeup +3dB convergence"))return 18;
        p.makeupDb=7.0; ++p.revision; c.requests.Push(p);
        float sample=0.2f; d.Process(&sample,1);
        if(!check(d.CurrentStatus().target.makeupDb==3.0 && std::isfinite(sample),
            "out-of-range makeup rejected"))return 19;
    }
    // PEQ covers its complete accepted parameter range, including near Nyquist.
    for (UINT32 rate : {32000u,48000u,192000u}) {
        Parameters p; p.enabled=true; p.amount=0.5; Control c; Processor d; d.Configure(rate,2,p,&c);
        std::array<float,256> a{}; double maxPeak=0;
        for (int n=0;n<1800;++n) {
            p.frequencyHz=(n%4==0?20:n%4==1?20000:n%4==2?1000:70);
            p.q=n%2?0.1:10; p.peqGainDb=n%3?12:-12; p.revision=n+1;
            c.requests.Push(p);
            for (int i=0;i<128;++i) {
                const double t=static_cast<double>(n*128+i)/rate;
                const double kick=std::exp(-std::fmod(t,0.2)*30)*std::sin(2*pi*55*t);
                a[i*2]=static_cast<float>(0.5*kick+0.25*std::sin(2*pi*997*t)); a[i*2+1]=-a[i*2];
            }
            d.Process(a.data(),128);
            HXAudioDsp::BridgePeakStats st;
            for (float v:a) {
                if (!check(std::isfinite(v),"extreme PEQ finite")) return 8;
                maxPeak=std::max(maxPeak,std::abs(double(v)));
                if (!check(std::abs(HXAudioDsp::BridgePeakGuard(v,st))<1,"extreme PEQ protection")) return 9;
            }
        }
        log<<"extreme PEQ rate="<<rate<<" peak="<<maxPeak<<'\n';
    }
    // Concurrent synthetic audio/UI threads use the same production transport.
    // Producer never accesses DSP. Bursts deliberately saturate the mailbox.
    {
        Control c; Parameters initial; Processor d; d.Configure(48000,2,initial,&c);
        std::atomic<bool> done{false},failed{false}; std::atomic<size_t> received{0},full{0};
        std::thread producer([&] {
            for (uint64_t i=1;i<=100000;++i) {
                Parameters p; p.gainDb=-double(i%25); p.amount=double(i%101)/100;
                p.enabled=i%2!=0; p.smallSpeaker=i%3!=0; p.frequencyHz=20+double(i%19981);
                p.peqGainDb=double(i%25)-12; p.q=0.1+double(i%100)/10; p.makeupDb=double(i%7); p.revision=i;
                if (!c.requests.Push(p)) ++full;
            }
            Parameters final; final.revision=100001; final.enabled=true; final.amount=0.5;
            while (!c.requests.Push(final)) std::this_thread::yield();
            done.store(true,std::memory_order_release);
        });
        uint64_t last=0;
        for (int n=0;n<200000;++n) {
            std::array<float,256> a{};
            for (size_t i=0;i<a.size();++i) a[i]=static_cast<float>(0.4*std::sin((n*256.0+i)*0.01));
            d.Process(a.data(),128);
            const auto status=d.CurrentStatus();
            if (status.target.revision<last || !Valid(status.target)) failed=true;
            last=status.target.revision; ++received;
            for (float v:a) if (!std::isfinite(v)) failed=true;
            Status telemetry; c.status.Latest(telemetry);
            if (done.load(std::memory_order_acquire) && last==100001 && !status.transitioning) break;
        }
        producer.join();
        if (!check(!failed && last==100001 && !d.CurrentStatus().transitioning,"concurrent convergence/integrity")) return 10;
        log<<"concurrent snapshots=100001 processedBlocks="<<received<<" queueFull="<<full<<" final="<<last<<'\n';
    }
    log<<"PASS "<<cases<<" realtime sweeps; 20ms ramps; full-range PEQ; 100001 concurrent snapshots. Synthetic audio only, no WASAPI listening claim.\n";
    return 0;
}
