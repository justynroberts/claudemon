import SwiftUI
import AppKit

// ─────────────────────────────────────────────────────────── palette
extension Color {
    init(hex: UInt32) {
        self.init(.sRGB,
                  red:   Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8)  & 0xFF) / 255,
                  blue:  Double(hex & 0xFF) / 255)
    }
    static let cmBg      = Color(hex: 0x14110F)
    static let cmCard    = Color(hex: 0x1F1B17)
    static let cmEdge    = Color(hex: 0x2A2520)
    static let cmText    = Color(hex: 0xF5F1EA)
    static let cmMute    = Color(hex: 0x8A857E)
    static let cmAccent  = Color(hex: 0xCC785C)
    static let cmOk      = Color(hex: 0x6FBF73)
    static let cmWarn    = Color(hex: 0xE6B566)
    static let cmErr     = Color(hex: 0xE5604C)
}

// ─────────────────────────────────────────────────────────── paths
let kConfigDir  = ("~/.config/claudemon" as NSString).expandingTildeInPath
let kConfigPath = kConfigDir + "/tailer.toml"
let kPlistPath  = ("~/Library/LaunchAgents/com.claudemon.tailer.plist" as NSString).expandingTildeInPath
let kLogPath    = ("~/Library/Logs/claudemon-tailer.log" as NSString).expandingTildeInPath
let kAgentLabel = "com.claudemon.tailer"

// ─────────────────────────────────────────────────────────── model
@MainActor
final class AppModel: ObservableObject {
    @Published var deviceURL     = "http://claudemon.local"
    @Published var sharedSecret  = ""
    @Published var interval      = "10"
    @Published var sessionBudget = "200000000"
    @Published var weekBudget    = "2000000000"
    @Published var monthBudget   = "6000000000"

    @Published var running       = false
    @Published var deviceOnline  = false
    @Published var deviceEnvs    = 0
    @Published var deviceIP      = ""
    @Published var lastMessage   = ""
    @Published var backfilling   = false

    private var timer: Timer?

    init() {
        loadConfig()
        running = isAgentLoaded()
        Task { await refreshStatus() }
        timer = Timer.scheduledTimer(withTimeInterval: 8, repeats: true) { [weak self] _ in
            Task { await self?.refreshStatus() }
        }
    }

