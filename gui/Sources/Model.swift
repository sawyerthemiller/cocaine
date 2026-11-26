// What the app knows and does: where cocaine is, whether it's built and up
// to date, the Leopard system root, the SMC keys, and running programs.

import AppKit
import Foundation

/// Which half of a universal program to run.
enum RunMode: String, CaseIterable, Identifiable {
  case auto, powerpc, intel
  var id: String { rawValue }
  var title: String {
    switch self {
    case .auto: return "Auto"
    case .powerpc: return "PowerPC"
    case .intel: return "Intel"
    }
  }
  /// cocaine's own option for it.
  var arguments: [String] {
    switch self {
    case .auto: return []
    case .powerpc: return ["--arch", "ppc"]
    case .intel: return ["--arch", "i386"]
    }
  }
}

/// The CPUs a program has code for.
struct Architectures: OptionSet {
  let rawValue: Int
  static let powerpc = Architectures(rawValue: 1)
  static let intel = Architectures(rawValue: 2)
  static let modern = Architectures(rawValue: 4)  // 64-bit only: not for cocaine
  static let classicPEF = Architectures(rawValue: 8)

  var description: String {
    var names: [String] = []
    if contains(.powerpc) { names.append(contains(.classicPEF) ? "PowerPC (PEF/CFM)" : "PowerPC") }
    if contains(.intel) { names.append("32-bit Intel") }
    if contains(.modern) { names.append("64-bit") }
    return names.isEmpty ? "nothing cocaine can run" : names.joined(separator: " and ")
  }

  /// Read the program's Mach-O or PowerPC PEF header (for an app, its main program's).
  static func of(_ url: URL) -> Architectures {
    var exe = url
    if let bundle = Bundle(url: url), let main = bundle.executableURL {
      exe = main
    }
    guard let handle = try? FileHandle(forReadingFrom: exe) else { return [] }
    defer { try? handle.close() }
    let head = handle.readData(ofLength: 4096)
    guard head.count >= 8 else { return [] }
    func be32(_ at: Int) -> UInt32 {
      guard at + 4 <= head.count else { return 0 }
      return head[at..<at + 4].reduce(0) { $0 << 8 | UInt32($1) }
    }
    func le32(_ at: Int) -> UInt32 { be32(at).byteSwapped }
    func kind(_ cpu: UInt32) -> Architectures {
      switch cpu {
      case 18: return .powerpc
      case 7: return .intel
      case 0x0100_0012, 0x0100_0007, 0x0100_000C: return .modern
      default: return []
      }
    }
    if head.count >= 40, be32(0) == 0x4A6F_7921, be32(4) == 0x7065_6666,
       be32(8) == 0x7077_7063, be32(12) == 1, (head[32] != 0 || head[33] != 0) {
      return [.powerpc, .classicPEF]
    }
    switch be32(0) {
    case 0xCAFE_BABE:  // universal: a list of 20-byte entries
      var found: Architectures = []
      let count = min(Int(be32(4)), 16)
      for i in 0..<count { found.formUnion(kind(be32(8 + 20 * i))) }
      return found
    case 0xFEED_FACE: return kind(be32(4))  // big-endian: PowerPC
    case 0xCEFA_EDFE: return kind(le32(4))  // little-endian: Intel
    case 0xFEED_FACF, 0xCFFA_EDFE: return .modern
    default: return []
    }
  }
}

@MainActor
final class AppModel: ObservableObject {
  static let repository = "https://github.com/sawyerthemiller/cocaine.git"

  static var defaultCheckout: String {
    let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
    return support.appendingPathComponent("Cocaine/cocaine").path
  }

