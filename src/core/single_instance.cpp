#include "core/single_instance.h"

#include "core/path_utf8.h"

namespace musicboard {
namespace {

// 共享内存的名字比互斥体多一个后缀，避免两个内核对象撞名
[[nodiscard]] std::wstring owner_mapping_name(std::wstring_view guard_name) {
    return std::wstring(guard_name) + L".Owner";
}

constexpr std::size_t kOwnerMappingSize = sizeof(DWORD);

}  // namespace

SingleInstanceGuard::~SingleInstanceGuard() {
    release();
}

void SingleInstanceGuard::release() noexcept {
    if (owner_view_ != nullptr) {
        ::UnmapViewOfFile(owner_view_);
        owner_view_ = nullptr;
    }
    if (owner_mapping_ != nullptr) {
        ::CloseHandle(owner_mapping_);
        owner_mapping_ = nullptr;
    }
    if (mutex_ != nullptr) {
        if (is_owner_) {
            ::ReleaseMutex(mutex_);
        }
        ::CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    is_owner_ = false;
    existing_process_id_ = 0;
}

bool SingleInstanceGuard::initialize(std::wstring_view name, SingleInstanceState& state,
                                     Failure& failure) {
    release();

    const std::wstring mutex_name(name);
    // bInitialOwner=false：先只创建，再用 WaitForSingleObject(0) 判断谁拿到。
    // 这样"已存在"和"刚创建"两种情况都能用同一套逻辑处理。
    mutex_ = ::CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (mutex_ == nullptr) {
        failure.set(fail_code::platform_api_failed,
                    "CreateMutexW 失败 (Win32 错误 " + std::to_string(::GetLastError()) + ")");
        return false;
    }

    const DWORD wait = ::WaitForSingleObject(mutex_, 0);
    if (wait == WAIT_OBJECT_0) {
        // 拿到了 —— 本进程是唯一实例，把 PID 写进共享内存供后来者读取
        is_owner_ = true;

        const std::wstring mapping_name = owner_mapping_name(name);
        owner_mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>(kOwnerMappingSize),
                                              mapping_name.c_str());
        if (owner_mapping_ != nullptr) {
            owner_view_ = ::MapViewOfFile(owner_mapping_, FILE_MAP_WRITE, 0, 0, kOwnerMappingSize);
            if (owner_view_ != nullptr) {
                *static_cast<DWORD*>(owner_view_) = ::GetCurrentProcessId();
            }
        }
        // 共享内存开不出来不影响"我是唯一实例"这个结论，
        // 只是后来者拿不到 PID、没法把窗口叫到前面。不当作致命失败。
        state = SingleInstanceState::acquired;
        return true;
    }

    if (wait == WAIT_ABANDONED) {
        // 上一个持有者没释放就退出了（崩溃）。这个互斥体现在归我们。
        // 这属于"确实发生过异常"，但结论仍然是我们可以继续启动。
        is_owner_ = true;
        state = SingleInstanceState::acquired;
        return true;
    }

    if (wait == WAIT_TIMEOUT) {
        // 已有实例。去共享内存里取它的 PID。
        const std::wstring mapping_name = owner_mapping_name(name);
        owner_mapping_ = ::OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name.c_str());
        if (owner_mapping_ != nullptr) {
            owner_view_ = ::MapViewOfFile(owner_mapping_, FILE_MAP_READ, 0, 0, kOwnerMappingSize);
            if (owner_view_ != nullptr) {
                existing_process_id_ = *static_cast<const DWORD*>(owner_view_);
            }
        }
        state = SingleInstanceState::already_running;
        return true;
    }

    failure.set(fail_code::platform_api_failed,
                "WaitForSingleObject 返回意外值 " + std::to_string(wait));
    return false;
}

}  // namespace musicboard
