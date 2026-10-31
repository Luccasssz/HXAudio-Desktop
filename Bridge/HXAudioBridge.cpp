#include "BuildVersion.h"
// Dedicated entry point: the legacy control panel entry point is never called.
// Reuse its DSP, loopback conversion and output worker without its routing policy.
#define wWinMain HXAudioLegacyEntryPoint
#include "../ControlPanel/HXAudioControl.cpp"
#undef wWinMain
#include "../Shared/HXAudioBridgePeakGuard.h"
#include "../tests/virtual_bass_tests.h"
#include "../Shared/HXAudioBridgeRealtimeDsp.h"
#include "../tests/bridge_realtime_tests.h"
#include "../tests/small_speaker_tests.h"
#include "../tests/clean_bass_tests.h"
#include "../tests/clean_bass_punch_tests.h"

namespace bridge {
constexpr int kSource = 4101, kTarget = 4102, kRefresh = 4103, kStart = 4104,
    kStop = 4105, kGain = 4106, kBass = 4107, kFreq = 4108, kPeqGain = 4109,
    kQ = 4110, kBuffer = 4111, kStatus = 4112, kBassAmount = 4113, kBassSlider = 4114, kGainSlider = 4115, kApplied = 4116, kSmallSpeaker = 4117, kCleanBass = 4118, kPunch = 4119;
HWND sourceBox = nullptr, targetBox = nullptr, statusText = nullptr, appliedText = nullptr;
std::unique_ptr<HXAudioBridgeDsp::Control> liveControl;
HXAudioBridgeDsp::Parameters desired;
uint64_t nextRevision = 0;
bool pendingPublish = false, syncingControls = false;
void SessionControls(HWND w, bool running) {
    for (int id : {kSource,kTarget,kBuffer,kRefresh,kStart}) EnableWindow(GetDlgItem(w,id), !running);
    EnableWindow(GetDlgItem(w,kStop), running);
}
std::vector<DeviceInfo> sources, targets;
HANDLE worker = nullptr, stop = nullptr;
std::atomic<long> state{0}, failure{0}, inputPeak{0}, outputPeak{0};
std::atomic<unsigned long long> dspOver{0}, dspInvalid{0}, protectedSamples{0},
    hardClips{0}, sourceInvalid{0}, resamplerProtected{0}, underruns{0}, missingFrames{0}, discontinuities{0};
bool failureNotified = false;
struct Config { std::wstring source, target; HXAudioBridgeDsp::Parameters parameters; DWORD buffer = 10; } config;

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}
bool IsSource(const std::wstring& name) {
    const auto n = Lower(name);
    return n.find(L"voicemeeter input") != std::wstring::npos ||
           n.find(L"voicemeeter aux input") != std::wstring::npos ||
           n.find(L"voicemeeter vaio3 input") != std::wstring::npos;
}
bool IsAllowedTarget(const std::wstring& sourceId, const std::wstring& targetId,
    const std::wstring& name) {
    const auto n = Lower(name);
    return !sourceId.empty() && !targetId.empty() && _wcsicmp(sourceId.c_str(), targetId.c_str()) != 0 &&
        n.find(L"voicemeeter") == std::wstring::npos && n.find(L"vb-audio") == std::wstring::npos &&
        n.find(L"hxaudio") == std::wstring::npos && n.find(L"virtual") == std::wstring::npos &&
        n.find(L"cable input") == std::wstring::npos;
}
int SelfTest() {
    if (!IsSource(L"VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)") ||
        IsAllowedTarget(L"same",L"same",L"Realtek") ||
        IsAllowedTarget(L"a",L"b",L"VoiceMeeter Input") ||
        !IsAllowedTarget(L"a",L"b",L"Realtek Speakers")) return 1;
    WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2;
    format.nSamplesPerSec = 48000; format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
    std::vector<int16_t> pcm(960);
    for (size_t i = 0; i < 480; ++i) pcm[i*2] = pcm[i*2+1] = static_cast<int16_t>(12000 * std::sin(2.0 * 3.141592653589793 * i * 1000.0 / 48000.0));
    std::vector<float> input; if (!ConvertToFloatBus(reinterpret_cast<const BYTE*>(pcm.data()),480,&format,input)) return 2;
    const auto original = input; DspSettings settings; settings.preampDb = -6; settings.peqBands.push_back({true,PeqFilterType::Peak,1000,6,0.9});
    DspProcessor dsp; dsp.Configure(48000,2,settings); dsp.Process(input.data(),480);
    bool changed = false; for (size_t i=0;i<input.size();++i) { if (!std::isfinite(input[i])) return 3; if (std::abs(input[i]-original[i])>0.0001f) changed=true; }
    if (!changed) return 4;
    ByteRing ring(input.size()*sizeof(float)*2, sizeof(float)*2); ring.SetPrimeTarget(sizeof(float)*2);
    ring.Write(reinterpret_cast<const BYTE*>(input.data()),input.size()*sizeof(float));
    std::vector<float> out(input.size()); if (ring.Read(reinterpret_cast<BYTE*>(out.data()),out.size()*sizeof(float)) != out.size()*sizeof(float) || out != input) return 5;
    if (!ConvertToFloatBus(nullptr,480,&format,out) || std::any_of(out.begin(),out.end(),[](float v){return v!=0.0f;})) return 6;
    return 0;
}
void Refresh() {
    if (worker) return;
    sources.clear(); targets.clear();
    SendMessageW(sourceBox, CB_RESETCONTENT, 0, 0); SendMessageW(targetBox, CB_RESETCONTENT, 0, 0);
    ComPtr<IMMDeviceEnumerator> e; ComPtr<IMMDeviceCollection> list;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e))) ||
        FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
        SetWindowTextW(statusText, L"无法枚举播放设备。请检查 Windows 音频服务。"); return;
    }
    UINT count = 0; list->GetCount(&count); int preferred = -1;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> d; if (FAILED(list->Item(i, &d))) continue;
        const std::wstring name = DeviceName(d.Get()), id = DeviceId(d.Get());
        if (id.empty()) continue;
        if (IsSource(name)) { sources.push_back({id,name,{}}); SendMessageW(sourceBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str())); }
        else if (IsAllowedTarget(L"source", id, name)) {
            if (preferred < 0 && Lower(name).find(L"realtek") != std::wstring::npos) preferred = static_cast<int>(targets.size());
            targets.push_back({id,name,{}}); SendMessageW(targetBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    if (!sources.empty()) SendMessageW(sourceBox, CB_SETCURSEL, 0, 0);
    if (!targets.empty()) SendMessageW(targetBox, CB_SETCURSEL, preferred >= 0 ? preferred : 0, 0);
    SetWindowTextW(statusText, sources.empty() ? L"未找到 Voicemeeter Input；请先安装并启用它。" :
        targets.empty() ? L"未找到可用的物理输出设备。" : L"就绪：请手动将播放程序输出设为所选 Voicemeeter Input。");
}
HRESULT RunSession(const Config& c, HANDLE stopEvent) {
    ComPtr<IMMDeviceEnumerator> e; ComPtr<IMMDevice> source, target;
    ComPtr<IAudioClient> client; ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* fmt = nullptr;
    HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE failedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!captureEvent || !failedEvent) { if (captureEvent) CloseHandle(captureEvent); if (failedEvent) CloseHandle(failedEvent); return E_OUTOFMEMORY; }
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    if (SUCCEEDED(hr)) hr = e->GetDevice(c.source.c_str(), &source);
    if (SUCCEEDED(hr)) hr = e->GetDevice(c.target.c_str(), &target);
    if (SUCCEEDED(hr) && (!IsSource(DeviceName(source.Get())) || !IsAllowedTarget(c.source, c.target, DeviceName(target.Get())))) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = source->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
    WAVEFORMATEXTENSIBLE bus{}; if (SUCCEEDED(hr)) bus = BuildFloatBusFormat(fmt);
    HXAudioBridgeDsp::Processor dsp; if (SUCCEEDED(hr)) dsp.Configure(bus.Format.nSamplesPerSec, bus.Format.nChannels, c.parameters, liveControl.get());
    std::unique_ptr<TargetWorker> output;
    if (SUCCEEDED(hr)) {
        output = std::make_unique<TargetWorker>(c.target, DeviceName(target.Get()), L"ENDPOINT|" + c.target,
            &bus.Format, c.buffer, stopEvent, failedEvent, 1, false, false, true);
        if (!output->Start()) hr = E_FAIL;
        else if (!output->WaitUntilReady(10000) || !output->IsActive()) hr = FAILED(output->Result()) ? output->Result() : E_FAIL;
    }
    bool eventDriven = false; UINT32 period = 0;
    if (SUCCEEDED(hr)) { client.Reset(); hr = ActivateSharedClient(source.Get(), fmt,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, c.buffer, client, eventDriven, period); }
    if (SUCCEEDED(hr) && eventDriven) hr = client->SetEventHandle(captureEvent);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (SUCCEEDED(hr)) state.store(2);
    std::vector<float> samples; ULONGLONG lastPacket = GetTickCount64(), nextStateCheck = 0;
    while (SUCCEEDED(hr) && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE events[] = {stopEvent, failedEvent, captureEvent};
        const DWORD wait = WaitForMultipleObjects(eventDriven ? 3 : 2, events, FALSE, eventDriven ? 500 : std::max<DWORD>(1, c.buffer / 2));
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 1) { hr = AUDCLNT_E_DEVICE_INVALIDATED; break; }
        if (wait != WAIT_TIMEOUT && (!eventDriven || wait != WAIT_OBJECT_0 + 2)) { hr = E_FAIL; break; }
        if (GetTickCount64() >= nextStateCheck) {
            DWORD ss = 0, ts = 0;
            if (FAILED(source->GetState(&ss)) || FAILED(target->GetState(&ts)) || ss != DEVICE_STATE_ACTIVE || ts != DEVICE_STATE_ACTIVE) { hr = AUDCLNT_E_DEVICE_INVALIDATED; break; }
            nextStateCheck = GetTickCount64() + 500;
        }
        underruns.store(output->UnderrunCallbacks()); missingFrames.store(output->MissingFrames());
        resamplerProtected.store(output->ResamplerProtected());
        UINT32 packet = 0; hr = capture->GetNextPacketSize(&packet);
        if (packet == 0 && GetTickCount64() - lastPacket > 1000) { inputPeak.store(0); outputPeak.store(0); }
        while (SUCCEEDED(hr) && packet) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr); if (FAILED(hr)) break;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) discontinuities.fetch_add(1);
            bool converted = ConvertToFloatBus((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, frames, fmt, samples);
            if (converted) {
                lastPacket = GetTickCount64();
                for (auto& sample : samples) if (!std::isfinite(sample)) { sourceInvalid.fetch_add(1); sample = 0.0f; }
                inputPeak.store(FloatPeakQ31(samples)); dsp.Process(samples.data(), frames);
                HXAudioDsp::BridgePeakStats stats;
                for (auto& sample : samples) sample = HXAudioDsp::BridgePeakGuard(sample, stats);
                dspOver.fetch_add(stats.overRange); dspInvalid.fetch_add(stats.nonFinite);
                protectedSamples.fetch_add(stats.protectedSamples); hardClips.fetch_add(stats.hardClips);
                outputPeak.store(FloatPeakQ31(samples)); output->Push(samples.data(), frames);
                underruns.store(output->UnderrunCallbacks()); missingFrames.store(output->MissingFrames());
            }
            const HRESULT release = capture->ReleaseBuffer(frames);
            if (FAILED(release)) hr = release; else if (!converted) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
            if (SUCCEEDED(hr)) hr = capture->GetNextPacketSize(&packet);
        }
    }
    if (client) client->Stop();
    SetEvent(stopEvent); if (output) { output->Join(); underruns.store(output->UnderrunCallbacks()); missingFrames.store(output->MissingFrames()); }
    if (fmt) CoTaskMemFree(fmt); CloseHandle(failedEvent); CloseHandle(captureEvent);
    return hr;
}
DWORD WINAPI Thread(void*) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT hr = FAILED(co) ? co : RunSession(config, stop);
    if (SUCCEEDED(co)) CoUninitialize();
    failure.store(static_cast<long>(hr)); state.store(FAILED(hr) ? 3 : 0); return 0;
}
void Stop() {
    if (stop) SetEvent(stop);
    if (worker) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); worker = nullptr; }
    if (stop) { CloseHandle(stop); stop = nullptr; }
    liveControl.reset(); pendingPublish=false;
}
bool Number(HWND w, int id, double minimum, double maximum, double& value) {
    wchar_t text[64]{};
    if (GetWindowTextLengthW(GetDlgItem(w,id)) >= static_cast<int>(ARRAYSIZE(text))) return false;
    GetDlgItemTextW(w, id, text, ARRAYSIZE(text)); wchar_t* end = nullptr;
    value = wcstod(text, &end); return end != text && *end == 0 && end[-1] != L'.' && std::isfinite(value) && value >= minimum && value <= maximum;
}
bool ReadDsp(HWND w, HXAudioBridgeDsp::Parameters& p) {
    double percent=0;
    if (!Number(w,kGain,-24,0,p.gainDb) || !Number(w,kFreq,20,20000,p.frequencyHz) ||
        !Number(w,kPeqGain,-12,12,p.peqGainDb) || !Number(w,kQ,0.1,10,p.q) ||
        !Number(w,kBassAmount,0,100,percent) || !Number(w,kPunch,0,6,p.punchDb)) return false;
    p.smallSpeaker=SendDlgItemMessageW(w,kSmallSpeaker,BM_GETCHECK,0,0)==BST_CHECKED;
    p.cleanBass=SendDlgItemMessageW(w,kCleanBass,BM_GETCHECK,0,0)==BST_CHECKED;
    p.amount=percent/100; p.enabled=SendDlgItemMessageW(w,kBass,BM_GETCHECK,0,0)==BST_CHECKED;
    return HXAudioBridgeDsp::Valid(p);
}
void UpdateLive(HWND w) {
    HXAudioBridgeDsp::Parameters p;
    if (!ReadDsp(w,p)) {
        SetWindowTextW(appliedText,L"输入未完成或超出范围：保留上次有效参数。前级 −24…0 dB，PEQ 20…20000 Hz / ±12 dB / Q 0.1…10，低频 0…100%，清晰低频冲击力 0…+6 dB。");
        return;
    }
    SendDlgItemMessageW(w,kBassSlider,TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(p.amount*100)));
    SendDlgItemMessageW(w,kGainSlider,TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround((p.gainDb+24)*10)));
    if (!HXAudioBridgeDsp::SameValues(p,desired)) {
        desired=p; desired.revision=++nextRevision; pendingPublish=true;
    }
    if (liveControl && pendingPublish && liveControl->requests.Push(desired)) pendingPublish=false;
    if (!liveControl) SetWindowTextW(appliedText,L"DSP 参数已就绪；播放中可实时调整。低频默认 15%，可尝试 30% 或 50%。");
}
void Start(HWND w) {
    if (worker) return;
    if (RouterProcessIsRunning()) { MessageBoxW(w,L"检测到旧版 HXAudio 路由器正在运行。请先关闭它，避免重复播放。",L"HXAudio Bridge",MB_ICONWARNING); return; }
    const LRESULT a = SendMessageW(sourceBox, CB_GETCURSEL, 0, 0), b = SendMessageW(targetBox, CB_GETCURSEL, 0, 0);
    if (a < 0 || b < 0 || static_cast<size_t>(a) >= sources.size() || static_cast<size_t>(b) >= targets.size()) { MessageBoxW(w, L"请选择 Voicemeeter 输入和物理输出。", L"HXAudio Bridge", MB_ICONWARNING); return; }
    Config next; next.source = sources[a].id; next.target = targets[b].id;
    double buffer = 0;
    if (!IsAllowedTarget(next.source, next.target, targets[b].name) ||
        !ReadDsp(w,next.parameters) || !Number(w,kBuffer,5,100,buffer)) {
        MessageBoxW(w,L"请选择有效设备和 DSP 参数；缓冲范围为 5–100 ms。",L"HXAudio Bridge",MB_ICONWARNING); return;
    }
    desired=next.parameters; desired.revision=++nextRevision; next.parameters=desired;
    liveControl=std::make_unique<HXAudioBridgeDsp::Control>(); pendingPublish=false;
    next.buffer = static_cast<DWORD>(buffer);
    config = std::move(next); stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) { liveControl.reset(); state.store(3); failure.store(E_OUTOFMEMORY); return; }
    sourceInvalid=0; resamplerProtected=0; dspOver=0; dspInvalid=0; protectedSamples=0; hardClips=0; underruns=0; missingFrames=0; discontinuities=0;
    state.store(1); inputPeak.store(0); outputPeak.store(0); failure.store(0); failureNotified = false;
    worker = CreateThread(nullptr,0,Thread,nullptr,0,nullptr);
    if (!worker) { CloseHandle(stop); stop = nullptr; liveControl.reset(); state.store(3); failure.store(E_FAIL); }
    else SessionControls(w,true);
}
HWND Add(HWND w, const wchar_t* klass, const wchar_t* label, DWORD style, int x,int y,int width,int height,int id = 0) {
    return CreateWindowW(klass,label,WS_CHILD|WS_VISIBLE|style,x,y,width,height,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
}
LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        Add(w,L"STATIC",L"Voicemeeter 输入：",0,15,15,160,22);
        sourceBox=Add(w,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,15,38,500,200,kSource);
        Add(w,L"STATIC",L"物理输出：",0,15,75,160,22);
        targetBox=Add(w,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,15,98,500,200,kTarget);
        Add(w,L"BUTTON",L"刷新设备",0,525,38,100,27,kRefresh);
        Add(w,L"STATIC",L"前级 dB（实时）",0,15,140,135,22); Add(w,L"EDIT",L"-6",WS_BORDER|WS_TABSTOP,150,137,60,25,kGain);
        HWND gainSlider=Add(w,TRACKBAR_CLASSW,L"",TBS_HORZ|WS_TABSTOP,220,133,390,35,kGainSlider);
        SendMessageW(gainSlider,TBM_SETRANGE,TRUE,MAKELPARAM(0,240)); SendMessageW(gainSlider,TBM_SETPOS,TRUE,180);
        Add(w,L"BUTTON",L"虚拟低频（实时）",BS_AUTOCHECKBOX|WS_TABSTOP,15,180,185,25,kBass);
        Add(w,L"STATIC",L"强度 %",0,210,182,70,22); Add(w,L"EDIT",L"15",WS_BORDER|WS_TABSTOP,280,179,55,25,kBassAmount);
        HWND bassSlider=Add(w,TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_AUTOTICKS|WS_TABSTOP,345,175,265,35,kBassSlider);
        SendMessageW(bassSlider,TBM_SETRANGE,TRUE,MAKELPARAM(0,100)); SendMessageW(bassSlider,TBM_SETTICFREQ,10,0); SendMessageW(bassSlider,TBM_SETPOS,TRUE,15);
        Add(w,L"BUTTON",L"小扬声器优化（取消勾选 = 原模式 A/B）",BS_AUTOCHECKBOX|WS_TABSTOP,15,213,580,25,kSmallSpeaker);
        Add(w,L"BUTTON",L"清晰低频试验（多频带；仅小扬声器优化生效）",BS_AUTOCHECKBOX|WS_TABSTOP,15,241,590,25,kCleanBass);
        Add(w,L"STATIC",L"PEQ Hz（实时）",0,15,283,130,22); Add(w,L"EDIT",L"1000",WS_BORDER|WS_TABSTOP,145,280,75,25,kFreq);
        Add(w,L"STATIC",L"dB",0,235,283,30,22); Add(w,L"EDIT",L"0",WS_BORDER|WS_TABSTOP,270,280,60,25,kPeqGain);
        Add(w,L"STATIC",L"Q",0,345,283,25,22); Add(w,L"EDIT",L"0.9",WS_BORDER|WS_TABSTOP,375,280,60,25,kQ);
        Add(w,L"STATIC",L"低频力度 +dB",0,440,283,110,22); Add(w,L"EDIT",L"0",WS_BORDER|WS_TABSTOP,554,280,55,25,kPunch);
        appliedText=Add(w,L"STATIC",L"",0,15,318,610,65,kApplied);
        Add(w,L"STATIC",L"会话参数：输入 / 物理输出 / 缓冲需停止后修改",0,15,393,470,22);
        Add(w,L"STATIC",L"缓冲 ms",0,15,423,80,22); Add(w,L"EDIT",L"20",WS_BORDER|WS_TABSTOP,95,420,55,25,kBuffer);
        Add(w,L"BUTTON",L"开始处理",WS_TABSTOP,175,417,120,30,kStart); Add(w,L"BUTTON",L"停止",WS_TABSTOP,310,417,100,30,kStop);
        statusText=Add(w,L"STATIC",L"",0,15,463,610,110,kStatus);
        Add(w,L"STATIC",L"路由提示：请在 Voicemeeter 中关闭直通物理输出，并避免输出回送输入。无法自动检测其内部路由，不能保证无反馈。停止桥接后请手动切回正常播放设备。",0,15,583,610,65);
        SetTimer(w,1,100,nullptr); Refresh(); SessionControls(w,false); UpdateLive(w); return 0;
    }
    case WM_HSCROLL: {
        const int id=GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id==kBassSlider || id==kGainSlider) {
            const LRESULT pos=SendMessageW(reinterpret_cast<HWND>(lp),TBM_GETPOS,0,0);
            wchar_t text[40]{};
            if (id==kBassSlider) swprintf_s(text,L"%ld",static_cast<long>(pos));
            else swprintf_s(text,L"%.1f",pos/10.0-24);
            syncingControls=true; SetDlgItemTextW(w,id==kBassSlider?kBassAmount:kGain,text); syncingControls=false;
            UpdateLive(w);
        }
        return 0;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wp), notification=HIWORD(wp);
        if (notification==EN_CHANGE && !syncingControls &&
            (id==kGain || id==kFreq || id==kPeqGain || id==kQ || id==kBassAmount || id==kPunch)) {
            SetTimer(w,2,60,nullptr); return 0; // debounce incomplete typing
        }
        if ((id==kBass || id==kSmallSpeaker || id==kCleanBass) && notification==BN_CLICKED) { UpdateLive(w); return 0; }
        switch (id) {
        case kRefresh: Refresh(); break;
        case kStart: Start(w); break;
        case kStop: Stop(); state.store(0); SessionControls(w,false); SetWindowTextW(statusText,L"已停止。请手动切换播放设备。"); break;
        } return 0;
    }
    case WM_TIMER: {
        if (wp==2) { KillTimer(w,2); UpdateLive(w); return 0; }
        if (liveControl) {
            if (pendingPublish && liveControl->requests.Push(desired)) pendingPublish=false;
            HXAudioBridgeDsp::Status applied;
            if (liveControl->status.Latest(applied)) {
                wchar_t text[350]{};
                swprintf_s(text,L"当前 DSP：前级 %.1f dB / 低频 %.1f%%（%s）\nPEQ %.1f Hz / %.1f dB / Q %.2f；%s / %s / %s（力度 +%.1f dB）",applied.gainDb,applied.amount*100,applied.enableMix==0?L"关闭":applied.enableMix==1?L"开启":L"切换中",applied.peqHz,applied.peqDb,applied.peqQ,applied.transitioning?L"平滑过渡中":L"已应用",applied.target.smallSpeaker?L"小扬声器":L"原模式",applied.target.smallSpeaker?(applied.target.cleanBass?L"清晰低频试验":L"饱满低频原版"):L"—",applied.punchDb);
                HXAudioBridgeDsp::Parameters edited;
                if (ReadDsp(w,edited)) SetWindowTextW(appliedText,text);
            }
        }
        const long s=state.load(); if (s==2) { wchar_t text[600]{}; swprintf_s(text,L"运行中：输入峰值 %.1f%%，处理后 %.1f%%。\nDSP 超幅 %llu / 非有限 %llu / 软保护 %llu / 硬削波 %llu\n渲染缺数据 %llu 次 / 缺帧 %llu / 捕获不连续 %llu（累计，含启动瞬态）\n输入非有限 %llu / 重采样软保护 %llu",inputPeak.load()*100.0/2147483647.0,outputPeak.load()*100.0/2147483647.0,dspOver.load(),dspInvalid.load(),protectedSamples.load(),hardClips.load(),underruns.load(),missingFrames.load(),discontinuities.load(),sourceInvalid.load(),resamplerProtected.load()); SetWindowTextW(statusText,text); }
        else if (s==1) SetWindowTextW(statusText,L"正在连接设备…");
        else if (s==3) { wchar_t text[240]{}; swprintf_s(text,L"桥接已停止（0x%08lX）。检查设备，刷新后手动重试；请切回系统输出。",static_cast<unsigned long>(failure.load())); SetWindowTextW(statusText,text); if (worker && WaitForSingleObject(worker,0)==WAIT_OBJECT_0) { Stop(); SessionControls(w,false); } if (!failureNotified) { failureNotified = true; MessageBoxW(w,text,L"HXAudio Bridge 已断开",MB_OK|MB_ICONWARNING); } }
        return 0;
    }
    case WM_CLOSE: Stop(); DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
