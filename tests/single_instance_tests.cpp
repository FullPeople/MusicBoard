// 单实例守卫测试
//
// 这个机制的本质是跨进程的，同进程内重复获取命名互斥体不会失败 ——
// 所以必须真的起第二个进程来验，否则测的是个假象。
//
// 结构：
//   父进程（默认模式）  起一个子进程持有守卫 → 自己再获取同一个名字 → 期望 already_running
//   子进程（--child-hold <name>）  持有守卫直到被杀

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/path_utf8.h"
#include "core/single_instance.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

std::wstring executable_path() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD written = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::wstring(buffer, written);
}

// 子进程拿到守卫之后用它通知父进程。
//
// 没有这个握手，父进程的探测会和子进程抢同一把互斥体：
// 子进程可能正好在父进程持有它的那一瞬间去获取、然后失败退出。
// 实测就是这样 —— 子进程报"已经有人持有"，而那个人是父进程自己。
[[nodiscard]] std::wstring ready_event_name(const std::wstring& guard_name) {
    return guard_name + L".Ready";
}

// 起一个自己，带上参数。返回进程句柄与 pid。
struct ChildProcess final {
    HANDLE handle = nullptr;
    DWORD pid = 0;

    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess() {
        if (handle != nullptr) {
            ::TerminateProcess(handle, 0);
            ::WaitForSingleObject(handle, 2000);
            ::CloseHandle(handle);
        }
    }
};

[[nodiscard]] bool spawn_child(const std::wstring& arguments, ChildProcess& child,
                              Failure& failure) {
    std::wstring command_line = executable_path();
    if (command_line.empty()) {
        failure.set(fail_code::platform_api_failed, "取不到自身可执行文件路径");
        return false;
    }
    command_line += L" ";
    command_line += arguments;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};

    // CreateProcessW 会修改命令行缓冲，所以必须传可写的
    std::wstring mutable_command = command_line;
    if (!::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          nullptr, &startup, &info)) {
        failure.set(fail_code::platform_api_failed,
                    "CreateProcessW 失败 (Win32 错误 " + std::to_string(::GetLastError()) + ")");
        return false;
    }
    ::CloseHandle(info.hThread);
    child.handle = info.hProcess;
    child.pid = info.dwProcessId;
    return true;
}

// 轮询尝试获取守卫：子进程可能还没起来，所以不能只试一次。
// 拿到 already_running 且 PID 对得上就算成功 —— 那正是我们要验证的状态。
//
// 每轮在局部作用域里建一个守卫，出作用域即析构释放；
// SingleInstanceGuard 刻意不可拷贝也不可移动（内核句柄的所有权不该被搬来搬去），
// 所以不能把它当返回值传出去。
[[nodiscard]] bool wait_until_held(const std::wstring& name, DWORD expected_pid,
                                  DWORD& observed_pid,
                                  const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        SingleInstanceGuard guard;
        SingleInstanceState state = SingleInstanceState::acquired;
        Failure failure;
        if (!guard.initialize(name, state, failure)) {
            return false;
        }
        if (state == SingleInstanceState::already_running) {
            observed_pid = guard.existing_process_id();
            if (observed_pid == expected_pid) {
                return true;
            }
            // 别的进程占着这个名字（理论上不该发生），继续等
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return false;
}

// 子进程：持有守卫直到被杀
int run_child_hold(const std::wstring& name) {
    SingleInstanceGuard guard;
    SingleInstanceState state = SingleInstanceState::acquired;
    Failure failure;
    if (!guard.initialize(name, state, failure)) {
        std::printf("child: 初始化失败 %s\n", failure.to_string().c_str());
        return 2;
    }
    if (state != SingleInstanceState::acquired) {
        // 父进程测试逻辑有问题才会走到这里
        std::printf("child: 期望拿到守卫，实际 already_running\n");
        return 3;
    }
    // 告诉父进程"我已经持有了"，它才可以开始探测。
    // 少了这一步，父子会抢同一把互斥体。
    const HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, ready_event_name(name).c_str());
    if (ready != nullptr) {
        ::SetEvent(ready);
        ::CloseHandle(ready);
    }

    // 守到被杀。父进程用 TerminateProcess 结束我们。
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void test_already_running_detects_other_process(Registry& r) {
    const std::wstring name = L"Local\\MusicBoard.Test.Guard.{A1B2C3D4-0001-4000-8000-000000000001}";

    // 先把就绪事件建出来，再起子进程 —— 否则可能错过它发出的信号
    const HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, ready_event_name(name).c_str());
    r.check(ready != nullptr, "创建就绪事件成功");

    ChildProcess child;
    Failure spawn_failure;
    r.check_invocation([&] { return spawn_child(L"--child-hold " + name, child, spawn_failure); }, "能起一个持有守卫的子进程", spawn_failure);
    if (child.handle == nullptr) {
        if (ready != nullptr) {
            ::CloseHandle(ready);
        }
        return;
    }

    if (ready != nullptr) {
        const DWORD wait = ::WaitForSingleObject(ready, 5000);
        r.check_eq(wait == WAIT_OBJECT_0, "子进程在 5 秒内报告已持有守卫",
                   "等待结果=" + std::to_string(wait));
        ::CloseHandle(ready);
    }

    DWORD observed_pid = 0;
    const bool detected =
        wait_until_held(name, child.pid, observed_pid, std::chrono::milliseconds(5000));

    r.check_eq(detected, "第二个实例被识别为 already_running", "观察到 PID=" + std::to_string(observed_pid) +
                                                                  " 期望=" + std::to_string(child.pid));
    r.check(observed_pid == child.pid, "共享内存里携带的是那个子进程的 PID");
    r.check(observed_pid != ::GetCurrentProcessId(), "拿到的不是自己的 PID（否则等于没检测到）");
}

