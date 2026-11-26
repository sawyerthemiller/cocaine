// swiftc -parse-as-library gui/Sources/Shell.swift gui/Sources/Model.swift tests/gui-architectures.swift -o /tmp/gui-architectures && /tmp/gui-architectures [cocaine-path [game-path]]
import Foundation

@main
struct ArchitectureTests {
  @MainActor
  static func main() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try! FileManager.default.removeItem(at: dir) }

    func check(_ name: String, _ bytes: [UInt8], _ expected: Architectures) throws {
      let file = dir.appendingPathComponent(name)
      try Data(bytes).write(to: file)
      precondition(Architectures.of(file) == expected, "wrong architecture for \(name)")
    }

    var pef = Array("Joy!peffpwpc".utf8) + [0, 0, 0, 1] + Array(repeating: UInt8(0), count: 24)
    pef[33] = 3
    try check("pef", pef, [.powerpc, .classicPEF])
    let ppcMachO: [UInt8] = [0xfe, 0xed, 0xfa, 0xce, 0, 0, 0, 18]
    let intelMachO: [UInt8] = [0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0]
    let universal: [UInt8] = [0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 2] +
      [0, 0, 0, 18] + Array(repeating: 0, count: 16) +
      [0, 0, 0, 7] + Array(repeating: 0, count: 16)
    for name in ["Halo Demo", "Halo"] {
      let halo = dir.appendingPathComponent(name)
      for header in [pef, ppcMachO, intelMachO, universal] {
        try Data(header).write(to: halo)
        precondition(AppModel.legacyTextureFilterForRun(halo, inherited: [:]) == "1",
                     "missing Halo preset for \(name)")
        for override in ["0", "1"] {
          precondition(AppModel.legacyTextureFilterForRun(halo, inherited: ["COCAINE_GL_LEGACY_TEXTURE_FILTER": override]) == nil)
        }
      }
      try Data([0xcf, 0xfa, 0xed, 0xfe, 7, 0, 0, 1]).write(to: halo)
      precondition(AppModel.legacyTextureFilterForRun(halo, inherited: [:]) == nil)
    }
    precondition(AppModel.legacyTextureFilterForRun(dir.appendingPathComponent("pef"), inherited: [:]) == nil)
    let haloBundle = dir.appendingPathComponent("Halo.app")
    let haloContents = haloBundle.appendingPathComponent("Contents")
    let haloMacOS = haloContents.appendingPathComponent("MacOS")
    try FileManager.default.createDirectory(at: haloMacOS, withIntermediateDirectories: true)
    let haloPlist = ["CFBundleExecutable": "Halo", "CFBundlePackageType": "APPL",
                     "CFBundleIdentifier": "test.cocaine.halo"]
    try PropertyListSerialization.data(fromPropertyList: haloPlist, format: .xml, options: 0)
      .write(to: haloContents.appendingPathComponent("Info.plist"))
    let haloExecutable = haloMacOS.appendingPathComponent("Halo")
    try Data(universal).write(to: haloExecutable)
    precondition(AppModel.legacyTextureFilterForRun(haloBundle, inherited: [:]) == "1")
    precondition(AppModel.legacyTextureFilterForRun(haloExecutable, inherited: [:]) == "1")
    let bundle = dir.appendingPathComponent("Extensionless Application")
    let contents = bundle.appendingPathComponent("Contents")
    let macOS = contents.appendingPathComponent("MacOS")
    try FileManager.default.createDirectory(at: macOS, withIntermediateDirectories: true)
    let plist: [String: String] = ["CFBundleExecutable": "Game", "CFBundlePackageType": "APPL",
                                   "CFBundleIdentifier": "test.cocaine.extensionless"]
    let data = try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
    try data.write(to: contents.appendingPathComponent("Info.plist"))
    try Data(pef).write(to: macOS.appendingPathComponent("Game"))
    precondition(Architectures.of(bundle) == [.powerpc, .classicPEF],
                 "extensionless app bundle not recognized")
    let demo = dir.appendingPathComponent("Ghost Recon Demo")
    let demoContents = demo.appendingPathComponent("Contents")
    let demoMacOS = demoContents.appendingPathComponent("MacOS")
    try FileManager.default.createDirectory(at: demoMacOS, withIntermediateDirectories: true)
    let demoPlist: [String: String] = ["CFBundleExecutable": "Ghost Recon Demo",
                                       "CFBundlePackageType": "APPL",
                                       "CFBundleIdentifier": "com.aspyr.ghostrecon",
                                       "CFBundleName": "Ghost Recon Demo"]
    let demoData = try PropertyListSerialization.data(fromPropertyList: demoPlist, format: .xml, options: 0)
    try demoData.write(to: demoContents.appendingPathComponent("Info.plist"))
    let demoExecutable = demoMacOS.appendingPathComponent("Ghost Recon Demo")
    try Data(pef).write(to: demoExecutable)
    precondition(AppModel.displayForRun(demo, inherited: [:]) == "800x600")
    precondition(AppModel.displayForRun(demoExecutable, inherited: [:]) == "800x600")
    precondition(AppModel.displayForRun(demo, inherited: ["COCAINE_DISPLAY": "1024x768"]) == nil)
    precondition(AppModel.displayForRun(bundle, inherited: [:]) == nil)
    precondition(AppModel.displayForRun(demoMacOS.appendingPathComponent("Other"), inherited: [:]) == nil)
    precondition(AppModel.viewerDoubleForRun(demo, inherited: [:]) == "1")
    precondition(AppModel.viewerDoubleForRun(demoExecutable, inherited: [:]) == "1")
    precondition(AppModel.viewerDoubleForRun(demo, inherited: ["COCAINE_VIEWER_DOUBLE": "0"]) == nil)
    precondition(AppModel.viewerDoubleForRun(demo, inherited: ["COCAINE_DISPLAY": "1024x768"]) == "1")
    precondition(AppModel.viewerDoubleForRun(bundle, inherited: [:]) == nil)
    precondition(AppModel.viewerDoubleForRun(demoMacOS.appendingPathComponent("Other"), inherited: [:]) == nil)
    let full = dir.appendingPathComponent("Ghost Recon")
    let fullContents = full.appendingPathComponent("Contents")
    let fullMacOS = fullContents.appendingPathComponent("MacOS")
    try FileManager.default.createDirectory(at: fullMacOS, withIntermediateDirectories: true)
    var fullPlist = demoPlist
    fullPlist["CFBundleName"] = "Ghost Recon"
    try PropertyListSerialization.data(fromPropertyList: fullPlist, format: .xml, options: 0)
      .write(to: fullContents.appendingPathComponent("Info.plist"))
    try Data(pef).write(to: fullMacOS.appendingPathComponent("Ghost Recon Demo"))
    precondition(AppModel.displayForRun(full, inherited: [:]) == nil)
    precondition(AppModel.viewerDoubleForRun(full, inherited: [:]) == nil)
    try check("truncated", Array(pef.prefix(16)), [])
    var wrongCPU = pef
    wrongCPU.replaceSubrange(8..<12, with: Array("m68k".utf8))
    try check("wrong-cpu", wrongCPU, [])
    var badVersion = pef
    badVersion[15] = 2
    try check("bad-version", badVersion, [])
    var noSections = pef
    noSections[33] = 0
    try check("no-sections", noSections, [])
    try check("ppc-mach-o", [0xfe, 0xed, 0xfa, 0xce, 0, 0, 0, 18], [.powerpc])
    try check("intel-mach-o", [0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0], [.intel])
    if CommandLine.arguments.count > 1 {
      func checkCLI(_ name: String, _ arch: String, _ message: String, _ status: Int32 = 126) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: CommandLine.arguments[1])
        process.arguments = ["--info", "--arch", arch, dir.appendingPathComponent(name).path]
        let output = Pipe()
        process.standardError = output
        process.standardOutput = output
        try process.run()
        let error = String(decoding: output.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        process.waitUntilExit()
        precondition(process.terminationStatus == status && error.contains(message), "wrong CLI result: \(error)")
      }
      try checkCLI("pef", "ppc", "PowerPC PEF/CFM", 0)
      try checkCLI("Extensionless Application", "ppc", "PowerPC PEF/CFM", 0)
      try checkCLI("pef", "i386", "is a PowerPC PEF/CFM file, not i386")
      try checkCLI("no-sections", "ppc", "not a Mach-O file")
    }
    if CommandLine.arguments.count > 2 {
      precondition(Architectures.of(URL(fileURLWithPath: CommandLine.arguments[2])) == [.powerpc, .classicPEF])
    }
    if CommandLine.arguments.count > 3 {
      precondition(Architectures.of(URL(fileURLWithPath: CommandLine.arguments[3])) == [.powerpc, .classicPEF])
    }
    print("GUI architecture tests passed")
  }
}
