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

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <vector>

// Rumble Roses XX Android port: network play over the local Wi-Fi.
// Live matchmaking is replaced by LAN discovery (xam_net.cpp): a host
// advertises its session, XSessionSearch lists the hosts that answer.
REXCVAR_DECLARE(bool, net_play);

namespace rex::kernel::xam {
struct NetPlayAdvert {
  uint32_t ip = 0;
  std::vector<uint8_t> blob;
};
void NetPlayHostAdvertise(const std::vector<uint8_t>& blob);
void NetPlayHostStop();
std::vector<NetPlayAdvert> NetPlaySearch(int timeout_ms);
uint32_t NetPlayFillXnAddr(uint8_t* out);

namespace {
constexpr uint32_t kSessionInfoSize = 60;   // XNKID + XNADDR + XNKEY
constexpr uint32_t kSearchResultSize = 92;  // XSESSION_SEARCHRESULT
constexpr uint32_t kContextSize = 8;        // XUSER_CONTEXT
constexpr uint32_t kPropertySize = 24;      // XUSER_PROPERTY (id, pad, XUSER_DATA)

struct NetSession {
  bool active = false;
  bool host = false;
  uint32_t flags = 0;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
  uint32_t filled_public = 0;
  uint32_t filled_private = 0;
  uint64_t nonce = 0;
  uint8_t info[kSessionInfoSize] = {};
};

std::mutex g_net_mutex;
NetSession g_session;
std::map<uint32_t, uint32_t> g_contexts;                   // id -> value
std::map<uint32_t, std::vector<uint8_t>> g_properties;     // id -> raw big-endian value

uint64_t RandomU64() {
  static std::mt19937_64 rng{std::random_device{}()};
  return rng();
}

void PutBe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (i * 8)));
}
uint32_t GetBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Advert: info[60], open/filled public/private (4 x u32), contexts, properties.
std::vector<uint8_t> BuildAdvertLocked() {
  std::vector<uint8_t> blob(g_session.info, g_session.info + kSessionInfoSize);
  uint32_t used_public = std::min(g_session.filled_public, g_session.public_slots);
  uint32_t used_private = std::min(g_session.filled_private, g_session.private_slots);
  PutBe32(blob, g_session.public_slots - used_public);
  PutBe32(blob, g_session.private_slots - used_private);
  PutBe32(blob, used_public);
  PutBe32(blob, used_private);
  PutBe32(blob, uint32_t(g_contexts.size()));
  for (auto& [id, value] : g_contexts) {
    PutBe32(blob, id);
    PutBe32(blob, value);
  }
  PutBe32(blob, uint32_t(g_properties.size()));
  for (auto& [id, raw] : g_properties) {
    PutBe32(blob, id);
    uint8_t value[8] = {};
    std::memcpy(value, raw.data(), std::min<size_t>(raw.size(), 8));
    blob.insert(blob.end(), value, value + 8);
  }
  return blob;
}

void RefreshAdvertLocked() {
  if (REXCVAR_GET(net_play) && g_session.active && g_session.host) {
    NetPlayHostAdvertise(BuildAdvertLocked());
  }
}

