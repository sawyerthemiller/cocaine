#pragma once

// Path translation between the guest's view of the file system and the
// host's, for callers that have only the system root's path: a guest's own
// process (Process::host_path and guest_path forward here) and the file
// picker's helper, which must agree with it about every path it hands back.
// It depends on nothing but the system root, the real home folder and the disk.

#include <string>
#include <vector>

struct statfs;

// Where a path the guest names lives on the host. System directories are
// redirected into the system root. `self_path` is where the cocaine program
// is, for the one path that leads to its bundled desktop picture.
std::string guest_to_host_path(const std::string& sysroot, const std::string& guest,
                               bool follow_last = true, const std::string& self_path = "");
// The reverse, for paths handed back to the guest: the system root's own
// path is taken off the front of anything inside it.
std::string host_to_guest_path(const std::string& sysroot, const std::string& host);
// Where `path`, inside the real home folder, lives in the system root if it
// is part of what the guest system keeps to itself (the Library and the
// dot-files); an empty string if it is one of the shared parts.
std::string private_home_path(const std::string& sysroot, const std::string& path);
// The home folder of the person running cocaine, as this Mac has it.
const std::string& real_home();
// For tests only: have the translation take another folder for the home.
void set_real_home_for_tests(const std::string& dir);
// The bundled desktop picture, found from where the cocaine program is.
std::string bundled_wallpaper_path(const std::string& self_path);

// Browsable host volumes mounted under /Volumes, or an errno on failure.
int mounted_volumes(std::vector<struct statfs>& volumes, int flags);

// What became of a file or folder picked on this Mac for a guest.
enum class PickedPath {
  Ok,
  Missing,      // not there, or not an absolute path
  Unreachable,  // the guest's own Library or system folders stand in its place
  HiddenDisk,   // on a volume the guest's volume list doesn't hold
  TopFolder,    // one of this Mac's top-level folders, picked as a folder
};
// Works out the path to hand a guest for `pick`, an absolute host path a
// person chose in the Mac's file picker: an existing file or folder, or for
// `save` a name (new or existing) in an existing folder. On Ok, `guest` is
// that path: the stored spelling of what exists, the system root's prefix
// taken off, and a new name as it was typed. Anything else means the guest
// would not find the same file under any path, and `guest` is left alone.
PickedPath guest_path_for_pick(const std::string& sysroot, const std::string& pick, bool save,
                               std::string& guest);
// What to tell the person about a pick that was refused. Empty for Ok.
const char* picked_path_refusal(PickedPath result);
