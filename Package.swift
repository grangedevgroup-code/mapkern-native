// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "mapkern-native",
    platforms: [.iOS(.v15), .macOS(.v13)],
    products: [
        .library(name: "Mapkern", targets: ["Mapkern"]),
    ],
    targets: [
        .binaryTarget(
            name: "CMapkern",
            url: "https://github.com/grangedevgroup-code/mapkern-native/releases/download/v0.1.0/Mapkern.xcframework.zip",
            checksum: "cdd52b17cb8874cf3a191fbdb6a66f35d9625a1553b72df47e9e509a1dd3db20"
        ),
        .target(
            name: "Mapkern",
            dependencies: ["CMapkern"],
            path: "platform/apple/Sources/Mapkern",
            linkerSettings: [
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("Foundation"),
                .linkedLibrary("c++"),
            ]
        ),
    ],
    swiftLanguageModes: [.v5]
)
