#include "session/pacing.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"

bool initial_low_latency_pacing() {
  const char* value = getenv("COCAINE_GL_LOW_LATENCY");
  if (!value || strcmp(value, "0") == 0) return false;
  if (strcmp(value, "1") == 0) return true;
  log_warn("COCAINE_GL_LOW_LATENCY must be 0 or 1; using the usual pacing");
  return false;
}

std::string session_pacing_path(const std::string& session) {
  if (session.empty() || session.find('/') != std::string::npos ||
      session.find("..") != std::string::npos) return {};
  char tmp[PATH_MAX] = "/tmp/";
  confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp);
  return std::string(tmp) + session + "/gl-pacing";
}

uint32_t* create_session_pacing(const std::string& session, bool initial, std::string& error) {
  const std::string path = session_pacing_path(session);
  if (path.empty()) {
    error = "invalid session name";
    return nullptr;
  }
  const std::string dir = path.substr(0, path.rfind('/'));
  if (mkdir(dir.c_str(), 0700) != 0) {
    error = "cannot create session control directory: " + std::string(strerror(errno));
    return nullptr;
  }
  const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    error = "cannot create session pacing control: " + std::string(strerror(errno));
    rmdir(dir.c_str());
    return nullptr;
  }
  const uint32_t value = initial ? 1 : 0;
  ssize_t written;
  do {
    written = write(fd, &value, sizeof value);
  } while (written < 0 && errno == EINTR);
  if (written != sizeof value) {
    error = "cannot initialize session pacing control: " +
            std::string(written < 0 ? strerror(errno) : "short write");
    close(fd);
    unlink(path.c_str());
    rmdir(dir.c_str());
    return nullptr;
  }
  void* map = mmap(nullptr, sizeof value, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) error = "cannot map session pacing control: " + std::string(strerror(errno));
  close(fd);
  if (map == MAP_FAILED) {
    unlink(path.c_str());
    rmdir(dir.c_str());
    return nullptr;
  }
  return static_cast<uint32_t*>(map);
}

uint32_t* open_session_pacing(const std::string& session, bool writable, std::string& error) {
  const std::string path = session_pacing_path(session);
  if (path.empty()) {
    error = "invalid session name";
    return nullptr;
  }
  const int fd = open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_NOFOLLOW);
  if (fd < 0) {
    error = "cannot open session pacing control: " + std::string(strerror(errno));
    return nullptr;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    error = "cannot stat session pacing control: " + std::string(strerror(errno));
    close(fd);
    return nullptr;
  }
  if (!S_ISREG(st.st_mode) || st.st_size != sizeof(uint32_t)) {
    error = "invalid session pacing control file";
    close(fd);
    return nullptr;
  }
  void* map = mmap(nullptr, sizeof(uint32_t), PROT_READ | (writable ? PROT_WRITE : 0),
                   MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) error = "cannot map session pacing control: " + std::string(strerror(errno));
  close(fd);
  return map == MAP_FAILED ? nullptr : static_cast<uint32_t*>(map);
}
