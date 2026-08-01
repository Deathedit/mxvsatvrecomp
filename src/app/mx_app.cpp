#include "app/mx_app.h"

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>

#include "hooks/native_bridge.h"
#include "mx_init.h"

std::unique_ptr<rex::ui::WindowedApp> MxApp::Create(
    rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<MxApp>(new MxApp(ctx, "mx", PPCImageConfig));
}

void MxApp::OnPreSetup(rex::RuntimeConfig& config) {
  REXLOG_INFO("MxApp::OnPreSetup");

  if (!config.gpu_plugin.empty()) {
    REXLOG_INFO("MxApp::OnPreSetup - gpu_plugin='{}' requested, deferring to runtime",
                config.gpu_plugin);
    mx::native::g_plugin_mode = true;
    return;
  }

  auto gs = std::make_unique<rex::system::D3D12GraphicsSystem>();
  m_graphicsSystem = gs.get();
  config.graphics = std::move(gs);
}

void MxApp::OnPostSetup() {
  REXLOG_INFO("MxApp::OnPostSetup");

  // Dump all registered cvars (post plugin load — includes GPU plugin cvars)

  /*
  REXLOG_INFO("MxApp::OnPostSetup — dumping all cvars:");
  for (const auto& name : rex::cvar::ListFlags()) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    std::string cur = rex::cvar::GetFlagByName(name);
    bool nondef = rex::cvar::HasNonDefaultValue(name);
    REXLOG_INFO("  cvar: {} = '{}' (default='{}', nondef={})",
                name, cur,
                info ? info->default_value : std::string{"?"},
                nondef);
  }
  */

  rex::ReXApp::OnPostSetup();
  if (m_graphicsSystem == nullptr) {
    REXLOG_INFO("MxApp::OnPostSetup - no graphics system");
    return;
  }
  auto* w = window();
  if (w == nullptr) {
    REXLOG_INFO("MxApp::OnPostSetup - no window");
    return;
  }
  HWND hwnd = static_cast<HWND>(w->GetNativeWindowHandle());
  REXLOG_INFO("MxApp::OnPostSetup - hwnd=0x{:08X}", (uintptr_t)hwnd);
  mx::native::SetWindowHandle(hwnd);
  m_graphicsSystem->InitializeRenderer(hwnd);
  REXLOG_INFO("MxApp::OnPostSetup done");
}
