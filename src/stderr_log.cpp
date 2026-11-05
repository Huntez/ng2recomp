// ng2 - Ninja Gaiden II static recompilation
//
// Windows: what the libraries print to stderr (plume's Vulkan errors, a failed
// pipeline or image, the GPU driver's, FFmpeg's) goes into ng2.log too: a game
// window has no console, so it would otherwise be lost. stderr becomes a pipe
// read by a thread that logs each line. An identical line is logged 5 times at
// most. (On Linux stderr stays as it is: a terminal or the journal has it, and
// a crash's last message, written just before the process ends, mustn't wait
// in a pipe.)

#include "stderr_log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

#include <fcntl.h>

#if defined(_WIN32)
#include <io.h>
#define ng2_pipe(fds) _pipe(fds, 65536, _O_BINARY)
#define ng2_dup _dup
#define ng2_dup2 _dup2
#define ng2_read _read
#define ng2_write _write
#define ng2_close _close
#define ng2_fileno _fileno
#else
#include <unistd.h>
#define ng2_pipe(fds) pipe(fds)
#define ng2_dup dup
#define ng2_dup2 dup2
#define ng2_read read
#define ng2_write write
#define ng2_close close
#define ng2_fileno fileno
#endif

#include <rex/logging.h>
#include <spdlog/spdlog.h>

namespace ng2::stderr_log {

void Start() {
#if !defined(_WIN32)
  return;
#else
  // The logger's own errors (ng2.log can't be written) would go to stderr,
  // into this pipe, from the thread that reads it: dropped instead.
  spdlog::set_error_handler([](const std::string&) {});
  // A window program's stderr has no file behind it: given one first, so its
  // descriptor can be pointed at the pipe.
  if (ng2_fileno(stderr) < 0) {
    if (!std::freopen("NUL", "w", stderr)) {
      return;
    }
  }
  int fd = ng2_fileno(stderr);
  int fds[2];
  if (fd < 0 || ng2_pipe(fds) != 0) {
    return;
  }
  int previous = ng2_dup(fd);  // (the terminal, or nothing)
  std::fflush(stderr);
  if (ng2_dup2(fds[1], fd) < 0) {  // (-1 on failure, on both)
    ng2_close(fds[0]);
    ng2_close(fds[1]);
    if (previous >= 0) {
      ng2_close(previous);
    }
    return;
  }
  ng2_close(fds[1]);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  int reader = fds[0];
  std::thread([reader, previous] {
    std::unordered_map<std::string, int> seen;
    std::string line;
    char buffer[4096];
    for (;;) {
      int n = int(ng2_read(reader, buffer, sizeof(buffer)));
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        ng2_close(reader);  // (writers then fail at once instead of blocking)
        return;
      }
      if (previous >= 0) {
        (void)ng2_write(previous, buffer, unsigned(n));
      }
      for (int i = 0; i < n; i++) {
        char c = buffer[i];
        if (c != '\n' && c != '\r') {
          if (line.size() < 2000) {
            line += c;
          }
          continue;
        }
        if (seen.size() >= 4096) {
          seen.clear();  // (lines with addresses or counts in them: counted afresh)
        }
        if (!line.empty() && line.rfind("[*** LOG ERROR", 0) != 0 && ++seen[line] <= 5) {
          REXLOG_WARN("stderr: {}", line);
        }
        line.clear();
      }
    }
  }).detach();  // (runs for the whole game; nothing to join at exit)
#endif
}

}  // namespace ng2::stderr_log
