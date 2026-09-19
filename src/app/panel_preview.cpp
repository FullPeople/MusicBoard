// 给 spike 用的面板预览：拿**真实曲库**填一个 PanelState，画一帧出来。
//
// 这不是完整的程序（那需要消息循环、播放、拖放接线）。它的用处很具体：
// 把已经做完的绘制层对着真素材渲染出来，让人（和后续的验收记录）
// 能看见"面板到底长什么样"，而不是只有一堆布局断言。
//
// 所以它只做三件事：扫曲库 → 填状态 → 画一帧存 PNG。

#include "panel_preview.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/decoder.h"
#include "audio/waveform.h"
#include "core/failure.h"
#include "core/path_utf8.h"
#include "core/product_settings.h"
#include "library/library_scanner.h"
#include "platform/global_hotkeys.h"

namespace musicboard::app {
namespace {

[[nodiscard]] std::string container_badge(AudioContainer container) {
    switch (container) {
        case AudioContainer::mp3:
            return "MP3";
        case AudioContainer::ogg:
            return "OGG";
        case AudioContainer::wav:
            return "WAV";
        case AudioContainer::flac:
            return "FLAC";
        case AudioContainer::mp4:
            return "M4A";
        default:
            return "?";
    }
}

}  // namespace

bool build_preview_state(const std::filesystem::path& library_root,
                         ui::PanelState& state, Failure& failure) {
    ScanOptions options;
    options.root = library_root;
    LibrarySnapshot snapshot;
    if (!scan_library(options, snapshot, failure)) {
        return false;
    }

    state = ui::PanelState{};
    state.title = "音乐板";
    state.bgm_library.search_placeholder = "搜索音乐…";
    state.library_label = "曲库 " + path_to_utf8(library_root);

    // ---- 分类芯片 ----
    state.bgm_library.chips.push_back("全部");
    for (const LibraryCategory& category : snapshot.bgm) {
        state.bgm_library.chips.push_back(category.name);
    }
    state.bgm_library.active_chip = 0;

    // ---- 曲目卡（最多前 24 张，够填满一屏还多） ----
    constexpr std::size_t kCards = 24;
    std::size_t taken = 0;
    for (const LibraryCategory& category : snapshot.bgm) {
        for (const LibraryTrack& track : category.tracks) {
            if (taken >= kCards) {
                break;
            }
            ui::TrackView view;
            view.name = track.name;
            view.category = category.name;
            view.badge = container_badge(track.container);
            view.duration_seconds = track.duration_seconds;
            view.path = track.path;
            // 第一张当成"正在播放"，这样卡片的高亮态也能被看见
            view.playing = (taken == 0);
            // 第三张当成"已选中"
            view.selected = (taken == 2);
            state.bgm_library.tracks.push_back(std::move(view));
            ++taken;
        }
        if (taken >= kCards) {
            break;
        }
    }

    // ---- BGM 台：拿第一首真的解一遍，让音波图是真的 ----
    if (!state.bgm_library.tracks.empty()) {
        const ui::TrackView& first = state.bgm_library.tracks.front();
        DecodedAudio audio;
        Failure decode_failure;
        if (decode_audio_file(first.path, audio, decode_failure)) {
            Failure envelope_failure;
            state.bgm.envelope =
                compute_waveform_envelope(audio, kDefaultWaveformBuckets, envelope_failure);
        }
        state.bgm.has_track = true;
        state.bgm.name = first.name;
        state.bgm.category = first.category;
        state.bgm.format = first.badge;
        state.bgm.duration_seconds = first.duration_seconds;
        state.bgm.position_seconds = first.duration_seconds * 0.42;
        state.bgm.playing = true;
        state.bgm.path = first.path;
        state.bgm.level = 0.62f;
        state.status_chip = "正在播放  " + first.name;
    } else {
        state.status_chip = "曲库是空的";
    }

    // ---- 音效台：四台各放一个，两种状态各来一个 ----
    {
        std::vector<LibraryTrack> sfx_tracks;
        for (const LibraryCategory& category : snapshot.sfx) {
            for (const LibraryTrack& track : category.tracks) {
                sfx_tracks.push_back(track);
                if (sfx_tracks.size() >= 4) {
                    break;
                }
            }
            if (sfx_tracks.size() >= 4) {
                break;
            }
        }
        for (std::size_t i = 0; i < sfx_slot_count; ++i) {
            ui::DeckView& deck = state.sfx[i];
            deck.looping = true;
            deck.fading = (i == 1);          // 第二台开了淡入淡出，能看出开关的区别
            deck.volume_percent = static_cast<std::uint16_t>(100 - i * 22);
            if (i < sfx_tracks.size()) {
                deck.has_track = true;
                deck.name = sfx_tracks[i].name;
                deck.format = container_badge(sfx_tracks[i].container);
                deck.duration_seconds = sfx_tracks[i].duration_seconds;
                deck.path = sfx_tracks[i].path;
                deck.position_seconds = sfx_tracks[i].duration_seconds * 0.5;
                deck.playing = (i == 0 || i == 1);   // 前两台"在响"
                deck.level = deck.playing ? 0.7f : 0.0f;
                // 其中一台算个可解码的包络
                if (i < 2) {
                    DecodedAudio audio;
                    Failure decode_failure;
                    if (decode_audio_file(sfx_tracks[i].path, audio, decode_failure)) {
                        Failure envelope_failure;
                        deck.envelope = compute_waveform_envelope(
                            audio, kDefaultWaveformBuckets, envelope_failure);
                    }
                }
            }
        }
    }

    // ---- 右列（音效库）：预览里也要有内容 ----
    //
    // 少了这一段，渲染出来的预览右列是空的 —— 而右列恰恰是最容易出问题的
    // 那一栏（命中段错开、音效卡与音乐卡长得不一样），空着就等于没预览。
    {
        state.sfx_library.search_placeholder = "搜索音效…";
        state.sfx_library.chips.push_back("全部");
        for (const LibraryCategory& category : snapshot.sfx) {
            state.sfx_library.chips.push_back(category.name);
        }
        state.sfx_library.active_chip = 0;

        constexpr std::size_t kSfxCards = 12;
        std::size_t taken_sfx = 0;
        for (const LibraryCategory& category : snapshot.sfx) {
            for (const LibraryTrack& track : category.tracks) {
                if (taken_sfx >= kSfxCards) {
                    break;
                }
                ui::TrackView view;
                view.name = track.name;
                view.category = category.name;
                view.badge = container_badge(track.container);
                view.duration_seconds = track.duration_seconds;
                view.path = track.path;
                view.playing = (taken_sfx == 0);    // 第一张算"正在响"
                state.sfx_library.tracks.push_back(std::move(view));
                ++taken_sfx;
            }
            if (taken_sfx >= kSfxCards) {
                break;
            }
        }
    }

    // ---- 快捷键角标与总线音量 ----
    const std::vector<HotkeyBinding>& defaults = default_sfx_hotkeys(sfx_slot_count);
    for (std::size_t i = 0; i < sfx_slot_count && i < defaults.size(); ++i) {
        state.sfx_hotkeys[i] = describe_hotkey(defaults[i].modifiers, defaults[i].virtual_key);
    }
    state.bgm_volume_percent = 82;
    state.sfx_volume_percent = 100;
    state.bgm_level = 0.62f;
    state.sfx_level = 0.45f;

    // ---- 悬停预览：拿第二首当被悬停的那张 ----
    if (state.bgm_library.tracks.size() > 1) {
        state.bgm_library.hovered_track = 1;
        state.bgm_library.peek_active = true;
        state.bgm_library.peek_name = state.bgm_library.tracks[1].name;
        DecodedAudio audio;
        Failure decode_failure;
        if (decode_audio_file(state.bgm_library.tracks[1].path, audio, decode_failure)) {
            Failure envelope_failure;
            state.bgm_library.peek_envelope =
                compute_waveform_envelope(audio, kDefaultWaveformBuckets, envelope_failure);
        }
    }

    std::printf("预览用曲目 音乐 %zu / 音效 %zu 张, 分类 %zu + %zu 个, "
                "音效台有 %zu 台在演出\n",
                state.bgm_library.tracks.size(), state.sfx_library.tracks.size(),
                state.bgm_library.chips.size(), state.sfx_library.chips.size(),
                std::size_t{1});
    return true;
}

}  // namespace musicboard::app
