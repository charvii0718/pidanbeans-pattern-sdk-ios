// swift-tools-version:5.7
import PackageDescription

let package = Package(
  name: "PidanPattern", platforms: [.iOS("15.1"), .macOS(.v11)],
  products: [.library(name: "PidanPattern", targets: ["PidanPattern"])],
  targets: [
    .target(name: "PidanCore", path: "Native", publicHeadersPath: "include"),
    .target(name: "PidanPattern", dependencies: ["PidanCore"]),
    .testTarget(name: "PidanPatternTests", dependencies: ["PidanPattern"], path: "Tests"),
  ], cxxLanguageStandard: .cxx17
)
