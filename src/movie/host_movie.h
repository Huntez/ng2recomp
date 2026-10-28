// ng2 - Ninja Gaiden II static recompilation
//
// Movies decoded on the host with FFmpeg, standing in for the XDK's XMedia2
// player (whose WMV decoder needs GPU work the recompilation doesn't
// provide). One HostMovie per player the game creates: a thread decodes the
// file ahead into a short frame queue, and the game pulls the frame that is
// due when it asks (wall clock from its first request) into its own Y/U/V
// textures. Audio, when the game asks for it, goes to its own SDL stream.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ng2::movie {

class HostMovie {
 public:
  // Opens path; null (and logged) if it can't be played. A looping movie
  // starts over at its end and never finishes.
  static std::unique_ptr<HostMovie> Open(const std::filesystem::path& path, bool audio,
                                         bool loop);
  ~HostMovie();

  struct Planes {
    uint8_t* data[3];     // Y, U, V
    uint32_t pitch[3];    // bytes
    uint32_t size[3];     // bytes available
  };
  // Copies the frame due now into planes. False once the movie has ended.
  bool CopyFrame(const Planes& planes);

  bool Finished() const;
  int Width() const;
  int Height() const;
  void SetVolume(float volume);

  HostMovie(const HostMovie&) = delete;
  HostMovie& operator=(const HostMovie&) = delete;

 private:
  HostMovie();
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace ng2::movie
