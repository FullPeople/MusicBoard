#include <windows.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include "audio/online_audio.h"
#include "audio/audio_engine.h"
#include "library/online_job.h"
#include "core/path_utf8.h"
#include "tests/test_harness.h"
using namespace musicboard;
using namespace musicboard::test;
namespace {
struct Collect final:AudioChunkConsumer{
    std::vector<int16_t> samples;size_t limit=48000*5*2;
    bool finished()const noexcept override{return samples.size()>=limit;}
    bool consume(const AudioFormat& format,const int16_t* data,size_t frames,Failure& failure)override{
        if(format.sample_rate!=48000||format.channels!=2){failure.set("test.format","Unexpected PCM format");return false;}
        const size_t count=std::min(frames*2,limit-samples.size());samples.insert(samples.end(),data,data+count);return true;
    }
};
}
int wmain(int argc,wchar_t** argv){
    Registry r;if(argc<3){r.skip("Specify bundled media-tools directory and owned HTTP fixture URL");return r.finish("online_audio_tests");}
    const std::filesystem::path tools=argv[1];const std::string url=wide_to_utf8(argv[2]);
    OnlineAudioSource source{tools/L"ffmpeg.exe",url,{}};Failure failure;Collect reference;
    const bool read=read_online_chunks(source,reference,failure);
    r.check_eq(read&&reference.samples.size()==reference.limit,"HTTP PCM reaches bounded five-second consumer",failure.to_string());
    failure.clear();Collect suffix;suffix.limit=48000*2*2;
    const bool seek=read_online_chunks(source,suffix,failure,nullptr,2.0);
    const size_t offset=48000*2*2;
    r.check_eq(seek&&reference.samples.size()>=offset+suffix.samples.size()&&std::equal(suffix.samples.begin(),suffix.samples.end(),reference.samples.begin()+offset),"Online preload boundary matches sequential audio exactly",failure.to_string());
    failure.clear();std::shared_ptr<const PreparedAudio> prepared;
    const bool ready=prepare_online_audio(source,30.0,prepared,failure);
    r.check_eq(ready&&prepared->online&&prepared->head.samples.size()==192000&&!prepared->complete,"Online preparation retains only two seconds",failure.to_string());
    if(ready){
        failure.clear();Collect continued;continued.limit=48000*3*2;
        const bool resumed=read_prepared_online_chunks(*prepared,continued,failure,nullptr,2.0);
        const size_t head=prepared->head.samples.size();
        r.check_eq(resumed&&continued.samples.size()==continued.limit&&reference.samples.size()==head+continued.samples.size()&&
            std::equal(prepared->head.samples.begin(),prepared->head.samples.end(),reference.samples.begin())&&
            std::equal(continued.samples.begin(),continued.samples.end(),reference.samples.begin()+head),
            "Prepared decoder continuation preserves exact PCM without duplicated or missing frames",failure.to_string());
    }
    failure.clear();std::atomic<bool> cancelled{true};Collect unused;
    const auto began=std::chrono::steady_clock::now();
    r.check_eq(!read_online_chunks(source,unused,failure,&cancelled)&&failure.code=="audio.cancelled"&&std::chrono::steady_clock::now()-began<std::chrono::seconds(2),"Cancellation ends owned decoder promptly",failure.to_string());
    failure.clear();Collect missing;OnlineAudioSource absent=source;absent.url+=".missing";
    r.check_eq(!read_online_chunks(absent,missing,failure)&&!failure.empty()&&missing.samples.empty(),"HTTP failure cannot become successful empty audio",failure.to_string());
    failure.clear();OnlineJob cancelledJob;
    const bool cancelledStarted=cancelledJob.start(url,tools,failure);cancelledJob.cancel();
    const auto cancelledDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(cancelledStarted&&!cancelledJob.ready()&&std::chrono::steady_clock::now()<cancelledDeadline)Sleep(5);
    r.check_eq(cancelledStarted&&cancelledJob.ready()&&cancelledJob.failure().code=="audio.cancelled"&&!cancelledJob.audio(),"Cancelled online preparation never publishes playable data",cancelledJob.failure().to_string());
    failure.clear();OnlineJob job;
    const bool started=job.start(url,tools,failure);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(40);
    while(started&&!job.ready()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    r.check_eq(started&&job.ready()&&job.failure().empty()&&job.audio(),"Direct URL resolves metadata and prepares online audio",started?job.failure().to_string():failure.to_string());
    if(argc==5){
        failure.clear();Collect tail;tail.limit=48000*60*2;OnlineAudioSource shortSource=source;shortSource.url=wide_to_utf8(argv[3]);shortSource.duration_seconds=30;
        r.check_eq(!read_online_chunks(shortSource,tail,failure)&&failure.code=="online.truncated","Premature audio EOF contradicting source duration is reported",failure.to_string());
        failure.clear();std::shared_ptr<const PreparedAudio> rejected;
        r.check_eq(!prepare_online_audio(shortSource,30,rejected,failure)&&!rejected&&failure.code=="online.truncated","Truncated preparation never publishes a false full-length track",failure.to_string());
        failure.clear();Collect waiting;OnlineAudioSource stalled=source;stalled.url=wide_to_utf8(argv[4]);std::atomic<bool> stop{false};bool streamed=false;
        std::thread blocked([&]{streamed=read_online_chunks(stalled,waiting,failure,&stop);});Sleep(250);
        const auto cancelledAt=std::chrono::steady_clock::now();stop=true;blocked.join();
        r.check_eq(!streamed&&failure.code=="audio.cancelled"&&std::chrono::steady_clock::now()-cancelledAt<std::chrono::seconds(2),"Cancel active stalled network decoder without waiting for timeout",failure.to_string());
    }
    if(ready){
        AudioEngine engine;failure.clear();
        if(!engine.initialize(failure))r.skip("Audio output unavailable: "+failure.to_string());
        else{
            failure.clear();const bool playing=engine.play_prepared(AudioBus::bgm,0,prepared,"authored online fixture",0,false,failure);
            const auto playbackDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
            while(playing&&std::chrono::steady_clock::now()<playbackDeadline){
                engine.update(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());Sleep(5);
            }
            const auto state=engine.slot_state(AudioBus::bgm,0);
            r.check_eq(playing&&state.in_use&&state.position_seconds>3.5,"XAudio2 online playback progresses beyond two-second head",failure.to_string()+" position="+std::to_string(state.position_seconds));
            failure.clear();const auto stoppedAt=std::chrono::steady_clock::now();
            r.check_eq(engine.stop(AudioBus::bgm,0,0,failure)&&std::chrono::steady_clock::now()-stoppedAt<std::chrono::seconds(2),"Stopping online voice joins decoder promptly",failure.to_string());
        }
    }
    return r.finish("online_audio_tests");
}
