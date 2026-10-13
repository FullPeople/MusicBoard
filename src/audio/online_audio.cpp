#include "audio/online_audio.h"
#include "core/path_utf8.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace musicboard {
namespace {
    struct Handle { HANDLE value=nullptr; ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);} };
std::wstring quote(const std::wstring& value){
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}out.append(slashes*(c==L'"'?2:1),L'\\');slashes=0;if(c==L'"')out+=L'\\';out+=c;}
    out.append(slashes*2,L'\\');return out+L'"';
}
bool pipe(Handle& read,Handle& write,Failure& failure){
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    if(!CreatePipe(&read.value,&write.value,&sa,65536)||!SetHandleInformation(read.value,HANDLE_FLAG_INHERIT,0)){
        failure.set("online.pipe","Pipe Win32="+std::to_string(GetLastError()));return false;
    }return true;
}
bool launch(const std::filesystem::path& executable,std::wstring command,HANDLE input,HANDLE output,HANDLE errors,
    HANDLE job,Handle& process,Handle& thread,Failure& failure){
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(attributes,1,0,&bytes)){
        failure.set("online.start","Process attribute initialization Win32="+std::to_string(GetLastError()));return false;
    }
    struct Cleanup{LPPROC_THREAD_ATTRIBUTE_LIST value;~Cleanup(){DeleteProcThreadAttributeList(value);}} cleanup{attributes};
    std::array<HANDLE,3> handles{input,output,errors};
    for(HANDLE handle:handles)if(!SetHandleInformation(handle,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT)){
        failure.set("online.start","Process pipe inheritance Win32="+std::to_string(GetLastError()));return false;
    }
    if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(handles),nullptr,nullptr)){
        failure.set("online.start","Process pipe isolation Win32="+std::to_string(GetLastError()));return false;
    }
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=input;startup.StartupInfo.hStdOutput=output;startup.StartupInfo.hStdError=errors;
    startup.lpAttributeList=attributes;PROCESS_INFORMATION info{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup.StartupInfo,&info)){
        failure.set("online.start","Media process start Win32="+std::to_string(GetLastError()));return false;
    }
    process.value=info.hProcess;thread.value=info.hThread;
    if(!AssignProcessToJobObject(job,process.value)||ResumeThread(thread.value)==DWORD(-1)){
        const DWORD code=GetLastError();TerminateProcess(process.value,1);WaitForSingleObject(process.value,5000);
        failure.set("online.start","Contained media process start Win32="+std::to_string(code));return false;
    }
    return true;
}
}
bool read_online_chunks(const OnlineAudioSource& source,AudioChunkConsumer& consumer,Failure& failure,
    const std::atomic<bool>* cancelled,double from){
    if(cancelled&&cancelled->load()){failure.set("audio.cancelled","Online playback cancelled");return false;}
    if(!std::isfinite(from)||from<0||!(source.url.starts_with("https://")||source.url.starts_with("http://"))||
       source.url.find_first_of("\r\n\0",0,3)!=std::string::npos){
        failure.set("online.url","Invalid HTTP audio source or position");return false;
    }
    std::error_code ec;if(!std::filesystem::is_regular_file(source.decoder,ec)){
        failure.set("online.decoder","Decoder missing: "+path_to_utf8(source.decoder));return false;
    }
    const bool relayed=!source.resolver.empty();
    if(relayed&&(!std::filesystem::is_regular_file(source.resolver,ec)||source.resolver_arguments.empty())){
        failure.set("online.resolver","Platform transfer tool is unavailable");return false;
    }
    std::vector<std::string> arguments={"-nostdin","-hide_banner","-loglevel","error"};
    if(relayed)arguments.insert(arguments.end(),{"-protocol_whitelist","pipe","-i","pipe:0"});
    else{
        arguments.insert(arguments.end(),{"-rw_timeout","15000000","-reconnect","1","-reconnect_streamed","1",
            "-reconnect_delay_max","3","-protocol_whitelist","http,https,httpproxy,tcp,tls,crypto"});
        if(!source.headers.empty()){arguments.push_back("-headers");arguments.push_back(source.headers);}
        arguments.insert(arguments.end(),{"-i",source.url});
    }
    if(from>0){arguments.push_back("-ss");arguments.push_back(std::to_string(from));}
    arguments.insert(arguments.end(),{"-vn","-sn","-dn","-ac","2","-ar","48000","-f","s16le","pipe:1"});
    std::wstring command=quote(source.decoder.wstring());
    for(const auto& arg:arguments){std::wstring wide;if(!utf8_to_wide(arg,wide,failure))return false;command+=L" "+quote(wide);}
    Handle pcmRead,pcmWrite,errorRead,errorWrite,job,process,thread;
    Handle mediaRead,mediaWrite,resolverErrorRead,resolverErrorWrite,resolverProcess,resolverThread,nullInput;
    if(!pipe(pcmRead,pcmWrite,failure)||!pipe(errorRead,errorWrite,failure))return false;
    job.value=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.value||!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){
        failure.set("online.job","Job isolation Win32="+std::to_string(GetLastError()));return false;
    }
    SECURITY_ATTRIBUTES inputSecurity{sizeof(inputSecurity),nullptr,TRUE};
    nullInput.value=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&inputSecurity,OPEN_EXISTING,0,nullptr);
    if(nullInput.value==INVALID_HANDLE_VALUE){failure.set("online.start","Null input Win32="+std::to_string(GetLastError()));return false;}
    if(relayed&&(!pipe(mediaRead,mediaWrite,failure)||!pipe(resolverErrorRead,resolverErrorWrite,failure)))return false;
    if(!launch(source.decoder,command,relayed?mediaRead.value:nullInput.value,pcmWrite.value,errorWrite.value,
        job.value,process,thread,failure))return false;
    if(relayed){
        std::wstring resolverCommand=quote(source.resolver.wstring());
        for(const auto& argument:source.resolver_arguments){std::wstring wide;if(!utf8_to_wide(argument,wide,failure))return false;resolverCommand+=L" "+quote(wide);}
        if(!launch(source.resolver,resolverCommand,nullInput.value,mediaWrite.value,resolverErrorWrite.value,
            job.value,resolverProcess,resolverThread,failure))return false;
        CloseHandle(mediaRead.value);mediaRead.value=nullptr;CloseHandle(mediaWrite.value);mediaWrite.value=nullptr;
        CloseHandle(resolverErrorWrite.value);resolverErrorWrite.value=nullptr;
    }
    CloseHandle(pcmWrite.value);pcmWrite.value=nullptr;CloseHandle(errorWrite.value);errorWrite.value=nullptr;
    const auto stopOwned=[&](UINT code){
        TerminateJobObject(job.value,code);WaitForSingleObject(process.value,5000);
        if(relayed)WaitForSingleObject(resolverProcess.value,5000);
    };
    std::array<std::int16_t,8192> samples{};size_t pending=0;std::string errors;bool any=false;
    auto last=std::chrono::steady_clock::now();uint64_t decoded_frames=0;
    for(;;){
        if(cancelled&&cancelled->load()){stopOwned(1);failure.set("audio.cancelled","Online playback cancelled");return false;}
        if(consumer.finished()){stopOwned(0);return true;}
        DWORD available=0;
        if(relayed&&PeekNamedPipe(resolverErrorRead.value,nullptr,0,nullptr,&available,nullptr)&&available){
            std::array<char,4096> bytes{};DWORD count=0;
            if(ReadFile(resolverErrorRead.value,bytes.data(),std::min<DWORD>(available,static_cast<DWORD>(bytes.size())),&count,nullptr))
                errors.append(bytes.data(),std::min<size_t>(count,16384-errors.size()));
        }
        available=0;
        if(PeekNamedPipe(errorRead.value,nullptr,0,nullptr,&available,nullptr)&&available){
            std::array<char,4096> bytes{};DWORD count=0;
            if(ReadFile(errorRead.value,bytes.data(),std::min<DWORD>(available,static_cast<DWORD>(bytes.size())),&count,nullptr))
                errors.append(bytes.data(),std::min<size_t>(count,16384-errors.size()));
        }
        available=0;
        if(PeekNamedPipe(pcmRead.value,nullptr,0,nullptr,&available,nullptr)&&available){
            DWORD count=0;const DWORD room=static_cast<DWORD>(sizeof(samples)-pending);
            if(!ReadFile(pcmRead.value,reinterpret_cast<char*>(samples.data())+pending,std::min(room,available),&count,nullptr)){
                failure.set("online.read","PCM read Win32="+std::to_string(GetLastError()));return false;
            }
            last=std::chrono::steady_clock::now();pending+=count;size_t complete=pending/4*4;
            if(complete&&!consumer.consume(AudioFormat{48000,2,16},samples.data(),complete/4,failure))return false;
            decoded_frames+=complete/4;
            any|=complete>0;pending-=complete;
            if(pending)std::memmove(samples.data(),reinterpret_cast<char*>(samples.data())+complete,pending);
            continue;
        }
        if(WaitForSingleObject(process.value,0)==WAIT_OBJECT_0){
            DWORD code=0;if(!GetExitCodeProcess(process.value,&code)||code||pending||!any){
                failure.set("online.decode","Online decoder exit="+std::to_string(code)+" "+errors);return false;
            }
            if(relayed){
                WaitForSingleObject(resolverProcess.value,5000);
                DWORD resolverCode=STILL_ACTIVE;
                if(!GetExitCodeProcess(resolverProcess.value,&resolverCode)||resolverCode){
                    failure.set("online.transfer","Platform transfer exit="+std::to_string(resolverCode)+" "+errors);return false;
                }
            }
            if(source.duration_seconds>from&&static_cast<double>(decoded_frames)/48000+.5<source.duration_seconds-from){
                failure.set("online.truncated","Online audio ended early: decoded="+std::to_string(static_cast<double>(decoded_frames)/48000)+" expected="+std::to_string(source.duration_seconds-from));return false;
            }
            return true;
        }
        if(std::chrono::steady_clock::now()-last>std::chrono::seconds(20)){
            stopOwned(1);failure.set("online.timeout","No audio received for 20 seconds "+errors);return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
// A newly prepared entry retains one decoder until its first playback takes
// ownership. At most eight native read chunks wait behind the two-second head.
// Unused entries stop their decoder after ten seconds rather than retaining an
// idle process for every online entry in the library.
class OnlineAudioSession final : private AudioChunkConsumer {
public:
    explicit OnlineAudioSession(OnlineAudioSource source) : source_(std::move(source)) {
        worker_=std::thread([this] {
            Failure failure;
            const bool ok=read_online_chunks(source_,*this,failure,&cancelled_);
            const std::lock_guard lock(mutex_);
            if(!ok&&!cancelled_)failure_=std::move(failure);
            ended_=true;wake_.notify_all();
        });
    }
    ~OnlineAudioSession() override { cancel(); }
    bool take_head(std::vector<int16_t>& head,Failure& failure,const std::atomic<bool>* cancelled) {
        std::unique_lock lock(mutex_);
        if(cancelled&&cancelled->load()) {
            lock.unlock();cancel();failure.set("audio.cancelled","Online preparation cancelled");return false;
        }
        while(!head_ready_&&!ended_) {
            if(cancelled&&cancelled->load()) {
                lock.unlock();cancel();failure.set("audio.cancelled","Online preparation cancelled");return false;
            }
            wake_.wait_for(lock,std::chrono::milliseconds(20));
        }
        if(cancelled&&cancelled->load()) {
            lock.unlock();cancel();failure.set("audio.cancelled","Online preparation cancelled");return false;
        }
        if(!failure_.empty()) { failure=failure_;return false; }
        if(head_.empty()) { failure.set("online.decode","Online source did not produce audio");return false; }
        head_frames_=head_.size()/2;
        head=std::move(head_);head_ready_=true;
        return true;
    }
    bool claim(double from) {
        const std::lock_guard lock(mutex_);
        if(claimed_||cancelled_||!head_ready_||!std::isfinite(from)||from<0||
            std::abs(from-static_cast<double>(head_frames_)/48000)>1.0/48000)return false;
        claimed_=true;wake_.notify_all();return true;
    }
    bool read_tail(AudioChunkConsumer& consumer,Failure& failure,const std::atomic<bool>* cancelled) {
        for(;;) {
            std::vector<int16_t> block;
            {
                std::unique_lock lock(mutex_);
                while(queue_.empty()&&!ended_) {
                    if(cancelled&&cancelled->load()) {
                        lock.unlock();cancel();failure.set("audio.cancelled","Online playback cancelled");return false;
                    }
                    wake_.wait_for(lock,std::chrono::milliseconds(20));
                }
                if(queue_.empty()) {
                    if(!failure_.empty()){failure=failure_;return false;}
                    return true;
                }
                block=std::move(queue_.front());queue_.pop_front();wake_.notify_all();
            }
            if((cancelled&&cancelled->load())||consumer.finished()) {
                cancel();
                if(cancelled&&cancelled->load()){failure.set("audio.cancelled","Online playback cancelled");return false;}
                return true;
            }
            if(!consumer.consume(AudioFormat{48000,2,16},block.data(),block.size()/2,failure)) {cancel();return false;}
        }
    }
private:
    bool finished() const noexcept override {
        const std::lock_guard lock(mutex_);
        if(head_ready_&&!claimed_&&ready_at_!=std::chrono::steady_clock::time_point{}&&
            std::chrono::steady_clock::now()-ready_at_>std::chrono::seconds(10))cancelled_=true;
        return cancelled_;
    }
    void cancel() {
        cancelled_=true;wake_.notify_all();
        if(worker_.joinable())worker_.join();
    }
    bool consume(const AudioFormat&,const int16_t* samples,size_t frames,Failure& failure) override {
        std::unique_lock lock(mutex_);
        size_t count=frames*2;
        if(!head_ready_) {
            const size_t prefix=std::min(count,192000-head_.size());
            head_.insert(head_.end(),samples,samples+prefix);samples+=prefix;count-=prefix;
            if(head_.size()==192000){head_ready_=true;ready_at_=std::chrono::steady_clock::now();wake_.notify_all();}
        }
        while(count) {
            while(queue_.size()==8&&!cancelled_) {
                if(!claimed_&&std::chrono::steady_clock::now()-ready_at_>std::chrono::seconds(10))cancelled_=true;
                wake_.wait_for(lock,std::chrono::milliseconds(20));
            }
            if(cancelled_){failure.set("audio.cancelled","Online decoder cancelled");return false;}
            const size_t part=std::min<size_t>(count,8192);
            queue_.emplace_back(samples,samples+part);samples+=part;count-=part;wake_.notify_all();
        }
        return true;
    }
    OnlineAudioSource source_;
    std::thread worker_;
    mutable std::atomic<bool> cancelled_{false};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<int16_t> head_;
    std::deque<std::vector<int16_t>> queue_;
    size_t head_frames_=0;
    bool head_ready_=false,ended_=false,claimed_=false;
    std::chrono::steady_clock::time_point ready_at_{};
    Failure failure_;
};

bool read_prepared_online_chunks(const PreparedAudio& prepared,AudioChunkConsumer& consumer,
    Failure& failure,const std::atomic<bool>* cancelled,double from) {
    if(cancelled&&cancelled->load()){failure.set("audio.cancelled","Online playback cancelled");return false;}
    if(prepared.online_session&&prepared.online_session->claim(from))
        return prepared.online_session->read_tail(consumer,failure,cancelled);
    return read_online_chunks(*prepared.online,consumer,failure,cancelled,from);
}

bool prepare_online_audio(const OnlineAudioSource& source,double duration,std::shared_ptr<const PreparedAudio>& prepared,
    Failure& failure,const std::atomic<bool>* cancelled){
    prepared.reset();if(!std::isfinite(duration)||duration<=0){failure.set("online.duration","Online source requires a finite duration");return false;}
    auto value=std::make_shared<PreparedAudio>();auto online=std::make_shared<OnlineAudioSource>(source);online->duration_seconds=duration;value->online=online;
    value->head.format={48000,2,16};value->total_frames=static_cast<uint64_t>(duration*48000);value->head.backend="ffmpeg_online";
    auto session=std::make_shared<OnlineAudioSession>(*online);
    if(!session->take_head(value->head.samples,failure,cancelled))return false;
    value->online_session=std::move(session);
    value->head.duration_seconds=static_cast<double>(value->head.samples.size()/2)/48000;
    prepared=std::move(value);return true;
}
}
