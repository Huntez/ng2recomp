// ng2 - Ninja Gaiden II static recompilation
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdio>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/system.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>

#include "aspect.h"
#include "dlc.h"
#include "gameplay.h"
#include "gpu/video.h"
#include "movie/movie.h"
#include "null_audio.h"
#include "recomp_settings.h"
#include "script_input.h"
#include "windows_setup.h"

class Ng2App : public rex::ReXApp {
 public:
  // Title Update 2's default.xexp (the AppImage also checks its SHA-1).
  static constexpr uintmax_t kTitleUpdate2Size = 2318336;
  static constexpr uint32_t kSystemUiNotification = 0x9;  // XN_SYS_UI
  bool paused_in_background_ = false;

  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ng2App>(new Ng2App(ctx, "ng2",
        PPCImageConfig));
  }

  // NG2_CONFIG=<file>: settings file instead of <exe_dir>/ng2.toml (test
  // runs keep away from the player's settings).
  void OnConfigurePaths(rex::PathConfig& paths) override {
#ifdef _WIN32
    // The game folder and Title Update 2 (asked for once), settings and
    // saves in %LOCALAPPDATA%: what the AppImage's launcher does on Linux.
    if (!ng2::windows_setup::ConfigurePaths(paths)) {
      std::_Exit(0);
    }
#endif
    const char* config = std::getenv("NG2_CONFIG");
    if (config && *config) {
      paths.config_path = config;
    }
    ng2::recomp_settings::SetConfigPath(paths.config_path);
    ng2::video::SetPipelineListPath(paths.config_path.parent_path() / "ng2-pipelines.bin");
    ng2::recomp_settings::SetAppContext(&app_context());
  }

  // Game-specific defaults (from Xenia's compatibility notes for 544307D5),
  // unless the user set them: the game reads the zero page.
  void OnPostInitLogging() override {
    const auto* info = rex::cvar::GetFlagInfo("protect_zero");
    if (info && info->source == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("protect_zero", "false");
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The native renderer (unless NG2_RENDERER=xenos) draws and presents. The GPU
    // plugin's null backend still runs the game's command stream (fences,
    // interrupts, vblank) so its D3D library works unmodified.
    if (ng2::video::IsNativeRenderer()) {
      config.gpu_plugin.clear();
      config.graphics = rex::system::LoadGpuPlugin("xenos", "null");
      if (!config.graphics) {
        // Without it the game's D3D waits forever on a ring nobody reads.
        REXLOG_ERROR("the native renderer needs the xenos GPU plugin's null backend");
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                  "The GPU plugin (librexgpu-xenos) is missing or too old.");
        std::_Exit(1);
      }
      REXLOG_INFO("native renderer: GPU emulation off (null command processor)");
    }

    // NG2_AUDIO=0: run the audio pipeline without an output device.
    const char* audio = std::getenv("NG2_AUDIO");
    if (audio && audio[0] == '0') {
      config.audio_factory = REX_AUDIO_BACKEND(ng2::NullAudioSystem);
      REXLOG_INFO("NG2_AUDIO=0: sound off (no output device)");
    }

    // NG2_PAD_SCRIPT / NG2_PAD_LIVE: a scripted controller for player one.
    if (std::getenv("NG2_PAD_SCRIPT") || std::getenv("NG2_PAD_LIVE")) {
      config.input_factory = [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
        auto input = rex::input::CreateDefaultInputSystem(tool_mode);
        if (auto script = ng2::ScriptInputDriver::FromEnvironment()) {
          input->AddDriver(std::move(script));
        }
        return input;
      };
    }
  }

  void OnPostSetup() override {
    // The recompiled code is Title Update 2's. Without the update next to
    // default.xex the game's data would be 1.0's, which that code misreads.
    std::error_code ec;
    if (std::filesystem::file_size(game_data_root() / "default.xexp", ec) != kTitleUpdate2Size) {
      REXLOG_ERROR("No Title Update 2 default.xexp in {}", game_data_root().string());
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                "Title Update 2 is missing: its default.xexp must be next to "
                                "default.xex in the game folder.");
      std::_Exit(1);
    }
    ng2::movie::SetGameDataRoot(game_data_root());
    if (ng2::video::IsNativeRenderer() && !ng2::video::CreateHostDevice()) {
      REXLOG_ERROR("native renderer: cannot create the Vulkan device");
    }
    ng2::recomp_settings::ApplyWindowAtStart();
  }

  void OnShutdown() override {
    if (ng2::video::IsNativeRenderer()) {
      ng2::video::DestroyHostDevice();
    }
  }

  // Pause in Background: the Guide's notification (XN_SYS_UI) pauses the
  // game while the window isn't focused.
  void OnWindowFocusChanged(bool focused) override {
    auto* kernel = runtime() ? runtime()->kernel_state() : nullptr;
    if (!kernel) {
      return;
    }
    bool pause = !focused && ng2::gameplay::PauseInBackground();
    if (pause != paused_in_background_) {
      paused_in_background_ = pause;
      kernel->BroadcastNotification(kSystemUiNotification, pause ? 1 : 0);
    }
  }

  // Downloadable content from the game folder's dlc/ (installed once).
  void OnPreLaunchModule() override {
    ng2::dlc::InstallPackages(runtime()->kernel_state(), game_data_root(), user_data_root());
  }

  // NG2_DUMP_IMAGE=<path>: write the loaded (decrypted, decompressed) XEX
  // image for offline analysis, e.g. finding code reached only by pointer.
  void OnPostLoadXexImage() override {
    // Aspect Ratio (aspect.cpp) writes the game's 16:9 constant, which sits
    // in read-only image data: its page is made writable.
    auto* heap = runtime()->memory()->LookupHeap(ng2::aspect::kConstantAddress);
    if (!heap || !heap->Protect(ng2::aspect::kConstantAddress, 4,
                                rex::memory::kMemoryProtectRead |
                                    rex::memory::kMemoryProtectWrite)) {
      REXLOG_ERROR("aspect ratio: the game's aspect constant can't be made writable");
    } else {
      ng2::aspect::SetConstantWritable(true);
    }
    const char* path = std::getenv("NG2_DUMP_IMAGE");
    if (!path || !*path) {
      return;
    }
    const uint8_t* image =
        runtime()->memory()->TranslateVirtual<const uint8_t*>(REX_IMAGE_BASE);
    FILE* f = std::fopen(path, "wb");
    if (!f) {
      REXLOG_ERROR("NG2_DUMP_IMAGE: cannot open {}", path);
      return;
    }
    size_t written = std::fwrite(image, 1, REX_IMAGE_SIZE, f);
    std::fclose(f);
    REXLOG_INFO("NG2_DUMP_IMAGE: wrote {} of {} bytes to {}", written,
                REX_IMAGE_SIZE, path);
  }
};
