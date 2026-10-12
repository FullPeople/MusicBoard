# MSVC 的编译选项集中在这里，所有目标共用一套。
#
# 为什么单独成文件：这几个开关不是"风格偏好"，每一个都对应一个在这台机器上
# 实际踩到过的坑（见 docs/IMPLEMENTATION.zh-CN.md 的"构建约定"一节）。

function(musicboard_apply_compiler_options target)
    if(MSVC)
        # UNICODE / _UNICODE —— 必须。
        # 不定义的话，Win32 头里的 IDC_ARROW、TEXT() 之类会解析成窄字符版本，
        # 在 LoadCursorW 这种显式宽字符 API 上直接类型不匹配。
        # 这是一个 Windows 程序的基础设置，不该靠逐处加 cast 去补。
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)

        # NOMINMAX —— 必须。
        # windows.h 默认把 min / max 定义成宏，于是 std::max(...) 会被展开成
        # (a > b ? a : b)，在 std::clamp 之类的地方直接语法错误，
        # 报错是 "error C2589: '(' : '::' 右边的非法标记"，完全看不出原因。
        target_compile_definitions(${target} PRIVATE NOMINMAX)

        target_compile_options(${target} PRIVATE
            # /utf-8 —— 必须。
            # MSVC 默认按系统活动代码页读源码（中文系统上是 936/GBK），
            # UTF-8 源码里的中文会被打乱，u8"" 字面量也随之解析失败。
            # 实测：不加这个开关，本仓库任何含中文的源文件都编译不过。
            /utf-8

            # /Zc:__cplusplus —— 必须。
            # 不加的话 __cplusplus 恒为 199711，所有 C++ 版本判断全部失效。
            /Zc:__cplusplus

            # /permissive- —— 关掉 MSVC 的宽松模式，贴近标准。
            /permissive-

            # /W4 起步。刻意不用 /Wall：它会把系统头里的噪音也报出来。
            /W4

            # 中文源文件里混入的窄字符串一律按 UTF-8 解释（配合 /utf-8）
            /Zc:char8_t
        )

        if(MUSICBOARD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    endif()
endfunction()
