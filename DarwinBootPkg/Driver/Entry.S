// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>

ASM_FUNC(DarwinCurrentEl)
  mrs x0, CurrentEL
  ret

// x0=boot_args PA, x1=entry PA, x2=owned PA, x3=owned size, x4=frame buffer
// PA (0 for none), x5=its size in bytes. EL1 only.
//
// The bands painted below are stage indications, not proof: a band that shows means the
// store to that pixel landed, and a band that does not show cannot prove the code that
// paints it never ran. The surface, its colour order, or the panel's own refresh can hide
// a store that did happen.
//
ASM_FUNC(DarwinEnter)
  msr daifset, #15
  //
  // Top 1/16 of the framebuffer, before cache maintenance, MMU shutdown and the branch.
  //
  // 0xFF00FFFF is a BGR-ordered cyan, as x10 is built here. The channel order of the surface
  // this board hands over has not been checked, so read the marker as "this function was
  // reached" rather than as a specific colour; the white band below is what tells the two
  // apart whatever the order turns out to be.
  //
  mov x8, x4                    // framebuffer base, kept for the markers below
  mov x13, x5                   // its size in bytes
  cbz x4, 2f
  lsr x9, x5, #4
  bic x9, x9, #7
  mov w10, #0xFFFF             // blue + green
  movk w10, #0xFF00, lsl #16   // alpha, red = 0 -> cyan
  mov x11, x4
3:
  stp w10, w10, [x11], #8
  subs x9, x9, #8
  b.hi 3b
  dsb sy
2:
  add x5, x2, x3
  mrs x6, ctr_el0
  ubfx x6, x6, #16, #4
  mov x7, #4
  lsl x6, x7, x6
  sub x7, x6, #1
  bic x4, x2, x7
  dsb sy

  //
  // A white band under the first one, before the cache walk.
  //
  // The first band says this function was entered; white says the parameters for the walk
  // were computed. The walk covers the whole window one cache line at a time and is the next
  // place this can stall, so the pair narrows a stop to "the arithmetic above" or "the walk"
  // - as a hint about where execution got to, not as proof of where it did not.
  //
  cbz x13, 6f
  lsr x9, x13, #5
  bic x9, x9, #7
  add x11, x8, x13, lsr #5
  mov w10, #0xFFFF
  movk w10, #0xFFFF, lsl #16
7:
  stp w10, w10, [x11], #8
  subs x9, x9, #8
  b.hi 7b
  dsb sy
6:

1:
  dc civac, x4
  add x4, x4, x6
  cmp x4, x5
  b.lo 1b
  dsb sy
  ic iallu
  dsb sy
  isb
  mrs x4, sctlr_el1
  bic x4, x4, #1
  bic x4, x4, #4
  bic x4, x4, #0x1000
  msr sctlr_el1, x4
  isb
  tlbi vmalle1
  dsb sy
  isb

  //
  // A third band, painted after the MMU is off and immediately before the branch.
  //
  // It is drawn last, so if it is seen and nothing follows, the stop is at or after the
  // branch to the kernel. That is as far as the marker goes: the band cannot show that the
  // kernel's first instruction ran or did not run, and a missing band is not evidence that
  // this code was skipped.
  //
  // x1 is the entry and must survive; everything else is free.
  //
  cbz x13, 5f
  lsr x9, x13, #4
  bic x9, x9, #7
  add x11, x8, x13, lsr #4     // x11 = framebuffer + top 1/16
  //
  // The same 0xFF00FFFF as the first band: read as BGR that is cyan, and it is left as it is
  // because the colour is a reach indication, not a fixed value this loader relies on.
  //
  mov w10, #0xFFFF             // the two high channels are set below
  movk w10, #0xFF00, lsl #16   // one of them to FF, the other left at 00
4:
  stp w10, w10, [x11], #8
  subs x9, x9, #8
  b.hi 4b
  dsb sy
5:

  //
  // Arm the SoC watchdog, so a kernel that stops does not need a person to hold a button.
  //
  // These are the register offsets this loader was written against, and the values are kept
  // exactly as they were. They are hardware values, not calibrated timings: earlier comments
  // read them as milliseconds and as three minutes, and that reading has not been verified
  // here, so no wait time should be inferred from them, nor anything about how a running
  // kernel treats this timer.
  //
  //   WDT_RST        0x04  any write resets
  //   WDT_EN         0x08  1 enables
  //   WDT_BARK_TIME  0x10  pre-timeout tick value
  //   WDT_BITE_TIME  0x14  reset tick value
  //
  ldr x14, =0x17C10000
  ldr w15, =180000
  str w15, [x14, #0x14]        // bite tick value, unchanged
  ldr w15, =180000
  str w15, [x14, #0x10]        // no separate bark stage
  mov w15, #1
  str w15, [x14, #0x08]        // enable
  dsb sy

  //
  // Nothing below writes RAM this loader has not validated.
  //
  // An earlier revision guessed the ramoops record layout and the zone offsets behind this
  // address and wrote a record at several candidate places. The guess was never confirmed
  // (the record never appeared in pstore) and writing to a physical address that the
  // firmware map does not hand this loader is not something to keep behind a disabled
  // conditional. If a persistence channel is wanted, it has to be validated first and start
  // from memory the map does describe.
  //
  br x1
