#include "library/online_job.h"
#include "library/media_request_headers.h"
#include "library/url_classify.h"
#include "core/path_utf8.h"
#include <charconv>
#include <cmath>
#include <sstream>

namespace musicboard {
bool OnlineJob::start(const std::string& url,const std::filesystem::path& tools,Failure& failure){
    if(worker_.joinable()||!url_is_well_formed(url)){
        failure.set("online.request","Invalid URL or online task already started");return false;
    }
    for(const auto& name:{L"ffmpeg.exe",L"ffprobe.exe",L"yt-dlp.exe"}){
        std::error_code ec;if(!std::filesystem::is_regular_file(tools/name,ec)){
            failure.set("online.tools","Missing online tool: "+path_to_utf8(tools/name));return false;
        }
    }
    const std::string normalized=normalize_import_url(url);
    worker_=std::thread([this,normalized,tools]{run(normalized,tools);ready_=true;});return true;
}
void OnlineJob::run(std::string url,std::filesystem::path tools){
    auto execute=[&](const std::filesystem::path& executable,std::vector<std::string> arguments,std::string& output){
        ProcessRunner runner;ProcessRunOptions options;options.executable=executable;options.arguments=std::move(arguments);
        options.maximum_output_bytes=128*1024;options.stall_timeout_seconds=30;
        if(!runner.start(options,failure_))return false;
        if(!runner.containment_note().empty()){runner.cancel();failure_.set("online.job",runner.containment_note());return false;}
        while(runner.state()==ProcessState::running){
            if(cancelled_){runner.cancel();failure_.set("audio.cancelled","Online source preparation cancelled");return false;}
            runner.pump();std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if(runner.state()!=ProcessState::exited||runner.exit_code()!=0||runner.output_truncated()){
            failure_.set("online.resolve","Source preparation exit="+std::to_string(runner.exit_code())+" "+runner.failure_reason()+" "+runner.captured_output());return false;
        }
        output=runner.captured_output();return true;
    };
    OnlineAudioSource source{tools/L"ffmpeg.exe",url,{}};double duration=0;title_=stem_from_url(url);
    if(classify_url(url).kind==ImportSourceKind::direct_http){
        std::string metadata;
        if(!execute(tools/L"ffprobe.exe",{"-v","error","-rw_timeout","15000000","-show_entries","format=duration","-of","default=noprint_wrappers=1:nokey=1",url},metadata))return;
        std::istringstream rows(metadata);std::string line;while(std::getline(rows,line)){
            const auto parsed=std::from_chars(line.data(),line.data()+line.size(),duration);if(parsed.ec==std::errc()&&duration>0)break;
        }
    }else{
        std::string metadata;
        std::vector<std::string> arguments={"--ignore-config","--no-plugin-dirs","--no-playlist","--no-warnings","--skip-download","--socket-timeout","15","--retries","2","-f","bestaudio/best",
            "--user-agent",std::string(kBrowserUserAgent),
            "--print","URL=%(url)s","--print","DURATION=%(duration)s","--print","TITLE=%(title)s",
            "--print","AGENT=%(http_headers.User-Agent)s","--print","REFERER=%(http_headers.Referer)s"};
        std::error_code ec;if(std::filesystem::is_directory(tools/L"plugins",ec)){
            arguments.push_back("--plugin-dirs");arguments.push_back(path_to_utf8(tools/L"plugins"));
        }
        arguments.push_back(url);
        if(!execute(tools/L"yt-dlp.exe",std::move(arguments),metadata))return;
        std::istringstream rows(metadata);std::string line;
        while(std::getline(rows,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            if(line.starts_with("URL="))source.url=line.substr(4);
            else if(line.starts_with("DURATION=")){const auto parsed=std::from_chars(line.data()+9,line.data()+line.size(),duration);if(parsed.ec!=std::errc())duration=0;}
            else if(line.starts_with("TITLE="))title_=line.substr(6);
            else if(line.starts_with("AGENT=")&&line.substr(6)!="NA")source.headers+="User-Agent: "+line.substr(6)+"\r\n";
            else if(line.starts_with("REFERER=")&&line.substr(8)!="NA")source.headers+="Referer: "+line.substr(8)+"\r\n";
        }
        if(source.headers.find("User-Agent: ")==std::string::npos)
            source.headers="User-Agent: "+std::string(kBrowserUserAgent)+"\r\n"+source.headers;
        source.resolver=tools/L"yt-dlp.exe";
        source.resolver_arguments={"--ignore-config","--no-plugin-dirs","--no-playlist","--quiet","--no-warnings","--no-progress",
            "--socket-timeout","15","--retries","2","--fragment-retries","2","-f","bestaudio/best","-o","-",
            "--ffmpeg-location",path_to_utf8(tools),"--user-agent",std::string(kBrowserUserAgent)};
        if(std::filesystem::is_directory(tools/L"plugins",ec)){
            source.resolver_arguments.push_back("--plugin-dirs");source.resolver_arguments.push_back(path_to_utf8(tools/L"plugins"));
        }
        source.resolver_arguments.push_back(url);
    }
    if(url_is_well_formed(source.url)&&(!std::isfinite(duration)||duration<=0)&&classify_url(url).kind!=ImportSourceKind::direct_http){
        std::string metadata;std::vector<std::string> arguments={"-v","error","-rw_timeout","15000000"};
        if(!source.headers.empty()){arguments.push_back("-headers");arguments.push_back(source.headers);}
        arguments.insert(arguments.end(),{"-show_entries","format=duration","-of","default=noprint_wrappers=1:nokey=1",source.url});
        if(!execute(tools/L"ffprobe.exe",std::move(arguments),metadata))return;
        std::istringstream rows(metadata);std::string line;while(std::getline(rows,line)){
            const auto parsed=std::from_chars(line.data(),line.data()+line.size(),duration);if(parsed.ec==std::errc()&&duration>0)break;
        }
    }
    if(!url_is_well_formed(source.url)||!std::isfinite(duration)||duration<=0){
        failure_.set("online.metadata","No playable HTTP audio URL with finite duration was returned");return;
    }
    if(title_.empty())title_="Online audio";
    if(!prepare_online_audio(source,duration,audio_,failure_,&cancelled_))return;
}
}
