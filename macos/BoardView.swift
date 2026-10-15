import SwiftUI
import UniformTypeIdentifiers

@MainActor struct BoardView: View {
    @ObservedObject var board: AudioBoard
    var choose: () -> Void
    var link: () -> Void
    let orange = Color(red: 0.98, green: 0.51, blue: 0.24)
    func t(_ cn: String, _ en: String) -> String { board.text(cn, en) }
    func stamp(_ value: Double) -> String { String(format: "%d:%02d", Int(max(0, value)) / 60, Int(max(0, value)) % 60) }
    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 34, height: 34)
                Text("MusicBoard").font(.system(size: 19, weight: .semibold))
                Spacer()
                Button(action: choose) { Label(t("选择曲库", "Choose Library"), systemImage: "folder") }.accessibilityIdentifier("choose-library")
                Button(action: link) { Label(t("导入链接", "Import Link"), systemImage: "link") }.disabled(board.busy).accessibilityIdentifier("link-import")
                Button(board.preferences.english ? "EN" : "中文") {
                    board.preferences.english.toggle(); board.save(); board.onLanguage?()
                }.accessibilityIdentifier("language-toggle")
                Button { board.preferences.pinned.toggle(); board.onPin?(board.preferences.pinned); board.save() } label: {
                    Image(systemName: board.preferences.pinned ? "pin.fill" : "pin")
                }.help(t("置顶", "Always on Top")).accessibilityIdentifier("pin-toggle")
            }.padding(16)
            Divider()
            HSplitView {
                VStack(alignment: .leading, spacing: 12) {
                    Text(t("曲库", "LIBRARY")).font(.caption).foregroundColor(.secondary)
                    Button(t("全部音频", "All Tracks")) { board.category = "" }.buttonStyle(.plain).foregroundColor(board.category.isEmpty ? orange : .primary)
                    ForEach(board.categories, id: \.self) { name in
                        Button(name) { board.category = name }.buttonStyle(.plain).foregroundColor(board.category == name ? orange : .primary)
                    }
                    Spacer()
                    if let root = board.root {
                        Text(root.path).font(.caption2).foregroundColor(.secondary).lineLimit(3).textSelection(.enabled)
                        Button(t("重新加载", "Reload")) { board.selectLibrary(root) }.disabled(board.busy)
                    }
                    Text(t("拖入音频文件即可播放", "Drop audio files to play")).font(.caption).foregroundColor(.secondary)
                }.padding(16).frame(minWidth: 145, idealWidth: 175, maxWidth: 240)
                VStack(spacing: 12) {
                    HStack {
                        Image(systemName: "magnifyingglass").foregroundColor(.secondary)
                        TextField(t("搜索曲名或分类", "Search tracks or categories"), text: $board.search).textFieldStyle(.plain).accessibilityIdentifier("track-search")
                        if board.busy { ProgressView().controlSize(.small) }
                        Text("\(board.filtered.count)").foregroundColor(.secondary).monospacedDigit()
                    }.padding(10).background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 10))
                    ScrollView {
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 155), spacing: 12)], spacing: 12) {
                            ForEach(board.filtered) { song in
                                Button { board.play(song) } label: {
                                    VStack(alignment: .leading, spacing: 9) {
                                        HStack {
                                            Image(systemName: song.effect ? "waveform" : "opticaldisc").font(.system(size: 25)).foregroundColor(orange)
                                            Spacer()
                                            if board.current?.id == song.id || board.effects[song.id] != nil { Image(systemName: "speaker.wave.2.fill").foregroundColor(orange) }
                                        }
                                        Text(song.title).font(.system(size: 13, weight: .medium)).lineLimit(2).frame(height: 34, alignment: .topLeading)
                                        Text(song.category).font(.caption).foregroundColor(.secondary)
                                    }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
                                        .background(board.current?.id == song.id ? orange.opacity(0.16) : Color.white.opacity(0.045), in: RoundedRectangle(cornerRadius: 13))
                                        .overlay(RoundedRectangle(cornerRadius: 13).stroke(board.current?.id == song.id ? orange.opacity(0.65) : Color.white.opacity(0.08)))
                                }.buttonStyle(.plain).help(song.effect ? t("点击播放或停止音效", "Click to play or stop effect") : song.title)
                                    .contextMenu {
                                        Button(t("播放", "Play")) { board.play(song) }
                                        if song.url.isFileURL { Button(t("在访达中显示", "Show in Finder")) { NSWorkspace.shared.activateFileViewerSelecting([song.url]) } }
                                        if song.effect { Button(t("停止音效", "Stop Effect")) { if let player = board.effects.removeValue(forKey: song.id) { player.pause() } } }
                                    }
                            }
                        }.padding(2)
                        if board.songs.isEmpty && !board.busy {
                            VStack(spacing: 14) {
                                Image(systemName: "music.note.house").font(.system(size: 44)).foregroundColor(orange)
                                Text(t("选择曲库，开始你的下一场冒险", "Choose a library for your next adventure")).font(.headline)
                                Text(t("BGM 文件夹放背景音乐，SFX 文件夹放音效。也可以拖入音频，或导入链接。", "Put music in BGM and effects in SFX, drop audio files, or import a link.")).foregroundColor(.secondary).multilineTextAlignment(.center)
                                Button(t("选择曲库文件夹", "Choose Library Folder"), action: choose)
                            }.frame(maxWidth: .infinity).padding(.top, 65)
                        }
                    }
                }.padding(16).frame(minWidth: 360)
            }
            Divider()
            HStack(spacing: 16) {
                Image(systemName: "opticaldisc").font(.system(size: 42)).foregroundColor(orange)
                    .rotationEffect(.degrees(board.playing ? board.seconds * 18 : 0))
                    .frame(width: 54)
                VStack(alignment: .leading, spacing: 7) {
                    Text(board.current?.title ?? t("尚未播放", "Nothing playing")).font(.headline).lineLimit(1)
                    HStack {
                        Text(stamp(board.seconds)).font(.caption).monospacedDigit()
                        Slider(value: Binding(get: { min(board.seconds, max(1, board.duration)) }, set: { board.seek($0) }), in: 0...max(1, board.duration)).disabled(board.duration <= 0).accessibilityIdentifier("playback-seek")
                        Text(stamp(board.duration)).font(.caption).monospacedDigit()
                    }
                }
                Button { board.togglePause() } label: { Image(systemName: board.playing ? "pause.fill" : "play.fill").font(.title2).frame(width: 32, height: 30) }.disabled(board.current == nil).accessibilityIdentifier("play-pause")
                Button { board.stopAll() } label: { Image(systemName: "stop.fill").frame(width: 28, height: 30) }.help(t("停止所有音频", "Stop All Audio")).accessibilityIdentifier("stop-all")
                VStack(alignment: .leading, spacing: 7) {
                    Toggle(t("循环", "Loop"), isOn: $board.preferences.loop).onChange(of: board.preferences.loop) { _ in board.save() }
                    Toggle(t("淡入淡出", "Fade"), isOn: $board.preferences.fade).onChange(of: board.preferences.fade) { _ in board.save() }
                    Toggle(t("混音", "Mix"), isOn: $board.preferences.mix).onChange(of: board.preferences.mix) { _ in board.save() }
                }.toggleStyle(.checkbox).font(.caption).frame(width: 100)
                VStack(spacing: 7) {
                    HStack { Text(t("音乐", "Music")).font(.caption); Slider(value: $board.preferences.volume, in: 0...1).onChange(of: board.preferences.volume) { _ in board.updateVolumes() } }
                    HStack { Text(t("音效", "SFX")).font(.caption); Slider(value: $board.preferences.effectsVolume, in: 0...1).onChange(of: board.preferences.effectsVolume) { _ in board.updateVolumes() } }
                }.frame(width: 145)
            }.padding(16)
            HStack {
                Text(board.message).lineLimit(1)
                Spacer()
                Text(t("音效 \(board.effects.count)/4", "Effects \(board.effects.count)/4"))
            }.font(.caption).foregroundColor(.secondary).padding(.horizontal, 16).padding(.bottom, 9)
        }
        .frame(minWidth: 800, minHeight: 520)
        .background(Color(red: 0.075, green: 0.08, blue: 0.10))
        .preferredColorScheme(.dark).tint(orange)
        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
            for provider in providers {
                _ = provider.loadDataRepresentation(forTypeIdentifier: UTType.fileURL.identifier) { data, _ in
                    guard let data = data, let url = URL(dataRepresentation: data, relativeTo: nil) else { return }
                    DispatchQueue.main.async {
                        guard board.supported.contains(url.pathExtension.lowercased()) else { return }
                        let song = Song(url: url, title: url.deletingPathExtension().lastPathComponent, category: "Dropped", effect: false)
                        if !board.songs.contains(where: { $0.id == song.id }) { board.songs.append(song) }
                        board.play(song)
                    }
                }
            }; return true
        }
        .alert(t("无法完成操作", "Operation Failed"), isPresented: Binding(get: { board.errorText != nil }, set: { if !$0 { board.errorText = nil } })) {
            Button(t("确定", "OK")) { board.errorText = nil }
        } message: { Text(board.errorText ?? "") }
    }
}
