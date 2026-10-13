#include <windows.h>
#include <algorithm>
#include <chrono>
#include "audio/online_audio.h"
#include "library/online_job.h"
#include "core/path_utf8.h"
#include "tests/test_harness.h"
using namespace musicboard;
using namespace musicboard::test;

namespace {
struct Collect final : AudioChunkConsumer {
    std::vector<int16_t> samples;
    size_t limit=48000*3*2;
    bool finished()const noexcept override{return samples.size()>=limit;}
    bool consume(const AudioFormat&,const int16_t* data,size_t frames,Failure&)override{
        const size_t count=std::min(frames*2,limit-samples.size());
        samples.insert(samples.end(),data,data+count);return true;
    }
};
}
int wmain(int argc,wchar_t** argv){
    Registry r;
    if(argc!=4){r.skip("Requires owned HTTP delay source and authored reference file");return r.finish("online_handoff_tests");}
    OnlineJob job;Failure failure;
    const bool started=job.start(wide_to_utf8(argv[2]),argv[1],failure);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(40);
    while(started&&!job.ready()&&std::chrono::steady_clock::now()<deadline)Sleep(5);
    const bool ready=started&&job.ready()&&job.failure().empty()&&job.audio();
    r.check_eq(ready,"Native online preparation succeeds on delayed source",started?job.failure().to_string():failure.to_string());
    if(!ready)return r.finish("online_handoff_tests");
    auto audio=job.audio();
    r.check_eq(audio->head.samples.size()==192000,"Prepared head retains exactly two seconds","");
    Collect tail;
    const auto began=std::chrono::steady_clock::now();
    const bool continued=read_prepared_online_chunks(*audio,tail,failure,nullptr,audio->head.duration_seconds);
    const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count();
    std::printf("CONTINUATION_MILLISECONDS %lld\n",static_cast<long long>(milliseconds));
    r.check_eq(continued&&tail.samples.size()==tail.limit&&milliseconds<2000,
        "Continuation arrives without another delayed connection",failure.to_string());
    Collect reference;reference.limit=48000*5*2;failure.clear();
    const bool read=read_audio_chunks(argv[3],reference,failure);
    const size_t head=audio->head.samples.size();
    r.check_eq(read&&reference.samples.size()==head+tail.samples.size()&&
        std::equal(audio->head.samples.begin(),audio->head.samples.end(),reference.samples.begin())&&
        std::equal(tail.samples.begin(),tail.samples.end(),reference.samples.begin()+head),
        "Prepared head and continuation match independent local PCM",failure.to_string());
    return r.finish("online_handoff_tests");
}
