// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "Mapkern",
    platforms: [.iOS(.v15), .macOS(.v13)],
    products: [
        .library(name: "Mapkern", targets: ["Mapkern"]),
        .executable(name: "MapkernDemo", targets: ["MapkernDemo"]),
    ],
    targets: [
        .binaryTarget(name: "CMapkern", path: "Mapkern.xcframework"),
        .target(
            name: "Mapkern",
            dependencies: ["CMapkern"],
            linkerSettings: [
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("Foundation"),
                .linkedLibrary("c++"),
            ]
        ),
        .executableTarget(name: "MapkernDemo", dependencies: ["Mapkern"]),
    ],
    swiftLanguageModes: [.v5]
)
