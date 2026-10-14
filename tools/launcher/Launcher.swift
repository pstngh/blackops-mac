// Black Ops Launcher: a window to set up a local match against bots, then start
// the game (`blackops` next to the app, as tools/make_portable.sh installs it).
//
// The bots are the PC game's managed bots (maps/mp/gametypes/_bot.gsc,
// bot_spawner_Once): scr_bots_managed_allies/axis per team, or scr_bots_managed_all
// for free-for-all, and scr_bot_difficulty. The engine's sv_botsAllowMovement,
// sv_botsPressAttackBtn and sv_botsPressMeleeBtn default to off (the scripts turn
// them on only in developer blocks), so the launcher sets them. Friendly bots are
// on allies and the player joins allies through KB_CMDS (linux_main.cpp).
// Gun damage is kb_damagePercent (every gun but the sniper rifles, everyone's), Never reload
// kb_infiniteClip (the player's magazines never empty). Save logs (off by default): the game's
// output into blackops.log next to the app, and the game's own main/console_mp.log and
// main/games_mp.log (kb_logFiles); off, the game writes no log file.
//
// Build: tools/launcher/build.sh <game dir>
import AppKit
import SwiftUI

struct GameMap: Identifiable, Hashable {
    let id: String
    let name: String
}

// The base game's maps, with their in-game names.
let gameMaps: [GameMap] = [
    GameMap(id: "mp_array", name: "Array"),
    GameMap(id: "mp_cracked", name: "Cracked"),
    GameMap(id: "mp_crisis", name: "Crisis"),
    GameMap(id: "mp_firingrange", name: "Firing Range"),
    GameMap(id: "mp_duga", name: "Grid"),
    GameMap(id: "mp_hanoi", name: "Hanoi"),
    GameMap(id: "mp_cairo", name: "Havana"),
    GameMap(id: "mp_havoc", name: "Jungle"),
    GameMap(id: "mp_cosmodrome", name: "Launch"),
    GameMap(id: "mp_nuked", name: "Nuketown"),
    GameMap(id: "mp_radiation", name: "Radiation"),
    GameMap(id: "mp_mountain", name: "Summit"),
    GameMap(id: "mp_villa", name: "Villa"),
    GameMap(id: "mp_russianbase", name: "WMD"),
]

// The map packs' maps (First Strike, Escalation, Annihilation), listed when the game
// folder has their zone file.
let mapPackMaps: [GameMap] = [
    GameMap(id: "mp_berlinwall2", name: "Berlin Wall"),
    GameMap(id: "mp_gridlock", name: "Convoy"),
    GameMap(id: "mp_discovery", name: "Discovery"),
    GameMap(id: "mp_drivein", name: "Drive-In"),
    GameMap(id: "mp_area51", name: "Hangar 18"),
    GameMap(id: "mp_golfcourse", name: "Hazard"),
    GameMap(id: "mp_hotel", name: "Hotel"),
    GameMap(id: "mp_kowloon", name: "Kowloon"),
    GameMap(id: "mp_silo", name: "Silo"),
    GameMap(id: "mp_stadium", name: "Stadium"),
    GameMap(id: "mp_outskirts", name: "Stockpile"),
    GameMap(id: "mp_zoo", name: "Zoo"),
]

struct GameMode: Identifiable, Hashable {
    let id: String          // g_gametype
    let name: String
    let teams: Bool
    let objective: Bool     // the bot AI doesn't play objectives
}

let gameModes: [GameMode] = [
    GameMode(id: "tdm", name: "Team Deathmatch", teams: true, objective: false),
    GameMode(id: "dm", name: "Free-for-All", teams: false, objective: false),
    GameMode(id: "dom", name: "Domination", teams: true, objective: true),
    GameMode(id: "koth", name: "Headquarters", teams: true, objective: true),
    GameMode(id: "ctf", name: "Capture the Flag", teams: true, objective: true),
    GameMode(id: "sd", name: "Search and Destroy", teams: true, objective: true),
    GameMode(id: "dem", name: "Demolition", teams: true, objective: true),
    GameMode(id: "sab", name: "Sabotage", teams: true, objective: true),
]

// scr_bot_difficulty, with Combat Training's names.
let difficulties: [(id: String, name: String)] = [
    ("easy", "Recruit"), ("normal", "Regular"), ("hard", "Hardened"), ("fu", "Veteran"),
]

