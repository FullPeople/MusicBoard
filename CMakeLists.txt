cmake_minimum_required(VERSION 3.25)

project(MusicBoard VERSION 0.3.2 LANGUAGES C CXX RC)
add_compile_definitions(MUSICBOARD_PRODUCT_VERSION="${PROJECT_VERSION}")

if(NOT WIN32)
    message(FATAL_ERROR "音乐板当前只支持 Windows。")
endif()

option(MUSICBOARD_WARNINGS_AS_ERRORS "把编译警告当错误" ON)
option(MUSICBOARD_BUILD_TESTS "构建测试" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 全项目统一静态 CRT（/MT）。必须在任何 add_library 之前设置。
#
# 不要逐目标设置：一旦哪个目标漏了，就会出现 /MT 与 /MD 混用，
# 表现为 __imp_realloc / __imp_ldexp 这类"找不到 DLL 导入符号"的链接错误，
# 报错位置还离真正的原因很远。实测被 stb_vorbis 咬过一次。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/CompilerOptions.cmake)

# ============================================================================
# 分层静态库
#   依赖方向单向向下：上层可以依赖下层，反过来由 CMake 目标在编译期挡住。
#   这样"随手 include 上层头文件"这种事不需要靠自觉。
#
#     core   ← 与音频无关的纯工具：路径、错误契约、日志
#     audio  ← 解码。依赖 core
#   （后续阶段的 library / platform / ui 依次向上叠加）
# ============================================================================

# ---------------------------------------------------------------- core
add_library(musicboard_core STATIC
    src/core/path_utf8.cpp
    src/core/file_bytes.cpp
    src/core/logger.cpp
    src/core/product_settings.cpp
    src/core/single_instance.cpp
    src/core/crash_reporter.cpp
    src/core/sha256.cpp
)
target_include_directories(musicboard_core PUBLIC src)
musicboard_apply_compiler_options(musicboard_core)
# crash_reporter 用 MiniDumpWriteDump；sha256 用 CNG（bcrypt）
target_link_libraries(musicboard_core PRIVATE dbghelp bcrypt)

# ---------------------------------------------------------------- install
# 安装事务与产品注册。与窗口、音频无关，可以单独测。
add_library(musicboard_install STATIC
    src/install/install_transaction.cpp
    src/install/product_registration.cpp
)
target_include_directories(musicboard_install PUBLIC src)
musicboard_apply_compiler_options(musicboard_install)
target_link_libraries(musicboard_install PUBLIC musicboard_core)
# 开始菜单快捷方式走 IShellLink（COM）
target_link_libraries(musicboard_install PRIVATE shell32 ole32)

# ---------------------------------------------------------------- audio
add_library(musicboard_audio STATIC
    src/audio/decoder.cpp
    src/audio/decoder_media_foundation.cpp
    src/audio/decoder_vorbis.cpp
    src/audio/container_probe.cpp
    src/audio/voice_timeline.cpp
    src/audio/audio_engine.cpp
    src/audio/preload_cache.cpp
    src/audio/prepared_audio.cpp
    src/audio/stream_feed.cpp
    src/audio/envelope_worker.cpp
    src/audio/waveform.cpp
)
target_include_directories(musicboard_audio PUBLIC src)
musicboard_apply_compiler_options(musicboard_audio)
target_link_libraries(musicboard_audio PUBLIC musicboard_core)
target_link_libraries(musicboard_audio PRIVATE
    mfplat mfreadwrite mfuuid ole32   # Media Foundation
    xaudio2                            # XAudio2
)

# ------------------------------------------------- third_party: stb_vorbis
# 单文件、公共领域。用 STB_VORBIS_NO_STDIO 排除它的文件 API ——
# 那套 API 走 fopen + char*，在中文路径上必然失败。
# 整个项目只经内存解码，让这个坑在结构上不可能发生。
add_library(musicboard_stb_vorbis STATIC third_party/stb/stb_vorbis.c)
target_include_directories(musicboard_stb_vorbis PUBLIC third_party/stb)
target_compile_definitions(musicboard_stb_vorbis PUBLIC
    STB_VORBIS_NO_STDIO=1
    STB_VORBIS_NO_PUSHDATA_API=1
)
if(MSVC)
    target_compile_options(musicboard_stb_vorbis PRIVATE /w)   # 第三方代码不参与警告
endif()
target_link_libraries(musicboard_audio PRIVATE musicboard_stb_vorbis)

# ---------------------------------------------------------------- library
# 曲库：扫描、文件操作、导入事务。依赖 core + audio（要用 probe 探格式）。
# 这一层不碰窗口、不碰播放 —— 它是纯数据层，可以在没有音频设备、
# 没有窗口的进程里跑完整个测试。
add_library(musicboard_library STATIC
    src/library/library_loader.cpp
    src/library/library_scanner.cpp
    src/library/library_ops.cpp
    src/library/import_transaction.cpp
    src/library/url_classify.cpp
    src/library/process_runner.cpp
    src/library/staging.cpp
    src/library/import_job.cpp
    src/library/http_download.cpp
    src/library/cover_art.cpp
)
target_include_directories(musicboard_library PUBLIC src)
musicboard_apply_compiler_options(musicboard_library)
target_link_libraries(musicboard_library PUBLIC musicboard_core musicboard_audio)
# 回收站走 SHFileOperationW
target_link_libraries(musicboard_library PRIVATE shell32 winhttp)

# ---------------------------------------------------------------- ui
# 界面交互内核：命中测试、层次、滚动。纯逻辑，不碰 D2D 与窗口 ——
# 于是"悬停高亮闪一下""拖到一半断了""滚到底还在跑"这些毛病可以被穷举测，
# 而不是靠真的去点。
add_library(musicboard_ui STATIC
    src/ui/ui_core.cpp
    src/ui/cover_cache.cpp
    src/ui/panel_layout.cpp
    src/ui/orb_view.cpp
    src/ui/panel_view.cpp
)
target_include_directories(musicboard_ui PUBLIC src)
musicboard_apply_compiler_options(musicboard_ui)
target_link_libraries(musicboard_ui PUBLIC musicboard_core)
# 封面解码走 WIC，位图交给 D2D
target_link_libraries(musicboard_ui PRIVATE musicboard_library d2d1 dwrite windowscodecs)

# ---------------------------------------------------------------- platform
# 窗口、合成、DPI。只依赖 core；不碰 audio。
add_library(musicboard_platform STATIC
    src/platform/dpi.cpp
    src/platform/global_hotkeys.cpp
    src/platform/transparent_window.cpp
    src/platform/composition_window.cpp
    src/platform/orb_window.cpp
    src/platform/window_layers.cpp
    src/platform/link_import_window.cpp
)
target_include_directories(musicboard_platform PUBLIC src)
musicboard_apply_compiler_options(musicboard_platform)
target_link_libraries(musicboard_platform PUBLIC musicboard_core)
target_link_libraries(musicboard_platform PRIVATE
    d3d11 d2d1 dcomp dxgi dwrite windowscodecs shcore
)

# ============================================================================
# 可执行
# ============================================================================
# 正式程序
configure_file(packaging/application.rc.in "${CMAKE_CURRENT_BINARY_DIR}/MusicBoard.rc" @ONLY)
add_executable(musicboard
    "${CMAKE_CURRENT_BINARY_DIR}/MusicBoard.rc"
    src/app/main.cpp
    src/app/application.cpp
    src/app/link_import.cpp
    src/app/app_selftest.cpp
    src/app/display_selftest.cpp
    src/app/app_output.cpp
)
musicboard_apply_compiler_options(musicboard)
# **GUI 子系统**：双击时不能弹出一个常驻的黑色控制台窗口，更不能让那个
# 窗口一关就把程序带走。仍然用 wmain 作为入口（/ENTRY 指到 wmainCRTStartup），
# 于是从终端启动时可以通过 AttachConsole 把输出打在终端里 ——
# 见 src/app/app_output.cpp。
set_target_properties(musicboard PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup")
target_link_libraries(musicboard PRIVATE
    musicboard_library musicboard_ui musicboard_platform musicboard_audio musicboard_core
    # shell32：通知区域、文件拖放、快捷方式；ole32：Shell 与将来的 DoDragDrop
    shell32 ole32
)

# 安装/卸载程序。它自己也在安装包里 —— 卸载器与被装的版本天然一致。
add_executable(musicboard_setup src/app/setup_main.cpp src/app/app_output.cpp "${CMAKE_CURRENT_BINARY_DIR}/MusicBoard.rc")
musicboard_apply_compiler_options(musicboard_setup)
# 安装程序同样是 GUI 子系统：双击时不该弹控制台。
# 它的输出走 app_output（终端里有控制台就打在终端里，否则进日志文件）。
set_target_properties(musicboard_setup PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup")
target_link_libraries(musicboard_setup PRIVATE musicboard_install musicboard_core shell32 ole32)
set_target_properties(musicboard_setup PROPERTIES OUTPUT_NAME "MusicBoardSetup")

# 阶段 2 的窗口 spike：透明覆盖层 + 无边框 + 自绘标题栏 + DPI + 中文文本
add_executable(spike
    src/app/spike_main.cpp
    src/app/panel_preview.cpp
)
musicboard_apply_compiler_options(spike)
target_link_libraries(spike PRIVATE musicboard_platform musicboard_ui musicboard_audio)

# Set awareness before COM or any UI is initialized, also for packaged binaries.
foreach(gui_target musicboard musicboard_setup spike)
    target_link_options(${gui_target} PRIVATE "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/desktop.manifest")
endforeach()

# ============================================================================
# 工具
# ============================================================================
add_executable(scan_library tools/scan_library.cpp)
musicboard_apply_compiler_options(scan_library)
target_link_libraries(scan_library PRIVATE musicboard_audio)

# 阶段 4 的前置探针：这台机器有没有可用的音频输出设备
add_executable(audio_probe tools/audio_probe.cpp)
add_executable(import_copy_probe tools/import_copy_probe.cpp)
musicboard_apply_compiler_options(import_copy_probe)
target_link_libraries(import_copy_probe PRIVATE musicboard_library)
musicboard_apply_compiler_options(audio_probe)
target_link_libraries(audio_probe PRIVATE musicboard_core xaudio2 ole32)

add_executable(audio_transition_probe tools/audio_transition_probe.cpp)
musicboard_apply_compiler_options(audio_transition_probe)
target_link_libraries(audio_transition_probe PRIVATE musicboard_audio)

# ============================================================================
# 测试
# ============================================================================
if(MUSICBOARD_BUILD_TESTS)
    enable_testing()

    # 每个模块一个测试可执行，零测试框架依赖。
    function(musicboard_add_test name)
        add_executable(${name} tests/${name}.cpp)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        musicboard_apply_compiler_options(${name})
        target_link_libraries(${name} PRIVATE ${ARGN})
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    musicboard_add_test(core_tests musicboard_core)
    musicboard_add_test(settings_tests musicboard_core)
    musicboard_add_test(single_instance_tests musicboard_core)
    musicboard_add_test(crash_reporter_tests musicboard_core)
    musicboard_add_test(sha256_tests musicboard_core)
    musicboard_add_test(install_transaction_tests musicboard_install)
    musicboard_add_test(product_registration_tests musicboard_install)
    musicboard_add_test(dpi_tests musicboard_platform)
    musicboard_add_test(global_hotkey_tests musicboard_platform)
    musicboard_add_test(container_probe_tests musicboard_audio)
    musicboard_add_test(voice_timeline_tests musicboard_audio)
    musicboard_add_test(waveform_tests musicboard_audio)
    musicboard_add_test(envelope_worker_tests musicboard_audio)
    musicboard_add_test(preload_stream_tests musicboard_audio musicboard_library)
    musicboard_add_test(ui_core_tests musicboard_ui)
    musicboard_add_test(cover_cache_tests musicboard_ui)
    musicboard_add_test(panel_layout_tests musicboard_ui)
    musicboard_add_test(audio_engine_tests musicboard_audio)
    musicboard_add_test(decoder_tests musicboard_audio)
    musicboard_add_test(library_ops_tests musicboard_library)
    musicboard_add_test(import_transaction_tests musicboard_library)
    musicboard_add_test(library_scanner_tests musicboard_library)
    musicboard_add_test(url_classify_tests musicboard_library)
    musicboard_add_test(window_layers_tests musicboard_platform)
    musicboard_add_test(http_download_tests musicboard_library)
    musicboard_add_test(cover_art_tests musicboard_library)
    musicboard_add_test(process_runner_tests musicboard_library)
    musicboard_add_test(import_job_tests musicboard_library)

    # 假下载器：进程管理与导入作业的测试拿它当子进程。
    # 它不是测试，所以不进 ctest —— 它是一个被 launch 的目标。
    # 输出目录跟测试可执行放一起：测试靠"自己的 exe 在哪"就能找到它，
    # 不需要环境变量或其他约定（多配置生成器下也不会指错）。
    add_executable(fake_child tests/fake_child.cpp)
    musicboard_apply_compiler_options(fake_child)
    add_dependencies(process_runner_tests fake_child)
    add_dependencies(import_job_tests fake_child)

    # 解码测试要求真实曲库存在；没有就跳过而不是失败（CI 上没有这个目录）。
    if(DEFINED ENV{MUSICBOARD_TEST_LIBRARY})
        set_tests_properties(decoder_tests PROPERTIES
            ENVIRONMENT "MUSICBOARD_TEST_LIBRARY=$ENV{MUSICBOARD_TEST_LIBRARY}")
        set_tests_properties(library_scanner_tests PROPERTIES
            ENVIRONMENT "MUSICBOARD_TEST_LIBRARY=$ENV{MUSICBOARD_TEST_LIBRARY}")
        set_tests_properties(cover_art_tests PROPERTIES
            ENVIRONMENT "MUSICBOARD_TEST_LIBRARY=$ENV{MUSICBOARD_TEST_LIBRARY}")
    endif()
endif()