  /// The checkout this app was built in, when it's run from its gui/build
  /// folder: then there's no need to download another copy.
  static func enclosingCheckout() -> String? {
    let app = URL(fileURLWithPath: Bundle.main.bundlePath)
    let repo = app.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    guard app.deletingLastPathComponent().path.hasSuffix("/gui/build") else { return nil }
    let fm = FileManager.default
    guard fm.fileExists(atPath: repo.appendingPathComponent(".git").path),
          fm.fileExists(atPath: repo.appendingPathComponent("CMakeLists.txt").path) else { return nil }
    return repo.path
  }

  // Settings.
  @Published var checkoutPath: String {
    didSet {
      UserDefaults.standard.set(checkoutPath, forKey: "checkoutPath")
      refresh()
    }
  }
  /// Empty: the system root inside the checkout, sysroots/10.5.
  @Published var sysrootChoice: String {
    didSet {
      UserDefaults.standard.set(sysrootChoice, forKey: "sysrootPath")
      refresh()
    }
  }
  var sysrootPath: String { sysrootChoice.isEmpty ? checkoutPath + "/sysroots/10.5" : sysrootChoice }
  var binaryPath: String { checkoutPath + "/build/cocaine" }
  var keysFile: String { sysrootPath + "/.cocaine/osk" }
  private var builtCommitFile: String { checkoutPath + "/build/.built-commit" }

  // What's there.
  @Published private(set) var missingTools: [String] = []
  @Published private(set) var hasCheckout = false
  @Published private(set) var head: String?        // the checkout's commit
  @Published private(set) var branch: String?
  @Published private(set) var behind: Int?          // commits on GitHub not yet here
  @Published private(set) var lastCheck: Date?
  @Published private(set) var checkFailed = false
  @Published private(set) var binaryExists = false
  @Published private(set) var builtCommit: String?
  @Published private(set) var sysrootReady = false
  @Published private(set) var keysInstalled = false

  /// What's being done, while something is.
  @Published private(set) var busy: String?

  let installLog = LogBuffer()
  let runLog = LogBuffer()

  // Running programs.
  @Published var target: URL? {
    didSet { targetArchitectures = target.map(Architectures.of) ?? [] }
  }
  @Published private(set) var targetArchitectures: Architectures = []
  @Published var mode: RunMode = .auto
  @Published private(set) var running = false
  private var runCommand: RunningCommand?

  init() {
    let defaults = UserDefaults.standard
    checkoutPath = defaults.string(forKey: "checkoutPath") ?? Self.enclosingCheckout() ?? Self.defaultCheckout
    sysrootChoice = defaults.string(forKey: "sysrootPath") ?? ""
    if let last = defaults.stringArray(forKey: "recents")?.first, FileManager.default.fileExists(atPath: last) {
      target = URL(fileURLWithPath: last)
      targetArchitectures = Architectures.of(URL(fileURLWithPath: last))
    }
    refresh()
  }

  var isBuilt: Bool { binaryExists }
  /// Built from the commit checked out. Built some other way than through
  /// this app (no record of the commit): built after that commit was made.
  var buildIsCurrent: Bool {
    if let builtCommit { return builtCommit == head }
    guard binaryExists, let binaryDate, let headDate else { return false }
    return binaryDate >= headDate
  }
  @Published private(set) var binaryDate: Date?
  @Published private(set) var headDate: Date?
  var isReady: Bool { binaryExists && sysrootReady }

  // MARK: Looking

