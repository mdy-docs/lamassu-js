import AppKit
import SwiftUI

@main
struct LamassuReplApp: App {
    init() {
        // Run as a bare executable (`swift run`): become a regular app with a
        // Dock icon and take focus — a bundled app gets this from LaunchServices.
        DispatchQueue.main.async {
            NSApp.setActivationPolicy(.regular)
            NSApp.activate(ignoringOtherApps: true)
        }
    }

    var body: some Scene {
        WindowGroup("lamassu REPL") {
            ContentView()
        }
        .defaultSize(width: 680, height: 480)
    }
}
