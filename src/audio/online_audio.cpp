#include "audio/online_audio.h"
#include "core/path_utf8.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
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
    std::vector<std::string> arguments={"-nostdin","-hide_banner","-loglevel","error","-rw_timeout","15000000",
        "-reconnect","1","-reconnect_streamed","1","-reconnect_delay_max","3","-protocol_whitelist","http,https,httpproxy,tcp,tls,crypto"};
    if(!source.headers.empty()){arguments.push_back("-headers");arguments.push_back(source.headers);}
    arguments.insert(arguments.end(),{"-i",source.url});
    if(from>0){arguments.push_back("-ss");arguments.push_back(std::to_string(from));}
    arguments.insert(arguments.end(),{"-vn","-sn","-dn","-ac","2","-ar","48000","-f","s16le","pipe:1"});
    std::wstring command=quote(source.decoder.wstring());
    for(const auto& arg:arguments){std::wstring wide;if(!utf8_to_wide(arg,wide,failure))return false;command+=L" "+quote(wide);}
    Handle pcmRead,pcmWrite,errorRead,errorWrite,job,process,thread;
    if(!pipe(pcmRead,pcmWrite,failure)||!pipe(errorRead,errorWrite,failure))return false;
    job.value=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.value||!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){
        failure.set("online.job","Job isolation Win32="+std::to_string(GetLastError()));return false;
    }
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=pcmWrite.value;startup.hStdError=errorWrite.value;
    PROCESS_INFORMATION info{};
    if(!CreateProcessW(source.decoder.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,
        nullptr,nullptr,&startup,&info)){
        failure.set("online.start","Decoder start Win32="+std::to_string(GetLastError()));return false;
    }
    process.value=info.hProcess;thread.value=info.hThread;
    if(!AssignProcessToJobObject(job.value,process.value)||ResumeThread(thread.value)==DWORD(-1)){
        const DWORD code=GetLastError();TerminateProcess(process.value,1);
        failure.set("online.start","Contained decoder start Win32="+std::to_string(code));return false;
    }
    CloseHandle(pcmWrite.value);pcmWrite.value=nullptr;CloseHandle(errorWrite.value);errorWrite.value=nullptr;
    std::array<std::int16_t,8192> samples{};size_t pending=0;std::string errors;bool any=false;
    auto last=std::chrono::steady_clock::now();uint64_t decoded_frames=0;
    for(;;){
        if(cancelled&&cancelled->load()){TerminateJobObject(job.value,1);WaitForSingleObject(process.value,5000);failure.set("audio.cancelled","Online playback cancelled");return false;}
        if(consumer.finished()){TerminateJobObject(job.value,0);WaitForSingleObject(process.value,5000);return true;}
        DWORD available=0;
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
            if(source.duration_seconds>from&&static_cast<double>(decoded_frames)/48000+.5<source.duration_seconds-from){
                failure.set("online.truncated","Online audio ended early: decoded="+std::to_string(static_cast<double>(decoded_frames)/48000)+" expected="+std::to_string(source.duration_seconds-from));return false;
            }
            return true;
        }
        if(std::chrono::steady_clock::now()-last>std::chrono::seconds(20)){
            TerminateJobObject(job.value,1);failure.set("online.timeout","No audio received for 20 seconds "+errors);return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
bool prepare_online_audio(const OnlineAudioSource& source,double duration,std::shared_ptr<const PreparedAudio>& prepared,
    Failure& failure,const std::atomic<bool>* cancelled){
    prepared.reset();if(!std::isfinite(duration)||duration<=0){failure.set("online.duration","Online source requires a finite duration");return false;}
    auto value=std::make_shared<PreparedAudio>();auto online=std::make_shared<OnlineAudioSource>(source);online->duration_seconds=duration;value->online=online;
    value->head.format={48000,2,16};value->total_frames=static_cast<uint64_t>(duration*48000);value->head.backend="ffmpeg_online";
    struct Prefix final:AudioChunkConsumer{
        PreparedAudio& audio;explicit Prefix(PreparedAudio& a):audio(a){}
        bool finished()const noexcept override{return audio.head.samples.size()>=192000;}
        bool consume(const AudioFormat&,const int16_t* samples,size_t frames,Failure&)override{
            size_t count=std::min<size_t>(frames*2,192000-audio.head.samples.size());audio.head.samples.insert(audio.head.samples.end(),samples,samples+count);return true;
        }
    } prefix(*value);
    if(!read_online_chunks(*online,prefix,failure,cancelled))return false;
    value->head.duration_seconds=static_cast<double>(value->head.samples.size()/2)/48000;
    prepared=std::move(value);return true;
}
}