  /// Looks at what's on disk again: quick, no network.
  func refresh() {
    let fm = FileManager.default
    missingTools = ["git", "cmake", "ninja"].filter { Shell.which($0) == nil }
    if Shell.capture("/usr/bin/xcode-select", ["-p"]).status != 0 { missingTools.append("Xcode command line tools") }

    hasCheckout = fm.fileExists(atPath: checkoutPath + "/.git") && fm.fileExists(atPath: checkoutPath + "/CMakeLists.txt")
    if hasCheckout {
      let rev = Shell.capture("git", ["rev-parse", "HEAD"], in: checkoutPath)
      head = rev.status == 0 ? rev.output : nil
      let br = Shell.capture("git", ["rev-parse", "--abbrev-ref", "HEAD"], in: checkoutPath)
      branch = br.status == 0 ? br.output : nil
    } else {
      head = nil
      branch = nil
      behind = nil
    }
    binaryExists = fm.isExecutableFile(atPath: binaryPath)
    binaryDate = (try? fm.attributesOfItem(atPath: binaryPath))?[.modificationDate] as? Date
    if hasCheckout, let stamp = TimeInterval(Shell.capture("git", ["log", "-1", "--format=%ct"], in: checkoutPath).output) {
      headDate = Date(timeIntervalSince1970: stamp)
    } else {
      headDate = nil
    }
    builtCommit = (try? String(contentsOfFile: builtCommitFile, encoding: .utf8))?
      .trimmingCharacters(in: .whitespacesAndNewlines)
    sysrootReady = fm.fileExists(atPath: sysrootPath + "/usr/lib/dyld")
    let size = (try? fm.attributesOfItem(atPath: keysFile)[.size] as? Int) ?? 0
    keysInstalled = size >= 64
  }

  // MARK: Installing and updating

  private func perform(_ what: String, _ steps: () async -> Bool) async {
    guard busy == nil else { return }
    busy = what
    installLog.startSection()
    installLog.add("\(what)...")
    stepsShown = 0
    if await steps() {
      installLog.add("")
      installLog.add("Complete...")
    }
    busy = nil
    refresh()
  }

  /// How many steps the current job has shown, for the blank line between them.
  private var stepsShown = 0

  /// Runs one step of a job. With a label (the git steps), the label is shown
  /// in place of the command, then a blank line, then git's output, with a
  /// blank line between what it fetched and what it says about it. Without
  /// one, the command itself is shown, with no prompt.
  private func step(_ tool: String, _ args: [String], in dir: String? = nil, label: String? = nil) async -> Bool {
    if stepsShown > 0 { installLog.add("") }
    stepsShown += 1
    let log = installLog
    let status: Int32
    if let label {
      installLog.add(label)
      installLog.add("")
      let split = FetchSplitter()
      status = await RunningCommand(tool, args, in: dir).run { line in
        if split.needsBlank(before: line) { log.add("") }
        log.add(line == "Already up to date." ? "Already up to date..." : line)
      }
    } else {
      installLog.add(([(tool as NSString).lastPathComponent] + args).joined(separator: " "))
      status = await RunningCommand(tool, args, in: dir).run { line in
        log.add(line == "ninja: no work to do." ? "ninja: no work to do" : line)
      }
    }
    if status != 0 {
      installLog.add("")
      installLog.add("Stopped - exit status \(status)...")
    }
    return status == 0
  }

  private func buildSteps() async -> Bool {
    if !FileManager.default.fileExists(atPath: checkoutPath + "/build/build.ninja") {
      guard await step("cmake", ["-S", ".", "-B", "build", "-G", "Ninja"], in: checkoutPath) else { return false }
    }
    guard await step("cmake", ["--build", "build"], in: checkoutPath) else { return false }
    let rev = Shell.capture("git", ["rev-parse", "HEAD"], in: checkoutPath)
    if rev.status == 0 { try? (rev.output + "\n").write(toFile: builtCommitFile, atomically: true, encoding: .utf8) }
    return true
  }

  /// Downloads cocaine from GitHub and builds it.
  func install() async {
    await perform("Downloading and building cocaine") {
      let parent = (checkoutPath as NSString).deletingLastPathComponent
      try? FileManager.default.createDirectory(atPath: parent, withIntermediateDirectories: true)
      guard await step("git", ["clone", Self.repository, checkoutPath], label: Self.gettingSource) else { return false }
      return await buildSteps()
    }
  }

  /// Builds what's there.
  func build() async {
    await perform("Building cocaine") { await buildSteps() }
  }

