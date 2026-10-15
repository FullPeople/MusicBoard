import AppKit
import SwiftUI
import AVFoundation

@MainActor final class MusicBoardApp: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var board: AudioBoard!
    var window: NSWindow!
    var orb: NSPanel!
    var statusItem: NSStatusItem!
    var linkWindow: NSWindow?
    let arguments = CommandLine.arguments
    func argument(_ key: String) -> String? { guard let i = arguments.firstIndex(of: key), i + 1 < arguments.count else { return nil }; return arguments[i + 1] }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        let profile = argument("--profile").map { URL(fileURLWithPath: $0) } ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("MusicBoard")
        board = AudioBoard(profile: profile, tools: Bundle.main.resourceURL!.appendingPathComponent("media-tools"))
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1050, height: 700), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "MusicBoard"; window.identifier = NSUserInterfaceItemIdentifier("musicboard-main")
        window.isReleasedWhenClosed = false; window.delegate = self
        window.contentView = NSHostingView(rootView: BoardView(board: board, choose: { [weak self] in self?.chooseLibrary() }, link: { [weak self] in self?.showLink() }))
        window.center(); window.setFrameAutosaveName("MusicBoard.main")
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        board.onPin = { [weak self] pin in self?.applyPin(pin) }
        board.onLanguage = { [weak self] in self?.buildMenu() }
        createOrb(); applyPin(board.preferences.pinned); buildMenu()
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.image = NSImage(systemSymbolName: "music.note", accessibilityDescription: "MusicBoard")
        statusItem.button?.target = self; statusItem.button?.action = #selector(toggleWindow)
        if let path = argument("--library") { board.selectLibrary(URL(fileURLWithPath: path)) }
        else { board.loadDownloads() }
        window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        if let output = argument("--self-test") { runAcceptance(output: URL(fileURLWithPath: output), app: self) }
    }
    func applyPin(_ pin: Bool) {
        window.level = pin ? .floating : .normal
        orb.level = .floating
        linkWindow?.level = pin ? NSWindow.Level(rawValue: NSWindow.Level.floating.rawValue + 1) : .normal
    }
    func createOrb() {
        orb = NSPanel(contentRect: NSRect(x: 70, y: 120, width: 64, height: 64), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        orb.isOpaque = false; orb.backgroundColor = .clear; orb.hasShadow = true; orb.isMovableByWindowBackground = true
        orb.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]; orb.hidesOnDeactivate = false
        let button = NSButton(image: NSApp.applicationIconImage, target: self, action: #selector(toggleWindow))
        button.imageScaling = .scaleProportionallyUpOrDown; button.isBordered = false; button.frame = NSRect(x: 0, y: 0, width: 64, height: 64)
        button.setAccessibilityIdentifier("record-orb")
        let menu = NSMenu()
        let toggle = menu.addItem(withTitle: "显示 / 隐藏 · Show / Hide", action: #selector(toggleWindow), keyEquivalent: ""); toggle.target = self
        let pause = menu.addItem(withTitle: "播放 / 暂停 · Play / Pause", action: #selector(pauseMusic), keyEquivalent: ""); pause.target = self
        menu.addItem(.separator()); menu.addItem(withTitle: "退出 · Quit", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "")
        button.menu = menu; orb.contentView = button; orb.orderFrontRegardless()
    }
    func buildMenu() {
        let menu = NSMenu(); let app = NSMenu(); let file = NSMenu(); let playback = NSMenu()
        let appItem = NSMenuItem(); appItem.submenu = app; menu.addItem(appItem)
        app.addItem(withTitle: board.text("关于 MusicBoard", "About MusicBoard"), action: #selector(about), keyEquivalent: "").target = self
        app.addItem(withTitle: board.text("检查更新…", "Check for Updates…"), action: #selector(checkUpdates), keyEquivalent: "").target = self
        app.addItem(.separator())
        app.addItem(withTitle: board.text("退出 MusicBoard", "Quit MusicBoard"), action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let fileItem = NSMenuItem(title: board.text("文件", "File"), action: nil, keyEquivalent: ""); fileItem.submenu = file; menu.addItem(fileItem)
        file.addItem(withTitle: board.text("选择曲库…", "Choose Library…"), action: #selector(chooseLibrary), keyEquivalent: "o").target = self
        file.addItem(withTitle: board.text("导入音频链接…", "Import Audio Link…"), action: #selector(showLink), keyEquivalent: "l").target = self
        let playItem = NSMenuItem(title: board.text("播放", "Playback"), action: nil, keyEquivalent: ""); playItem.submenu = playback; menu.addItem(playItem)
        playback.addItem(withTitle: board.text("播放 / 暂停", "Play / Pause"), action: #selector(pauseMusic), keyEquivalent: " ").target = self
        playback.addItem(withTitle: board.text("停止所有音频", "Stop All Audio"), action: #selector(stopAll), keyEquivalent: ".").target = self
        playback.addItem(withTitle: board.text("显示 / 隐藏面板", "Show / Hide Panel"), action: #selector(toggleWindow), keyEquivalent: "m").target = self
        NSApp.mainMenu = menu
    }
    @objc func chooseLibrary() {
        let panel = NSOpenPanel(); panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.allowsMultipleSelection = false
        panel.message = board.text("选择包含 BGM 或 SFX 的曲库文件夹", "Choose a library folder containing BGM or SFX")
        panel.beginSheetModal(for: window) { [weak self] response in if response == .OK, let url = panel.url { self?.board.selectLibrary(url) } }
    }
    @objc func showLink() {
        if let existing = linkWindow { existing.makeKeyAndOrderFront(nil); return }
        let alert = NSAlert(); alert.messageText = board.text("导入音频链接", "Import Audio Link")
        alert.informativeText = board.text("支持音乐或视频页面、音频直链。在线播放只保留本次会话；下载后可以离线播放。", "Use a music/video page or a direct audio link. Online streams last for this session; downloads remain available offline.")
        alert.addButton(withTitle: board.text("在线播放", "Play Online")); alert.addButton(withTitle: board.text("下载并播放", "Download & Play")); alert.addButton(withTitle: board.text("取消", "Cancel"))
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 480, height: 26)); field.placeholderString = "https://…"; field.setAccessibilityIdentifier("link-url")
        alert.accessoryView = field
        alert.beginSheetModal(for: window) { [weak self] response in
            if response == .alertFirstButtonReturn { self?.board.importLink(field.stringValue, offline: false) }
            if response == .alertSecondButtonReturn { self?.board.importLink(field.stringValue, offline: true) }
        }
        alert.window.makeFirstResponder(field)
    }
    @objc func toggleWindow() {
        if window.isVisible { window.orderOut(nil) } else { window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true) }
    }
    @objc func pauseMusic() { board.togglePause() }
    @objc func stopAll() { board.stopAll() }
    @objc func about() { NSApp.orderFrontStandardAboutPanel(options: [.applicationName: "MusicBoard", .applicationVersion: "0.3.6 · macOS Preview 1", .credits: NSAttributedString(string: board.text("原生 Mac 测试版 · FullPeople\n无需麦克风、录屏或辅助功能权限。", "Native Mac preview · FullPeople\nNo microphone, screen recording or Accessibility permission required."))]) }
    @objc func checkUpdates() {
        NSWorkspace.shared.open(URL(string: "https://github.com/FullPeople/MusicBoard/releases/tag/v0.3.6-macos.1")!)
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { sender.orderOut(nil); return false }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply { board.stopAll(); board.save(); return .terminateNow }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool { window.makeKeyAndOrderFront(nil); return true }
}

let application = NSApplication.shared
let delegate = MusicBoardApp()
application.delegate = delegate
application.run()
