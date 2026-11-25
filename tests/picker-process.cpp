// The file picker's helper as a process, the way a guest's cocaine starts it:
// `cocaine --pick --session <name>` with a socket pair's end as descriptor 3,
// one request, one reply. No guest is involved, and no panel can appear:
// every helper here is started with COCAINE_NO_VIEWER=1 and without
// COCAINE_PICK_REAL, and answers from the COCAINE_PICK_* stand-in.
//
// usage: picker-process-test <path to cocaine>

#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>

#include "viewer/picker.h"

extern char** environ;

namespace {

int g_failed = 0, g_checked = 0;
#define CHECK(what)                                                            \
  do {                                                                         \
    g_checked++;                                                               \
    if (!(what)) {                                                             \
      fprintf(stderr, "picker process test, line %d: %s\n", __LINE__, #what);  \
      g_failed++;                                                              \
    }                                                                          \
  } while (0)

std::string g_cocaine;

double now() { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }

void write_file(const std::string& path, const std::string& text) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f || fwrite(text.data(), 1, text.size(), f) != text.size()) {
    fprintf(stderr, "picker process test: cannot write %s: %s\n", path.c_str(), strerror(errno));
    exit(1);
  }
  fclose(f);
}

std::string read_file(const std::string& path) {
  std::string text;
  if (FILE* f = fopen(path.c_str(), "r")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
  }
  return text;
}

int count(const std::string& text, const std::string& what) {
  int n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) n++;
  return n;
}

struct Helper {
  pid_t pid = 0;
  int fd = -1;  // this end of the socket pair
};

// Starts a helper with this test's environment, less anything that steers
// the picker, plus `vars`. The viewer is always "off".
Helper start(const std::map<std::string, std::string>& vars, bool with_socket = true) {
  Helper h;
  int ends[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) {
    perror("socketpair");
    exit(1);
  }
  // Out of the way of descriptor 3, and neither end leaks into the helper.
  const int mine = fcntl(ends[0], F_DUPFD_CLOEXEC, 10), theirs = fcntl(ends[1], F_DUPFD_CLOEXEC, 10);
  close(ends[0]);
  close(ends[1]);
  const int on = 1;
  setsockopt(mine, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);

  std::vector<std::string> env;
  for (char** e = environ; *e; e++) {
    if (strncmp(*e, "COCAINE_PICK_", 13) != 0 && strncmp(*e, "COCAINE_SCRIPT=", 15) != 0 &&
        strncmp(*e, "COCAINE_NO_VIEWER=", 18) != 0) {
      env.push_back(*e);
    }
  }
  env.push_back("COCAINE_NO_VIEWER=1");
  for (const auto& [name, value] : vars) {
    if (name == "COCAINE_PICK_REAL") continue;  // never, in a test
    env.push_back(name + "=" + value);
  }
  std::vector<char*> envp;
  for (std::string& e : env) envp.push_back(e.data());
  envp.push_back(nullptr);

  std::string program = g_cocaine, pick = "--pick", option = "--session";
  std::string session = "cocaine.session." + std::to_string(getpid()) + ".1";
  char* argv[] = {program.data(), pick.data(), option.data(), session.data(), nullptr};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (with_socket) posix_spawn_file_actions_adddup2(&actions, theirs, 3);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
  const int error = posix_spawn(&h.pid, program.c_str(), &actions, &attributes, argv, envp.data());
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  close(theirs);
  if (error) {
    fprintf(stderr, "picker process test: cannot start %s: %s\n", program.c_str(), strerror(error));
    exit(1);
  }
  h.fd = mine;
  return h;
}

// Waits for the helper to end; -1 if it hasn't within `seconds` (and then it
// is killed), else its exit status, or 1000 + the signal that ended it.
int finish(Helper& h, double seconds) {
  const double until = now() + seconds;
  int status = 0;
  for (;;) {
    const pid_t got = waitpid(h.pid, &status, WNOHANG);
    if (got == h.pid) break;
    if (got < 0 && errno != EINTR) return -2;
    if (now() > until) {
      kill(h.pid, SIGKILL);
      waitpid(h.pid, &status, 0);
      if (h.fd >= 0) close(h.fd);
      h.fd = -1;
      return -1;
    }
    usleep(5000);
  }
  if (h.fd >= 0) close(h.fd);
  h.fd = -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
}