// XSessionSearch results: header {count, pResults}, the result array, then each
// result's contexts and properties, all inside the title's buffer.
uint32_t WriteSearchResults(rex::memory::Memory* memory, uint32_t header_ptr, uint32_t buffer_size,
                            uint32_t max_results, const std::vector<NetPlayAdvert>& adverts,
                            uint32_t* required_size) {
  struct Parsed {
    const uint8_t* info;
    uint32_t slots[4];
    std::vector<std::pair<uint32_t, uint32_t>> contexts;
    std::vector<std::pair<uint32_t, const uint8_t*>> properties;
  };
  std::vector<Parsed> results;
  for (const auto& advert : adverts) {
    const auto& b = advert.blob;
    size_t pos = kSessionInfoSize + 16;
    if (b.size() < pos + 4) continue;
    Parsed parsed;
    parsed.info = b.data();
    for (int i = 0; i < 4; ++i) parsed.slots[i] = GetBe32(b.data() + kSessionInfoSize + i * 4);
    uint32_t nctx = GetBe32(b.data() + pos);
    pos += 4;
    if (b.size() < pos + nctx * 8 + 4) continue;
    for (uint32_t i = 0; i < nctx; ++i, pos += 8) {
      parsed.contexts.push_back({GetBe32(b.data() + pos), GetBe32(b.data() + pos + 4)});
    }
    uint32_t nprop = GetBe32(b.data() + pos);
    pos += 4;
    if (b.size() < pos + nprop * 12) continue;
    for (uint32_t i = 0; i < nprop; ++i, pos += 12) {
      parsed.properties.push_back({GetBe32(b.data() + pos), b.data() + pos + 4});
    }
    results.push_back(std::move(parsed));
    if (results.size() >= max_results) break;
  }

  uint32_t needed = 8 + uint32_t(results.size()) * kSearchResultSize;
  for (auto& r : results) {
    needed += uint32_t(r.contexts.size()) * kContextSize +
              uint32_t(r.properties.size()) * kPropertySize;
  }
  *required_size = needed;
  if (buffer_size < needed || !header_ptr) {
    return 0;
  }

  uint8_t* base = memory->TranslateVirtual(header_ptr);
  std::memset(base, 0, needed);
  uint32_t results_ptr = header_ptr + 8;
  uint32_t extra_ptr = results_ptr + uint32_t(results.size()) * kSearchResultSize;
  memory::store_and_swap<uint32_t>(base + 0, uint32_t(results.size()));
  memory::store_and_swap<uint32_t>(base + 4, results.empty() ? 0 : results_ptr);
  for (size_t i = 0; i < results.size(); ++i) {
    auto& r = results[i];
    uint8_t* out = memory->TranslateVirtual(results_ptr + uint32_t(i) * kSearchResultSize);
    std::memcpy(out, r.info, kSessionInfoSize);
    for (int k = 0; k < 4; ++k) memory::store_and_swap<uint32_t>(out + 60 + k * 4, r.slots[k]);
    memory::store_and_swap<uint32_t>(out + 76, uint32_t(r.properties.size()));
    memory::store_and_swap<uint32_t>(out + 80, uint32_t(r.contexts.size()));
    uint32_t props_ptr = r.properties.empty() ? 0 : extra_ptr;
    for (auto& [id, value] : r.properties) {
      uint8_t* p = memory->TranslateVirtual(extra_ptr);
      memory::store_and_swap<uint32_t>(p + 0, id);
      p[8] = uint8_t(id >> 28);  // XUSER_DATA type lives in the id's top nibble
      std::memcpy(p + 16, value, 8);
      extra_ptr += kPropertySize;
    }
    uint32_t ctx_ptr = r.contexts.empty() ? 0 : extra_ptr;
    for (auto& [id, value] : r.contexts) {
      uint8_t* p = memory->TranslateVirtual(extra_ptr);
      memory::store_and_swap<uint32_t>(p + 0, id);
      memory::store_and_swap<uint32_t>(p + 4, value);
      extra_ptr += kContextSize;
    }
    memory::store_and_swap<uint32_t>(out + 84, props_ptr);
    memory::store_and_swap<uint32_t>(out + 88, ctx_ptr);
  }
  return uint32_t(results.size());
}

// pcbResultsBuffer is a DWORD*; some callers pass the size itself.
uint32_t ReadSizeArg(rex::memory::Memory* memory, uint32_t value, uint32_t** size_out) {
  if (value >= 0x10000 && memory->LookupHeap(value)) {
    *size_out = reinterpret_cast<uint32_t*>(memory->TranslateVirtual(value));
    return memory::load_and_swap<uint32_t>(*size_out);
  }
  *size_out = nullptr;
  return value;
}

