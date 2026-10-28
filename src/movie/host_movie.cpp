// ng2 - Ninja Gaiden II static recompilation
//
// See host_movie.h.

#include "host_movie.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <rex/logging.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace ng2::movie {

namespace {

// Frames decoded ahead of the one on screen.
constexpr size_t kQueuedFrames = 6;

struct Frame {
  double time = 0.0;  // seconds from the start of the movie
  int width = 0, height = 0;
  std::vector<uint8_t> planes[3];  // tightly packed Y, U, V (4:2:0)
};

std::string AvError(int error) {
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(error, text, sizeof(text));
  return text;
}

bool AudioEnabled() {
  const char* v = std::getenv("NG2_AUDIO");
  return !(v && v[0] == '0');
}

}  // namespace

struct HostMovie::State {
  std::string name;
  AVFormatContext* format = nullptr;
  AVCodecContext* video = nullptr;
  AVCodecContext* audio = nullptr;
  // Audio reads its own handle to the file on its own thread, keeping about
  // half a second queued for SDL: paced by the video queue it starved
  // whenever the game hitched.
  AVFormatContext* audioFormat = nullptr;
  std::thread audioThread;
  int videoStream = -1, audioStream = -1;
  SwsContext* scaler = nullptr;
  SwrContext* resampler = nullptr;
  SDL_AudioStream* audioOut = nullptr;
  int outputChannels = 2;
  double startTime = 0.0;  // the first video frame's timestamp
  double decodedTime = 0.0;  // the latest decoded frame's time
  bool loop = false;
  double loopOffset = 0.0;  // added to frame times after each pass
  double frameDuration = 1.0 / 30.0;
  uint64_t framesDecoded = 0;
  int width = 0, height = 0;

  std::thread thread;
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Frame> queue;
  bool endOfFile = false;
  bool quit = false;

  // Owned by the game's thread.
  bool started = false;
  std::chrono::steady_clock::time_point start;
  bool finished = false;
  double lastShown = -1.0;

  ~State() {
    {
      std::lock_guard lock(mutex);
      quit = true;
    }
    wake.notify_all();
    if (thread.joinable()) {
      thread.join();
    }
    if (audioThread.joinable()) {
      audioThread.join();
    }
    if (audioOut) {
      SDL_DestroyAudioStream(audioOut);
    }
    swr_free(&resampler);
    sws_freeContext(scaler);
    avcodec_free_context(&audio);
    avcodec_free_context(&video);
    avformat_close_input(&audioFormat);
    avformat_close_input(&format);
  }