bool send_bytes(const Helper& h, const std::string& bytes) {
  size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t n = write(h.fd, bytes.data() + sent, bytes.size() - sent);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    sent += (size_t)n;
  }
  return true;
}

// The helper's reply, or false if none came in ten seconds or it was cut short.
bool receive(const Helper& h, PickReply& reply) {
  std::string bytes;
  const double until = now() + 10;
  while (!pick_message_length(bytes)) {
    struct pollfd watched = {h.fd, POLLIN, 0};
    const double left = until - now();
    if (left <= 0 || poll(&watched, 1, (int)(left * 1000) + 1) <= 0) {
      if (left <= 0) return false;
      continue;
    }
    char buf[4096];
    const ssize_t n = read(h.fd, buf, sizeof buf);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (n <= 0) return false;
    bytes.append(buf, (size_t)n);
  }
  std::string error;
  return pick_parse_reply(bytes, reply, error);
}

// One whole exchange: start, ask, hear the answer, see the helper end.
// `seconds`, if given, is how long the answer took.
PickReply ask(const std::map<std::string, std::string>& vars, const PickRequest& request,
              double* seconds = nullptr) {
  PickReply reply;
  reply.status = "(no reply)";
  Helper h = start(vars);
  const double from = now();
  if (send_bytes(h, pick_encode_request(request)) && !receive(h, reply)) reply.status = "(no reply)";
  if (seconds) *seconds = now() - from;
  if (reply.status != "(no reply)" && reply.pid != (int)h.pid) reply.status = "(wrong pid)";
  const int ended = finish(h, 5);
  if (ended != 0) {
    fprintf(stderr, "picker process test: the helper ended with %d\n", ended);
    reply.status = "(bad exit)";
  }
  return reply;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: picker-process-test <path to cocaine>\n");
    return 2;
  }
  g_cocaine = argv[1];
  signal(SIGPIPE, SIG_IGN);

  char made[PATH_MAX], real[PATH_MAX];
  confstr(_CS_DARWIN_USER_TEMP_DIR, made, sizeof made);
  strlcat(made, "cocaine-picker-process-XXXXXX", sizeof made);
  if (!mkdtemp(made) || !realpath(made, real)) {
    perror("mkdtemp");
    return 1;
  }
  const std::string top = real;
  // A made-up system root on the same disk as the files picked.
  const std::string root = top + "/root";
  mkdir(root.c_str(), 0755);
  mkdir((root + "/Applications").c_str(), 0755);
  mkdir((top + "/Folder").c_str(), 0755);
  write_file(root + "/Applications/Read Me.txt", "x");
  write_file(top + "/a.txt", "a");
  write_file(top + "/b.TXT", "b");
  write_file(top + "/song.mp3", "c");
  write_file(top + "/taken.txt", "d");
  const std::string log = top + "/requests.log";
  const std::string answers = top + "/answer";
  int asked = 0;  // requests that should each have left a line in the log

  PickRequest open;
  open.root = (int)getpid();
  open.sysroot = root;
  PickRequest save = open;
  save.save = true;

  // No stand-in and no viewer: cancel, and the reply says why.
  PickReply reply = ask({{"COCAINE_PICK_LOG", log}}, open);
  asked++;
  CHECK(reply.status == "cancel" && reply.paths.empty());
  CHECK(reply.error == "no viewer - file picker answered cancel");
  reply = ask({{"COCAINE_PICK_LOG", log}}, save);
  asked++;
  CHECK(reply.status == "cancel" && reply.error == "no viewer - file picker answered cancel");
  // A stand-in for the other kind of panel is no answer for this one.
  reply = ask({{"COCAINE_PICK_SAVE", top + "/out"}}, open);
  CHECK(reply.status == "cancel" && reply.error == "no viewer - file picker answered cancel");
  reply = ask({{"COCAINE_PICK_OPEN", top + "/a.txt"}}, save);
  CHECK(reply.status == "cancel" && reply.error == "no viewer - file picker answered cancel");

  // COCAINE_PICK_OPEN: one file, several, none.
  reply = ask({{"COCAINE_PICK_OPEN", top + "/a.txt"}, {"COCAINE_PICK_LOG", log}}, open);
  asked++;
  CHECK(reply.status == "ok" && reply.error.empty());
  CHECK(reply.paths == std::vector<std::string>{top + "/a.txt"});
  reply = ask({{"COCAINE_PICK_OPEN", ""}}, open);
  CHECK(reply.status == "cancel" && reply.paths.empty() && reply.error.empty());
  PickRequest many = open;
  many.multi = true;
  many.types = {"txt", "'TEXT'"};
  reply = ask({{"COCAINE_PICK_OPEN", top + "/a.txt\n" + top + "/b.TXT\n"}}, many);
  CHECK(reply.status == "ok" && reply.paths == (std::vector<std::string>{top + "/a.txt", top + "/b.TXT"}));
  // A file in the system root comes back as the guest knows it.
  reply = ask({{"COCAINE_PICK_OPEN", root + "/Applications/Read Me.txt"}}, open);
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{"/Applications/Read Me.txt"});
  // A folder, from a panel that gives folders.
  PickRequest folder = open;
  folder.files = false;
  folder.dirs = true;
  reply = ask({{"COCAINE_PICK_OPEN", top + "/Folder/"}}, folder);
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{top + "/Folder"});

  // COCAINE_PICK_SAVE: the name is finished as Leopard's OK button would.
  PickRequest text = save;
  text.required = "txt";
  text.exthidden = true;
  reply = ask({{"COCAINE_PICK_SAVE", top + "/out"}, {"COCAINE_PICK_LOG", log}}, text);
  asked++;
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{top + "/out.txt"} && reply.exthidden);
  reply = ask({{"COCAINE_PICK_SAVE", top + "/taken"}}, text);
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{top + "/taken.txt"});
  reply = ask({{"COCAINE_PICK_SAVE", top + "/as given.rtf"}}, save);
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{top + "/as given.rtf"} && !reply.exthidden);
  reply = ask({{"COCAINE_PICK_SAVE", ""}}, save);
  CHECK(reply.status == "cancel");
  struct stat st;
  CHECK(stat((top + "/out.txt").c_str(), &st) != 0);  // the helper names files; it writes none

  // COCAINE_PICK_DELAY holds the answer back.
  double took = 0;
  reply = ask({{"COCAINE_PICK_OPEN", top + "/a.txt"}, {"COCAINE_PICK_DELAY", "0.6"}}, open, &took);
  CHECK(reply.status == "ok" && took >= 0.55 && took < 5);
  reply = ask({{"COCAINE_PICK_OPEN", top + "/a.txt"}}, open, &took);
  CHECK(reply.status == "ok" && took < 5);

  // COCAINE_PICK_ANSWER: a file read afresh for every request, so one run
  // can give a different answer each time. It comes before the other two.
  const std::map<std::string, std::string> by_file = {
      {"COCAINE_PICK_ANSWER", answers}, {"COCAINE_PICK_LOG", log}, {"COCAINE_PICK_OPEN", top + "/song.mp3"},
      {"COCAINE_PICK_SAVE", top + "/never"}, {"COCAINE_PICK_DELAY", "30"}};
  write_file(answers, top + "/a.txt\n");
  reply = ask(by_file, open, &took);
  asked++;
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{top + "/a.txt"} && took < 5);
  write_file(answers, top + "/a.txt\n" + top + "/song.mp3\n");
  PickRequest any = open;
  any.multi = true;
  reply = ask(by_file, any);
  asked++;
  CHECK(reply.status == "ok" && reply.paths == (std::vector<std::string>{top + "/a.txt", top + "/song.mp3"}));
  write_file(answers, "");
  reply = ask(by_file, open);
  asked++;
  CHECK(reply.status == "cancel" && reply.error.empty());
  write_file(answers, top + "/kept\n");
  reply = ask(by_file, text);
  asked++;
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{top + "/kept.txt"});
  // With a delay line, which may be a fraction; alone, it is a slow cancel.
  write_file(answers, "delay 0.6\n" + top + "/song.mp3\n");
  reply = ask(by_file, open, &took);
  asked++;
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{top + "/song.mp3"});
  CHECK(took >= 0.55 && took < 5);
  write_file(answers, "delay 0.3\n");
  reply = ask(by_file, save, &took);
  asked++;
  CHECK(reply.status == "cancel" && took >= 0.25 && took < 5);
  // No such file, or nonsense in it: an error, not a cancel.
  unlink(answers.c_str());
  reply = ask(by_file, open);
  asked++;
  CHECK(reply.status == "error" && reply.error.find(answers) != std::string::npos && reply.paths.empty());
  write_file(answers, "not/absolute\n");
  reply = ask(by_file, open);
  asked++;
  CHECK(reply.status == "error" && !reply.error.empty());
  write_file(answers, "delay soon\n" + top + "/a.txt\n");
  reply = ask(by_file, open, &took);
  asked++;
  CHECK(reply.status == "error" && took < 5);

  // Answers the real panel could not have given are errors, each with a line.
  write_file(answers, top + "/a.txt\n" + top + "/song.mp3\n");
  reply = ask(by_file, open);  // two, from a panel that gives one
  asked++;
  CHECK(reply.status == "error" && !reply.error.empty() && reply.paths.empty());
  write_file(answers, top + "/a.txt\n");
  reply = ask(by_file, folder);  // a file, from a panel that gives folders
  asked++;
  CHECK(reply.status == "error" && !reply.error.empty() && reply.paths.empty());
  write_file(answers, top + "/Folder\n");
  reply = ask(by_file, open);  // a folder, from a panel that gives files
  asked++;
  CHECK(reply.status == "error" && reply.paths.empty());
  PickRequest torrents = open;
  torrents.types = {"org.bittorrent.torrent", "torrent"};
  write_file(answers, top + "/song.mp3\n");
  reply = ask(by_file, torrents);  // a file the type list greys out
  asked++;
  CHECK(reply.status == "error" && reply.error.find("greyed") != std::string::npos);
  write_file(answers, top + "/nothing-here.torrent\n");
  reply = ask(by_file, torrents);  // nothing there
  asked++;
  CHECK(reply.status == "error" && reply.paths.empty());
  write_file(answers, top + "/one\n" + top + "/two\n");
  reply = ask(by_file, save);  // two names to save under
  asked++;
  CHECK(reply.status == "error" && reply.paths.empty());
  write_file(answers, top + "/No Folder/name\n");
  reply = ask(by_file, save);  // a folder that isn't there
  asked++;
  CHECK(reply.status == "error" && reply.paths.empty());
  // Places the guest can't reach. Nothing is made there: the real Library is
  // only looked at, and the names to save under are never written.
  const struct passwd* me = getpwuid(getuid());
  const std::string home = me && me->pw_dir ? me->pw_dir : "";
  if (!home.empty() && stat((home + "/Library").c_str(), &st) == 0) {
    write_file(answers, home + "/Library\n");
    reply = ask(by_file, folder);
    asked++;
    CHECK(reply.status == "error" && reply.paths.empty());
    CHECK(reply.error.find("this app can't reach that place") == 0);
    write_file(answers, home + "/Library/cocaine-picker-test-never-made\n");
    reply = ask(by_file, save);
    asked++;
    CHECK(reply.status == "error" && reply.error.find("this app can't reach that place") == 0);
    write_file(answers, home + "/.cocaine-picker-test-never-made\n");
    reply = ask(by_file, save);
    asked++;
    CHECK(reply.status == "error" && reply.error.find("this app can't reach that place") == 0);
    CHECK(stat((home + "/Library/cocaine-picker-test-never-made").c_str(), &st) != 0);
    CHECK(stat((home + "/.cocaine-picker-test-never-made").c_str(), &st) != 0);
  }
  write_file(answers, root + "/Applications/New.txt\n");
  reply = ask(by_file, save);  // would land in this Mac's /Applications
  asked++;
  CHECK(reply.status == "error" && reply.error.find("this app can't reach that place") == 0);

  // A request this helper doesn't understand, and one that never ends.
  {
    Helper h = start({{"COCAINE_PICK_ANSWER", answers}, {"COCAINE_PICK_LOG", log}});
    asked++;
    PickReply odd;
    CHECK(send_bytes(h, std::string("v=2\0kind=open\0\0", 15)) && receive(h, odd));
    CHECK(odd.status == "error" && !odd.error.empty() && odd.pid == (int)h.pid);
    CHECK(finish(h, 5) == 0);
  }
  {
    Helper h = start({{"COCAINE_PICK_ANSWER", answers}});
    CHECK(send_bytes(h, std::string("v=1\0kind=open\0", 14)));  // no end
    usleep(200000);
    CHECK(waitpid(h.pid, nullptr, WNOHANG) == 0);  // still waiting for the rest
    shutdown(h.fd, SHUT_RDWR);
    close(h.fd);
    h.fd = -1;
    CHECK(finish(h, 2) == 125);  // nothing to answer
  }
  {
    // Started by hand, with no descriptor 3 at all.
    Helper h = start({}, false);
    CHECK(finish(h, 5) == 125);
  }

  // The requester going away during a delay (a guest killed while "the panel
  // is up") ends the helper within a second, and it answers nothing.
  for (const char* how : {"file", "variable"}) {
    write_file(answers, "delay 30\n" + top + "/a.txt\n");
    Helper h = strcmp(how, "file") == 0
                   ? start({{"COCAINE_PICK_ANSWER", answers}, {"COCAINE_PICK_LOG", log}})
                   : start({{"COCAINE_PICK_OPEN", top + "/a.txt"}, {"COCAINE_PICK_DELAY", "30"},
                            {"COCAINE_PICK_LOG", log}});
    asked++;
    CHECK(send_bytes(h, pick_encode_request(open)));
    usleep(400000);
    CHECK(waitpid(h.pid, nullptr, WNOHANG) == 0);  // waiting out its delay
    const double from = now();
    close(h.fd);
    h.fd = -1;
    const int ended = finish(h, 3);
    const double gone_in = now() - from;
    CHECK(ended == 0);
    CHECK(gone_in < 1.0);
    if (ended != 0 || gone_in >= 1.0) {
      fprintf(stderr, "picker process test: after a hang-up (%s) the helper ended with %d in %.2f s\n",
              how, ended, gone_in);
    }
  }
  // And a helper left to its delay is still there (so the above proves something).
  {
    write_file(answers, "delay 1.2\n");
    Helper h = start({{"COCAINE_PICK_ANSWER", answers}});
    CHECK(send_bytes(h, pick_encode_request(open)));
    usleep(600000);
    CHECK(waitpid(h.pid, nullptr, WNOHANG) == 0);
    PickReply late;
    CHECK(receive(h, late) && late.status == "cancel");
    CHECK(finish(h, 5) == 0);
  }

  // COCAINE_PICK_LOG: one line a request, whatever became of it, with the
  // request's fields as they were sent.
  PickRequest described = text;
  described.title = "Save\tAs";
  described.prompt = "Keep";
  described.message = "two\nlines";
  described.label = "Name\\here:";
  described.dir = top + "/Folder";
  described.name = "Untitled";
  described.types = {"rtf", "'TEXT'", "public.plain-text"};
  described.mkdir = true;
  described.note = "a note";
  write_file(answers, top + "/Folder/described\n");
  reply = ask(by_file, described);
  asked++;
  CHECK(reply.status == "ok" && reply.paths == std::vector<std::string>{top + "/Folder/described.txt"});
  const std::string logged = read_file(log);
  CHECK(count(logged, "\n") == asked);
  CHECK(count("\n" + logged, "\nrequest\tv=1\t") == asked - 1);  // all but the one that wasn't version 1
  CHECK(count("\n" + logged, "\nrequest\tv=2\tkind=open\n") == 1);
  const std::string last = logged.substr(logged.rfind("\nrequest", logged.size() - 2) + 1);
  CHECK(last.find("\tkind=save\t") != std::string::npos);
  CHECK(last.find("\troot=" + std::to_string(getpid()) + "\t") != std::string::npos);
  CHECK(last.find("\tsysroot=" + root + "\t") != std::string::npos);
  CHECK(last.find("\ttitle=Save\\tAs\tprompt=Keep\tmessage=two\\nlines\tlabel=Name\\\\here:\t") != std::string::npos);
  CHECK(last.find("\tdir=" + top + "/Folder\tname=Untitled\t") != std::string::npos);
  CHECK(last.find("\ttype=rtf\ttype='TEXT'\ttype=public.plain-text\t") != std::string::npos);
  CHECK(last.find("\tmulti=0\tfiles=1\tdirs=0\tmkdir=1\tpackages=0\taliases=1\thidden=0\t") != std::string::npos);
  CHECK(last.find("\trequired=txt\tothertypes=0\tcanhideext=0\texthidden=1\tnote=a note\n") != std::string::npos);

  // No helper of this test's is left behind, and no "panel is up" file was made.
  CHECK(stat(pick_flag_path((int)getpid()).c_str(), &st) != 0);

  // Only this test's own temporary folder, made above.
  nftw(top.c_str(), [](const char* path, const struct stat*, int, struct FTW*) { return remove(path); }, 16,
       FTW_DEPTH | FTW_PHYS);
  if (g_failed) {
    fprintf(stderr, "picker process test: %d checks failed\n", g_failed);
    return 1;
  }
  printf("picker process test: %d checks passed\n", g_checked);
  return 0;
}