X_HRESULT NetPlaySearchMessage(rex::memory::Memory* memory, const char* name, uint32_t num_results,
                               uint32_t size_arg, uint32_t results_ptr) {
  uint32_t* size_ptr = nullptr;
  uint32_t buffer_size = ReadSizeArg(memory, size_arg, &size_ptr);
  auto adverts = NetPlaySearch(1500);
  uint32_t required = 0;
  uint32_t written =
      WriteSearchResults(memory, results_ptr, buffer_size, num_results ? num_results : 16, adverts,
                         &required);
  REXKRNL_INFO("[net] {}: {} session(s) found, buffer {} needed {}", name, adverts.size(),
               buffer_size, required);
  if (required > buffer_size || !results_ptr) {
    if (size_ptr) memory::store_and_swap<uint32_t>(size_ptr, required);
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  (void)written;
  return X_E_SUCCESS;
}
}  // namespace
}  // namespace rex::kernel::xam

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      if (REXCVAR_GET(net_play) && user_index == 0) {
        std::lock_guard<std::mutex> lock(g_net_mutex);
        g_contexts[context_id] = context_value;
        RefreshAdvertLocked();
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      if (REXCVAR_GET(net_play) && user_index == 0 && value_ptr && value_size <= 8) {
        auto* value = memory_->TranslateVirtual<uint8_t*>(value_ptr);
        REXKRNL_INFO("[net] XGIUserSetPropertyEx({:08X}, {} bytes)", property_id, value_size);
        std::lock_guard<std::mutex> lock(g_net_mutex);
        g_properties[property_id] = std::vector<uint8_t>(value, value + value_size);
        RefreshAdvertLocked();
      }
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      // Raw dump so we can confirm the actual buffer layout the game sends.
      uint32_t raw0 = buffer_length >= 4 ? memory::load_and_swap<uint32_t>(buffer + 0) : 0;
      uint32_t raw4 = buffer_length >= 8 ? memory::load_and_swap<uint32_t>(buffer + 4) : 0;
      REXKRNL_INFO("XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = raw0;
      uint32_t achievements_ptr = raw4;

      // Empirically confirmed from log: each entry is {u32 padding/user_index, u32 id, ...}.
      // The achievement ID sits at offset 4, not 0. Stride 8 covers the observed fields.
      constexpr uint32_t kEntryIdOffset = 4;
      constexpr uint32_t kEntryStride = 8;
      constexpr uint32_t kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXKRNL_WARN("XGIUserWriteAchievements: count={} unreasonable, ignoring",
                       achievement_count);
          return X_E_FAIL;
        }
        uint32_t span_end = achievements_ptr + achievement_count * kEntryStride - 1;
        if (!memory_->LookupHeap(achievements_ptr) || !memory_->LookupHeap(span_end)) {
          REXKRNL_WARN("XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return X_E_FAIL;
        }
        auto* base = memory_->TranslateVirtual(achievements_ptr);
        for (uint32_t i = 0; i < achievement_count; ++i) {
          uint32_t id = memory::load_and_swap<uint32_t>(base + i * kEntryStride + kEntryIdOffset);
          REXKRNL_INFO("XGIUserWriteAchievements: id={} ({})", id, i);
          kernel_state_->UnlockAchievement(id);
        }
      } else {
        REXKRNL_INFO("XGIUserWriteAchievements: skipped (count={} ptr={:08X})", achievement_count,
                     achievements_ptr);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t user_xuid = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);
      if (REXCVAR_GET(net_play)) {
        std::lock_guard<std::mutex> lock(g_net_mutex);
        g_session = NetSession();
        g_session.active = true;
        g_session.host = (flags & 1) != 0;  // XSESSION_CREATE_HOST
        g_session.flags = flags;
        g_session.public_slots = num_slots_public;
        g_session.private_slots = num_slots_private;
        g_session.nonce = RandomU64();
        uint8_t* info = session_info_ptr ? memory_->TranslateVirtual(session_info_ptr) : nullptr;
        if (g_session.host) {
          uint64_t kid = RandomU64();
          std::memcpy(g_session.info, &kid, 8);
          g_session.info[0] = 0x80 | (g_session.info[0] & 0x1F);  // online peer session
          NetPlayFillXnAddr(g_session.info + 8);
          uint64_t key[2] = {RandomU64(), RandomU64()};
          std::memcpy(g_session.info + 44, key, 16);
          if (info) std::memcpy(info, g_session.info, kSessionInfoSize);
        } else if (info) {
          // Joining: the title passes the XSESSION_INFO it got from the search.
          std::memcpy(g_session.info, info, kSessionInfoSize);
        }
        if (nonce_ptr) {
          memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(nonce_ptr), g_session.nonce);
        }
        REXKRNL_INFO("[net] XSessionCreate(flags {:08X}, {} public, {} private) as {}", flags,
                     num_slots_public, num_slots_private, g_session.host ? "host" : "guest");
        RefreshAdvertLocked();
      }
      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      if (REXCVAR_GET(net_play)) {
        REXKRNL_INFO("[net] XSessionDelete");
        std::lock_guard<std::mutex> lock(g_net_mutex);
        if (g_session.host) NetPlayHostStop();
        g_session = NetSession();
      }

      return X_E_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);
      bool is_local = xuid_array_ptr == 0;

      REXKRNL_DEBUG("{}({:08X}, {}, {}, {:08X}, {:08X})",
                    is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", session_ptr,
                    user_count, xuid_array_ptr, user_index_array, private_slots_array);
      if (REXCVAR_GET(net_play)) {
        REXKRNL_INFO("[net] {}({} user(s))", is_local ? "XSessionJoinLocal" : "XSessionJoinRemote",
                     user_count);
        std::lock_guard<std::mutex> lock(g_net_mutex);
        g_session.filled_public += user_count;
        RefreshAdvertLocked();
      }
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      if (REXCVAR_GET(net_play)) REXKRNL_INFO("[net] XSessionStart");

      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      if (REXCVAR_GET(net_play)) REXKRNL_INFO("[net] XSessionEnd");

      return X_E_SUCCESS;
    }
    case 0x000B0016: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);
      if (REXCVAR_GET(net_play)) {
        REXKRNL_INFO("[net] XSessionSearch(query {}, {} results, {} props, {} contexts)",
                     proc_index, num_results, num_props, num_ctx);
        return NetPlaySearchMessage(memory_, "XSessionSearch", num_results, results_buffer_size,
                                    search_results_ptr);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t maxPublicSlots = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t maxPrivateSlots = memory::load_and_swap<uint16_t>(buffer + 12);

      REXKRNL_DEBUG("XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                    maxPublicSlots, maxPrivateSlots);

      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 36);

      // session_search
      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      //
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 32);

      REXKRNL_DEBUG("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);
      if (REXCVAR_GET(net_play)) {
        REXKRNL_INFO("[net] XSessionSearchEx(query {}, {} results)", proc_index, num_results);
        return NetPlaySearchMessage(memory_, "XSessionSearchEx", num_results, results_buffer_size,
                                    search_results_ptr);
      }

      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t details_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_details_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                    details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);
      if (REXCVAR_GET(net_play) && session_details_ptr) {
        // XSESSION_LOCAL_DETAILS, without the member list.
        std::lock_guard<std::mutex> lock(g_net_mutex);
        uint8_t* d = memory_->TranslateVirtual(session_details_ptr);
        std::memset(d, 0, 128);
        auto ctx = [](uint32_t id) {
          auto it = g_contexts.find(id);
          return it == g_contexts.end() ? 0u : it->second;
        };
        uint32_t members = std::max<uint32_t>(1, g_session.filled_public + g_session.filled_private);
        memory::store_and_swap<uint32_t>(d + 0, 0);                         // host user index
        memory::store_and_swap<uint32_t>(d + 4, ctx(0x0000800A));          // game type
        memory::store_and_swap<uint32_t>(d + 8, ctx(0x0000800B));          // game mode
        memory::store_and_swap<uint32_t>(d + 12, g_session.flags);
        memory::store_and_swap<uint32_t>(d + 16, g_session.public_slots);
        memory::store_and_swap<uint32_t>(d + 20, g_session.private_slots);
        memory::store_and_swap<uint32_t>(
            d + 24, g_session.public_slots - std::min(g_session.public_slots, g_session.filled_public));
        memory::store_and_swap<uint32_t>(
            d + 28,
            g_session.private_slots - std::min(g_session.private_slots, g_session.filled_private));
        memory::store_and_swap<uint32_t>(d + 32, members);
        memory::store_and_swap<uint32_t>(d + 36, 0);                        // returned members
        memory::store_and_swap<uint32_t>(d + 40, 1);                        // XSESSION_STATE_LOBBY
        memory::store_and_swap<uint64_t>(d + 48, g_session.nonce);
        std::memcpy(d + 56, g_session.info, kSessionInfoSize);
        REXKRNL_INFO("[net] XSessionGetDetails -> {} member(s)", members);
      }

      return X_E_SUCCESS;
    }
    case 0x000B001E: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                    session_info_ptr, user_index, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XSessionGetInvitationData - unimplemented({}, {:08X})", user_index,
                    session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t session_duration_sec = memory::load_and_swap<uint32_t>(buffer + 16);  // 300
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
                    obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
                    results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                    context_id);
      uint32_t value = 0;
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0060: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 52);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  if (REXCVAR_GET(net_play)) {
    REXKRNL_INFO("[net] unhandled XGI message {:08X} (len {})", message, buffer_length);
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
