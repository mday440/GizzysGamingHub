// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "MikesGameRoom",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "MikesGameRoom", targets: ["MikesGameRoom"]),
        .executable(name: "mgrcli", targets: ["mgrcli"]),
        .executable(name: "mgrzbic", targets: ["mgrzbic"]),
    ],
    targets: [
        // Emulator core in portable C: formats, crypto, AArch64 CPU, HLE kernel.
        .target(
            name: "MGRCore",
            path: "Sources/MGRCore",
            exclude: ["ZSTD_LICENSE"],
            publicHeadersPath: "include",
            cSettings: [.define("_DARWIN_C_SOURCE"), .define("ZSTD_DISABLE_ASM")],
            linkerSettings: [.linkedFramework("Hypervisor")]   // native CPU backend (cpu_hvf.c)
        ),
        // macOS app (SwiftUI).
        .executableTarget(
            name: "MikesGameRoom",
            dependencies: ["MGRCore"],
            path: "Sources/MikesGameRoom"
        ),
        // Command-line front end for scripting and debugging.
        .executableTarget(
            name: "mgrcli",
            dependencies: ["MGRCore"],
            path: "Tools",
            sources: ["mgrcli.c"]
        ),
        // ZBIC segment decoder for the imported nxnative builder (Engines/nxnative), instead of the GPLv2 zbic package.
        .executableTarget(
            name: "mgrzbic",
            dependencies: ["MGRCore"],
            path: "Sources/mgrzbic"
        ),
    ]
)
