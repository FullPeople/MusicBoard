#pragma once
#include <thread>
#include "audio/online_audio.h"
#include "library/process_runner.h"
namespace musicboard {
class OnlineJob final {
public:
    ~OnlineJob(){cancel();if(worker_.joinable())worker_.join();}
    [[nodiscard]] bool start(const std::string& url,const std::filesystem::path& tools,Failure& failure);
    void cancel(){cancelled_=true;}
    [[nodiscard]] bool ready()const noexcept{return ready_.load();}
    [[nodiscard]] const Failure& failure()const noexcept{return failure_;}
    [[nodiscard]] const std::shared_ptr<const PreparedAudio>& audio()const noexcept{return audio_;}
    [[nodiscard]] const std::string& title()const noexcept{return title_;}
private:
    void run(std::string url,std::filesystem::path tools);
    std::thread worker_;
    std::atomic<bool> cancelled_{false},ready_{false};
    Failure failure_;
    std::shared_ptr<const PreparedAudio> audio_;
    std::string title_;
};
}
