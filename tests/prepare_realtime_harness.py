"""Build a portable harness from exact production source, without Win32 glue."""
import pathlib
import sys
root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'ControlPanel/HXAudioControl.cpp').read_text(encoding='utf-8')
text = '''#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <limits>
#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <new>
using UINT32=uint32_t;
static thread_local bool countAllocations=false;
static thread_local size_t allocationCount=0;
void* operator new(size_t n) { if(countAllocations) ++allocationCount; if(void* p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }
void operator delete[](void* p,size_t) noexcept { std::free(p); }
'''
text += f'#include "{(root / "Shared/HXAudioDspHeadroom.h").as_posix()}"\n'
text += source[source.index('    enum class PeqFilterType'):source.index('    std::vector<PeqBand> g_peqEditorBands')]
text += source[source.index('    struct BiquadCoefficients'):source.index('    struct StaticGainOriginal')]
for file in ['Shared/HXAudioBridgePeakGuard.h', 'Shared/HXAudioBridgeRealtimeDsp.h', 'tests/bridge_realtime_tests.h', 'tests/small_speaker_tests.h', 'tests/clean_bass_tests.h', 'tests/clean_bass_punch_tests.h']:
    text += f'#include "{(root / file).as_posix()}"\n'
text += '''
int main() {
    if(RunSmallSpeakerTests()) return 30;
    if(RunCleanBassTests()) return 31;
    if(RunCleanBassPunchTests()) return 32;
    const int result=RunBridgeRealtimeTests();
    if(result) return result;
    HXAudioBridgeDsp::Control control;
    HXAudioBridgeDsp::Parameters p;
    HXAudioBridgeDsp::Processor dsp; dsp.Configure(48000,2,p,&control);
    std::array<float,256> block{};
    countAllocations=true;
    for(int i=0;i<10000;++i) {
        p.frequencyHz=20+(i*71)%19981; p.peqGainDb=i%25-12; p.q=0.1+(i%100)/10.0;
        p.amount=(i%101)/100.0; p.enabled=i%2!=0; p.smallSpeaker=i%3!=0; p.cleanBass=i%5==0; p.punchDb=double(i%7); p.gainDb=-double(i%25); p.revision=i+1;
        control.requests.Push(p); dsp.Process(block.data(),128);
        HXAudioBridgeDsp::Status status; control.status.Latest(status);
    }
    countAllocations=false;
    std::ofstream log("bridge-realtime-memory.log");
    log << (allocationCount==0?"PASS":"FAIL") << " production Process() allocations="<<allocationCount<<" across 10000 live updates.\\n";
    return allocationCount==0?0:20;
}
'''
pathlib.Path(sys.argv[1]).write_text(text, encoding='utf-8')
