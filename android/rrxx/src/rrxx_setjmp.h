// Guest setjmp/longjmp for the ahead-of-time translated code.
//
// Rumble Roses XX uses the CRT's setjmp (0x824726C0) / longjmp (0x82472470) for libjpeg's
// error handling: its jpeg error_exit (0x822E5320) longjmps back to the decoder's caller.
// Translated literally, the guest longjmp only reloads the guest registers (r1, r13-r31,
// f14-f31, cr, lr) and then *returns* to the host function that called it. Execution then
// carries on in the wrong function with the wrong stack, and a few returns later r31 is 0. In
// 0.1.2 this showed up as the null+0x1A4 read in libjpeg's read_markers (sub_822FD9F8).
//
// The fix pairs each guest setjmp with a host one. rrxx/tools/apply_code_patches.py adds an
// RrxxJmpFrame to every function that calls the guest setjmp, and calls _setjmp in that
// function's own frame. The guest longjmp, after restoring the guest registers, finds the
// frame for its guest jmp_buf and _longjmps there. The host stack then unwinds to the
// function that called setjmp, just as the guest stack does on the console.
#pragma once

#include <setjmp.h>

#include <cstdint>

#include <rex/logging.h>

struct RrxxJmpFrame {
  jmp_buf jb;
  RrxxJmpFrame* next = nullptr;  // older frame on this thread
  uint32_t guest_buf = 0;        // guest jmp_buf address (setjmp's r3)
  uint32_t guest_sp = 0;         // guest r1 at the setjmp call
  bool armed = false;

  RrxxJmpFrame() = default;
  RrxxJmpFrame(const RrxxJmpFrame&) = delete;
  RrxxJmpFrame& operator=(const RrxxJmpFrame&) = delete;
  ~RrxxJmpFrame() { Disarm(); }

  static RrxxJmpFrame*& Top() {
    static thread_local RrxxJmpFrame* top = nullptr;
    return top;
  }

  // Called right before the paired _setjmp in the frame that owns this object.
  void Arm(uint32_t buf, uint32_t sp) {
    guest_buf = buf;
    guest_sp = sp;
    if (!armed) {
      next = Top();
      Top() = this;
      armed = true;
    }
  }

  void Disarm() {
    if (!armed) return;
    for (RrxxJmpFrame** p = &Top(); *p; p = &(*p)->next) {
      if (*p == this) {
        *p = next;
        break;
      }
    }
    armed = false;
  }
};

// Called by the translated guest longjmp after it has restored the guest registers from the
// guest jmp_buf `buf` (r3 already holds the return value). Does not return if a live setjmp
// frame for `buf` exists on this thread.
[[gnu::noinline]] inline void RrxxGuestLongjmp(uint32_t buf, uint32_t guest_sp, uint32_t caller,
                                               uint32_t jpeg_msg_code) {
  RrxxJmpFrame* target = nullptr;
  for (RrxxJmpFrame* f = RrxxJmpFrame::Top(); f; f = f->next) {
    if (f->guest_buf == buf) {
      target = f;
      break;
    }
  }
  static int logged = 0;
  if (logged < 32) {
    ++logged;
    if (caller == 0x822E5354) {
      REXLOG_WARN("RRXXDBG guest longjmp from jpeg error_exit (JPEG message code {}) to {:08X}: {}",
                  jpeg_msg_code, buf, target ? "unwinding to its setjmp" : "NO SETJMP FRAME");
    } else {
      REXLOG_WARN("RRXXDBG guest longjmp from {:08X} to {:08X}: {}", caller, buf,
                  target ? "unwinding to its setjmp" : "NO SETJMP FRAME");
    }
  }
  if (!target) return;
  if (target->guest_sp != guest_sp) {
    REXLOG_WARN("RRXXDBG guest longjmp: stack {:08X} != setjmp's {:08X}", guest_sp,
                target->guest_sp);
  }
  // Frames newer than the target are being unwound without their destructors running.
  RrxxJmpFrame::Top() = target;
  _longjmp(target->jb, 1);
}

// After a longjmp lands, the guest registers were changed through `ctx` by functions called
// deeper down; make sure nothing cached from before the _setjmp is reused (ctx is __restrict).
[[gnu::noinline]] inline void RrxxCtxBarrier(void* ctx) { asm volatile("" : : "r"(ctx) : "memory"); }

// Replaces `sub_824726C0(ctx, base);` (guest setjmp) at a call site. _setjmp has to run in
// the calling function's own frame, so this is a macro.
#define RRXX_GUEST_SETJMP(frame)                                \
  do {                                                          \
    (frame).Arm(ctx.r3.u32, ctx.r1.u32);                        \
    if (_setjmp((frame).jb) == 0) {                             \
      sub_824726C0(ctx, base);                                  \
    } else { /* the guest longjmp restored ctx, with r3 = value */ \
      RrxxCtxBarrier(&ctx);                                     \
    }                                                           \
  } while (0)