    // ── config file (flat TOML; preserves other lines like [groups]) ──
    func loadConfig() {
        guard let text = try? String(contentsOfFile: kConfigPath, encoding: .utf8) else { return }
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            guard !line.hasPrefix("#"), let eq = line.firstIndex(of: "=") else { continue }
            let key = line[..<eq].trimmingCharacters(in: .whitespaces)
            var val = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            val = val.trimmingCharacters(in: CharacterSet(charactersIn: "\"'"))
            switch key {
            case "device_url":     deviceURL = val
            case "shared_secret":  sharedSecret = val
            case "interval":       interval = val
            case "session_budget": sessionBudget = val
            case "week_budget":    weekBudget = val
            case "month_budget":   monthBudget = val
            default: break
            }
        }
    }

    func saveConfig() {
        try? FileManager.default.createDirectory(atPath: kConfigDir,
                                                 withIntermediateDirectories: true)
        let updates: [(String, String)] = [
            ("device_url",     "\"\(deviceURL)\""),
            ("shared_secret",  "\"\(sharedSecret)\""),
            ("interval",       interval),
            ("session_budget", sessionBudget),
            ("week_budget",    weekBudget),
            ("month_budget",   monthBudget),
        ]
        var lines = (try? String(contentsOfFile: kConfigPath, encoding: .utf8))?
            .components(separatedBy: "\n") ?? []
        for (key, value) in updates {
            let newLine = "\(key.padding(toLength: 14, withPad: " ", startingAt: 0)) = \(value)"
            if let i = lines.firstIndex(where: {
                $0.trimmingCharacters(in: .whitespaces).hasPrefix(key + " ") ||
                $0.trimmingCharacters(in: .whitespaces).hasPrefix(key + "=")
            }) {
                lines[i] = newLine
            } else {
                lines.append(newLine)
            }
        }
        try? (lines.joined(separator: "\n")).write(toFile: kConfigPath,
                                                    atomically: true, encoding: .utf8)
        // Tighten perms on the file that holds the secret.
        try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: kConfigPath)
        lastMessage = "Saved."
        if running { restart() }
    }

    // ── launchd control ──
    private func tailerScriptPath() -> String {
        Bundle.main.path(forResource: "claudemon-tailer", ofType: "py") ?? ""
    }

    private func writePlist() {
        let plist = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0"><dict>
          <key>Label</key><string>\(kAgentLabel)</string>
          <key>ProgramArguments</key><array>
            <string>/usr/bin/python3</string>
            <string>\(tailerScriptPath())</string>
          </array>
          <key>RunAtLoad</key><true/>
          <key>KeepAlive</key><true/>
          <key>ThrottleInterval</key><integer>10</integer>
          <key>StandardOutPath</key><string>\(kLogPath)</string>
          <key>StandardErrorPath</key><string>\(kLogPath)</string>
        </dict></plist>
        """
        let dir = (kPlistPath as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        try? plist.write(toFile: kPlistPath, atomically: true, encoding: .utf8)
    }

    @discardableResult
    private func launchctl(_ args: [String]) -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        p.arguments = args
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
        try? p.run(); p.waitUntilExit()
        return String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
    }

    func isAgentLoaded() -> Bool {
        !launchctl(["list"]).split(separator: "\n").filter { $0.contains(kAgentLabel) }.isEmpty
    }

    func start() {
        guard !tailerScriptPath().isEmpty else { lastMessage = "Bundled tailer missing."; return }
        saveConfigQuiet()
        writePlist()
        launchctl(["unload", kPlistPath])   // in case a stale one is loaded
        _ = launchctl(["load", "-w", kPlistPath])
        running = isAgentLoaded()
        lastMessage = running ? "Tailer started." : "Failed to start."
    }

    func stop() {
        _ = launchctl(["unload", "-w", kPlistPath])
        running = isAgentLoaded()
        lastMessage = "Tailer stopped."
    }

    func restart() { stop(); start() }

    // ── one-shot backfill of historical usage ──
    func backfill() {
        let script = tailerScriptPath()
        guard !script.isEmpty else { lastMessage = "Bundled tailer missing."; return }
        saveConfigQuiet()
        backfilling = true
        lastMessage = "Backfilling history…"
        Task { [weak self] in
            let ok = await AppModel.runBackfill(script: script)
            guard let self else { return }
            self.backfilling = false
            self.lastMessage = ok ? "Backfill complete." : "Backfill failed — see Logs."
            await self.refreshStatus()
        }
    }

    nonisolated static func runBackfill(script: String) async -> Bool {
        await withCheckedContinuation { cont in
            DispatchQueue.global().async {
                let p = Process()
                p.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
                p.arguments = [script, "--backfill"]
                do { try p.run(); p.waitUntilExit(); cont.resume(returning: p.terminationStatus == 0) }
                catch { cont.resume(returning: false) }
            }
        }
    }

    private func saveConfigQuiet() {
        let wasRunning = running; running = false
        saveConfig(); running = wasRunning
    }

    // ── device status ──
    func refreshStatus() async {
        guard let url = URL(string: deviceURL.trimmingCharacters(in: .whitespaces) + "/status") else { return }
        var req = URLRequest(url: url); req.timeoutInterval = 5
        do {
            let (data, _) = try await URLSession.shared.data(for: req)
            if let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                deviceOnline = (obj["state"] as? String) == "online"
                deviceEnvs   = (obj["envs"] as? Int) ?? 0
                deviceIP     = (obj["ip"] as? String) ?? ""
                return
            }
        } catch { }
        deviceOnline = false
    }

    var menuSymbol: String {
        if running && deviceOnline { return "chart.bar.fill" }
        if running { return "chart.bar.xaxis" }
        return "chart.bar"
    }
}

// ─────────────────────────────────────────────────────────── views
struct StatusPill: View {
    let online: Bool
    let running: Bool
    var body: some View {
        let (label, color): (String, Color) =
            !running ? ("stopped", .cmMute)
            : online ? ("online", .cmOk)
            : ("unreachable", .cmWarn)
        HStack(spacing: 6) {
            Circle().fill(color).frame(width: 7, height: 7)
            Text(label).font(.system(size: 11, weight: .semibold)).foregroundColor(color)
        }
        .padding(.horizontal, 9).padding(.vertical, 4)
        .background(color.opacity(0.12)).clipShape(Capsule())
    }
}

struct Field: View {
    let label: String
    @Binding var text: String
    var secure = false
    var mono = false
    @State private var reveal = false
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label.uppercased())
                .font(.system(size: 10, weight: .semibold)).tracking(0.5)
                .foregroundColor(.cmMute)
            HStack(spacing: 6) {
                Group {
                    if secure && !reveal {
                        SecureField("", text: $text)
                    } else {
                        TextField("", text: $text)
                    }
                }
                .textFieldStyle(.plain)
                .font(.system(size: 13, design: mono ? .monospaced : .default))
                .foregroundColor(.cmText)
                if secure {
                    Button { reveal.toggle() } label: {
                        Image(systemName: reveal ? "eye.slash" : "eye").font(.system(size: 11))
                    }
                    .buttonStyle(.plain).foregroundColor(.cmMute)
                }
            }
            .padding(.horizontal, 10).padding(.vertical, 8)
            .background(Color.cmBg)
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.cmEdge, lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: 8))
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var m: AppModel
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // header
            HStack {
                ZStack {
                    RoundedRectangle(cornerRadius: 5).stroke(Color.cmAccent, lineWidth: 2)
                        .frame(width: 22, height: 22)
                    Circle().fill(Color.cmAccent).frame(width: 6, height: 6)
                }
                Text("claudemon").font(.system(size: 17, weight: .bold)).foregroundColor(.cmText)
                Spacer()
                StatusPill(online: m.deviceOnline, running: m.running)
            }
            .padding(16)

            Divider().overlay(Color.cmEdge)

            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    Text("CONNECTION").sectionHeader()
                    Field(label: "Device URL", text: $m.deviceURL)
                    Field(label: "Shared secret", text: $m.sharedSecret, secure: true, mono: true)

                    Text("BUDGETS  ·  TOKENS").sectionHeader().padding(.top, 4)
                    HStack(spacing: 10) {
                        Field(label: "Session 5h", text: $m.sessionBudget, mono: true)
                        Field(label: "Week 7d", text: $m.weekBudget, mono: true)
                    }
                    HStack(spacing: 10) {
                        Field(label: "Month 30d", text: $m.monthBudget, mono: true)
                        Field(label: "Interval s", text: $m.interval, mono: true)
                    }
                }
                .padding(16)
            }

            Divider().overlay(Color.cmEdge)

            // actions
            VStack(spacing: 10) {
                HStack(spacing: 10) {
                    Button(action: { m.saveConfig() }) {
                        Text("Save").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(CMButton(filled: false))

                    Button(action: { m.running ? m.stop() : m.start() }) {
                        Text(m.running ? "Stop tailer" : "Start tailer").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(CMButton(filled: true, danger: m.running))
                }

                Button(action: { m.backfill() }) {
                    HStack(spacing: 6) {
                        if m.backfilling { ProgressView().controlSize(.small).scaleEffect(0.7) }
                        Text(m.backfilling ? "Backfilling…" : "Backfill history now")
                    }.frame(maxWidth: .infinity)
                }
                .buttonStyle(CMButton(filled: false))
                .disabled(m.backfilling)

                HStack {
                    if !m.deviceIP.isEmpty && m.deviceOnline {
                        Text("\(m.deviceIP)  ·  \(m.deviceEnvs) envs")
                            .font(.system(size: 11)).foregroundColor(.cmMute)
                    } else if !m.lastMessage.isEmpty {
                        Text(m.lastMessage).font(.system(size: 11)).foregroundColor(.cmMute)
                    }
                    Spacer()
                    Button("Logs") { NSWorkspace.shared.open(URL(fileURLWithPath: kLogPath)) }
                        .buttonStyle(.plain).font(.system(size: 11)).foregroundColor(.cmAccent)
                    Text("·").foregroundColor(.cmEdge)
                    Button("Quit") { NSApp.terminate(nil) }
                        .buttonStyle(.plain).font(.system(size: 11)).foregroundColor(.cmMute)
                }
            }
            .padding(16)
        }
        .frame(width: 380)
        .background(Color.cmCard)
        .task { await m.refreshStatus() }
    }
}

extension Text {
    func sectionHeader() -> some View {
        self.font(.system(size: 10, weight: .bold)).tracking(1).foregroundColor(.cmMute)
    }
}

struct CMButton: ButtonStyle {
    var filled: Bool
    var danger: Bool = false
    func makeBody(configuration: Configuration) -> some View {
        let bg = filled ? (danger ? Color.cmErr : Color.cmAccent) : Color.cmEdge
        let fg = filled ? Color.cmBg : Color.cmText
        configuration.label
            .font(.system(size: 13, weight: .semibold))
            .padding(.vertical, 9)
            .background(bg.opacity(configuration.isPressed ? 0.75 : 1))
            .foregroundColor(fg)
            .clipShape(RoundedRectangle(cornerRadius: 9))
    }
}

// ─────────────────────────────────────────────────────────── app
@main
struct ClaudemonCompanionApp: App {
    @StateObject private var model = AppModel()
    var body: some Scene {
        MenuBarExtra {
            ContentView().environmentObject(model)
                .preferredColorScheme(.dark)
        } label: {
            Image(systemName: model.menuSymbol)
        }
        .menuBarExtraStyle(.window)
    }
}
