import AppKit
import AVFoundation
import Combine

struct Song: Identifiable, Codable, Equatable {
    var id: String { url.absoluteString }
    let url: URL
    let title: String
    let category: String
    let effect: Bool
}

struct BoardPreferences: Codable {
    var library: Data?
    var english = false
    var pinned = true
    var fade = true
    var mix = true
    var loop = true
    var volume: Double = 0.8
    var effectsVolume: Double = 0.8
}

enum BoardError: LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let s) = self { return s }; return nil }
}

func runTool(_ url: URL, _ arguments: [String], timeout: TimeInterval = 120) throws -> Data {
    let process = Process()
    process.executableURL = url; process.arguments = arguments
    let log = FileManager.default.temporaryDirectory.appendingPathComponent("musicboard-tool-\(UUID().uuidString)")
    guard FileManager.default.createFile(atPath: log.path, contents: nil) else { throw BoardError.message("Cannot create helper log") }
    let handle = try FileHandle(forWritingTo: log)
    defer { try? handle.close(); try? FileManager.default.removeItem(at: log) }
    process.standardOutput = handle; process.standardError = handle
    try process.run()
    let deadline = Date().addingTimeInterval(timeout)
    while process.isRunning && Date() < deadline { Thread.sleep(forTimeInterval: 0.08) }
    if process.isRunning {
        process.terminate()
        let end = Date().addingTimeInterval(3)
        while process.isRunning && Date() < end { Thread.sleep(forTimeInterval: 0.05) }
        if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        process.waitUntilExit()
        throw BoardError.message("Media helper timed out")
    }
    process.waitUntilExit(); try handle.synchronize()
    let data = try Data(contentsOf: log)
    if process.terminationStatus != 0 {
        throw BoardError.message(String(decoding: data.suffix(6000), as: UTF8.self))
    }
    return data
}

@MainActor final class AudioBoard: ObservableObject {
    @Published var preferences = BoardPreferences()
    @Published var songs: [Song] = []
    @Published var search = ""
    @Published var category = ""
    @Published var current: Song?
    @Published var playing = false
    @Published var seconds: Double = 0
    @Published var duration: Double = 0
    @Published var busy = false
    @Published var message = ""
    @Published var errorText: String?
    @Published var root: URL?
    @Published var effects: [String: AVPlayer] = [:]
    var active = AVPlayer()
    var outgoing: AVPlayer?
    var onPin: ((Bool) -> Void)?
    var onLanguage: (() -> Void)?
    private var envelope: Timer?
    private var timer: Timer?
    private var observedEnd: NSObjectProtocol?
    private var observedStatus: NSKeyValueObservation?
    private var scanGeneration = UUID()
    private var loadGeneration = UUID()
    private var lastSaved: BoardPreferences?
    let profile: URL
    let tools: URL
    let supported: Set<String> = ["mp3", "wav", "m4a", "aac", "aiff", "aif", "caf", "flac", "ogg", "opus", "webm", "mp4"]

    init(profile: URL, tools: URL) {
        self.profile = profile; self.tools = tools
        do {
            try FileManager.default.createDirectory(at: profile, withIntermediateDirectories: true)
            let path = profile.appendingPathComponent("preferences.json")
            if FileManager.default.fileExists(atPath: path.path) {
                preferences = try JSONDecoder().decode(BoardPreferences.self, from: Data(contentsOf: path))
            }
        } catch { errorText = error.localizedDescription }
        timer = Timer.scheduledTimer(withTimeInterval: 0.15, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            let value = self.active.currentTime().seconds
            self.seconds = value.isFinite ? max(0, value) : 0
            let length = self.active.currentItem?.duration.seconds ?? 0
            self.duration = length.isFinite ? max(0, length) : 0
        }
        if let data = preferences.library {
            do {
                var stale = false
                let url = try URL(resolvingBookmarkData: data, options: [.withoutUI], relativeTo: nil, bookmarkDataIsStale: &stale)
                selectLibrary(url)
            } catch { errorText = error.localizedDescription }
        }
    }

    func text(_ chinese: String, _ english: String) -> String { preferences.english ? english : chinese }
    var filtered: [Song] {
        songs.filter { (search.isEmpty || ($0.title + " " + $0.category).localizedCaseInsensitiveContains(search)) && (category.isEmpty || $0.category == category) }
    }
    var categories: [String] { Array(Set(songs.map(\.category))).sorted() }

