/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <algorithm>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // XFriendsCreateEnumerator(user, start, count, pcbBuffer*, phEnum*). buffer_length
      // points at the title's argument list: 16-byte entries {u32 type, pad, u64 value},
      // each value the address of the argument. Hand back an empty friends list (titles
      // read pcbBuffer/phEnum without checking the result, so failing here crashes them).
      if (!buffer_length) {
        return X_E_INVALIDARG;
      }
      auto args = memory_->TranslateVirtual<const uint8_t*>(buffer_length);
      auto arg_ptr = [&](int i) {
        return uint32_t(memory::load_and_swap<uint64_t>(args + i * 16 + 8));
      };
      uint32_t user_index = 0, count = 0;
      if (uint32_t p = arg_ptr(0)) user_index = memory::load_and_swap<uint32_t>(
                                       memory_->TranslateVirtual<const uint8_t*>(p));
      if (uint32_t p = arg_ptr(2)) count = memory::load_and_swap<uint32_t>(
                                       memory_->TranslateVirtual<const uint8_t*>(p));
      count = std::clamp<uint32_t>(count, 1, 100);
      constexpr uint32_t kFriendSize = 0xB8;  // XONLINE_FRIEND
      auto e = object_ref<XStaticUntypedEnumerator>(
          new XStaticUntypedEnumerator(kernel_state_, count, kFriendSize));
      auto result = e->Initialize(user_index, app_id(), 0x5801E, 0x5801F, 0);
      if (XFAILED(result)) {
        return X_E_FAIL;
      }
      if (uint32_t p = arg_ptr(3)) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(p),
                                         count * kFriendSize);
      }
      if (uint32_t p = arg_ptr(4)) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(p), e->handle());
      }
      REXKRNL_INFO("XFriendsCreateEnumerator(user {}, count {}): empty friends list",
                   user_index, count);
      return X_E_SUCCESS;
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
