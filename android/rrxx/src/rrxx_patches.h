// Rumble-XX patches for Rumble Roses XX (github.com/Rumble-XX/Rumble-Roses-XX,
// "4B4E07D1 - Rumble Roses Double X.patch.toml", NA/JP release), as launcher options.
//
// Five of them change instructions; this build's code is translated ahead of time, so those
// sites are gated in the generated code on bits of g_rrxx_code_patches
// (rrxx/tools/apply_code_patches.py). The other two change data and are written into guest
// memory once the XEX is loaded, as Xenia does, after checking the original bytes are there.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(rrxx_blur_off, false, "Patches", "Rumble-XX: Blur Off");
REXCVAR_DEFINE_INT32(rrxx_bloom, 0, "Patches",
                     "Rumble-XX bloom: 0 normal, 1 slight (Bloom Slight Off 2), 2 off (both)");
REXCVAR_DEFINE_BOOL(rrxx_no_weapon_icons, false, "Patches", "Rumble-XX: Remove Weapon Icons");
REXCVAR_DEFINE_BOOL(rrxx_no_queen_banner, false, "Patches", "Rumble-XX: Remove Queen Match Banner");
REXCVAR_DEFINE_BOOL(rrxx_no_crowds, false, "Patches", "Rumble-XX: Remove Background Crowds");
REXCVAR_DEFINE_BOOL(rrxx_ultrawide, false, "Patches",
                    "Rumble-XX: Change Ratio to 21:9 (needs present_letterbox = false)");

extern "C" {
// Read by the generated code at the patched instructions; set before the game starts.
uint32_t g_rrxx_code_patches = 0;
}

namespace rrxx_patches {

enum : uint32_t {
  kBlurOff = 0x1,      // 8226BF20, 8226C0A4 (li r6,0), skip the call at 8226DE14
  kBloomOff1 = 0x2,    // 8226C12C
  kBloomOff2 = 0x4,    // 8226C150
  kNoWeaponIcons = 0x8,  // 8228A7EC
  kNoQueenBanner = 0x10,  // 822A5B84
};

// Writes guest bytes if the current ones are `expected` (the game version the patch is for).
inline bool WriteChecked(const char* name, uint32_t address, const uint8_t* expected,
                         const uint8_t* bytes, uint32_t size) {
  auto* rt = rex::Runtime::instance();
  auto* memory = rt ? rt->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  if (!heap) {
    REXLOG_WARN("Patch '{}': no guest memory at {:08X}", name, address);
    return false;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (std::memcmp(host, expected, size) != 0) {
    REXLOG_WARN("Patch '{}': unexpected bytes at {:08X} (another game version?), not applied", name,
                address);
    return false;
  }
  constexpr uint32_t writable = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
  uint32_t old_protect{};
  if (!heap->Protect(address, size, writable, &old_protect)) {
    REXLOG_WARN("Patch '{}': can't make {:08X} writable", name, address);
    return false;
  }
  std::memcpy(host, bytes, size);
  heap->Protect(address, size, old_protect, nullptr);
  return true;
}

inline void Apply() {
  uint32_t code = 0;
  if (REXCVAR_GET(rrxx_blur_off)) code |= kBlurOff;
  const int32_t bloom = REXCVAR_GET(rrxx_bloom);
  if (bloom >= 1) code |= kBloomOff2;
  if (bloom >= 2) code |= kBloomOff1;
  if (REXCVAR_GET(rrxx_no_weapon_icons)) code |= kNoWeaponIcons;
  if (REXCVAR_GET(rrxx_no_queen_banner)) code |= kNoQueenBanner;
  g_rrxx_code_patches = code;

  bool crowds = false, ultrawide = false;
  if (REXCVAR_GET(rrxx_no_crowds)) {
    const uint8_t expected[1] = {0x01}, value[1] = {0x00};
    crowds = WriteChecked("Remove Background Crowds", 0x82498DAF, expected, value, 1);
  }
  if (REXCVAR_GET(rrxx_ultrawide)) {
    // 16:9 (1.7778f) -> 21.5:9 (2.3889f), big-endian float.
    const uint8_t expected[4] = {0x3F, 0xE3, 0x8E, 0x39}, value[4] = {0x40, 0x18, 0xE3, 0x8E};
    ultrawide = WriteChecked("Change Ratio to 21:9", 0x82012D00, expected, value, 4);
  }
  REXLOG_INFO(
      "Rumble-XX patches: blur off {}, bloom {}, no weapon icons {}, no queen banner {}, "
      "no crowds {}, 21:9 {}",
      (code & kBlurOff) != 0, bloom <= 0 ? "normal" : bloom == 1 ? "slight" : "off",
      (code & kNoWeaponIcons) != 0, (code & kNoQueenBanner) != 0, crowds, ultrawide);
}

}  // namespace rrxx_patches