    func save() {
        do { try JSONEncoder().encode(preferences).write(to: profile.appendingPathComponent("preferences.json"), options: .atomic) }
        catch { errorText = error.localizedDescription }
    }
    func selectLibrary(_ url: URL) {
        let token = UUID(); scanGeneration = token; root = url; busy = true
        do { preferences.library = try url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil); save() }
        catch { busy = false; errorText = error.localizedDescription; return }
        let allowed = supported
        DispatchQueue.global(qos: .userInitiated).async {
            var result: [Song] = []; var failure: Error?
            let enumerator = FileManager.default.enumerator(at: url, includingPropertiesForKeys: [.isRegularFileKey, .isSymbolicLinkKey], options: [.skipsHiddenFiles, .skipsPackageDescendants]) { _, error in failure = error; return false }
            if let enumerator = enumerator {
                for case let file as URL in enumerator {
                    do {
                        let values = try file.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
                        if values.isSymbolicLink == true { continue }
                        guard values.isRegularFile == true, allowed.contains(file.pathExtension.lowercased()) else { continue }
                        let parts = file.path.replacingOccurrences(of: url.path + "/", with: "").split(separator: "/").map(String.init)
                        let effect = parts.first?.uppercased() == "SFX"
                        let group = parts.count > 2 ? parts[1] : (effect ? "SFX" : "BGM")
                        result.append(Song(url: file, title: file.deletingPathExtension().lastPathComponent, category: group, effect: effect))
                    } catch { failure = error; break }
                }
            } else { failure = BoardError.message("Cannot read selected library") }
            let complete = result.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
            let scanError = failure?.localizedDescription
            DispatchQueue.main.async {
                guard self.scanGeneration == token else { return }
                self.busy = false
                if let error = scanError { self.errorText = error; return }
                self.songs = complete; self.loadDownloads(); self.category = ""
                self.message = self.text("已加载 \(complete.count) 首音频", "Loaded \(complete.count) tracks")
            }
        }
    }

    func playable(_ url: URL, completion: @escaping (Result<URL, Error>) -> Void) {
        guard ["ogg", "opus", "webm"].contains(url.pathExtension.lowercased()), url.isFileURL else { completion(.success(url)); return }
        let output = profile.appendingPathComponent("decoded-\(UUID().uuidString).caf")
        let executable = tools.appendingPathComponent("ffmpeg")
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result<URL, Error> {
                _ = try runTool(executable, ["-nostdin", "-v", "error", "-i", url.path, "-vn", "-c:a", "alac", "-y", output.path])
                return output
            }
            DispatchQueue.main.async { completion(result) }
        }
    }

    func play(_ song: Song) {
        if song.effect { playEffect(song); return }
        if current?.id == song.id { if !playing { togglePause() }; return }
        let token = UUID(); loadGeneration = token
        playable(song.url) { [weak self] result in
            guard let self = self, self.loadGeneration == token else { return }
            switch result {
            case .failure(let error): self.errorText = error.localizedDescription
            case .success(let url): self.start(song, url: url)
            }
        }
    }

    func start(_ song: Song, url: URL) {
        let incoming = AVPlayer(url: url)
        let old = active; let oldWasPlaying = playing
        envelope?.invalidate(); outgoing?.pause(); outgoing = old
        if let token = observedEnd { NotificationCenter.default.removeObserver(token) }
        observedStatus = incoming.currentItem?.observe(\.status, options: [.new]) { [weak self] item, _ in
            guard item.status == .failed else { return }
            DispatchQueue.main.async { self?.playing = false; self?.errorText = item.error?.localizedDescription ?? "Audio playback failed" }
        }
        active = incoming; current = song; playing = true; seconds = 0
        observedEnd = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime, object: incoming.currentItem, queue: .main) { [weak self, weak incoming] _ in
            guard let self = self, let incoming = incoming else { return }
            if self.preferences.loop { incoming.seek(to: .zero); incoming.play() } else { self.playing = false }
        }
        let volume = Float(preferences.volume)
        let crossfade = preferences.mix && oldWasPlaying
        if !preferences.fade && !crossfade { old.pause(); outgoing = nil; incoming.volume = volume; incoming.play(); return }
        let span: Double = crossfade ? 2.5 : (oldWasPlaying ? 1.2 : 0.6)
        incoming.volume = 0
        if crossfade || !oldWasPlaying { incoming.play() }
        let start = ProcessInfo.processInfo.systemUptime; let oldVolume = old.volume
        envelope = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self, weak incoming] timer in
            guard let self = self, let incoming = incoming else { timer.invalidate(); return }
            let t = min(1, (ProcessInfo.processInfo.systemUptime - start) / span)
            if crossfade {
                old.volume = oldVolume * Float(cos(t * .pi / 2)); incoming.volume = Float(self.preferences.volume) * Float(sin(t * .pi / 2))
            } else if oldWasPlaying && t < 0.5 { old.volume = oldVolume * Float(1 - t * 2) }
            else {
                old.pause(); if incoming.rate == 0 { incoming.play() }
                incoming.volume = Float(self.preferences.volume) * Float(oldWasPlaying ? (t - 0.5) * 2 : t)
            }
            if t >= 1 { old.pause(); self.outgoing = nil; timer.invalidate(); self.envelope = nil }
        }
    }

    func togglePause() {
        guard current != nil else { return }
        envelope?.invalidate(); outgoing?.pause(); outgoing = nil
        if playing {
            playing = false
            if !preferences.fade { active.pause(); return }
            let player = active; let start = ProcessInfo.processInfo.systemUptime; let gain = player.volume
            envelope = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] timer in
                let t = min(1, (ProcessInfo.processInfo.systemUptime - start) / 0.6)
                player.volume = gain * Float(1 - t)
                if t >= 1 { player.pause(); timer.invalidate(); self?.envelope = nil }
            }
        } else { active.volume = Float(preferences.volume); active.play(); playing = true }
    }
    func stopMusic() {
        loadGeneration = UUID(); envelope?.invalidate(); envelope = nil
        active.pause(); outgoing?.pause(); outgoing = nil
        active.replaceCurrentItem(with: nil); current = nil; playing = false; seconds = 0; duration = 0
    }
    func stopAll() { stopMusic(); for player in effects.values { player.pause() }; effects.removeAll() }
    func seek(_ seconds: Double) { active.seek(to: CMTime(seconds: max(0, min(seconds, duration)), preferredTimescale: 600)) }
    func updateVolumes() {
        if envelope == nil { active.volume = Float(preferences.volume) }
        for player in effects.values { player.volume = Float(preferences.effectsVolume) }
        save()
    }
    func playEffect(_ song: Song) {
        if let player = effects[song.id] { player.pause(); effects.removeValue(forKey: song.id); return }
        guard effects.count < 4 else { errorText = text("最多同时播放 4 个音效，请先停止一个。", "Stop one effect before starting a fifth."); return }
        playable(song.url) { [weak self] result in
            guard let self = self else { return }
            switch result {
            case .failure(let error): self.errorText = error.localizedDescription
            case .success(let url):
                guard self.effects.count < 4 else { return }
                let player = AVPlayer(url: url); player.volume = Float(self.preferences.effectsVolume)
                self.effects[song.id] = player; player.play()
                NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime, object: player.currentItem, queue: .main) { [weak self, weak player] _ in
                    if self?.effects[song.id] === player { self?.effects.removeValue(forKey: song.id) }
                }
            }
        }
    }

    func importLink(_ text: String, offline: Bool) {
        guard !busy else { return }
        guard let url = URL(string: text.trimmingCharacters(in: .whitespacesAndNewlines)), ["http", "https"].contains(url.scheme?.lowercased() ?? ""), url.host != nil, url.user == nil, url.password == nil else {
            errorText = self.text("请输入有效的 HTTP 或 HTTPS 链接。", "Enter a valid HTTP or HTTPS link."); return
        }
        let tools = self.tools; let folder = profile.appendingPathComponent("imports/\(UUID().uuidString)")
        busy = true; message = self.text("正在解析音频链接……", "Resolving audio link…")
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result<Song, Error> {
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                if offline {
                    _ = try runTool(tools.appendingPathComponent("yt-dlp"), ["--ignore-config", "--no-playlist", "--no-progress", "--no-warnings", "--socket-timeout", "15", "--retries", "3", "--ffmpeg-location", tools.path, "-f", "bestaudio/best", "-x", "--audio-format", "m4a", "-o", folder.appendingPathComponent("%(title).100s.%(ext)s").path, "--", url.absoluteString], timeout: 600)
                    let files = try FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil).filter { $0.pathExtension == "m4a" }
                    guard let file = files.first, files.count == 1 else { throw BoardError.message("Download did not produce one playable audio file") }
                    return Song(url: file, title: file.deletingPathExtension().lastPathComponent, category: "Downloads", effect: false)
                }
                if ["mp3", "m4a", "wav", "aac", "mp4", "m3u8"].contains(url.pathExtension.lowercased()) {
                    return Song(url: url, title: url.deletingPathExtension().lastPathComponent, category: "Online", effect: false)
                }
                let data = try runTool(tools.appendingPathComponent("yt-dlp"), ["--ignore-config", "--no-playlist", "--no-warnings", "--socket-timeout", "15", "--retries", "2", "--skip-download", "-j", "-f", "bestaudio[ext=m4a]/best[ext=mp4]/bestaudio", "--", url.absoluteString])
                guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any], let direct = json["url"] as? String, let stream = URL(string: direct), ["http", "https"].contains(stream.scheme ?? "") else { throw BoardError.message("No directly playable audio stream returned") }
                return Song(url: stream, title: json["title"] as? String ?? url.host!, category: "Online", effect: false)
            }
            DispatchQueue.main.async {
                self.busy = false
                switch result {
                case .failure(let error): self.errorText = error.localizedDescription
                case .success(let song):
                    self.songs.append(song); self.category = ""; self.search = ""; self.play(song)
                    self.message = self.text(offline ? "已下载到本机，可离线播放" : "已添加在线音频（仅本次会话）", offline ? "Downloaded for offline playback" : "Online audio added for this session")
                    if offline { self.saveDownloads() }
                }
            }
        }
    }
    func saveDownloads() {
        do { try JSONEncoder().encode(songs.filter { $0.category == "Downloads" }).write(to: profile.appendingPathComponent("downloads.json"), options: .atomic) }
        catch { errorText = error.localizedDescription }
    }
    func loadDownloads() {
        let path = profile.appendingPathComponent("downloads.json")
        guard FileManager.default.fileExists(atPath: path.path) else { return }
        do { songs += try JSONDecoder().decode([Song].self, from: Data(contentsOf: path)).filter { FileManager.default.fileExists(atPath: $0.url.path) } }
        catch { errorText = error.localizedDescription }
    }
}
