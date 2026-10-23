#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace fulltools {
enum class SetupStep : LONG { idle, envelope, extract, verify, copy, registration, cleanup, complete };
struct SetupSnapshot { SetupStep step = SetupStep::idle; std::uint64_t completed = 0, total = 1; };
inline int progress_units(const SetupSnapshot& value) {
    const int starts[]{0, 0, 1500, 5000, 7000, 9600, 9800, 10000};
    const int ends[]{0, 1500, 5000, 7000, 9600, 9800, 9999, 10000};
    const auto index = static_cast<unsigned>(value.step);
    if (index >= 8) return 0;
    const auto done = value.completed > value.total ? value.total : value.completed;
    return starts[index] + (value.total ? static_cast<int>(done * static_cast<unsigned>(ends[index] - starts[index]) / value.total) : 0);
}
class SetupProgressChannel final {
    struct alignas(8) Shared { volatile LONG magic, sequence, step, reserved; volatile LONG64 completed, total; };
    HANDLE handle_ = nullptr;
    Shared* shared_ = nullptr;
    std::wstring name_;
    void close() { if (shared_) UnmapViewOfFile(shared_); if (handle_) CloseHandle(handle_); shared_ = nullptr; handle_ = nullptr; }
    bool map() {
        shared_ = static_cast<Shared*>(MapViewOfFile(handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
        if (!shared_) { close(); return false; } return true;
    }
public:
    ~SetupProgressChannel() { close(); }
    SetupProgressChannel() = default;
    SetupProgressChannel(const SetupProgressChannel&) = delete;
    SetupProgressChannel& operator=(const SetupProgressChannel&) = delete;
    bool create(const std::wstring& name) {
        close(); name_ = name;
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
        if (!handle_ || GetLastError() == ERROR_ALREADY_EXISTS) { close(); return false; }
        if (!map()) return false;
        InterlockedExchange(&shared_->magic, 0x46545032); write(SetupStep::idle, 0, 1); return true;
    }
    bool open(const std::wstring& name) {
        if (!name.starts_with(L"Local\\MusicBoard.Setup.Progress.") || name.size() > 120) return false;
        close(); name_ = name; handle_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        if (!handle_ || !map()) return false;
        if (InterlockedCompareExchange(&shared_->magic, 0, 0) != 0x46545032) { close(); return false; } return true;
    }
    const std::wstring& name() const { return name_; }
    void write(SetupStep step, std::uint64_t completed, std::uint64_t total) {
        if (!shared_) return;
        InterlockedIncrement(&shared_->sequence);
        InterlockedExchange64(&shared_->completed, static_cast<LONG64>(completed));
        InterlockedExchange64(&shared_->total, static_cast<LONG64>(total));
        InterlockedExchange(&shared_->step, static_cast<LONG>(step));
        InterlockedIncrement(&shared_->sequence);
    }
    bool read(SetupSnapshot& out) const {
        if (!shared_) return false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto before = InterlockedCompareExchange(&shared_->sequence, 0, 0); if (before & 1) continue;
            SetupSnapshot value;
            value.step = static_cast<SetupStep>(InterlockedCompareExchange(&shared_->step, 0, 0));
            value.completed = static_cast<std::uint64_t>(InterlockedCompareExchange64(&shared_->completed, 0, 0));
            value.total = static_cast<std::uint64_t>(InterlockedCompareExchange64(&shared_->total, 0, 0));
            if (before == InterlockedCompareExchange(&shared_->sequence, 0, 0)) { out = value; return true; }
        }
        return false;
    }
};
}
