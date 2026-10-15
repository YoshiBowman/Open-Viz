// Openviz as a Mac app: a window around the page that launcher.py serves.
// It starts launcher.py, shows its page, and stops it again when the window is closed.
// Built by launcher/build_mac_app.sh, which writes down the project folder it was built from; or,
// for the app in the .dmg on GitHub, by publish/publish.py, which puts the whole project inside
// the app (Resources/Openviz) for it to set up in ~/Openviz.
import Cocoa
import WebKit

final class AppDelegate: NSObject, NSApplicationDelegate, WKUIDelegate, WKNavigationDelegate {
    private var window: NSWindow!
    private var web: WKWebView!
    private var server: Process?
    private var output = Data()
    private var loaded = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMenu()
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1040, height: 780),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "Openviz"
        window.minSize = NSSize(width: 560, height: 480)
        window.backgroundColor = NSColor(red: 0.07, green: 0.075, blue: 0.086, alpha: 1)
        window.center()
        window.setFrameAutosaveName("Launcher")

        web = WKWebView(frame: window.contentView!.bounds, configuration: WKWebViewConfiguration())
        web.autoresizingMask = [.width, .height]
        web.uiDelegate = self
        web.navigationDelegate = self
        web.setValue(false, forKey: "drawsBackground")    // no white flash before the page arrives
        window.contentView!.addSubview(web)
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        startServer()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ notification: Notification) {
        server?.terminationHandler = nil
        server?.terminate()
    }

    // MARK: the server

    // The project folder: the one build_mac_app.sh wrote down, or the one this app carries, put in place.
    private func projectFolder() -> String? {
        if let pathFile = Bundle.main.url(forResource: "repo-path", withExtension: "txt"),
           let repo = try? String(contentsOf: pathFile, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines) {
            return repo
        }
        guard let carried = Bundle.main.resourceURL?.appendingPathComponent("Openviz"),
              FileManager.default.fileExists(atPath: carried.appendingPathComponent("launcher/launcher.py").path) else {
            fail("This copy of the app is incomplete. Download it again, or build it with launcher/build_mac_app.sh.")
            return nil
        }
        return putInPlace(carried)
    }

    // The project this app carries goes to ~/Openviz (or OPENVIZ_FOLDER), and again whenever the app
    // is a different version from what is there. What is copied over is only the app's own files: the
    // library (shows, fixtures, settings) and what was built on this Mac stay.
    private func putInPlace(_ carried: URL) -> String? {
        let files = FileManager.default
        let given = ProcessInfo.processInfo.environment["OPENVIZ_FOLDER"]
        let target = URL(fileURLWithPath: given ?? files.homeDirectoryForCurrentUser.appendingPathComponent("Openviz").path)
        if files.fileExists(atPath: target.appendingPathComponent(".git").path) {
            return target.path    // a copy made with Git is brought up to date with git pull, not from here
        }
        let isOpenviz = files.fileExists(atPath: target.appendingPathComponent("launcher/launcher.py").path)
        let others = ((try? files.contentsOfDirectory(atPath: target.path)) ?? []).filter { !$0.hasPrefix(".") }
        if !isOpenviz && !others.isEmpty {
            fail("Openviz keeps its shows and its own files in a folder called Openviz in your home folder, and \(target.path) already holds other things. Move or rename that folder, then open Openviz again.")
            return nil
        }
        let version = { (folder: URL) in (try? String(contentsOf: folder.appendingPathComponent("openviz-version.txt"), encoding: .utf8)) ?? "" }
        if !isOpenviz || version(carried) != version(target) {
            // --noqtn: a downloaded app is in quarantine, and the programs Openviz runs must not inherit it.
            let copy = Process()
            copy.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
            copy.arguments = ["--noqtn", carried.path, target.path]
            do {
                try copy.run()
                copy.waitUntilExit()
            } catch {
                copy.terminate()
            }
            if copy.terminationStatus != 0 {
                fail("Openviz could not be put in \(target.path). Is there room on the disk, and may it be written to?")
                return nil
            }
        }
        return target.path
    }

    private func startServer() {
        // Openviz runs on the Python that comes with Xcode's tools, which Unreal Engine needs too.
        let tools = Process()
        tools.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
        tools.arguments = ["-p"]
        tools.standardOutput = FileHandle.nullDevice
        tools.standardError = FileHandle.nullDevice
        if (try? tools.run()) == nil || { tools.waitUntilExit(); return tools.terminationStatus != 0 }() {
            return fail("Openviz needs Xcode, as Unreal Engine does on a Mac. Install Xcode from the App Store or developer.apple.com, open it once, then open Openviz again.")
        }
        guard let repo = projectFolder() else { return }
        let script = repo + "/launcher/launcher.py"
        let logs = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0].appendingPathComponent("Logs")
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        // OPENVIZ_LOG and OPENVIZ_LAUNCHER_ARGS are for trying a copy of the app beside the one in use
        // (publish/publish.py's checks): another log, and launcher options such as --no-relay.
        let given = ProcessInfo.processInfo.environment
        let logURL = given["OPENVIZ_LOG"].map { URL(fileURLWithPath: $0) } ?? logs.appendingPathComponent("OpenvizLauncher.log")
        FileManager.default.createFile(atPath: logURL.path, contents: nil)

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = ["python3", script, "--no-open", "--verbose", "--parent", String(ProcessInfo.processInfo.processIdentifier)]
            + (given["OPENVIZ_LAUNCHER_ARGS"] ?? "").split(separator: " ").map(String.init)
        var environment = ProcessInfo.processInfo.environment
        environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"
        process.environment = environment
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = try? FileHandle(forWritingTo: logURL)

        // launcher.py prints "URL http://127.0.0.1:<port>/#<token>" once it is listening.
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            DispatchQueue.main.async {
                guard let self = self, !self.loaded else { return }
                self.output.append(data)
                guard let text = String(data: self.output, encoding: .utf8), let line = text.split(separator: "\n").first(where: { $0.hasPrefix("URL ") }),
                      text.contains("\n"), let url = URL(string: String(line.dropFirst(4))) else { return }
                self.loaded = true
                handle.readabilityHandler = nil
                self.web.load(URLRequest(url: url))
            }
        }
        process.terminationHandler = { [weak self] _ in
            DispatchQueue.main.async {
                guard let self = self, !self.loaded else { return }
                self.fail("The launcher could not start. It expects the project in:\n\(repo)\n\nIf the project folder was moved, build the app again with launcher/build_mac_app.sh. If macOS asked whether this app may access that folder, it needs to be allowed. Details are in \(logURL.path).")
            }
        }
        do {
            try process.run()
            server = process
        } catch {
            fail("python3 could not be started: \(error.localizedDescription)")
        }
    }

    private func fail(_ message: String) {
        let alert = NSAlert()
        alert.messageText = "Openviz could not start"
        alert.informativeText = message
        alert.runModal()
        NSApp.terminate(nil)
    }

    // MARK: the page

    // The "choose a file" button of the page.
    func webView(_ webView: WKWebView, runOpenPanelWith parameters: WKOpenPanelParameters, initiatedByFrame frame: WKFrameInfo,
                 completionHandler: @escaping ([URL]?) -> Void) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = parameters.allowsMultipleSelection
        panel.canChooseDirectories = false
        panel.beginSheetModal(for: window) { result in completionHandler(result == .OK ? panel.urls : nil) }
    }

    // Only the launcher's own page is shown here; any other link opens in the browser.
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        guard let url = navigationAction.request.url else { return decisionHandler(.cancel) }
        if url.host == "127.0.0.1" || url.scheme == "about" {
            decisionHandler(.allow)
        } else {
            NSWorkspace.shared.open(url)
            decisionHandler(.cancel)
        }
    }

    // MARK: menu

    private func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem()
        appItem.submenu = NSMenu()
        appItem.submenu!.addItem(withTitle: "Hide Openviz", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appItem.submenu!.addItem(.separator())
        appItem.submenu!.addItem(withTitle: "Quit Openviz", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        main.addItem(appItem)

        // Without an Edit menu, Command-V would not paste into the search or login fields.
        let editItem = NSMenuItem()
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = edit
        main.addItem(editItem)

        let windowItem = NSMenuItem()
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowMenu.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        windowItem.submenu = windowMenu
        main.addItem(windowItem)
        NSApp.mainMenu = main
        NSApp.windowsMenu = windowMenu
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