  /// Asks GitHub whether there's anything newer.
  func checkForUpdates() async {
    guard hasCheckout, busy == nil else { return }
    busy = "Checking for updates"
    let log = installLog
    let status = await RunningCommand("git", ["fetch", "--quiet", "origin"], in: checkoutPath).run { log.add($0) }
    let count = Shell.capture("git", ["rev-list", "--count", "HEAD..@{u}"], in: checkoutPath)
    checkFailed = status != 0 || count.status != 0
    behind = checkFailed ? nil : Int(count.output)
    lastCheck = Date()
    busy = nil
    refresh()
  }

  /// Fetches what's new and rebuilds.
  func update() async {
    await perform("Updating cocaine") {
      guard await step("git", ["pull", "--ff-only"], in: checkoutPath, label: Self.gettingSource) else { return false }
      behind = 0
      return await buildSteps()
    }
  }

  /// Unpacks Leopard from its install disc image into the system root.
  func importSysroot(from image: URL) async {
    await perform("Importing Leopard from \(image.lastPathComponent)") {
      await step("/bin/sh", ["tools/import-sysroot.sh", image.path, sysrootPath], in: checkoutPath)
    }
  }

  private static let gettingSource = "Getting source from the GH repo..."

  // MARK: Keys

  enum KeyError: LocalizedError {
    case badLength(String), badCharacters(String), noSystemRoot
    var errorDescription: String? {
      switch self {
      case .badLength(let which): return "\(which) must be exactly 32 characters."
      case .badCharacters(let which): return "\(which) can only hold plain letters, digits and punctuation."
      case .noSystemRoot: return "Set up the Leopard system root first (Install tab): the keys are kept inside it."
      }
    }
  }

  /// Saves the two keys, OSK0 then OSK1, where cocaine looks for them.
  /// Readable only by this user; never logged or shown.
  func saveKeys(_ first: String, _ second: String) throws {
    for (key, name) in [(first, "OSK0"), (second, "OSK1")] {
      guard key.utf8.count == 32 else { throw KeyError.badLength(name) }
      guard key.utf8.allSatisfy({ $0 >= 0x20 && $0 < 0x7F }) else { throw KeyError.badCharacters(name) }
    }
    guard FileManager.default.fileExists(atPath: sysrootPath) else { throw KeyError.noSystemRoot }
    let dir = (keysFile as NSString).deletingLastPathComponent
    try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    let temp = keysFile + ".new"
    FileManager.default.createFile(atPath: temp, contents: Data((first + second).utf8),
                                   attributes: [.posixPermissions: 0o600])
    if FileManager.default.fileExists(atPath: keysFile) { try FileManager.default.removeItem(atPath: keysFile) }
    try FileManager.default.moveItem(atPath: temp, toPath: keysFile)
    refresh()
  }

  /// Moves the keys file to the Trash.
  func removeKeys() throws {
    try FileManager.default.trashItem(at: URL(fileURLWithPath: keysFile), resultingItemURL: nil)
    refresh()
  }

  // MARK: Running

  static func isGhostReconDemo(_ target: URL) -> Bool {
    let enclosing = target.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    guard let bundle = Bundle(url: target) ?? Bundle(url: enclosing),
          bundle.bundleIdentifier == "com.aspyr.ghostrecon",
          bundle.object(forInfoDictionaryKey: "CFBundleName") as? String == "Ghost Recon Demo",
          let executable = bundle.executableURL else { return false }
    let selected = target.standardizedFileURL.resolvingSymlinksInPath()
    let app = bundle.bundleURL.standardizedFileURL.resolvingSymlinksInPath()
    let main = executable.standardizedFileURL.resolvingSymlinksInPath()
    return selected == app || selected == main
  }

  static func displayForRun(_ target: URL, inherited: [String: String]) -> String? {
    guard inherited["COCAINE_DISPLAY"] == nil, isGhostReconDemo(target) else { return nil }
    return "800x600"
  }