// A listen server has 18 slots (sv_maxclients): the host and the demo recorder
// take two.
let maxBots = 16

@main
struct LauncherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        Window("Black Ops", id: "launcher") {
            LauncherView()
        }
        .windowResizability(.contentSize)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

struct LauncherView: View {
    @AppStorage("map") private var map = "mp_nuked"
    @AppStorage("mode") private var mode = "tdm"
    @AppStorage("difficulty") private var difficulty = "normal"
    @AppStorage("enemyBots") private var enemyBots = 4
    @AppStorage("friendlyBots") private var friendlyBots = 3
    @AppStorage("ffaBots") private var ffaBots = 7
    @AppStorage("timeLimit") private var timeLimit = -1    // minutes; -1 the mode's default, 0 none
    @AppStorage("noScoreLimit") private var noScoreLimit = false
    @AppStorage("showArms") private var showArms = true     // cg_drawArms
    @AppStorage("damagePercent") private var damagePercent = 100    // kb_damagePercent
    @AppStorage("neverReload") private var neverReload = true      // kb_infiniteClip
    @AppStorage("saveLogs") private var saveLogs = false           // blackops.log, kb_logFiles
    @State private var error: String?

    private let installedMapPackMaps: [GameMap] = {
        let zone = Bundle.main.bundleURL.deletingLastPathComponent().appendingPathComponent("zone/Common")
        return mapPackMaps.filter {
            FileManager.default.fileExists(atPath: zone.appendingPathComponent("\($0.id).ff").path)
        }
    }()
    private var availableMaps: [GameMap] { gameMaps + installedMapPackMaps }

    private var selectedMode: GameMode { gameModes.first { $0.id == mode } ?? gameModes[0] }

    var body: some View {
        VStack(spacing: 0) {
            Form {
                Section("Match") {
                    Picker("Map", selection: $map) {
                        Text("Random").tag("random")
                        Divider()
                        ForEach(gameMaps) { Text($0.name).tag($0.id) }
                        if !installedMapPackMaps.isEmpty {
                            Divider()
                            ForEach(installedMapPackMaps) { Text($0.name).tag($0.id) }
                        }
                    }
                    Picker("Mode", selection: $mode) {
                        ForEach(gameModes) { Text($0.name).tag($0.id) }
                    }
                    Picker("Time limit", selection: $timeLimit) {
                        Text("Default").tag(-1)
                        ForEach([5, 10, 15, 20, 30], id: \.self) { Text("\($0) minutes").tag($0) }
                        Text("None").tag(0)
                    }
                    Toggle("No score limit", isOn: $noScoreLimit)
                }
                Section {
                    Picker("Difficulty", selection: $difficulty) {
                        ForEach(difficulties, id: \.id) { Text($0.name).tag($0.id) }
                    }
                    .pickerStyle(.segmented)
                    if selectedMode.teams {
                        Stepper("Enemy bots: \(enemyBots)", value: $enemyBots, in: 0...min(8, maxBots - friendlyBots))
                        Stepper("Friendly bots: \(friendlyBots)", value: $friendlyBots, in: 0...min(8, maxBots - enemyBots))
                    } else {
                        Stepper("Bots: \(ffaBots)", value: $ffaBots, in: 1...maxBots)
                    }
                } header: {
                    Text("Bots")
                } footer: {
                    if selectedMode.objective {
                        Text("Bots don't go for objectives in this mode; they just fight.")
                            .foregroundStyle(.secondary)
                    }
                }
                Section {
                    Stepper("Gun damage: \(damagePercent)%", value: $damagePercent, in: 5...100, step: 5)
                    Toggle("Never reload", isOn: $neverReload)
                } header: {
                    Text("Guns")
                } footer: {
                    Text("Gun damage lowers every gun but the snipers, yours and the bots'; 100% is normal. Never reload: your magazine never empties.")
                        .foregroundStyle(.secondary)
                }
                Section("View") {
                    Toggle("Show arms", isOn: $showArms)
                }
                Section {
                    Toggle("Save logs", isOn: $saveLogs)
                } footer: {
                    Text("The game's messages, in blackops.log in the game folder. Off: no log files, and a crash leaves no report.")
                        .foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)

            HStack {
                Button("Main Menu") { start(match: false) }
                Spacer()
                Button("Play") { start(match: true) }
                    .keyboardShortcut(.defaultAction)
                    .controlSize(.large)
            }
            .padding([.horizontal, .bottom], 20)
            .padding(.top, 4)
        }
        .frame(width: 440, height: 840)
        .alert("Can't start the game", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK") { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear {
            // A map pack's map that is no longer installed.
            if map != "random" && !availableMaps.contains(where: { $0.id == map }) {
                map = "mp_nuked"
            }
            // KB_LAUNCHER_TEST=<dir>: save the window as <dir>/launcher.png, then Play.
            if let dir = ProcessInfo.processInfo.environment["KB_LAUNCHER_TEST"] {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                    if let window = NSApp.windows.first, let image = windowImage(window) {
                        let rep = NSBitmapImageRep(cgImage: image)
                        try? rep.representation(using: .png, properties: [:])?
                            .write(to: URL(fileURLWithPath: dir).appendingPathComponent("launcher.png"))
                    }
                    start(match: true)
                }
            }
        }
    }

