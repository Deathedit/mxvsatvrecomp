#pragma once

#include <memory>

#include <rex/rex_app.h>

#include "app/graphics_system.h"

// The ReXGlue application. OnPreSetup installs the D3D12 graphics system
// (unless a GPU plugin was requested, in which case the runtime owns
// rendering); OnPostSetup hands it the window handle once one exists.
class MxApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx);

  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;

 private:
  rex::system::D3D12GraphicsSystem* m_graphicsSystem = nullptr;
};