  static func viewerDoubleForRun(_ target: URL, inherited: [String: String]) -> String? {
    guard inherited["COCAINE_VIEWER_DOUBLE"] == nil, isGhostReconDemo(target) else { return nil }
    return "1"
  }

  static func legacyTextureFilterForRun(_ target: URL, inherited: [String: String]) -> String? {
    let executable = Bundle(url: target)?.executableURL ?? target
    guard inherited["COCAINE_GL_LEGACY_TEXTURE_FILTER"] == nil,
          ["Halo", "Halo Demo"].contains(executable.lastPathComponent),
          !Architectures.of(target).intersection([.powerpc, .intel]).isEmpty else { return nil }
    return "1"
  }

  func run() async {
    guard let target, !running, isReady else { return }
    remember(target)
    runLog.clear()
    let args = mode.arguments + [target.path]
    runLog.add("cocaine \(args.joined(separator: " "))")
    running = true
    stopPressed = false
    var environment = ["COCAINE_SYSROOT": sysrootPath]
    if let display = Self.displayForRun(target, inherited: Shell.environment) {
      environment["COCAINE_DISPLAY"] = display
      runLog.add("Using an 800 x 600 display for Ghost Recon Demo...")
    }
    if let doublePixels = Self.viewerDoubleForRun(target, inherited: Shell.environment) {
      environment["COCAINE_VIEWER_DOUBLE"] = doublePixels
      runLog.add("Doubling the display pixels for Ghost Recon Demo...")
    }
    if let legacyFilter = Self.legacyTextureFilterForRun(target, inherited: Shell.environment) {
      environment["COCAINE_GL_LEGACY_TEXTURE_FILTER"] = legacyFilter
      runLog.add("Using legacy texture filtering for Halo...")
    }
    let command = RunningCommand(binaryPath, args, environment: environment)
    runCommand = command
    let log = runLog
    let status = await command.run { log.add($0) }
    runLog.add("")
    runLog.add(stopPressed ? "Stopped..." : status == 0 ? "Complete..." : "Ended - exit status \(status)...")
    running = false
    runCommand = nil
  }

  private var stopPressed = false

  /// Ends every cocaine process on this Mac, not just the one started here:
  /// a run left over from the terminal, its daemons, a stuck viewer. Asks
  /// them to stop, then makes sure of any still there three seconds later.
  /// Matches the process name exactly, so this app (Cocaine) is left alone.
  func killAll() async {
    let found = Shell.capture("/usr/bin/pgrep", ["-x", "cocaine"]).output
      .split(separator: "\n").count
    runLog.add("")
    guard found > 0 else {
      runLog.add("No cocaine processes running...")
      return
    }
    runLog.add("Killing \(found) cocaine process\(found == 1 ? "" : "es")...")
    stopPressed = running
    Shell.capture("/usr/bin/pkill", ["-x", "cocaine"])
    try? await Task.sleep(nanoseconds: 3_000_000_000)
    if Shell.capture("/usr/bin/pgrep", ["-x", "cocaine"]).status == 0 {
      Shell.capture("/usr/bin/pkill", ["-9", "-x", "cocaine"])
    }
    runLog.add("Killed all...")
  }

  func stop() {
    stopPressed = true
    runCommand?.stop()
  }

  /// The Run screen has no recent list. This only puts the last program back
  /// next time the app opens. The key stays "recents" so a list saved by an
  /// older build still restores its first entry.
  private func remember(_ url: URL) {
    UserDefaults.standard.set([url.path], forKey: "recents")
  }
}

/// Tells where git's fetch report ("From ..." and the indented ref lines
/// under it) ends, so a blank line can go between it and the rest.
private final class FetchSplitter: @unchecked Sendable {
  private var inFetch = false

  func needsBlank(before line: String) -> Bool {
    if line.hasPrefix("From ") || (inFetch && line.hasPrefix(" ")) {
      inFetch = true
      return false
    }
    defer { inFetch = false }
    return inFetch
  }
}