    // The window as the window server shows it (CGWindowListCreateImage, which the
    // SDK no longer declares; an app may capture its own windows).
    private func windowImage(_ window: NSWindow) -> CGImage? {
        typealias CreateImage = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage") else { return nil }
        let create = unsafeBitCast(symbol, to: CreateImage.self)
        // kCGWindowListOptionIncludingWindow, kCGWindowImageBoundsIgnoreFraming
        return create(.null, 1 << 3, UInt32(window.windowNumber), 1 << 0)?.takeRetainedValue()
    }

    private func matchArguments() -> [String] {
        let gametype = selectedMode.id
        var args: [String] = []
        func set(_ dvar: String, _ value: String) { args += ["+set", dvar, value] }
        set("sv_botsAllowMovement", "1")
        set("sv_botsPressAttackBtn", "1")
        set("sv_botsPressMeleeBtn", "1")
        set("scr_bots_managed_spawn", "1")
        set("scr_bot_difficulty", difficulty)
        if selectedMode.teams {
            set("scr_bots_managed_all", "0")
            set("scr_bots_managed_allies", String(friendlyBots))
            set("scr_bots_managed_axis", String(enemyBots))
        } else {
            set("scr_bots_managed_all", String(ffaBots))
        }
        set("g_gametype", gametype)
        if timeLimit >= 0 { set("scr_\(gametype)_timelimit", String(timeLimit)) }
        if noScoreLimit { set("scr_\(gametype)_scorelimit", "0") }
        let mapName = map == "random" ? (availableMaps.randomElement()?.id ?? "mp_nuked") : map
        args += ["+map", mapName]
        return args
    }

    private func start(match: Bool) {
        let folder = Bundle.main.bundleURL.deletingLastPathComponent()
        let game = folder.appendingPathComponent("blackops")
        guard FileManager.default.isExecutableFile(atPath: game.path) else {
            error = "There is no blackops next to this app, in \(folder.path)."
            return
        }
        let process = Process()
        process.executableURL = game
        process.currentDirectoryURL = folder
        var environment = ProcessInfo.processInfo.environment
        // First-person arms (cg_drawArms 0: the weapon alone), gun damage and never reloading; the game saves them.
        process.arguments = ["+set", "cg_drawArms", showArms ? "1" : "0", "+set", "kb_damagePercent", String(min(max(damagePercent, 5), 100)),
                             "+set", "kb_infiniteClip", neverReload ? "1" : "0", "+set", "kb_logFiles", saveLogs ? "1" : "0"]
        if match {
            process.arguments! += matchArguments()
            // Join the team the friendly bots are on (allies); free-for-all has one.
            let team = selectedMode.teams ? "allies" : "autoassign"
            environment["KB_CMDS"] = "3:openscriptmenu team_marinesopfor \(team)"
        }
        process.environment = environment
        // The game's console output, for a crash report; without Save logs, nowhere.
        if saveLogs {
            let logURL = folder.appendingPathComponent("blackops.log")
            FileManager.default.createFile(atPath: logURL.path, contents: nil)
            if let log = try? FileHandle(forWritingTo: logURL) {
                process.standardOutput = log
                process.standardError = log
            }
        } else {
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
        }
        do {
            try process.run()
        } catch {
            self.error = error.localizedDescription
            return
        }
        NSApp.terminate(nil)
    }
}
