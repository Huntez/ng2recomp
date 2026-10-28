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

#include "gpu/video.h"
#include "null_audio.h"
#include "script_input.h"

class Ng2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ng2App>(new Ng2App(ctx, "ng2",
        PPCImageConfig));
  }

  // NG2_CONFIG=<file>: settings file instead of <exe_dir>/ng2.toml (test
  // runs keep away from the player's settings).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const char* config = std::getenv("NG2_CONFIG");
    if (config && *config) {
      paths.config_path = config;
    }
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
    // NG2_RENDERER=native: the native renderer draws and presents. The GPU
    // plugin's null backend still runs the game's command stream (fences,
    // interrupts, vblank) so its D3D library works unmodified.
    if (ng2::video::IsNativeRenderer()) {
      config.gpu_plugin.clear();
      config.graphics = rex::system::LoadGpuPlugin("xenos", "null");
      if (!config.graphics) {
        // Without it the game's D3D waits forever on a ring nobody reads.
        REXLOG_ERROR("NG2_RENDERER=native needs the xenos GPU plugin's null backend");
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                  "The GPU plugin (librexgpu-xenos) is missing or too old.");
        std::_Exit(1);
      }
      REXLOG_INFO("NG2_RENDERER=native: GPU emulation off (null command processor)");
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
    if (ng2::video::IsNativeRenderer() && !ng2::video::CreateHostDevice()) {
      REXLOG_ERROR("native renderer: cannot create the Vulkan device");
    }
  }

  void OnShutdown() override {
    if (ng2::video::IsNativeRenderer()) {
      ng2::video::DestroyHostDevice();
    }
  }

  // NG2_DUMP_IMAGE=<path>: write the loaded (decrypted, decompressed) XEX
  // image for offline analysis, e.g. finding code reached only by pointer.
  void OnPostLoadXexImage() override {
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
