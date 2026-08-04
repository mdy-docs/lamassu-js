// swift-tools-version: 5.9
//
// LamassuRepl — example macOS SwiftUI app: a JavaScript REPL running on the
// lamassu-js engine via the LamassuJS Swift package (packages/swift).
//
// Run it:  swift run
import PackageDescription

let package = Package(
    name: "LamassuRepl",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(path: "../swift")
    ],
    targets: [
        .executableTarget(
            name: "LamassuRepl",
            dependencies: [.product(name: "LamassuJS", package: "swift")]
        )
    ]
)
