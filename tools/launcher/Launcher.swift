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
// The map is chosen from a mosaic of the maps' pictures (a window of its own), the ones the game's
// own map menu shows; build.sh copies them from the game folder into the app (game art, not in git).
//
// Build: tools/launcher/build.sh <game dir>
import AppKit
import SwiftUI

struct GameMap: Identifiable, Hashable {
    let id: String
    let name: String
    let picture: String     // the image of the game's map menu (mp/mapstable.csv's material), in main/*.iwd
}

// The base game's maps, with their in-game names.
let gameMaps: [GameMap] = [
    GameMap(id: "mp_array", name: "Array", picture: "menu_mp_map_select_array_final"),
    GameMap(id: "mp_cracked", name: "Cracked", picture: "menu_mp_map_select_cracked_final"),
    GameMap(id: "mp_crisis", name: "Crisis", picture: "menu_mp_map_select_crisis_final"),
    GameMap(id: "mp_firingrange", name: "Firing Range", picture: "menu_mp_map_select_firing_range_final"),
    GameMap(id: "mp_duga", name: "Grid", picture: "menu_mp_map_select_duga_final"),
    GameMap(id: "mp_hanoi", name: "Hanoi", picture: "menu_mp_map_select_hanoi_final"),
    GameMap(id: "mp_cairo", name: "Havana", picture: "menu_mp_map_select_havana_final"),
    GameMap(id: "mp_havoc", name: "Jungle", picture: "menu_mp_map_select_havoc_final"),
    GameMap(id: "mp_cosmodrome", name: "Launch", picture: "menu_mp_map_select_cosmo_final"),
    GameMap(id: "mp_nuked", name: "Nuketown", picture: "menu_mp_map_select_nuked_final"),
    GameMap(id: "mp_radiation", name: "Radiation", picture: "menu_mp_map_select_radiation_final"),
    GameMap(id: "mp_mountain", name: "Summit", picture: "menu_mp_map_select_mount_final"),
    GameMap(id: "mp_villa", name: "Villa", picture: "menu_mp_map_select_villa_final"),
    GameMap(id: "mp_russianbase", name: "WMD", picture: "menu_mp_map_select_russian_base_final"),
]

// The map packs' maps (First Strike, Escalation, Annihilation), listed when the game
// folder has their zone file. Hotel's picture is the map pack's (patch_mp names the cut
// base-game map's, a daytime pool).
let mapPackMaps: [GameMap] = [
    GameMap(id: "mp_berlinwall2", name: "Berlin Wall", picture: "menu_mp_map_select_berwall2_final"),
    GameMap(id: "mp_gridlock", name: "Convoy", picture: "menu_mp_map_select_gridlock_final"),
    GameMap(id: "mp_discovery", name: "Discovery", picture: "menu_mp_map_select_discovery2_final"),
    GameMap(id: "mp_drivein", name: "Drive-In", picture: "menu_mp_map_select_drivein_final"),
    GameMap(id: "mp_area51", name: "Hangar 18", picture: "menu_mp_map_select_area51_final"),
    GameMap(id: "mp_golfcourse", name: "Hazard", picture: "menu_mp_map_select_golfcourse_final"),
    GameMap(id: "mp_hotel", name: "Hotel", picture: "menu_mp_map_select_hot2_final"),
    GameMap(id: "mp_kowloon", name: "Kowloon", picture: "menu_mp_map_select_kow_final"),
    GameMap(id: "mp_silo", name: "Silo", picture: "menu_mp_map_select_silo_final"),
    GameMap(id: "mp_stadium", name: "Stadium", picture: "menu_mp_map_select_stad_final"),
    GameMap(id: "mp_outskirts", name: "Stockpile", picture: "menu_mp_map_select_outskirts_final"),
    GameMap(id: "mp_zoo", name: "Zoo", picture: "menu_mp_map_select_zoo_final"),
]

