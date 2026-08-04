// swift-tools-version: 5.9
//
// LamassuJS — Swift bindings for the lamassu-js engine.
//
// The C target compiles the engine straight from the repo's src/ and
// third_party/ trees via symlinks (engine, re-src, re-include), so there is
// exactly one copy of the C code in the monorepo. The build mirrors the
// Makefile's native flavour: both halves (frontend + runtime) plus the
// baru-re regex engine, with LAMASSU_HAS_REGEX defined.
import PackageDescription

let package = Package(
    name: "LamassuJS",
    platforms: [.macOS(.v13), .iOS(.v16)],
    products: [
        .library(name: "LamassuJS", targets: ["LamassuJS"])
    ],
    targets: [
        .target(
            name: "CLamassu",
            exclude: [
                "engine/reactor.c",   // wasip1 fleet reactor, not a library source
                "engine/wasm_api.c",  // emscripten embedding, not a library source
                "re-src/regex_wasm.c" // baru-re's own wasm embedding
            ],
            publicHeadersPath: "include",
            cSettings: [
                .define("LAMASSU_HAS_REGEX"),
                .headerSearchPath("engine"),
                .headerSearchPath("engine/runtime"),
                .headerSearchPath("engine/frontend"),
                .headerSearchPath("re-include"),
            ]
        ),
        .target(name: "LamassuJS", dependencies: ["CLamassu"]),
        .testTarget(name: "LamassuJSTests", dependencies: ["LamassuJS"]),
    ]
)
