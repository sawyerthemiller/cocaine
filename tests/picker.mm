// The file picker helper's plain functions: the messages, the open panel's
// type filter (with the type lists the test apps really pass), how a saved
// name is finished, the stand-in answer file, the log line, and the check
// that a picked path is one the guest can reach. No panel, no NSApplication.
//
// Everything is made under a temporary folder, with a made-up system root
// and a made-up home folder inside it: nothing is created in the real home.

#include "../src/viewer/picker.mm"

#include <dirent.h>
#include <sys/xattr.h>

#include <algorithm>

static int g_failed = 0, g_checked = 0;
#define CHECK(what)                                                    \
  do {                                                                 \
    g_checked++;                                                       \
    if (!(what)) {                                                     \
      fprintf(stderr, "picker test, line %d: %s\n", __LINE__, #what);  \
      g_failed++;                                                      \
    }                                                                  \
  } while (0)

static void make_file(const std::string& path, const char* text = "x") {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0 || write(fd, text, strlen(text)) < 0) {
    fprintf(stderr, "picker test: cannot make %s: %s\n", path.c_str(), strerror(errno));
    exit(1);
  }
  close(fd);
}

static void make_folder(const std::string& path) {
  std::string so_far;
  size_t at = 1;
  while (at <= path.size()) {
    size_t end = path.find('/', at);
    if (end == std::string::npos) end = path.size();
    so_far = path.substr(0, end);
    mkdir(so_far.c_str(), 0755);
    at = end + 1;
  }
}

// Gives a file an HFS type code, as files from the old days carry.
static void set_type_code(const std::string& path, const char* code) {
  char info[32] = {};
  memcpy(info, code, 4);
  if (setxattr(path.c_str(), "com.apple.FinderInfo", info, sizeof info, 0, 0) != 0) {
    fprintf(stderr, "picker test: cannot set a type code on %s: %s\n", path.c_str(), strerror(errno));
    exit(1);
  }
}

// Whether a folder's listing has exactly this name, byte for byte.
static bool listed(const std::string& folder, const std::string& name) {
  bool found = false;
  if (DIR* d = opendir(folder.c_str())) {
    while (const dirent* e = readdir(d)) found |= name == e->d_name;
    closedir(d);
  }
  return found;
}

// A message made of these fields.
static std::string message(std::initializer_list<const char*> fields) {
  std::string m;
  for (const char* field : fields) m += std::string(field) + '\0';
  return m + '\0';
}

static std::string leaf_of(const std::string& path) { return path.substr(path.rfind('/') + 1); }

static PickRequest open_request(std::vector<std::string> types) {
  PickRequest r;
  r.types = std::move(types);
  return r;
}

static bool enables(const PickRequest& r, const std::string& path) {
  return pick_open_enables(r, pick_url(path), false);
}

static PickedPath check(const std::string& root, const std::string& pick, bool save, std::string& guest) {
  guest = "(untouched)";
  return guest_path_for_pick(root, pick, save, guest);
}