int RealtimeUiTest(HWND window) {
    std::ofstream log("bridge-realtime-ui.log");
    liveControl=std::make_unique<HXAudioBridgeDsp::Control>();
    HXAudioBridgeDsp::Parameters initial;
    HXAudioBridgeDsp::Processor processor; processor.Configure(48000,2,initial,liveControl.get());
    std::atomic<bool> finish{false},failed{false}; std::atomic<uint64_t> settled{0},blocks{0};
    std::thread audio([&] {
        while (!finish.load()) {
            std::array<float,256> samples{};
            for (size_t i=0;i<samples.size();++i) samples[i]=static_cast<float>(0.5*std::sin((blocks.load()*256+i)*0.01));
            processor.Process(samples.data(),128);
            for (float sample:samples) if (!std::isfinite(sample)) failed=true;
            auto status=processor.CurrentStatus(); if (!status.transitioning) settled.store(status.target.revision);
            ++blocks; std::this_thread::yield();
        }
    });
    SessionControls(window,true);
    bool valid=!IsWindowEnabled(sourceBox) && !IsWindowEnabled(targetBox) && !IsWindowEnabled(GetDlgItem(window,kBuffer));
    for (const wchar_t* invalid : {L"",L"-",L"1.",L"NaN",L"Inf",L"1e",L"11"}) {
        const uint64_t before=desired.revision;
        SetDlgItemTextW(window,kQ,invalid); SendMessageW(window,WM_TIMER,2,0);
        valid=valid && desired.revision==before;
    }
    SetDlgItemTextW(window,kQ,L"0.9"); SendMessageW(window,WM_TIMER,2,0);
    for (int i=0;i<1000;++i) {
        SendDlgItemMessageW(window,kBassSlider,TBM_SETPOS,TRUE,i%101);
        SendMessageW(window,WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(GetDlgItem(window,kBassSlider)));
        SendDlgItemMessageW(window,kGainSlider,TBM_SETPOS,TRUE,i%241);
        SendMessageW(window,WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(GetDlgItem(window,kGainSlider)));
        wchar_t text[40]{}; swprintf_s(text,L"%d",20+(i*71)%19981); SetDlgItemTextW(window,kFreq,text);
        swprintf_s(text,L"%d",i%25-12); SetDlgItemTextW(window,kPeqGain,text);
        swprintf_s(text,L"%.1f",0.1+(i%100)/10.0); SetDlgItemTextW(window,kQ,text);
        SetDlgItemTextW(window,kPunch,i%3==0?L"0":i%3==1?L"3":L"6");
        SendMessageW(window,WM_TIMER,2,0);
        SendDlgItemMessageW(window,kBass,BM_SETCHECK,i%2?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(kBass,BN_CLICKED),0);
        SendDlgItemMessageW(window,kSmallSpeaker,BM_SETCHECK,i%2?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(kSmallSpeaker,BN_CLICKED),0);
        SendDlgItemMessageW(window,kCleanBass,BM_SETCHECK,i%3?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(kCleanBass,BN_CLICKED),0);
        SendMessageW(window,WM_TIMER,1,0);
    }
    SetDlgItemTextW(window,kGain,L"-6"); SetDlgItemTextW(window,kFreq,L"1000");
    SetDlgItemTextW(window,kPeqGain,L"3"); SetDlgItemTextW(window,kQ,L"0.9"); SetDlgItemTextW(window,kPunch,L"4");
    SendDlgItemMessageW(window,kSmallSpeaker,BM_SETCHECK,BST_CHECKED,0);
    SendDlgItemMessageW(window,kCleanBass,BM_SETCHECK,BST_CHECKED,0); SetDlgItemTextW(window,kBassAmount,L"50");
    SendDlgItemMessageW(window,kBass,BM_SETCHECK,BST_CHECKED,0); SendMessageW(window,WM_TIMER,2,0);
    const uint64_t finalRevision=desired.revision;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while (settled.load()!=finalRevision && std::chrono::steady_clock::now()<deadline) {
        SendMessageW(window,WM_TIMER,1,0); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    finish=true; audio.join();
    valid=valid && !failed && settled.load()==finalRevision && blocks.load()>0;
    HXAudioBridgeDsp::Parameters readback; valid=valid && ReadDsp(window,readback) && readback.amount==0.5 && readback.peqGainDb==3 && readback.punchDb==4 && readback.smallSpeaker && readback.cleanBass;
    SessionControls(window,false); valid=valid && IsWindowEnabled(sourceBox) && IsWindowEnabled(GetDlgItem(window,kBuffer));
    log<<(valid?"PASS":"FAIL")<<" native Win32 edit/slider/toggle events, invalid edits retained, 1000 UI sweeps with synthetic audio; blocks="<<blocks<<" settledRevision="<<settled<<" final="<<finalRevision<<". No WASAPI session opened by this test.\n";
    liveControl.reset(); return valid?0:1;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    const std::wstring args = commandLine ? commandLine : L"";
    if (args.find(L"--bass-punch-self-test") != std::wstring::npos) return RunCleanBassPunchTests();
    if (args.find(L"--clean-bass-self-test") != std::wstring::npos) return RunCleanBassTests();
    if (args.find(L"--small-speaker-self-test") != std::wstring::npos) return RunSmallSpeakerTests();
    if (args.find(L"--realtime-dsp-self-test") != std::wstring::npos) return RunBridgeRealtimeTests();
    if (args.find(L"--virtual-bass-self-test") != std::wstring::npos) return RunVirtualBassRegressionTests();
    if (args.find(L"--dsp-self-test") != std::wstring::npos) return RunDspSelfTest();
    if (args.find(L"--bridge-self-test") != std::wstring::npos) return bridge::SelfTest();
    HANDLE mutex = CreateMutexW(nullptr,FALSE,L"Local\\HXAudioVoiceMeeterBridge");
    if (!mutex || GetLastError()==ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); MessageBoxW(nullptr,L"HXAudio Bridge 已在运行。",L"HXAudio Bridge",MB_ICONWARNING); return 1; }
    const HRESULT co = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if (FAILED(co)) { CloseHandle(mutex); return 2; }
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    g_cpu = DetectCpuTopology();
    WNDCLASSW cls{}; cls.lpfnWndProc=bridge::Proc; cls.hInstance=instance; cls.lpszClassName=L"HXAudioVoiceMeeterBridge";
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); RegisterClassW(&cls);
    HWND window=CreateWindowW(cls.lpszClassName,HX_WINDOW_TITLE,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,660,710,nullptr,nullptr,instance,nullptr);
    if (!window) { CoUninitialize(); CloseHandle(mutex); return 3; }
    if (args.find(L"--realtime-ui-self-test") != std::wstring::npos) {
        const int result=bridge::RealtimeUiTest(window); DestroyWindow(window); CoUninitialize(); CloseHandle(mutex); return result;
    }
    ShowWindow(window,show); MSG msg{}; while (GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize(); CloseHandle(mutex); return 0;
}
