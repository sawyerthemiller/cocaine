#pragma once

// The file picker's helper, and the messages it trades with the guest side.
//
// A guest program's open and save panels are shown by this Mac's own file
// picker. The guest's cocaine process starts `cocaine --pick --session <name>`
// with one end of a socket pair as the helper's descriptor 3, writes one
// request there and reads one reply; the helper shows the panel (or answers
// from the COCAINE_PICK_* stand-in) and goes away. Both ends are this same
// program, but the launcher can rebuild it while a guest runs, so messages
// say which version they are.
//
// A message is a run of `key=value` fields, each ended by a zero byte, and
// ends with an empty field (a lone zero byte). Booleans are 0 or 1, a key
// nobody knows is passed over, and `type` and `path` may come more than once.

#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

struct PickRequest {
  bool save = false;        // kind: `save`, else `open`
  int root = 0;             // the session's root process
  std::string sysroot;      // the system root, as an absolute host path
  std::string title, prompt, message;
  std::string label;        // the name field's label (save)
  std::string dir;          // the folder to start in, as an absolute HOST path; empty: the panel's choice
  std::string name;         // the suggested name, as the name field spells it
  // What an open panel lets through, as Leopard has them: an extension, an
  // HFS type code in quotes ('TEXT'), or a uniform type identifier. For a
  // save panel, the extensions a name may end in.
  std::vector<std::string> types;
  bool multi = false, files = true, dirs = false, mkdir = false;
  bool packages = false;    // file packages are opened like folders
  bool aliases = true, hidden = false;
  std::string required;     // the extension a saved name must end in, if any
  bool othertypes = false;  // ... unless the person gave another extension
  bool canhideext = false, exthidden = false;
  std::string note;         // a line for the person, shown with the message
};

struct PickReply {
  int pid = 0;                     // the helper's
  std::string status;              // `ok`, `cancel` or `error`
  std::vector<std::string> paths;  // GUEST paths, ready to hand to the app
  bool exthidden = false;
  std::string error;               // why, for `error`; a remark for the log otherwise
};

typedef std::vector<std::pair<std::string, std::string>> PickFields;

// How many bytes the first whole message in `bytes` takes, 0 if its end
// hasn't arrived yet.
inline size_t pick_message_length(const std::string& bytes) {
  size_t at = 0;
  while (at < bytes.size()) {
    const size_t end = bytes.find('\0', at);
    if (end == std::string::npos) return 0;
    if (end == at) return at + 1;
    at = end + 1;
  }
  return 0;
}

// A message's fields in order, up to its end.
inline PickFields pick_fields(const std::string& message) {
  PickFields fields;
  size_t at = 0;
  while (at < message.size()) {
    size_t end = message.find('\0', at);
    if (end == std::string::npos) end = message.size();
    if (end == at) break;
    const std::string field = message.substr(at, end - at);
    const size_t equals = field.find('=');
    if (equals == std::string::npos) fields.emplace_back(field, "");
    else fields.emplace_back(field.substr(0, equals), field.substr(equals + 1));
    at = end + 1;
  }
  return fields;
}

inline void pick_put(std::string& message, const char* key, const std::string& value) {
  message += key;
  message += '=';
  message += value.c_str();  // a value can't hold a zero byte: it ends there
  message += '\0';
}

inline void pick_put_flag(std::string& message, const char* key, bool value) {
  pick_put(message, key, value ? "1" : "0");
}

