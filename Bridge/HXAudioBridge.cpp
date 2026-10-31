// Dedicated entry point: the legacy control panel entry point is never called.
// Reuse its DSP, loopback conversion and output worker without its routing policy.
#define wWinMain HXAudioLegacyEntryPoint
#include "../ControlPanel/HXAudioControl.cpp"
#undef wWinMain

namespace bridge {
constexpr int kSource = 4101, kTarget = 4102, kRefresh = 4103, kStart = 4104,
    kStop = 4105, kGain = 4106, kBass = 4107, kFreq = 4108, kPeqGain = 4109,
    kQ = 4110, kBuffer = 4111, kStatus = 4112;
HWND sourceBox = nullptr, targetBox = nullptr, statusText = nullptr;
std::vector<DeviceInfo> sources, targets;
HANDLE worker = nullptr, stop = nullptr;
std::atomic<long> state{0}, failure{0}, inputPeak{0}, outputPeak{0};
struct Config { std::wstring source, target; DspSettings dsp; DWORD buffer = 10; } config;

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
    DspProcessor dsp; if (SUCCEEDED(hr)) dsp.Configure(bus.Format.nSamplesPerSec, bus.Format.nChannels, c.dsp);
    std::unique_ptr<TargetWorker> output;
    if (SUCCEEDED(hr)) {
        output = std::make_unique<TargetWorker>(c.target, DeviceName(target.Get()), L"ENDPOINT|" + c.target,
            &bus.Format, c.buffer, stopEvent, failedEvent, 1, false, false);
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
    std::vector<float> samples;
    while (SUCCEEDED(hr) && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE events[] = {stopEvent, failedEvent, captureEvent};
        const DWORD wait = WaitForMultipleObjects(eventDriven ? 3 : 2, events, FALSE, eventDriven ? 500 : std::max<DWORD>(1, c.buffer / 2));
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 1) { hr = AUDCLNT_E_DEVICE_INVALIDATED; break; }
        if (wait != WAIT_TIMEOUT && (!eventDriven || wait != WAIT_OBJECT_0 + 2)) { hr = E_FAIL; break; }
        DWORD ss = 0, ts = 0;
        if (FAILED(source->GetState(&ss)) || FAILED(target->GetState(&ts)) || ss != DEVICE_STATE_ACTIVE || ts != DEVICE_STATE_ACTIVE) { hr = AUDCLNT_E_DEVICE_INVALIDATED; break; }
        UINT32 packet = 0; hr = capture->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr); if (FAILED(hr)) break;
            bool converted = ConvertToFloatBus((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, frames, fmt, samples);
            if (converted) {
                inputPeak.store(FloatPeakQ31(samples)); dsp.Process(samples.data(), frames);
                for (auto& sample : samples) sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
                outputPeak.store(FloatPeakQ31(samples)); output->Push(samples.data(), frames);
            }
            const HRESULT release = capture->ReleaseBuffer(frames);
            if (FAILED(release)) hr = release; else if (!converted) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
            if (SUCCEEDED(hr)) hr = capture->GetNextPacketSize(&packet);
        }
    }
    if (client) client->Stop();
    SetEvent(stopEvent); if (output) output->Join();
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
}
bool Number(HWND w, int id, double minimum, double maximum, double& value) {
    wchar_t text[64]{}; GetDlgItemTextW(w, id, text, ARRAYSIZE(text)); wchar_t* end = nullptr;
    value = wcstod(text, &end); return end != text && *end == 0 && std::isfinite(value) && value >= minimum && value <= maximum;
}
void Start(HWND w) {
    if (worker) return;
    const LRESULT a = SendMessageW(sourceBox, CB_GETCURSEL, 0, 0), b = SendMessageW(targetBox, CB_GETCURSEL, 0, 0);
    if (a < 0 || b < 0 || static_cast<size_t>(a) >= sources.size() || static_cast<size_t>(b) >= targets.size()) { MessageBoxW(w, L"请选择 Voicemeeter 输入和物理输出。", L"HXAudio Bridge", MB_ICONWARNING); return; }
    Config next; next.source = sources[a].id; next.target = targets[b].id;
    double gain = 0, hz = 0, peq = 0, q = 0, buffer = 0;
    if (!IsAllowedTarget(next.source, next.target, targets[b].name) || !Number(w,kGain,-24,0,gain) ||
        !Number(w,kFreq,20,20000,hz) || !Number(w,kPeqGain,-12,12,peq) || !Number(w,kQ,0.1,10,q) || !Number(w,kBuffer,5,100,buffer)) {
        MessageBoxW(w,L"检查设备和参数：增益 -24 至 0 dB，频率 20–20000 Hz，PEQ ±12 dB，Q 0.1–10，缓冲 5–100 ms。",L"HXAudio Bridge",MB_ICONWARNING); return;
    }
    next.dsp.preampDb = gain; next.dsp.virtualBassEnabled = SendDlgItemMessageW(w,kBass,BM_GETCHECK,0,0) == BST_CHECKED;
    next.dsp.virtualBassAmount = next.dsp.virtualBassEnabled ? 0.5 : 0;
    next.dsp.peqBands.push_back({true,PeqFilterType::Peak,hz,peq,q}); next.buffer = static_cast<DWORD>(buffer);
    config = std::move(next); stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) { state.store(3); failure.store(E_OUTOFMEMORY); return; }
    state.store(1); inputPeak.store(0); outputPeak.store(0); failure.store(0);
    worker = CreateThread(nullptr,0,Thread,nullptr,0,nullptr);
    if (!worker) { CloseHandle(stop); stop = nullptr; state.store(3); failure.store(E_FAIL); }
}
HWND Add(HWND w, const wchar_t* klass, const wchar_t* label, DWORD style, int x,int y,int width,int height,int id = 0) {
    return CreateWindowW(klass,label,WS_CHILD|WS_VISIBLE|style,x,y,width,height,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
}
LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        Add(w,L"STATIC",L"Voicemeeter 输入：",0,15,15,160,22);
        sourceBox=Add(w,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,15,38,500,200,kSource);
        Add(w,L"STATIC",L"物理输出：",0,15,75,160,22);
        targetBox=Add(w,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,15,98,500,200,kTarget);
        Add(w,L"BUTTON",L"刷新设备",0,525,38,100,27,kRefresh);
        Add(w,L"STATIC",L"前级 dB",0,15,140,80,22); Add(w,L"EDIT",L"-6",WS_BORDER,95,137,55,25,kGain);
        Add(w,L"BUTTON",L"虚拟低频",BS_AUTOCHECKBOX,170,138,110,25,kBass);
        Add(w,L"STATIC",L"缓冲 ms",0,300,140,75,22); Add(w,L"EDIT",L"20",WS_BORDER,378,137,50,25,kBuffer);
        Add(w,L"STATIC",L"PEQ: Hz",0,15,180,75,22); Add(w,L"EDIT",L"1000",WS_BORDER,90,177,65,25,kFreq);
        Add(w,L"STATIC",L"dB",0,170,180,25,22); Add(w,L"EDIT",L"0",WS_BORDER,200,177,55,25,kPeqGain);
        Add(w,L"STATIC",L"Q",0,270,180,20,22); Add(w,L"EDIT",L"0.9",WS_BORDER,295,177,55,25,kQ);
        Add(w,L"BUTTON",L"开始处理",0,15,225,120,30,kStart); Add(w,L"BUTTON",L"停止",0,145,225,100,30,kStop);
        statusText=Add(w,L"STATIC",L"",0,15,275,610,45,kStatus);
        Add(w,L"STATIC",L"请在 Voicemeeter 中关闭直通物理输出，避免重复播放；停止桥接前请手动切回系统输出。",0,15,325,610,50);
        SetTimer(w,1,250,nullptr); Refresh(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) { case kRefresh: Refresh(); break; case kStart: Start(w); break; case kStop: Stop(); state.store(0); SetWindowTextW(statusText,L"已停止。请手动切换播放设备。"); break; } return 0;
    case WM_TIMER: {
        const long s=state.load(); if (s==2) { wchar_t text[200]{}; swprintf_s(text,L"运行中：输入峰值 %.1f%%，处理后 %.1f%%。",inputPeak.load()*100.0/2147483647.0,outputPeak.load()*100.0/2147483647.0); SetWindowTextW(statusText,text); }
        else if (s==1) SetWindowTextW(statusText,L"正在连接设备…");
        else if (s==3) { wchar_t text[240]{}; swprintf_s(text,L"桥接已停止（0x%08lX）。检查设备，刷新后手动重试；请切回系统输出。",static_cast<unsigned long>(failure.load())); SetWindowTextW(statusText,text); if (worker && WaitForSingleObject(worker,0)==WAIT_OBJECT_0) Stop(); }
        return 0;
    }
    case WM_CLOSE: Stop(); DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    const std::wstring args = commandLine ? commandLine : L"";
    if (args.find(L"--dsp-self-test") != std::wstring::npos) return RunDspSelfTest();
    if (args.find(L"--bridge-self-test") != std::wstring::npos) return bridge::SelfTest();
    HANDLE mutex = CreateMutexW(nullptr,FALSE,L"Local\\HXAudioVoiceMeeterBridge");
    if (!mutex || GetLastError()==ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); MessageBoxW(nullptr,L"HXAudio Bridge 已在运行。",L"HXAudio Bridge",MB_ICONWARNING); return 1; }
    const HRESULT co = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if (FAILED(co)) { CloseHandle(mutex); return 2; }
    g_cpu = DetectCpuTopology();
    WNDCLASSW cls{}; cls.lpfnWndProc=bridge::Proc; cls.hInstance=instance; cls.lpszClassName=L"HXAudioVoiceMeeterBridge";
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); RegisterClassW(&cls);
    HWND window=CreateWindowW(cls.lpszClassName,L"HXAudio Voicemeeter Bridge (实验版)",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,660,430,nullptr,nullptr,instance,nullptr);
    if (!window) { CoUninitialize(); CloseHandle(mutex); return 3; }
    ShowWindow(window,show); MSG msg{}; while (GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize(); CloseHandle(mutex); return 0;
}