  AVCodecContext* OpenDecoder(AVFormatContext* source, int stream) {
    const AVCodecParameters* parameters = source->streams[stream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) {
      REXLOG_ERROR("movie: {}: no decoder for {}", name, avcodec_get_name(parameters->codec_id));
      return nullptr;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(context, parameters);
    context->thread_count = 2;
    if (int error = avcodec_open2(context, codec, nullptr); error < 0) {
      REXLOG_ERROR("movie: {}: cannot open the {} decoder: {}", name, codec->name, AvError(error));
      avcodec_free_context(&context);
    }
    return context;
  }

  bool OpenAudioOutput() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXLOG_WARN("movie: {}: no audio ({})", name, SDL_GetError());
      return false;
    }
    // Mono, stereo and 5.1 go out as they are (SDL's channel order matches
    // FFmpeg's), so the system mixes them for its speakers; anything else
    // becomes stereo.
    AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO, surround = AV_CHANNEL_LAYOUT_5POINT1_BACK;
    if (!av_channel_layout_compare(&audio->ch_layout, &mono) ||
        !av_channel_layout_compare(&audio->ch_layout, &surround)) {
      layout = audio->ch_layout;
    } else if (audio->ch_layout.nb_channels == 1) {
      layout = mono;  // an unspecified single channel
    }
    outputChannels = layout.nb_channels;
    if (swr_alloc_set_opts2(&resampler, &layout, AV_SAMPLE_FMT_FLT, 48000, &audio->ch_layout,
                            audio->sample_fmt, audio->sample_rate, 0, nullptr) < 0 ||
        swr_init(resampler) < 0) {
      REXLOG_WARN("movie: {}: cannot convert its audio", name);
      return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, outputChannels, 48000};
    audioOut =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!audioOut) {
      REXLOG_WARN("movie: {}: no audio device ({})", name, SDL_GetError());
      return false;
    }
    return true;
  }

  void PushAudio(const AVFrame* frame) {
    int samples = swr_get_out_samples(resampler, frame->nb_samples);
    if (samples <= 0) {
      return;
    }
    std::vector<float> buffer(size_t(samples) * outputChannels);
    uint8_t* out = reinterpret_cast<uint8_t*>(buffer.data());
    int converted = swr_convert(resampler, &out, samples,
                                const_cast<const uint8_t**>(frame->extended_data),
                                frame->nb_samples);
    if (converted > 0) {
      SDL_PutAudioStreamData(audioOut, buffer.data(),
                             converted * outputChannels * int(sizeof(float)));
    }
  }

  // Converts a decoded picture to packed 4:2:0 planes.
  Frame MakeFrame(const AVFrame* picture) {
    Frame frame;
    frame.width = picture->width;
    frame.height = picture->height;
    int64_t timestamp = picture->best_effort_timestamp;
    AVRational base = format->streams[videoStream]->time_base;
    // Without a timestamp: one frame after the previous one.
    frame.time = timestamp == AV_NOPTS_VALUE
                     ? (framesDecoded ? decodedTime + frameDuration : loopOffset)
                     : timestamp * av_q2d(base) - startTime + loopOffset;
    framesDecoded++;
    decodedTime = frame.time;

    int chromaWidth = (frame.width + 1) / 2, chromaHeight = (frame.height + 1) / 2;
    frame.planes[0].resize(size_t(frame.width) * frame.height);
    frame.planes[1].resize(size_t(chromaWidth) * chromaHeight);
    frame.planes[2].resize(size_t(chromaWidth) * chromaHeight);
    uint8_t* dst[4] = {frame.planes[0].data(), frame.planes[1].data(), frame.planes[2].data()};
    int dstPitch[4] = {frame.width, chromaWidth, chromaWidth};
    if (picture->format == AV_PIX_FMT_YUV420P) {
      av_image_copy(dst, dstPitch, const_cast<const uint8_t**>(picture->data),
                    picture->linesize, AV_PIX_FMT_YUV420P, frame.width, frame.height);
    } else {
      scaler = sws_getCachedContext(scaler, frame.width, frame.height,
                                    AVPixelFormat(picture->format), frame.width, frame.height,
                                    AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
      if (scaler) {
        sws_scale(scaler, picture->data, picture->linesize, 0, frame.height, dst, dstPitch);
      }
    }
    return frame;
  }

  bool Quitting() {
    std::lock_guard lock(mutex);
    return quit;
  }

  void DecodeAudio() {
    const int kAhead = 48000 / 2 * outputChannels * int(sizeof(float));  // 0.5 s
    AVPacket* packet = av_packet_alloc();
    AVFrame* samples = av_frame_alloc();
    auto drain = [&] {
      while (avcodec_receive_frame(audio, samples) == 0) {
        PushAudio(samples);
      }
    };
    bool decodedAny = false;
    while (!Quitting()) {
      if (SDL_GetAudioStreamQueued(audioOut) > kAhead) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      if (av_read_frame(audioFormat, packet) < 0) {
        avcodec_send_packet(audio, nullptr);
        drain();
        if (!loop || !decodedAny || av_seek_frame(audioFormat, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
          break;
        }
        avcodec_flush_buffers(audio);
        decodedAny = false;
        continue;
      }
      if (packet->stream_index == audioStream && avcodec_send_packet(audio, packet) == 0) {
        decodedAny = true;
        drain();
      }
      av_packet_unref(packet);
    }
    av_frame_free(&samples);
    av_packet_free(&packet);
  }

  void Decode() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* picture = av_frame_alloc();
    bool firstFrame = true;
    auto drain = [&](AVCodecContext* context) {
      while (avcodec_receive_frame(context, picture) == 0) {
        if (firstFrame) {
          firstFrame = false;
          int64_t timestamp = picture->best_effort_timestamp;
          startTime = timestamp == AV_NOPTS_VALUE
                          ? 0.0
                          : timestamp * av_q2d(format->streams[videoStream]->time_base);
        }
        Frame frame = MakeFrame(picture);
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return quit || queue.size() < kQueuedFrames; });
        if (quit) {
          return false;
        }
        queue.push_back(std::move(frame));
      }
      return true;
    };

    bool running = true;
    uint64_t framesAtPassStart = 0;
    while (running) {
      {
        std::lock_guard lock(mutex);
        if (quit) {
          break;
        }
      }
      int error = av_read_frame(format, packet);
      if (error < 0) {
        // End of file: flush the decoders.
        avcodec_send_packet(video, nullptr);
        running = drain(video);
        // A pass without a frame would loop forever.
        if (!running || !loop || framesDecoded == framesAtPassStart) {
          break;
        }
        framesAtPassStart = framesDecoded;
        // Start over, one frame after the last one shown.
        loopOffset = decodedTime + frameDuration;
        if (av_seek_frame(format, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
          break;
        }
        avcodec_flush_buffers(video);
        continue;
      }
      AVCodecContext* context = packet->stream_index == videoStream ? video : nullptr;
      if (context && avcodec_send_packet(context, packet) == 0) {
        running = drain(context);
      }
      av_packet_unref(packet);
    }
    av_frame_free(&picture);
    av_packet_free(&packet);
    std::lock_guard lock(mutex);
    endOfFile = true;
  }
};