void test_guard_released_after_owner_exits(Registry& r) {
    const std::wstring name = L"Local\\MusicBoard.Test.Guard.{A1B2C3D4-0002-4000-8000-000000000002}";

    {
        const HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, ready_event_name(name).c_str());
        ChildProcess child;
        Failure spawn_failure;
        r.check(spawn_child(L"--child-hold " + name, child, spawn_failure), "起子进程持有守卫");
        if (child.handle == nullptr) {
            if (ready != nullptr) {
                ::CloseHandle(ready);
            }
            return;
        }
        if (ready != nullptr) {
            ::WaitForSingleObject(ready, 5000);
            ::CloseHandle(ready);
        }

        DWORD observed = 0;
        r.check(wait_until_held(name, child.pid, observed, std::chrono::milliseconds(5000)),
                "子进程确实持有守卫");

        // 杀掉子进程（析构里做），接下来应当能重新拿到
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    SingleInstanceGuard guard;
    SingleInstanceState state = SingleInstanceState::already_running;
    Failure failure;
    r.check(guard.initialize(name, state, failure), "持有者退出后能重新初始化");
    r.check(state == SingleInstanceState::acquired, "持有者退出后守卫被释放（可以重新启动）");
    r.check(guard.acquired(), "acquired() 报告持有");
}

void test_same_process_second_guard_is_recursive(Registry& r) {
    // 这条不是产品行为，而是记录一个事实：命名互斥体对同一线程是可重入的，
    // 所以**同进程内测不出单实例**。写下来免得以后有人试图用同进程用例来"验证"。
    const std::wstring name = L"Local\\MusicBoard.Test.Guard.{A1B2C3D4-0003-4000-8000-000000000003}";

    SingleInstanceGuard first;
    SingleInstanceState state = SingleInstanceState::already_running;
    Failure failure;
    r.check(first.initialize(name, state, failure), "第一个守卫初始化成功");
    r.check(state == SingleInstanceState::acquired, "第一个守卫拿到所有权");

    SingleInstanceGuard second;
    SingleInstanceState second_state = SingleInstanceState::acquired;
    Failure second_failure;
    r.check(second.initialize(name, second_state, second_failure), "第二个守卫也能初始化");
    r.check_eq(second_state == SingleInstanceState::acquired,
               "同一线程重复获取命名互斥体会成功 —— 所以单实例必须跨进程测",
               "第二个守卫状态=" + std::string(second_state == SingleInstanceState::acquired
                                                  ? "acquired"
                                                  : "already_running"));
}

void test_lookup_name_is_stable(Registry& r) {
    // 正式的名字必须是个常量，不能每次启动变 —— 变了等于没有单实例
    r.check(!product_single_instance_guard_name.empty(), "正式守卫名非空");
    r.check(product_single_instance_guard_name.find(L"Local\\") == 0,
            "正式守卫名用 Local\\ 前缀（同一用户会话内唯一，不跨会话抢）");
    r.check(product_single_instance_guard_name.find(L"{") != std::wstring_view::npos,
            "正式守卫名里带 GUID，避免与其他程序撞名");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 子进程模式：持有守卫直到被杀
    if (argc >= 3 && std::wstring(argv[1]) == L"--child-hold") {
        return run_child_hold(argv[2]);
    }

    Registry r;
    test_lookup_name_is_stable(r);
    test_same_process_second_guard_is_recursive(r);
    test_already_running_detects_other_process(r);
    test_guard_released_after_owner_exits(r);
    return r.finish("single_instance_tests");
}