int main() {
  @autoreleasepool {
    // ---- Messages ----
    PickRequest sent;
    sent.save = true;
    sent.root = 4242;
    sent.sysroot = "/Volumes/Old Mac/root";
    sent.title = "Save Playlist";
    sent.prompt = "Save";
    sent.message = "two\nlines = fine";
    sent.label = "Save As:";
    sent.dir = "/Users/someone/Music";
    sent.name = "caf\xc3\xa9.m3u";
    sent.types = {"rtf", "'TEXT'", "public.data", "", "org.bittorrent.torrent"};
    sent.multi = true;
    sent.files = false;
    sent.dirs = true;
    sent.mkdir = true;
    sent.packages = true;
    sent.aliases = false;
    sent.hidden = true;
    sent.required = "m3u";
    sent.othertypes = true;
    sent.canhideext = true;
    sent.exthidden = true;
    sent.note = "a note";
    const std::string wire = pick_encode_request(sent);
    CHECK(pick_message_length(wire) == wire.size());
    CHECK(pick_message_length(wire.substr(0, wire.size() - 1)) == 0);
    CHECK(pick_message_length(wire.substr(0, 9)) == 0);
    CHECK(pick_message_length(wire + "more") == wire.size());
    CHECK(pick_message_length(std::string("\0", 1)) == 1);
    CHECK(pick_message_length("") == 0);
    CHECK(wire.compare(0, 4, std::string("v=1\0", 4)) == 0);
    PickRequest got;
    std::string error;
    CHECK(pick_parse_request(wire, got, error));
    CHECK(got.save && got.root == 4242 && got.sysroot == sent.sysroot && got.title == sent.title);
    CHECK(got.prompt == sent.prompt && got.message == sent.message && got.label == sent.label);
    CHECK(got.dir == sent.dir && got.name == sent.name && got.types == sent.types);
    CHECK(got.multi && !got.files && got.dirs && got.mkdir && got.packages && !got.aliases && got.hidden);
    CHECK(got.required == "m3u" && got.othertypes && got.canhideext && got.exthidden && got.note == "a note");
    // The other way round for every switch, and an open panel.
    PickRequest plain;
    CHECK(pick_parse_request(pick_encode_request(plain), got, error));
    CHECK(!got.save && got.root == 0 && got.types.empty() && got.title.empty());
    CHECK(!got.multi && got.files && !got.dirs && !got.mkdir && !got.packages && got.aliases && !got.hidden);
    CHECK(!got.othertypes && !got.canhideext && !got.exthidden);
    // A key from a newer cocaine is passed over; an older or newer version is not.
    const std::string extra = message({"v=1", "kind=open", "colour=red", "novalue", "multi=1"});
    CHECK(pick_message_length(extra) == extra.size());
    CHECK(pick_parse_request(extra, got, error) && got.multi && !got.save);
    CHECK(!pick_parse_request(message({"v=2", "kind=open"}), got, error) && !error.empty());
    CHECK(!pick_parse_request(message({"kind=open", "v=1"}), got, error));
    CHECK(!pick_parse_request(message({}), got, error));
    CHECK(!pick_parse_request(message({"v=1", "kind=print"}), got, error));
    CHECK(!pick_parse_request(message({"v=1"}), got, error));

    PickReply answer;
    answer.pid = 77;
    answer.status = "ok";
    answer.paths = {"/Users/someone/a=b.txt", "/Applications/caf\xc3\xa9"};
    answer.exthidden = true;
    answer.error = "said in passing";
    const std::string back = pick_encode_reply(answer);
    CHECK(pick_message_length(back) == back.size());
    PickReply read_back;
    CHECK(pick_parse_reply(back, read_back, error));
    CHECK(read_back.pid == 77 && read_back.status == "ok" && read_back.paths == answer.paths);
    CHECK(read_back.exthidden && read_back.error == "said in passing");
    for (const char* status : {"cancel", "error"}) {
      PickReply other;
      other.status = status;
      CHECK(pick_parse_reply(pick_encode_reply(other), read_back, error) && read_back.status == status);
      CHECK(read_back.paths.empty() && !read_back.exthidden);
    }
    CHECK(!pick_parse_reply(message({"v=1", "pid=3"}), read_back, error));
    CHECK(!pick_parse_reply(message({"v=1", "status=maybe"}), read_back, error));
    CHECK(!pick_parse_reply(message({"v=9", "status=ok"}), read_back, error));
    // A reply cut short has no end, which the requester takes for a failed pick.
    CHECK(pick_message_length(back.substr(0, back.size() - 1)) == 0);

    // ---- The log line ----
    CHECK(pick_log_line({}) == "request\n");
    CHECK(pick_log_line({{"v", "1"}, {"title", "a\tb\nc\\d\re"}, {"type", "txt"}, {"type", "'TEXT'"}}) ==
          "request\tv=1\ttitle=a\\tb\\nc\\\\d\\re\ttype=txt\ttype='TEXT'\n");
    const std::string logged = pick_log_line(pick_fields(wire));
    CHECK(std::count(logged.begin(), logged.end(), '\n') == 1 && logged.back() == '\n');
    CHECK(logged.find("\tmessage=two\\nlines = fine\t") != std::string::npos);
    CHECK(logged.find("\ttype=rtf\ttype='TEXT'\ttype=public.data\ttype=\ttype=org.bittorrent.torrent\t") !=
          std::string::npos);
    CHECK(logged.find("\tkind=save\t") != std::string::npos && logged.find("\tmulti=1\t") != std::string::npos);
    CHECK(logged.find("\tdir=/Users/someone/Music\tname=caf\xc3\xa9.m3u\t") != std::string::npos);

    // ---- The stand-in answer ----
    double delay = -1;
    std::vector<std::string> paths;
    CHECK(pick_parse_answer("delay 2.5\n/a b/c\n/d\n", true, delay, paths, error));
    CHECK(delay == 2.5 && paths == (std::vector<std::string>{"/a b/c", "/d"}));
    CHECK(pick_parse_answer("", true, delay, paths, error) && delay == 0 && paths.empty());
    CHECK(pick_parse_answer("delay 30\n", true, delay, paths, error) && delay == 30 && paths.empty());
    CHECK(pick_parse_answer("delay 0", true, delay, paths, error) && delay == 0 && paths.empty());
    CHECK(pick_parse_answer("/one", true, delay, paths, error) && delay == 0 && paths.size() == 1);
    CHECK(pick_parse_answer("/one\r\n\n/two\r\n", true, delay, paths, error));
    CHECK(paths == (std::vector<std::string>{"/one", "/two"}));
    CHECK(pick_parse_answer("\n\n", true, delay, paths, error) && paths.empty());
    CHECK(!pick_parse_answer("delay soon\n/a\n", true, delay, paths, error));
    CHECK(!pick_parse_answer("delay\n/a\n", true, delay, paths, error));
    CHECK(!pick_parse_answer("delay -1\n/a\n", true, delay, paths, error));
    CHECK(!pick_parse_answer("delay 1 2\n/a\n", true, delay, paths, error));
    CHECK(!pick_parse_answer("relative/path\n", true, delay, paths, error));
    error.clear();
    CHECK(!pick_parse_answer("/a\ndelay 2\n", true, delay, paths, error) && error.find("line 2") != std::string::npos);
    // The variables hold paths only: a delay line there is not a path.
    CHECK(!pick_parse_answer("delay 1\n/a\n", false, delay, paths, error));
    CHECK(pick_parse_answer("/a\n/b", false, delay, paths, error) && paths.size() == 2 && delay == 0);
    // A file whose name starts with the word is still a path.
    CHECK(pick_parse_answer("/delay 3", true, delay, paths, error) && paths.size() == 1 && delay == 0);

    // ---- Extensions ----
    CHECK(pick_extension("a.txt") == "txt" && pick_extension("a.tar.GZ") == "GZ");
    CHECK(pick_extension("readme").empty() && pick_extension(".profile").empty());
    CHECK(pick_extension("ends.").empty() && pick_extension("Notes v. 2").empty());
    CHECK(pick_extension("").empty() && pick_extension(".a.b") == "b");
    CHECK(pick_type_is_code("'TEXT'") && pick_type_is_code("'RTF '") && !pick_type_is_code("TEXT"));
    CHECK(pick_type_is_identifier("public.data") && !pick_type_is_identifier("'a.b '"));
    CHECK(pick_type_is_extension("txt") && !pick_type_is_extension("*") && !pick_type_is_extension(""));
    CHECK(!pick_type_is_extension("'TEXT'") && !pick_type_is_extension("public.text"));

    // ---- Files to pick ----
    char made[PATH_MAX];
    char real[PATH_MAX];
    confstr(_CS_DARWIN_USER_TEMP_DIR, made, sizeof made);
    strlcat(made, "cocaine-picker-test-XXXXXX", sizeof made);
    if (!mkdtemp(made) || !realpath(made, real)) {
      fprintf(stderr, "picker test: cannot make a temporary folder: %s\n", strerror(errno));
      return 1;
    }
    const std::string top = real;
    const std::string files = top + "/files";
    make_folder(files + "/Folder");
    make_folder(files + "/Letter.rtfd");
    make_file(files + "/Letter.rtfd/TXT.rtf");
    make_folder(files + "/Thing.app/Contents/MacOS");
    make_file(files + "/Thing.app/Contents/Info.plist",
              "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundlePackageType</key>"
              "<string>APPL</string></dict></plist>");
    for (const char* name : {"song.MP3", "movie.mkv", "notes.txt", "NOTES.TXT2", "Shout.TXT", "readme",
                             "linux.torrent", "LINUX.Torrent", "pic.JPG", "pic.jpeg", "page.html",
                             "old-text", "old-rtf", "new.rtf", "Notes v. 2", "list.m3u", "subs.srt"}) {
      make_file(files + "/" + name);
    }
    set_type_code(files + "/old-text", "TEXT");
    set_type_code(files + "/old-rtf", "RTF ");
    symlink((files + "/notes.txt").c_str(), (files + "/link-to-notes").c_str());
    symlink((files + "/Folder").c_str(), (files + "/link-to-folder").c_str());

    // VLC 0.9.10 (both CPUs): every open panel passes no types at all; its
    // folder pickers turn files off.
    const PickRequest vlc = open_request({});
    for (const char* name : {"song.MP3", "movie.mkv", "readme", "old-text", "Folder", "Letter.rtfd",
                             "Thing.app", "link-to-notes", "link-to-folder", "subs.srt"}) {
      CHECK(enables(vlc, files + "/" + name));
    }
    CHECK(!enables(vlc, files + "/not-there"));
    CHECK(pick_open_enables(vlc, pick_url(files + "/not-there"), true));

    // Transmission 1.54: its document type's identifier and extension.
    const PickRequest transmission = open_request({"org.bittorrent.torrent", "torrent"});
    CHECK(enables(transmission, files + "/linux.torrent"));
    CHECK(enables(transmission, files + "/LINUX.Torrent"));
    CHECK(enables(transmission, files + "/Folder"));
    CHECK(enables(transmission, files + "/link-to-folder"));
    for (const char* name : {"song.MP3", "notes.txt", "readme", "old-text", "Letter.rtfd", "Thing.app",
                             "link-to-notes"}) {
      CHECK(!enables(transmission, files + "/" + name));
    }

    // TextEdit 1.5: what NSDocumentController makes of its Info.plist, a mix
    // of extensions, quoted HFS codes and one identifier. public.data and "*"
    // let everything through.
    const std::vector<std::string> textedit_all = {
        "rtf", "'RTF '", "public.data", "rtfd", "html", "htm", "webarchive", "odt", "ott",
        "sxw", "stw", "docx", "doc", "'W8BN'", "'W6BN'", "xml", "txt", "text", "*", "'****'",
        "'TEXT'", "'sEXT'", "'ttro'"};
    const PickRequest textedit = open_request(textedit_all);
    for (const char* name : {"song.MP3", "movie.mkv", "notes.txt", "readme", "old-text", "Letter.rtfd",
                             "Thing.app", "Folder"}) {
      CHECK(enables(textedit, files + "/" + name));
    }
    // The same list without its two catch-alls shows each kind of entry at work.
    std::vector<std::string> narrowed;
    for (const std::string& type : textedit_all) {
      if (type != "public.data" && type != "*") narrowed.push_back(type);
    }
    const PickRequest strict = open_request(narrowed);
    for (const char* name : {"notes.txt", "Shout.TXT", "new.rtf", "page.html", "old-text", "old-rtf",
                             "Letter.rtfd", "Folder", "link-to-notes", "link-to-folder"}) {
      CHECK(enables(strict, files + "/" + name));
    }
    for (const char* name : {"song.MP3", "movie.mkv", "readme", "NOTES.TXT2", "linux.torrent", "pic.JPG",
                             "Thing.app", "Notes v. 2", "list.m3u"}) {
      CHECK(!enables(strict, files + "/" + name));
    }
    // A quoted code is a type code, never an extension, and the other way round.
    CHECK(enables(open_request({"'TEXT'"}), files + "/old-text"));
    CHECK(!enables(open_request({"'TEXT'"}), files + "/old-rtf"));
    CHECK(!enables(open_request({"'TEXT'"}), files + "/notes.txt"));
    CHECK(!enables(open_request({"TEXT"}), files + "/old-text"));
    CHECK(enables(open_request({"'RTF '"}), files + "/old-rtf"));
    // Identifiers go by what the file's own type conforms to.
    CHECK(enables(open_request({"public.plain-text"}), files + "/notes.txt"));
    CHECK(enables(open_request({"public.text"}), files + "/page.html"));
    CHECK(enables(open_request({"public.image"}), files + "/pic.JPG"));
    CHECK(enables(open_request({"public.jpeg"}), files + "/pic.jpeg"));
    CHECK(enables(open_request({"PUBLIC.JPEG"}), files + "/pic.JPG"));
    CHECK(!enables(open_request({"public.image"}), files + "/notes.txt"));
    CHECK(!enables(open_request({"public.movie", "no.such.type"}), files + "/notes.txt"));
    CHECK(enables(open_request({"public.item"}), files + "/readme"));
    CHECK(enables(open_request({"mp3", ""}), files + "/readme"));
    // Extensions are compared as written: jpg is not jpeg, whatever this Mac's
    // type table thinks.
    CHECK(enables(open_request({"jpg"}), files + "/pic.JPG"));
    CHECK(!enables(open_request({"jpg"}), files + "/pic.jpeg"));
    CHECK(enables(open_request({"JPEG", "mP3"}), files + "/song.MP3"));
    // Packages: a file by default, a folder when the app opens them like folders.
    CHECK(enables(open_request({"app"}), files + "/Thing.app"));
    CHECK(!enables(open_request({"txt"}), files + "/Thing.app"));
    CHECK(!enables(open_request({"txt"}), files + "/Letter.rtfd"));
    PickRequest inside = open_request({"txt"});
    inside.packages = true;
    CHECK(enables(inside, files + "/Thing.app") && enables(inside, files + "/Letter.rtfd"));

    // ---- Answers the real panel could not give ----
    CHECK(pick_answer_possible(vlc, {files + "/song.MP3"}, error));
    CHECK(!pick_answer_possible(vlc, {files + "/song.MP3", files + "/movie.mkv"}, error));
    PickRequest several = vlc;
    several.multi = true;
    CHECK(pick_answer_possible(several, {files + "/song.MP3", files + "/movie.mkv"}, error));
    CHECK(!pick_answer_possible(several, {files + "/song.MP3", files + "/not-there"}, error));
    CHECK(!pick_answer_possible(vlc, {files + "/Folder"}, error));
    CHECK(!pick_answer_possible(vlc, {files + "/link-to-folder"}, error));
    PickRequest folders = vlc;
    folders.files = false;
    folders.dirs = true;
    CHECK(pick_answer_possible(folders, {files + "/Folder"}, error));
    CHECK(pick_answer_possible(folders, {files + "/link-to-folder"}, error));
    CHECK(!pick_answer_possible(folders, {files + "/song.MP3"}, error));
    CHECK(!pick_answer_possible(folders, {files + "/Thing.app"}, error));
    PickRequest either = folders;
    either.files = true;
    CHECK(pick_answer_possible(either, {files + "/Folder"}, error));
    CHECK(pick_answer_possible(either, {files + "/Thing.app"}, error));
    CHECK(pick_answer_possible(transmission, {files + "/linux.torrent"}, error));
    error.clear();
    CHECK(!pick_answer_possible(transmission, {files + "/notes.txt"}, error) && !error.empty());
    CHECK(pick_answer_possible(strict, {files + "/Letter.rtfd"}, error));
    PickRequest strict_inside = strict;
    strict_inside.packages = true;
    CHECK(!pick_answer_possible(strict_inside, {files + "/Letter.rtfd"}, error));
    PickRequest saving;
    saving.save = true;
    CHECK(pick_answer_possible(saving, {files + "/new name"}, error));
    CHECK(pick_answer_possible(saving, {files + "/notes.txt"}, error));
    CHECK(pick_answer_possible(saving, {files + "/Letter.rtfd"}, error));
    CHECK(!pick_answer_possible(saving, {files + "/Folder"}, error));
    CHECK(!pick_answer_possible(saving, {files + "/a", files + "/b"}, error));
    CHECK(!pick_answer_possible(saving, {}, error));

    // ---- Finishing a saved name ----
    PickRequest save;
    save.save = true;
    CHECK(pick_finish_save_name(save, "anything") == "anything");
    CHECK(pick_finish_save_name(save, "a.xyz") == "a.xyz");
    CHECK(pick_prefill_name(save).empty());
    save.name = "Untitled";
    CHECK(pick_prefill_name(save) == "Untitled");
    // A required extension, other types not allowed (NSDocument's plain Save).
    save.required = "txt";
    CHECK(pick_prefill_name(save) == "Untitled.txt");
    save.name = "Untitled.rtf";
    CHECK(pick_prefill_name(save) == "Untitled.rtf");
    save.name = "";
    CHECK(pick_prefill_name(save).empty());
    CHECK(pick_finish_save_name(save, "notes") == "notes.txt");
    CHECK(pick_finish_save_name(save, "notes.txt") == "notes.txt");
    CHECK(pick_finish_save_name(save, "notes.TXT") == "notes.TXT");
    CHECK(pick_finish_save_name(save, "notes.doc") == "notes.doc.txt");
    CHECK(pick_finish_save_name(save, "Notes v. 2") == "Notes v. 2.txt");
    CHECK(pick_finish_save_name(save, ".hidden") == ".hidden.txt");
    CHECK(pick_finish_save_name(save, "").empty());
    // Other types allowed: only a name with no extension gains one.
    save.othertypes = true;
    CHECK(pick_finish_save_name(save, "notes") == "notes.txt");
    CHECK(pick_finish_save_name(save, "notes.doc") == "notes.doc");
    CHECK(pick_finish_save_name(save, "notes.txt") == "notes.txt");
    // The listed types count when they are extensions; the required one comes first.
    save.othertypes = false;
    save.types = {"'TEXT'", "public.plain-text", "rtf", "text", "*", ""};
    CHECK(pick_save_extensions(save) == (std::vector<std::string>{"txt", "rtf", "text"}));
    CHECK(pick_finish_save_name(save, "a.rtf") == "a.rtf");
    CHECK(pick_finish_save_name(save, "a.Text") == "a.Text");
    CHECK(pick_finish_save_name(save, "a.html") == "a.html.txt");
    save.required = "";
    CHECK(pick_finish_save_name(save, "a") == "a.rtf");
    CHECK(pick_prefill_name(save).empty());
    // Codes and identifiers alone give nothing to add.
    save.types = {"'TEXT'", "public.plain-text"};
    CHECK(pick_save_extensions(save).empty() && pick_finish_save_name(save, "a") == "a");
    save.required = "'TEXT'";
    CHECK(pick_finish_save_name(save, "a") == "a");
    save.name = "Untitled";
    CHECK(pick_prefill_name(save) == "Untitled");
    // VLC's playlist save names no type and adds the extension itself;
    // Transmission's torrent copy requires one.
    PickRequest playlist;
    playlist.save = true;
    playlist.name = "Untitled";
    CHECK(pick_prefill_name(playlist) == "Untitled" && pick_finish_save_name(playlist, "Mix") == "Mix");
    PickRequest copy;
    copy.save = true;
    copy.required = "torrent";
    copy.name = "linux.torrent";
    CHECK(pick_prefill_name(copy) == "linux.torrent");
    CHECK(pick_finish_save_name(copy, "linux") == "linux.torrent");
    CHECK(pick_finish_save_path(copy, "/a.b/linux") == "/a.b/linux.torrent");
    CHECK(pick_finish_save_path(copy, "/a/linux.torrent") == "/a/linux.torrent");
    CHECK(pick_finish_save_path(copy, "/linux") == "/linux.torrent");

    // ---- The path check ----
    // A made-up system root and home folder, both in the temporary folder
    // (so on one disk).
    const std::string root = top + "/root";
    const std::string home = top + "/home";
    set_real_home_for_tests(home);
    make_folder(root + "/System/Library/Fonts");
    make_folder(root + "/Applications/TextEdit.app/Contents");
    make_folder(root + "/Users/Shared");
    make_file(root + "/System/Library/Fonts/Old.dfont");
    make_file(root + "/Applications/Read Me.txt");
    make_file(root + "/Users/Shared/stray.txt");
    make_folder(home + "/Documents/Deep");
    make_folder(home + "/Library/Preferences");
    make_folder(home + "/.config");
    make_file(home + "/doc.txt");
    make_file(home + "/Documents/Deep/report.txt");
    make_file(home + "/Library/Preferences/app.plist");
    make_file(home + "/.profile");
    symlink((home + "/Library/Preferences").c_str(), (home + "/prefs-link").c_str());
    symlink((home + "/Documents").c_str(), (home + "/docs-link").c_str());
    symlink("doc.txt", (home + "/doc-link.txt").c_str());

    // The translation itself, as the guest's side uses it.
    CHECK(guest_to_host_path(root, "/System/Library/Fonts/Old.dfont") == root + "/System/Library/Fonts/Old.dfont");
    CHECK(guest_to_host_path(root, home + "/doc.txt") == home + "/doc.txt");
    CHECK(guest_to_host_path(root, home + "/Library/Preferences/app.plist") ==
          root + home + "/Library/Preferences/app.plist");
    CHECK(guest_to_host_path(root, home + "/.profile") == root + home + "/.profile");
    CHECK(private_home_path(root, home + "/Documents/x").empty());
    CHECK(private_home_path(root, home + "/Library") == root + home + "/Library");
    CHECK(guest_to_host_path(root, "/Applications/Read Me.txt") == root + "/Applications/Read Me.txt");
    CHECK(guest_to_host_path(root, "/Applications/Not In The Root") == "/Applications/Not In The Root");
    CHECK(guest_to_host_path("", "/System/Library") == "/System/Library");
    CHECK(host_to_guest_path(root, root + "/System/Library") == "/System/Library");
    CHECK(host_to_guest_path(root, root) == "/" && host_to_guest_path(root, root + "x/y") == root + "x/y");
    CHECK(host_to_guest_path(root, home + "/doc.txt") == home + "/doc.txt");

    std::string guest;
    // In the home folder: accepted, and handed over unchanged.
    CHECK(check(root, home + "/doc.txt", false, guest) == PickedPath::Ok && guest == home + "/doc.txt");
    CHECK(check(root, home + "/Documents/Deep/report.txt", false, guest) == PickedPath::Ok &&
          guest == home + "/Documents/Deep/report.txt");
    CHECK(check(root, home + "/Documents/Deep/", false, guest) == PickedPath::Ok && guest == home + "/Documents/Deep");
    CHECK(check(root, home, false, guest) == PickedPath::Ok && guest == home);
    // Through a link: the path the file really has.
    CHECK(check(root, home + "/docs-link/Deep/report.txt", false, guest) == PickedPath::Ok &&
          guest == home + "/Documents/Deep/report.txt");
    CHECK(check(root, home + "/doc-link.txt", false, guest) == PickedPath::Ok && guest == home + "/doc.txt");
    // Under the Library, a dot-file, and a link into the Library: the guest
    // has its own of those, so the same path leads it somewhere else.
    CHECK(check(root, home + "/Library/Preferences/app.plist", false, guest) == PickedPath::Unreachable);
    CHECK(guest == "(untouched)");
    CHECK(check(root, home + "/Library", false, guest) == PickedPath::Unreachable);
    CHECK(check(root, home + "/.profile", false, guest) == PickedPath::Unreachable);
    CHECK(check(root, home + "/prefs-link/app.plist", false, guest) == PickedPath::Unreachable);
    CHECK(check(root, home + "/prefs-link", false, guest) == PickedPath::Unreachable);
    // Inside the system root: the path the guest knows it by.
    CHECK(check(root, root + "/System/Library/Fonts/Old.dfont", false, guest) == PickedPath::Ok &&
          guest == "/System/Library/Fonts/Old.dfont");
    CHECK(check(root, root + "/Applications/Read Me.txt", false, guest) == PickedPath::Ok &&
          guest == "/Applications/Read Me.txt");
    CHECK(check(root, root + "/Applications/TextEdit.app", false, guest) == PickedPath::Ok &&
          guest == "/Applications/TextEdit.app");
    CHECK(check(root, root + "/Applications", false, guest) == PickedPath::Ok && guest == "/Applications");
    // The guest's own copy of the Library is reachable, by the home's path.
    make_folder(root + home + "/Library/Preferences");
    make_file(root + home + "/Library/Preferences/guest.plist");
    CHECK(check(root, root + home + "/Library/Preferences/guest.plist", false, guest) == PickedPath::Ok &&
          guest == home + "/Library/Preferences/guest.plist");
    // A part of the system root the guest never sees (its /Users is this Mac's).
    CHECK(check(root, root + "/Users/Shared/stray.txt", false, guest) == PickedPath::Unreachable);
    CHECK(check(root, root, false, guest) == PickedPath::Unreachable);
    // This Mac's own system files, which the guest's stand in front of.
    CHECK(check(root, "/System/Library/CoreServices/SystemVersion.plist", false, guest) == PickedPath::Unreachable);
    CHECK(check(root, "/usr/bin/true", false, guest) == PickedPath::Unreachable);
    // Missing things.
    CHECK(check(root, home + "/nothing here", false, guest) == PickedPath::Missing);
    CHECK(check(root, "doc.txt", false, guest) == PickedPath::Missing);
    CHECK(check(root, "", false, guest) == PickedPath::Missing);
    // /tmp and /private/tmp are one place; the guest gets the real spelling.
    char scratch[] = "/tmp/cocaine-picker-test-XXXXXX";
    const int scratch_fd = mkstemp(scratch);
    if (scratch_fd >= 0) {
      close(scratch_fd);
      const std::string in_private = std::string("/private") + scratch;
      CHECK(check(root, scratch, false, guest) == PickedPath::Ok && guest == in_private);
      CHECK(check(root, in_private, false, guest) == PickedPath::Ok && guest == in_private);
      CHECK(check(root, std::string("/tmp/new-") + leaf_of(scratch), true, guest) == PickedPath::Ok &&
            guest == "/private/tmp/new-" + leaf_of(scratch));
      CHECK(check(root, scratch, true, guest) == PickedPath::Ok && guest == in_private);
      unlink(scratch);
    } else {
      CHECK(scratch_fd >= 0);
    }
    // One of this Mac's top-level folders, picked as a folder.
    struct stat users, here;
    if (stat("/Users", &users) == 0 && stat(top.c_str(), &here) == 0 && users.st_dev == here.st_dev) {
      CHECK(check(root, "/Users", false, guest) == PickedPath::TopFolder);
      CHECK(check(root, "/Users/", false, guest) == PickedPath::TopFolder);
    } else {
      CHECK(check(root, "/Users", false, guest) != PickedPath::Ok);
    }
    CHECK(check(root, "/Volumes", false, guest) != PickedPath::Ok);
    // A disk the guest's volume list doesn't hold.
    CHECK(check(root, "/dev/null", false, guest) == PickedPath::HiddenDisk);
    CHECK(check(root, "/dev/cocaine-new-name", true, guest) == PickedPath::HiddenDisk);

    // Names with accents come back as they are stored, whichever way they
    // were asked for: this Mac's panel gives the decomposed spelling, while
    // files made by tools are mostly stored composed.
    const std::string composed = "caf\xc3\xa9.txt", decomposed = "cafe\xcc\x81.txt";
    const std::string composed2 = "\xc3\xbc" "ber.txt", decomposed2 = "u\xcc\x88" "ber.txt";
    make_file(home + "/Documents/" + composed);        // stored composed
    make_file(home + "/Documents/Deep/" + decomposed2);  // stored decomposed
    CHECK(listed(home + "/Documents", composed) && !listed(home + "/Documents", decomposed));
    CHECK(listed(home + "/Documents/Deep", decomposed2) && !listed(home + "/Documents/Deep", composed2));
    for (const std::string& asked : {composed, decomposed}) {
      CHECK(check(root, home + "/Documents/" + asked, false, guest) == PickedPath::Ok);
      CHECK(guest == home + "/Documents/" + composed && listed(home + "/Documents", leaf_of(guest)));
      CHECK(check(root, home + "/Documents/" + asked, true, guest) == PickedPath::Ok);
      CHECK(guest == home + "/Documents/" + composed);
    }
    for (const std::string& asked : {composed2, decomposed2}) {
      CHECK(check(root, home + "/Documents/Deep/" + asked, false, guest) == PickedPath::Ok);
      CHECK(guest == home + "/Documents/Deep/" + decomposed2 && listed(home + "/Documents/Deep", leaf_of(guest)));
    }

    // Saving: a new name in a folder that exists is handed over as typed.
    CHECK(check(root, home + "/Documents/new.txt", true, guest) == PickedPath::Ok &&
          guest == home + "/Documents/new.txt");
    CHECK(check(root, home + "/Documents/nouveau caf\xc3\xa9", true, guest) == PickedPath::Ok &&
          guest == home + "/Documents/nouveau caf\xc3\xa9");
    CHECK(check(root, home + "/Documents/nouveau cafe\xcc\x81", true, guest) == PickedPath::Ok &&
          guest == home + "/Documents/nouveau cafe\xcc\x81");
    CHECK(check(root, home + "/docs-link/new.txt", true, guest) == PickedPath::Ok &&
          guest == home + "/Documents/new.txt");
    CHECK(check(root, home + "/a:b.txt", true, guest) == PickedPath::Ok && guest == home + "/a:b.txt");
    // Onto a file that is there: the same file, under its stored name.
    CHECK(check(root, home + "/doc.txt", true, guest) == PickedPath::Ok && guest == home + "/doc.txt");
    struct stat other_case;
    if (stat((home + "/DOC.TXT").c_str(), &other_case) == 0) {  // the usual, case-blind kind of disk
      CHECK(check(root, home + "/DOC.TXT", true, guest) == PickedPath::Ok && guest == home + "/doc.txt");
      CHECK(check(root, home + "/DOC.TXT", false, guest) == PickedPath::Ok && guest == home + "/doc.txt");
    }
    // Onto a link: the link's own name, not what it points at.
    CHECK(check(root, home + "/doc-link.txt", true, guest) == PickedPath::Ok && guest == home + "/doc-link.txt");
    // A dot-name in the home folder, and anything under the Library, would
    // be written into the system root where nobody would look for it.
    CHECK(check(root, home + "/.newrc", true, guest) == PickedPath::Unreachable);
    CHECK(guest == "(untouched)");
    CHECK(check(root, home + "/.profile", true, guest) == PickedPath::Unreachable);
    CHECK(check(root, home + "/Library/new.txt", true, guest) == PickedPath::Unreachable);
    CHECK(check(root, home + "/Library/Preferences/app.plist", true, guest) == PickedPath::Unreachable);
    CHECK(check(root, home + "/prefs-link/new.plist", true, guest) == PickedPath::Unreachable);
    // A new name in the system root's Applications would land in this Mac's.
    CHECK(check(root, root + "/Applications/New.txt", true, guest) == PickedPath::Unreachable);
    CHECK(check(root, root + "/Applications/Read Me.txt", true, guest) == PickedPath::Ok &&
          guest == "/Applications/Read Me.txt");
    CHECK(check(root, root + "/System/Library/Fonts/New.dfont", true, guest) == PickedPath::Ok &&
          guest == "/System/Library/Fonts/New.dfont");
    CHECK(check(root, root + "/Users/Shared/new.txt", true, guest) == PickedPath::Unreachable);
    // No such folder, no name, not a folder.
    CHECK(check(root, home + "/No Folder/new.txt", true, guest) == PickedPath::Missing);
    CHECK(check(root, home + "/doc.txt/new.txt", true, guest) == PickedPath::Missing);
    CHECK(check(root, "/", true, guest) == PickedPath::Missing);
    CHECK(check(root, "new.txt", true, guest) == PickedPath::Missing);
    // Nothing above was written by the checks themselves.
    CHECK(access((home + "/.newrc").c_str(), F_OK) != 0 && access((root + home + "/.newrc").c_str(), F_OK) != 0);
    CHECK(access((home + "/Library/new.txt").c_str(), F_OK) != 0);
    CHECK(access((root + home + "/Library/new.txt").c_str(), F_OK) != 0);
    // Every refusal has something to say.
    for (PickedPath result : {PickedPath::Missing, PickedPath::Unreachable, PickedPath::HiddenDisk, PickedPath::TopFolder}) {
      CHECK(strlen(picked_path_refusal(result)) > 10);
    }
    CHECK(!*picked_path_refusal(PickedPath::Ok));

    // ---- A whole stand-in answer ----
    PickRequest ask;
    ask.sysroot = root;
    ask.types = {"txt"};
    ask.exthidden = true;
    PickReply reply = pick_stand_in_reply(ask, {});
    CHECK(reply.status == "cancel" && reply.paths.empty() && reply.pid == (int)getpid() && reply.exthidden);
    reply = pick_stand_in_reply(ask, {home + "/docs-link/Deep/report.txt"});
    CHECK(reply.status == "ok" && reply.error.empty());
    CHECK(reply.paths == std::vector<std::string>{home + "/Documents/Deep/report.txt"});
    reply = pick_stand_in_reply(ask, {root + "/Applications/Read Me.txt"});
    CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{"/Applications/Read Me.txt"});
    reply = pick_stand_in_reply(ask, {home + "/Library/Preferences/app.plist"});
    CHECK(reply.status == "error" && reply.paths.empty());  // greyed out by the type list
    ask.types.clear();
    reply = pick_stand_in_reply(ask, {home + "/Library/Preferences/app.plist"});
    CHECK(reply.status == "error" && reply.paths.empty());
    CHECK(reply.error.find("this app can't reach that place") == 0);
    CHECK(reply.error.find(home + "/Library/Preferences/app.plist") != std::string::npos);
    reply = pick_stand_in_reply(ask, {home + "/doc.txt", home + "/Documents/Deep/report.txt"});
    CHECK(reply.status == "error" && reply.paths.empty());
    ask.multi = true;
    reply = pick_stand_in_reply(ask, {home + "/doc.txt", home + "/Documents/Deep/report.txt"});
    CHECK(reply.status == "ok" && reply.paths.size() == 2);
    reply = pick_stand_in_reply(ask, {home + "/doc.txt", home + "/.profile"});
    CHECK(reply.status == "error" && reply.paths.empty());
    PickRequest keep;
    keep.save = true;
    keep.sysroot = root;
    keep.required = "txt";
    reply = pick_stand_in_reply(keep, {home + "/Documents/letter"});
    CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{home + "/Documents/letter.txt"});
    reply = pick_stand_in_reply(keep, {home + "/doc"});  // finished to a name that is taken: the same file
    CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{home + "/doc.txt"});
    reply = pick_stand_in_reply(keep, {home + "/.secret"});
    CHECK(reply.status == "error" && reply.paths.empty());
    reply = pick_stand_in_reply(keep, {home + "/Documents"});
    CHECK(reply.status == "error");
    CHECK(access((home + "/Documents/letter.txt").c_str(), F_OK) != 0);

    // ---- What the panel throws is caught ----
    std::string thrown;
    CHECK(pick_run_guarded(^NSModalResponse { return NSModalResponseOK; }, thrown) == NSModalResponseOK);
    CHECK(thrown.empty());
    const NSModalResponse after = pick_run_guarded(^NSModalResponse {
      [NSException raise:NSInternalInconsistencyException format:@"the panel fell over"];
      return NSModalResponseOK;
    }, thrown);
    CHECK(after != NSModalResponseOK && after != NSModalResponseCancel && thrown == "the panel fell over");
    thrown.clear();
    pick_run_guarded(^NSModalResponse {
      @throw @"not even an exception";
    }, thrown);
    CHECK(!thrown.empty());

    // The flag file's name is the session root's, in the user's temporary folder.
    confstr(_CS_DARWIN_USER_TEMP_DIR, made, sizeof made);
    CHECK(pick_flag_path(1234) == std::string(made) + "cocaine-picker-1234");

    [[NSFileManager defaultManager] removeItemAtPath:pick_string(top) error:nil];
  }
  if (g_failed) {
    fprintf(stderr, "picker test: %d checks failed\n", g_failed);
    return 1;
  }
  printf("picker test: %d checks passed\n", g_checked);
  return 0;
}