inline std::string pick_encode_request(const PickRequest& r) {
  std::string m;
  pick_put(m, "v", "1");
  pick_put(m, "kind", r.save ? "save" : "open");
  pick_put(m, "root", std::to_string(r.root));
  pick_put(m, "sysroot", r.sysroot);
  pick_put(m, "title", r.title);
  pick_put(m, "prompt", r.prompt);
  pick_put(m, "message", r.message);
  pick_put(m, "label", r.label);
  pick_put(m, "dir", r.dir);
  pick_put(m, "name", r.name);
  for (const std::string& type : r.types) pick_put(m, "type", type);
  pick_put_flag(m, "multi", r.multi);
  pick_put_flag(m, "files", r.files);
  pick_put_flag(m, "dirs", r.dirs);
  pick_put_flag(m, "mkdir", r.mkdir);
  pick_put_flag(m, "packages", r.packages);
  pick_put_flag(m, "aliases", r.aliases);
  pick_put_flag(m, "hidden", r.hidden);
  pick_put(m, "required", r.required);
  pick_put_flag(m, "othertypes", r.othertypes);
  pick_put_flag(m, "canhideext", r.canhideext);
  pick_put_flag(m, "exthidden", r.exthidden);
  pick_put(m, "note", r.note);
  m += '\0';
  return m;
}

inline bool pick_parse_request(const std::string& message, PickRequest& r, std::string& error) {
  const PickFields fields = pick_fields(message);
  if (fields.empty() || fields[0].first != "v" || fields[0].second != "1") {
    error = "the file picker was sent a request it doesn't understand (not version 1)";
    return false;
  }
  r = PickRequest();
  std::string kind;
  for (const auto& [key, value] : fields) {
    const bool on = value == "1";
    if (key == "kind") kind = value;
    else if (key == "root") r.root = atoi(value.c_str());
    else if (key == "sysroot") r.sysroot = value;
    else if (key == "title") r.title = value;
    else if (key == "prompt") r.prompt = value;
    else if (key == "message") r.message = value;
    else if (key == "label") r.label = value;
    else if (key == "dir") r.dir = value;
    else if (key == "name") r.name = value;
    else if (key == "type") r.types.push_back(value);
    else if (key == "multi") r.multi = on;
    else if (key == "files") r.files = on;
    else if (key == "dirs") r.dirs = on;
    else if (key == "mkdir") r.mkdir = on;
    else if (key == "packages") r.packages = on;
    else if (key == "aliases") r.aliases = on;
    else if (key == "hidden") r.hidden = on;
    else if (key == "required") r.required = value;
    else if (key == "othertypes") r.othertypes = on;
    else if (key == "canhideext") r.canhideext = on;
    else if (key == "exthidden") r.exthidden = on;
    else if (key == "note") r.note = value;
  }
  if (kind != "open" && kind != "save") {
    error = "the file picker was asked for a panel that is neither open nor save";
    return false;
  }
  r.save = kind == "save";
  return true;
}

inline std::string pick_encode_reply(const PickReply& r) {
  std::string m;
  pick_put(m, "v", "1");
  pick_put(m, "pid", std::to_string(r.pid));
  pick_put(m, "status", r.status);
  for (const std::string& path : r.paths) pick_put(m, "path", path);
  pick_put_flag(m, "exthidden", r.exthidden);
  pick_put(m, "error", r.error);
  m += '\0';
  return m;
}

inline bool pick_parse_reply(const std::string& message, PickReply& r, std::string& error) {
  const PickFields fields = pick_fields(message);
  if (fields.empty() || fields[0].first != "v" || fields[0].second != "1") {
    error = "the file picker sent a reply this cocaine doesn't understand (not version 1)";
    return false;
  }
  r = PickReply();
  for (const auto& [key, value] : fields) {
    if (key == "pid") r.pid = atoi(value.c_str());
    else if (key == "status") r.status = value;
    else if (key == "path") r.paths.push_back(value);
    else if (key == "exthidden") r.exthidden = value == "1";
    else if (key == "error") r.error = value;
  }
  if (r.status != "ok" && r.status != "cancel" && r.status != "error") {
    error = "the file picker's reply has no status";
    return false;
  }
  return true;
}

// The file that says a real panel is on the screen for the session with that
// root: it holds the helper's process number. The viewer reads it and, while
// that process lives, keeps the mouse and keys away from the guest.
inline std::string pick_flag_path(int root) {
  char tmp[PATH_MAX] = "/tmp/";
  confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp);
  return std::string(tmp) + "cocaine-picker-" + std::to_string(root);
}

// `cocaine --pick --session <name>`: answers the one request waiting on
// descriptor 3 and ends.
int run_picker(const std::string& session);