// Random's tile ("Location classified").
let randomPicture = "menu_mp_map_select_random_big"

// The pictures by map id (and "random"): Contents/Resources/maps/<id>.png, which build.sh makes.
let mapPictures: [String: NSImage] = {
    var pictures: [String: NSImage] = [:]
    for url in Bundle.main.urls(forResourcesWithExtension: "png", subdirectory: "maps") ?? [] {
        pictures[url.deletingPathExtension().lastPathComponent] = NSImage(contentsOf: url)
    }
    return pictures
}()

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

    init() {
        // `BlackOpsLauncher --map-pictures`: each map's id and picture, for build.sh to copy the pictures in.
        if CommandLine.arguments.dropFirst().first == "--map-pictures" {
            for map in gameMaps + mapPackMaps { print(map.id, map.picture) }
            print("random", randomPicture)
            exit(0)
        }
    }

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

    // The base game's maps and the installed map packs', by name.
    private let availableMaps: [GameMap] = {
        let zone = Bundle.main.bundleURL.deletingLastPathComponent().appendingPathComponent("zone/Common")
        let installedMapPackMaps = mapPackMaps.filter {
            FileManager.default.fileExists(atPath: zone.appendingPathComponent("\($0.id).ff").path)
        }
        return (gameMaps + installedMapPackMaps).sorted { $0.name < $1.name }
    }()

    private var selectedMode: GameMode { gameModes.first { $0.id == mode } ?? gameModes[0] }

    var body: some View {
        VStack(spacing: 0) {
            Form {
                Section("Match") {
                    // The whole row opens the map mosaic.
                    Button { showMapPicker() } label: {
                        HStack(spacing: 8) {
                            Text("Map")
                            Spacer()
                            Text(availableMaps.first { $0.id == map }?.name ?? "Random")
                            Image(systemName: "chevron.up.chevron.down")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
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
            // KB_LAUNCHER_TEST=<dir>: save the window as <dir>/launcher.png, then Play. With
            // KB_LAUNCHER_TEST_PICK=<x>,<y> it first opens the map mosaic, saves it as <dir>/maps.png
            // and clicks it at that pixel of maps.png.
            let environment = ProcessInfo.processInfo.environment
            if let dir = environment["KB_LAUNCHER_TEST"] {
                let pick = environment["KB_LAUNCHER_TEST_PICK"]?.split(separator: ",").compactMap { Double($0) }
                let launcherWindow = { NSApp.windows.first { $0.isVisible && !($0 is MapPickerWindow) } }
                func save(_ window: NSWindow?, _ name: String) {
                    if let window, let image = windowImage(window) {
                        let rep = NSBitmapImageRep(cgImage: image)
                        try? rep.representation(using: .png, properties: [:])?
                            .write(to: URL(fileURLWithPath: dir).appendingPathComponent(name))
                    }
                }
                if pick != nil {
                    NSApp.activate(ignoringOtherApps: true)
                    DispatchQueue.main.async { showMapPicker() }
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                    if let pick, pick.count == 2, let picker = MapPickerWindow.current {
                        save(picker, "maps.png")
                        click(picker, x: pick[0], y: pick[1])
                    }
                    DispatchQueue.main.asyncAfter(deadline: .now() + (pick == nil ? 0 : 1)) {
                        save(launcherWindow(), "launcher.png")
                        start(match: true)
                    }
                }
            }
        }
    }

    // The map mosaic, in a window of its own (wider than the launcher, for bigger pictures),
    // its top on the launcher's top, centred on it and kept on the screen.
    private func showMapPicker() {
        if let open = MapPickerWindow.current {
            open.makeKeyAndOrderFront(nil)
            return
        }
        let launcher = NSApp.windows.first { $0.isVisible && !($0 is MapPickerWindow) }
        let screen = (launcher?.screen ?? NSScreen.main)?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1280, height: 800)
        // Up to 7 columns (4 rows for all 27 tiles), fewer on a narrow screen.
        let columns = max(3, min(7, Int((screen.width - 40) / (MapTile.size + 10))))
        let window = MapPickerWindow(contentRect: .zero, styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Choose a Map"
        window.isReleasedWhenClosed = false
        let host = NSHostingController(rootView: MapPickerView(selection: map, maps: availableMaps, columns: columns) { [weak window] id in
            map = id
            window?.close()
        })
        window.contentViewController = host
        window.setContentSize(host.view.fittingSize)
        let size = window.frame.size
        let anchor = launcher?.frame ?? screen
        window.setFrameOrigin(NSPoint(x: min(max(anchor.midX - size.width / 2, screen.minX), screen.maxX - size.width),
                                      y: min(max(anchor.maxY - size.height, screen.minY), screen.maxY - size.height)))
        MapPickerWindow.current = window
        window.makeKeyAndOrderFront(nil)
    }

    // A left click at a pixel of the window's picture (as windowImage saves it: top left origin), for KB_LAUNCHER_TEST_PICK.
    private func click(_ window: NSWindow, x: Double, y: Double) {
        let scale = window.backingScaleFactor
        let location = NSPoint(x: x / scale, y: window.frame.height - y / scale)
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            if let event = NSEvent.mouseEvent(with: type, location: location, modifierFlags: [],
                                              timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber,
                                              context: nil, eventNumber: 0, clickCount: 1, pressure: 1) {
                window.sendEvent(event)
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

// A map's picture filling its frame; without one (an app built without the game's pictures) a
// gray tile with a symbol.
struct MapPicture: View {
    let id: String

    var body: some View {
        if let picture = mapPictures[id] {
            Image(nsImage: picture)
                .resizable()
                .interpolation(.high)
                .scaledToFill()
                .background(Color.black)
        } else {
            ZStack {
                Color.gray.opacity(0.35)
                Image(systemName: id == "random" ? "shuffle" : "map")
                    .font(.title2)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

// The map mosaic's window; Escape closes it. `current` keeps the open one.
final class MapPickerWindow: NSWindow {
    static var current: MapPickerWindow?

    override func cancelOperation(_ sender: Any?) { close() }

    override func close() {
        super.close()
        MapPickerWindow.current = nil
    }
}

// The map mosaic: a tile per map, Random first, then every map by name; a click chooses it.
struct MapPickerView: View {
    let selection: String
    let maps: [GameMap]
    let columns: Int
    let choose: (String) -> Void

    var body: some View {
        LazyVGrid(columns: Array(repeating: GridItem(.fixed(MapTile.size), spacing: 10), count: columns), spacing: 10) {
            tile(id: "random", name: "Random")
            ForEach(maps) { tile(id: $0.id, name: $0.name) }
        }
        .padding(20)
        .fixedSize()
    }

    private func tile(id: String, name: String) -> some View {
        MapTile(id: id, name: name, selected: selection == id) { choose(id) }
    }
}

struct MapTile: View {
    static let size: CGFloat = 150     // points: 300 pixels on a Retina screen, from the 240-pixel pictures

    let id: String
    let name: String
    let selected: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            ZStack(alignment: .bottomLeading) {
                MapPicture(id: id)
                    .frame(width: MapTile.size, height: MapTile.size)
                LinearGradient(colors: [.clear, .black.opacity(0.8)], startPoint: .center, endPoint: .bottom)
                Text(name)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(.white)
                    .lineLimit(1)
                    .padding(8)
            }
            .frame(width: MapTile.size, height: MapTile.size)
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .overlay {
                RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(selected ? Color.accentColor : Color.white.opacity(hovering ? 0.7 : 0),
                                  lineWidth: selected ? 4 : 2)
            }
            .overlay(alignment: .topTrailing) {
                if selected {
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 20))
                        .foregroundStyle(.white, Color.accentColor)
                        .padding(6)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .accessibilityLabel(name)
    }
}