HostMovie::HostMovie() : state_(std::make_unique<State>()) {}

HostMovie::~HostMovie() = default;

std::unique_ptr<HostMovie> HostMovie::Open(const std::filesystem::path& path, bool audio,
                                           bool loop) {
  std::unique_ptr<HostMovie> movie(new HostMovie());
  State& s = *movie->state_;
  s.loop = loop;
  s.name = path.filename().string();
  if (int error = avformat_open_input(&s.format, path.string().c_str(), nullptr, nullptr);
      error < 0) {
    REXLOG_ERROR("movie: cannot open {}: {}", path.string(), AvError(error));
    return nullptr;
  }
  avformat_find_stream_info(s.format, nullptr);
  s.videoStream = av_find_best_stream(s.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (s.videoStream < 0 || !(s.video = s.OpenDecoder(s.format, s.videoStream))) {
    REXLOG_ERROR("movie: {}: no playable video stream", s.name);
    return nullptr;
  }
  s.width = s.video->width;
  s.height = s.video->height;
  AVRational rate = s.format->streams[s.videoStream]->avg_frame_rate;
  if (rate.num > 0 && rate.den > 0) {
    s.frameDuration = av_q2d(av_inv_q(rate));
  }
  if (audio && AudioEnabled() &&
      avformat_open_input(&s.audioFormat, path.string().c_str(), nullptr, nullptr) >= 0 &&
      avformat_find_stream_info(s.audioFormat, nullptr) >= 0) {
    s.audioStream = av_find_best_stream(s.audioFormat, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (s.audioStream >= 0 && (s.audio = s.OpenDecoder(s.audioFormat, s.audioStream)) &&
        !s.OpenAudioOutput()) {
      SDL_DestroyAudioStream(s.audioOut);
      s.audioOut = nullptr;
    }
  }
  REXLOG_INFO("movie: {} {}x{} {}{}{}", s.name, s.video->width, s.video->height,
              avcodec_get_name(s.video->codec_id), s.audioOut ? " with audio" : "",
              loop ? ", looping" : "");
  s.thread = std::thread([&s] { s.Decode(); });
  if (s.audioOut) {
    s.audioThread = std::thread([&s] { s.DecodeAudio(); });
  }
  return movie;
}

bool HostMovie::CopyFrame(const Planes& planes) {
  State& s = *state_;
  auto now = std::chrono::steady_clock::now();
  if (!s.started) {
    s.started = true;
    s.start = now;
    if (s.audioOut) {
      SDL_ResumeAudioStreamDevice(s.audioOut);
    }
  }
  double time = std::chrono::duration<double>(now - s.start).count();

  std::unique_lock lock(s.mutex);
  // Drop frames whose successor is already due.
  bool dropped = false;
  while (s.queue.size() >= 2 && s.queue[1].time <= time) {
    s.queue.pop_front();
    dropped = true;
  }
  if (dropped) {
    s.wake.notify_all();
  }
  if (s.queue.empty()) {
    if (s.endOfFile) {
      s.finished = true;
    }
    return !s.finished;
  }
  const Frame& frame = s.queue.front();
  int widths[3] = {frame.width, (frame.width + 1) / 2, (frame.width + 1) / 2};
  int heights[3] = {frame.height, (frame.height + 1) / 2, (frame.height + 1) / 2};
  for (int p = 0; p < 3; p++) {
    if (!planes.data[p] || !planes.pitch[p]) {
      continue;
    }
    uint32_t rows = std::min<uint32_t>(heights[p], planes.size[p] / planes.pitch[p]);
    uint32_t bytes = std::min<uint32_t>(widths[p], planes.pitch[p]);
    for (uint32_t y = 0; y < rows; y++) {
      std::memcpy(planes.data[p] + size_t(y) * planes.pitch[p],
                  frame.planes[p].data() + size_t(y) * widths[p], bytes);
    }
  }
  s.lastShown = frame.time;
  // The last frame stays up until the movie's end time has passed.
  if (s.endOfFile && s.queue.size() == 1) {
    double duration = s.format->duration > 0 ? double(s.format->duration) / AV_TIME_BASE : 0.0;
    if (time > std::max(frame.time + 0.1, duration)) {
      s.queue.clear();
      s.finished = true;
    }
  }
  return !s.finished;
}

bool HostMovie::Finished() const {
  return state_->finished;
}

int HostMovie::Width() const {
  return state_->width;
}

int HostMovie::Height() const {
  return state_->height;
}

void HostMovie::SetVolume(float volume) {
  if (state_->audioOut) {
    SDL_SetAudioStreamGain(state_->audioOut, std::clamp(volume, 0.0f, 1.0f));
  }
}

}  // namespace ng2::movie
