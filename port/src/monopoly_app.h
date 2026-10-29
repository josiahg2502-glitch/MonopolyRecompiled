// monopoly - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include "netplay_bridge.h"
#include "script_input.h"

REXCVAR_DECLARE(std::string, input_script);
REXCVAR_DECLARE(bool, net_host);
REXCVAR_DECLARE(int32_t, net_port);
REXCVAR_DECLARE(std::string, net_name);

class MonopolyApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MonopolyApp>(new MonopolyApp(ctx, "monopoly",
        PPCImageConfig));
  }

  // Seed the user's shader storage from the precompiled cache shipped next to the exe, so every
  // known shader is translated and its pipeline built at startup, not on first use.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe_dir = rex::filesystem::GetExecutableFolder();
    // Without --game_data_root, look for the extracted disc in "game" next to the exe.
    if (paths.game_data_root.empty() && fs::is_regular_file(exe_dir / "game" / "default.xex", ec))
      paths.game_data_root = exe_dir / "game";
    fs::path shipped = exe_dir / "shader_cache";
    for (auto& entry : fs::recursive_directory_iterator(shipped, ec)) {
      if (!entry.is_regular_file()) continue;
      fs::path dest = paths.cache_root / fs::relative(entry.path(), shipped);
      // Storage files only grow, so a smaller user copy predates the shipped one.
      if (fs::exists(dest) && fs::file_size(dest, ec) >= entry.file_size()) continue;
      fs::create_directories(dest.parent_path(), ec);
      if (!fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec))
        REXLOG_WARN("shader cache: could not seed {}: {}", dest.string(), ec.message());
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The game needs the Xenos GPU plugin; don't require --gpu_plugin on every launch.
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";

    if (REXCVAR_GET(net_host)) {
      std::string error;
      auto host = netplay::Host::Start(uint16_t(REXCVAR_GET(net_port)), REXCVAR_GET(net_name), &error);
      if (!host) {
        REXLOG_ERROR("online: could not host: {}", error);
      } else {
        REXLOG_INFO("online: hosting on port {}; players join with monopoly_net --join <this PC>",
                    REXCVAR_GET(net_port));
        host_ = std::move(host);
        config.audio_factory = [host = host_](rex::runtime::FunctionDispatcher* dispatcher)
            -> std::unique_ptr<rex::system::IAudioSystem> {
          return std::make_unique<monopoly::NetAudioSystem>(dispatcher, host);
        };
      }
    }
  }

  void OnPostSetup() override {
    auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    if (!input) return;
    if (!REXCVAR_GET(input_script).empty()) {
      if (auto driver = monopoly::ScriptInputDriver::Load(REXCVAR_GET(input_script)))
        input->AddDriver(std::move(driver));  // InputSystem re-enumerates drivers on every poll
    }
    if (host_) {
      input->AddDriver(std::make_unique<monopoly::RemoteInputDriver>(host_));
      input->SetDeviceAssignment(std::make_unique<monopoly::NetplayAssignment>());
      capture_ = std::make_unique<monopoly::FrameCapture>(runtime(), host_);
    }
  }

  void OnShutdown() override {
    capture_.reset();  // stop reading the presenter before it goes away
  }

 private:
  std::shared_ptr<netplay::Host> host_;
  std::unique_ptr<monopoly::FrameCapture> capture_;
};
