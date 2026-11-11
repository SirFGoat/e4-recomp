/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>

REXCVAR_DEFINE_BOOL(xlive_trace, false, "Kernel",
                    "Log every XAM app message (Xbox LIVE / stats / sessions) with its buffer");

namespace rex {
namespace system {
namespace xam {

namespace {
void TraceMessage(memory::Memory* memory, const char* kind, uint32_t app_id, uint32_t message,
                  uint32_t buffer_ptr, uint32_t buffer_length) {
  if (!REXCVAR_GET(xlive_trace)) {
    return;
  }
  std::string words;
  if (buffer_ptr) {
    auto buffer = memory->TranslateVirtual<const uint8_t*>(buffer_ptr);
    uint32_t n = std::min<uint32_t>(buffer_length ? buffer_length / 4 : 8, 16);
    for (uint32_t i = 0; i < n; ++i) {
      words += fmt::format(" {:08X}", memory::load_and_swap<uint32_t>(buffer + i * 4));
    }
  }
  REXKRNL_INFO("xlive_trace {} app={:02X} msg={:08X} buf={:08X} len={}:{}", kind, app_id, message,
               buffer_ptr, buffer_length, words);
}
}  // namespace

App::App(KernelState* kernel_state, uint32_t app_id)
    : kernel_state_(kernel_state), memory_(kernel_state->memory()), app_id_(app_id) {}

void AppManager::RegisterApp(std::unique_ptr<App> app) {
  assert_zero(app_lookup_.count(app->app_id()));
  app_lookup_.insert({app->app_id(), app.get()});
  apps_.push_back(std::move(app));
}

X_HRESULT AppManager::DispatchMessageSync(uint32_t app_id, uint32_t message, uint32_t buffer_ptr,
                                          uint32_t buffer_length) {
  App* app;
  {
    auto it = app_lookup_.find(app_id);
    if (it == app_lookup_.end()) {
      return X_E_NOTFOUND;
    }
    app = it->second;
  }
  TraceMessage(REX_KERNEL_STATE()->memory(), "sync", app_id, message, buffer_ptr, buffer_length);
  X_HRESULT result = app->DispatchMessageSync(message, buffer_ptr, buffer_length);
  if (REXCVAR_GET(xlive_trace)) {
    REXKRNL_INFO("xlive_trace   -> {:08X}", uint32_t(result));
  }
  return result;
}

X_HRESULT AppManager::DispatchMessageAsync(uint32_t app_id, uint32_t message, uint32_t buffer_ptr,
                                           uint32_t buffer_length) {
  App* app;
  {
    auto it = app_lookup_.find(app_id);
    if (it == app_lookup_.end()) {
      return X_E_NOTFOUND;
    }
    app = it->second;
  }
  TraceMessage(REX_KERNEL_STATE()->memory(), "async", app_id, message, buffer_ptr, buffer_length);
  X_HRESULT result = app->DispatchMessageSync(message, buffer_ptr, buffer_length);
  if (REXCVAR_GET(xlive_trace)) {
    REXKRNL_INFO("xlive_trace   -> {:08X}", uint32_t(result));
  }
  return result;
}

}  // namespace xam
}  // namespace system
}  // namespace rex
