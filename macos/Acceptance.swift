import AppKit
import AVFoundation

@MainActor func runAcceptance(output: URL, app: MusicBoardApp) {
    Task { await performAcceptance(output: output, app: app) }
}

@MainActor func performAcceptance(output: URL, app: MusicBoardApp) async {
        var checks: [String: Bool] = [:]
        var measurements: [String: Double] = [:]
        var failure: String?
        let fm = FileManager.default
        func check(_ name: String, _ value: Bool) throws {
            checks[name] = value
            if !value { throw BoardError.message("Acceptance failed: " + name) }
        }
        func wait(_ seconds: Double) async { try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000)) }
        func until(_ predicate: () -> Bool, timeout: Double = 15) async -> Bool {
            let end = Date().addingTimeInterval(timeout)
            while !predicate() && Date() < end { await wait(0.08) }
            return predicate()
        }
        do {
            try fm.createDirectory(at: output, withIntermediateDirectories: true)
            let library = output.appendingPathComponent("authored-library")
            let bgm = library.appendingPathComponent("BGM/Adventure")
            let sfx = library.appendingPathComponent("SFX/Effects")
            try fm.createDirectory(at: bgm, withIntermediateDirectories: true)
            try fm.createDirectory(at: sfx, withIntermediateDirectories: true)
            let format = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 2)!
            let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 44100 * 8)!
            buffer.frameLength = buffer.frameCapacity
            for channel in 0..<2 {
                for i in 0..<Int(buffer.frameLength) { buffer.floatChannelData![channel][i] = Float(sin(Double(i) * 440 * 2 * .pi / 44100) * 0.18) }
            }
            for url in [bgm.appendingPathComponent("雨夜.wav"), bgm.appendingPathComponent("森林.wav"), sfx.appendingPathComponent("木门.wav")] {
                let file = try AVAudioFile(forWriting: url, settings: format.settings); try file.write(from: buffer)
            }
            let flac = bgm.appendingPathComponent("无损.flac")
            _ = try runTool(app.board.tools.appendingPathComponent("ffmpeg"), ["-nostdin", "-v", "error", "-i", bgm.appendingPathComponent("雨夜.wav").path, "-c:a", "flac", flac.path])
            app.board.selectLibrary(library)
            let scanned = await until { !app.board.busy }
            try check("SelectedLibraryLoadsWithoutWritingOriginalAudio", scanned && app.board.songs.count == 4 && app.board.errorText == nil)
            try check("ChineseFilenamesAndCategoriesPreserved", app.board.songs.contains { $0.title == "雨夜" && $0.category == "Adventure" })
            try check("EffectsDetectedByLibraryFolder", app.board.songs.filter(\.effect).count == 1)
            let first = app.board.songs.first { $0.title == "雨夜" }!
            let second = app.board.songs.first { $0.title == "森林" }!
            let effect = app.board.songs.first { $0.effect }!
            let initialBytes = try Data(contentsOf: first.url)
            app.board.search = "雨夜"
            try check("SearchFindsChineseTrack", app.board.filtered.count == 1)
            app.board.search = ""; app.board.category = "Effects"
            try check("CategorySeparatesEffects", app.board.filtered.count == 1 && app.board.filtered[0].effect)
            app.board.category = ""
            try check("MainWindowVisible", app.window.isVisible && app.window.contentView!.bounds.width >= 800)
            try check("OrbVisibleAcrossSpaces", app.orb.isVisible && app.orb.collectionBehavior.contains(.canJoinAllSpaces))
            app.applyPin(true); try check("PinUsesNativeFloatingWindowLevel", app.window.level == .floating)
            app.applyPin(false); try check("UnpinRestoresNativeNormalLevel", app.window.level == .normal)
            app.applyPin(true)
            app.board.preferences.fade = false; app.board.preferences.mix = false; app.board.preferences.loop = false
            app.board.play(first)
            let advancing = await until { app.board.active.currentTime().seconds > 0.25 }
            try check("ActualAVPlayerAdvancesAuthoredLocalAudio", advancing && app.board.active.currentItem?.status == .readyToPlay)
            let decoded = try AVAudioFile(forReading: first.url)
            let probe = AVAudioPCMBuffer(pcmFormat: decoded.processingFormat, frameCapacity: 4096)!
            try decoded.read(into: probe)
            var squares: Double = 0
            for i in 0..<Int(probe.frameLength) { let sample = Double(probe.floatChannelData![0][i]); squares += sample * sample }
            let rms = sqrt(squares / Double(probe.frameLength)); measurements["AuthoredToneDecodedRMS"] = rms
            try check("ActualDecoderProducesNonzeroPCM", probe.frameLength == 4096 && rms > 0.11 && rms < 0.14)
            let previous = app.board.active
            app.board.play(Song(url: output.appendingPathComponent("missing-file.wav"), title: "Missing", category: "Fixture", effect: false))
            let rejected = await until { app.board.errorText != nil }
            try check("FailedIncomingAudioPreservesCurrentPlayback", rejected && app.board.active === previous && app.board.playing && previous.rate > 0)
            app.board.errorText = nil
            app.board.togglePause(); await wait(0.2)
            let paused = app.board.active.currentTime().seconds; await wait(0.2)
            try check("PauseStopsClock", !app.board.playing && abs(app.board.active.currentTime().seconds - paused) < 0.05)
            app.board.togglePause(); await wait(0.3)
            try check("ResumeContinuesClock", app.board.playing && app.board.active.currentTime().seconds > paused + 0.1)
            app.board.seek(3); await wait(0.4)
            try check("SeekMovesActualPlayer", app.board.active.currentTime().seconds >= 2.9 && app.board.active.currentTime().seconds < 4)
            app.board.playEffect(effect)
            let effectStarts = await until { app.board.effects[effect.id]?.currentTime().seconds ?? 0 > 0.15 }
            try check("EffectPlaysAlongsideMusic", effectStarts && app.board.active.rate > 0 && app.board.effects.count == 1)
            app.board.playEffect(effect)
            try check("EffectSecondClickStopsOnlyEffect", app.board.effects.isEmpty && app.board.playing)
            app.board.preferences.mix = true
            app.board.play(second); await wait(1.25)
            try check("CrossfadeHasTwoIndependentAdvancingPlayers", app.board.outgoing?.rate ?? 0 > 0 && app.board.active.rate > 0)
            let power = pow(Double(app.board.outgoing?.volume ?? 0), 2) + pow(Double(app.board.active.volume), 2)
            try check("CrossfadeEnvelopePreservesConstantPower", abs(power - pow(app.board.preferences.volume, 2)) < 0.04)
            await wait(1.45)
            try check("CrossfadeRetiresOutgoingVoice", app.board.outgoing == nil && app.board.current?.id == second.id)
            app.board.preferences.fade = true; app.board.togglePause(); await wait(0.8)
            try check("FadePauseReachesSilenceAndStops", app.board.active.volume == 0 && app.board.active.rate == 0 && !app.board.playing)
            app.board.togglePause(); await wait(0.3)
            app.board.preferences.volume = 0.35; app.board.updateVolumes()
            try check("MusicVolumeUpdatesPlayer", abs(app.board.active.volume - 0.35) < 0.001)
            app.board.preferences.english = true; app.board.save(); app.buildMenu()
            try check("EnglishMenuAndLabelsSwitch", app.board.text("中文", "English") == "English" && NSApp.mainMenu!.items[1].title == "File")
            app.board.preferences.english = false; app.board.save(); app.buildMenu()
            try check("ChineseMenuRestored", NSApp.mainMenu!.items[1].title == "文件")
            let saved = try JSONDecoder().decode(BoardPreferences.self, from: Data(contentsOf: app.board.profile.appendingPathComponent("preferences.json")))
            try check("PreferencesPersistAtomically", saved.volume == 0.35 && !saved.english && saved.library != nil)
            app.toggleWindow(); try check("HideKeepsOrbAndPlayback", !app.window.isVisible && app.orb.isVisible && app.board.playing)
            app.toggleWindow(); try check("OrbActionRestoresMainPanel", app.window.isVisible)
            app.board.stopAll()
            try check("StopAllRetiresEveryVoice", app.board.active.currentItem == nil && app.board.effects.isEmpty && app.board.outgoing == nil)
            app.board.play(app.board.songs.first { $0.url == flac }!)
            let lossless = await until { app.board.active.currentTime().seconds > 0.2 }
            try check("NativeLosslessAudioPlayback", lossless && app.board.errorText == nil)
            if let direct = app.argument("--fixture-url") {
                app.board.stopAll(); app.board.preferences.fade = false; app.board.preferences.mix = false
                app.board.importLink(direct, offline: false)
                let online = await until { !app.board.busy && app.board.current?.url.isFileURL == false && app.board.active.currentTime().seconds > 0.2 }
                try check("OnlineDirectHTTPAudioActuallyAdvances", online && app.board.errorText == nil)
                app.board.stopAll(); app.board.importLink(direct, offline: true)
                let download = await until({ !app.board.busy }, timeout: 90)
                try check("OfflineImportCreatesPlayableLocalAudio", download && app.board.current?.category == "Downloads" && app.board.current!.url.isFileURL && app.board.errorText == nil)
                let file = app.board.current!.url
                try check("DownloadedAudioHasDecodableSamples", try AVAudioFile(forReading: file).length > 44100)
                try check("OfflineCatalogPersists", fm.fileExists(atPath: app.board.profile.appendingPathComponent("downloads.json").path))
                app.board.stopAll()
            }
            try check("LibraryAudioRemainsByteIdentical", try Data(contentsOf: first.url) == initialBytes)
            app.board.importLink("file:///etc/passwd", offline: false)
            try check("ImportRejectsNonHTTPURLs", app.board.errorText != nil && !app.board.busy)
            app.board.errorText = nil; app.board.preferences.volume = 0.8; app.board.updateVolumes()
            await wait(0.4)
            let view = app.window.contentView!
            if let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                view.cacheDisplay(in: view.bounds, to: bitmap)
                if let png = bitmap.representation(using: .png, properties: [:]) { try png.write(to: output.appendingPathComponent("window.png")); try check("ActualHostedInterfaceScreenshotCaptured", png.count > 20000) }
            }
            try check("ScreenshotEvidenceExists", fm.fileExists(atPath: output.appendingPathComponent("window.png").path))
        } catch { failure = error.localizedDescription }
        let report: [String: Any] = ["Passed": failure == nil && !checks.isEmpty && checks.values.allSatisfy { $0 }, "Checks": checks, "Measurements": measurements, "Failure": failure as Any? ?? NSNull(), "Architecture": ProcessInfo.processInfo.machineHardwareName, "Scope": "Native macOS application window and real AVPlayer/decoder, generated tones and local HTTP audio/download. No private media. No physical speaker, Gatekeeper/Finder, multiple physical monitors or all public platform URL claim."]
        do {
            try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys]).write(to: output.appendingPathComponent("report.json"))
        } catch { fputs("Report write failed: \(error)\n", stderr) }
        app.board.stopAll()
        exit(failure == nil ? 0 : 1)
}

extension ProcessInfo {
    var machineHardwareName: String { var name = utsname(); uname(&name); return withUnsafePointer(to: &name.machine) { $0.withMemoryRebound(to: CChar.self, capacity: 1) { String(cString: $0) } } }
}
