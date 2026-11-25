// PhyTest bring-up helpers, on top of the generated ucie.h.
//
// For the lab and for simulation. Every wait is bounded. A failed check
// prints what it expected and what it read, then continues, so one run
// reports every problem. A program calls `pt_finish()` at the end; it exits
// nonzero if any check failed.

#ifndef __PHYTEST_H__
#define __PHYTEST_H__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <riscv-pk/encoding.h>
#include "mmio.h"
#include "ucie.h"

#define UCIE0_REG_BASE 0x200000UL
#define UCIE1_REG_BASE 0x208000UL

// Lane groups of four lanes each in the TX pattern SRAM.
#define PT_LANE_GROUPS ((UCIE_NUM_TEST_LANES + 3) / 4)
// The mainband lanes a run scores: the data lanes, valid and track. The
// loopback lane is only scored with `UCIE_TEST_TARGET_LOOPBACK`.
#define PT_MAINBAND_LANES (UCIE_TRACK_LANE + 1)
#define PT_ALL_LANES_MASK ((1ULL << UCIE_NUM_TEST_LANES) - 1)

// Bound on every wait, in core cycles, not reads: on silicon an MMIO read is
// fast next to a slow divided clock. The bound is generous.
#define PT_TIMEOUT 2000000ULL

// The chip clock the UCIe clock register block runs on, in MHz: the rate the
// clock meters count against. 500 MHz on Iris out of reset.
#ifndef PT_CHIP_MHZ
#define PT_CHIP_MHZ 500ULL
#endif

// Global delay taps that centre the forwarded clock on the data of a
// looped-back link at 16 GT/s on the Iris harness (as in the UCIe repo's tests
// and `setup_ucie_digital`). The reset value, the longest delay, works for a
// free-running clock but leaves a gated burst's first data UI with no edge.
// On silicon, override it with the code mainband training finds.
#ifndef PT_CENTRED_DELAY_TAPS
#define PT_CENTRED_DELAY_TAPS 29ULL
#endif

static int pt_failures = 0;
static const char *pt_port = "";

#define PT_CHECK(cond, ...)                                       \
  do {                                                            \
    if (!(cond)) {                                                \
      printf("FAIL [%s] %s:%d: ", pt_port, __FILE__, __LINE__);   \
      printf(__VA_ARGS__);                                        \
      printf("\n");                                               \
      pt_failures++;                                              \
    }                                                             \
  } while (0)

#define PT_CHECK_EQ(what, got, want)                                         \
  do {                                                                      \
    uint64_t _g = (got), _w = (want);                                       \
    PT_CHECK(_g == _w, "%s: read 0x%lx, expected 0x%lx", (what), _g, _w);   \
  } while (0)

static inline uint64_t pt_rd(uintptr_t base, uint64_t reg) {
  return reg_read64(base + reg);
}

static inline void pt_wr(uintptr_t base, uint64_t reg, uint64_t v) {
  reg_write64(base + reg, v);
}

static inline void pt_wait_cycles(uint64_t n) {
  uint64_t start = rdcycle();
  while (rdcycle() - start < n) {
  }
}

// Polls `reg` until it reads at least `want`. Returns whether it got there.
static inline int pt_wait_ge(uintptr_t base, uint64_t reg, uint64_t want,
                             const char *what) {
  uint64_t start = rdcycle();
  uint64_t v;
  while ((v = pt_rd(base, reg)) < want) {
    if (rdcycle() - start > PT_TIMEOUT) {
      PT_CHECK(0, "timed out waiting for %s: read %lu, wanted %lu", what, v,
               want);
      return 0;
    }
  }
  return 1;
}

static inline int pt_wait_eq(uintptr_t base, uint64_t reg, uint64_t want,
                             const char *what) {
  uint64_t start = rdcycle();
  uint64_t v;
  while ((v = pt_rd(base, reg)) != want) {
    if (rdcycle() - start > PT_TIMEOUT) {
      PT_CHECK(0, "timed out waiting for %s: read 0x%lx, wanted 0x%lx", what,
               v, want);
      return 0;
    }
  }
  return 1;
}

// Waits for `n` more RX idle words, which shows the RX datapath is out of
// reset and a forwarded clock is arriving. The counter is a free-running
// 32-bit count, so the difference is taken modulo 2^32.
static inline int pt_wait_idle_words(uintptr_t base, uint32_t n) {
  uint32_t w0 = (uint32_t)pt_rd(base, UCIE_RX_IDLE_WORDS_OBSERVED);
  uint64_t start = rdcycle();
  while ((uint32_t)((uint32_t)pt_rd(base, UCIE_RX_IDLE_WORDS_OBSERVED) - w0) <
         n) {
    if (rdcycle() - start > PT_TIMEOUT) {
      PT_CHECK(0, "no idle words arriving: RX datapath held, or no clock");
      return 0;
    }
  }
  return 1;
}

// Waits for the clocking block to finish an apply.
static inline int pt_wait_clk_cfg(uintptr_t base) {
  return pt_wait_eq(base, UCIE_CLK_CFG_BUSY, 0, "clock config apply");
}

// ===== Seeds =====

// Distinct per lane, so a lane scored against another lane's reference, or
// two lanes swapped anywhere between the TX and the RX, reads as errors.
static inline uint64_t pt_seed(int lane) {
  return UCIE_TRAIN_LFSR_SEED + (uint64_t)lane * 0x1111ULL;
}

static inline void pt_set_seeds(uintptr_t base, uint64_t (*tx)(int),
                                uint64_t (*rx)(int)) {
  for (int lane = 0; lane < UCIE_NUM_TEST_LANES; lane++) {
    pt_wr(base, UCIE_TX_LFSR_SEED + lane * UCIE_TX_LFSR_SEED_WIDTH, tx(lane));
    pt_wr(base, UCIE_RX_LFSR_SEED + lane * UCIE_RX_LFSR_SEED_WIDTH, rx(lane));
  }
}

// ===== Error counts =====

static inline uint64_t pt_errors(uintptr_t base, int lane) {
  return pt_rd(base, UCIE_RX_BIT_ERRORS + lane * UCIE_RX_BIT_ERRORS_WIDTH);
}

static inline uint64_t pt_errors_early(uintptr_t base, int lane) {
  return pt_rd(base,
               UCIE_RX_BIT_ERRORS_EARLY + lane * UCIE_RX_BIT_ERRORS_EARLY_WIDTH);
}

// Checks the nominal count of every lane in `mask` against `want(lane)`.
static inline void pt_check_errors(uintptr_t base, uint64_t mask,
                                   uint64_t (*want)(int), const char *what) {
  for (int lane = 0; lane < UCIE_NUM_TEST_LANES; lane++) {
    if (!((mask >> lane) & 1)) continue;
    uint64_t got = pt_errors(base, lane), w = want ? want(lane) : 0;
    PT_CHECK(got == w, "%s: lane %d counted %lu errors, expected %lu", what,
             lane, got, w);
  }
}

#define PT_MAINBAND_MASK ((1ULL << PT_MAINBAND_LANES) - 1)

// The crossings between the PHY test clock and the lane clocks. Either flag
// set means words were dropped, which reads like bit errors.
static inline void pt_check_fifo_flags(uintptr_t base, const char *what) {
  PT_CHECK(pt_rd(base, UCIE_TX_TEST_FIFO_UNDERFLOW) == 0,
           "%s: TX test FIFO underflowed", what);
  PT_CHECK(pt_rd(base, UCIE_RX_TEST_FIFO_OVERFLOW) == 0,
           "%s: RX test FIFO overflowed", what);
}

// ===== Clock meters =====

// Reads a clock meter (`UCIE_CLK_METER_*`) in MHz, given the chip clock the
// meters count against. The meters respond in any link state, so this works
// before the PhyTest registers do.
static inline uint64_t pt_meter_mhz(uintptr_t base, uint64_t reg,
                                    uint64_t chip_mhz) {
  pt_wait_cycles(4 * UCIE_CLK_METER_WINDOW_CYCLES);
  return pt_rd(base, reg) * chip_mhz / UCIE_CLK_METER_WINDOW_CYCLES;
}

// Checks a meter reads `mhz` to within 2%.
static inline void pt_check_meter(uintptr_t base, uint64_t reg, uint64_t mhz,
                                  const char *what) {
  uint64_t got = pt_meter_mhz(base, reg, PT_CHIP_MHZ);
  PT_CHECK(got * 50 >= mhz * 49 && got * 50 <= mhz * 51,
           "%s clock meter reads %lu MHz, expected %lu MHz", what, got, mhz);
}

// ===== Runs =====

// Programs a finite mainband run of `n` packets each way, then resets both
// FSMs, which reseeds the LFSRs. It starts nothing. Set options that need the
// RX datapath out of reset (`txClkGate`) between this and `pt_go`.
static inline void pt_arm(uintptr_t base, uint64_t mode, uint64_t n) {
  pt_wr(base, UCIE_TEST_TARGET, UCIE_TEST_TARGET_MAINBAND);
  pt_wr(base, UCIE_TX_TEST_MODE, mode);
  pt_wr(base, UCIE_TX_DATA_MODE,
        n ? UCIE_DATA_MODE_FINITE : UCIE_DATA_MODE_INFINITE);
  pt_wr(base, UCIE_RX_DATA_MODE,
        n ? UCIE_DATA_MODE_FINITE : UCIE_DATA_MODE_INFINITE);
  pt_wr(base, UCIE_TX_PACKETS_TO_SEND, n);
  pt_wr(base, UCIE_RX_PACKETS_TO_RECEIVE, n);
  reset_fsms(base);
  pt_wait_idle_words(base, 4);
}

// Starts the TX, and for a finite run waits for both ends to finish.
static inline int pt_go(uintptr_t base, uint64_t n) {
  pt_wr(base, UCIE_TX_EXECUTE, 1);
  if (!n) return 1;
  int ok = pt_wait_eq(base, UCIE_TX_TEST_STATE, UCIE_TX_TEST_STATE_DONE,
                      "TX run to finish");
  ok &= pt_wait_ge(base, UCIE_RX_PACKETS_RECEIVED, n, "RX packets");
  PT_CHECK_EQ("TX packets sent", pt_rd(base, UCIE_TX_PACKETS_SENT), n);
  PT_CHECK_EQ("RX packets received", pt_rd(base, UCIE_RX_PACKETS_RECEIVED), n);
  return ok;
}

static inline uint64_t pt_zero(int lane) { return 0; }

// A finite LFSR run with distinct seeds, expected clean on every mainband
// lane and with no words dropped at either crossing.
static inline void pt_lfsr_check(uintptr_t base, uint64_t n, const char *what) {
  pt_set_seeds(base, pt_seed, pt_seed);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, n);
  pt_go(base, n);
  pt_check_errors(base, PT_MAINBAND_MASK, pt_zero, what);
  pt_check_fifo_flags(base, what);
}

// ===== Pattern SRAM and capture SRAM =====

static inline void pt_write_chunk(uintptr_t base, int group, int row,
                                  const uint32_t w[4]) {
  pt_wr(base, UCIE_TX_DATA_LANE_GROUP, group);
  pt_wr(base, UCIE_TX_DATA_OFFSET, row);
  pt_wr(base, UCIE_TX_DATA_CHUNK_IN0, ((uint64_t)w[1] << 32) | w[0]);
  pt_wr(base, UCIE_TX_DATA_CHUNK_IN1, ((uint64_t)w[3] << 32) | w[2]);
  pt_wr(base, UCIE_TX_WRITE_CHUNK, 1);
}

// Loads `rows` rows of `word(row, lane)` into the TX pattern SRAM and sets
// the repeat period. The TX FSM has to be idle: it only takes writes there.
static inline void pt_load_pattern(uintptr_t base, int rows,
                                   uint32_t (*word)(int, int)) {
  for (int r = 0; r < rows; r++) {
    for (int g = 0; g < PT_LANE_GROUPS; g++) {
      uint32_t w[4];
      for (int i = 0; i < 4; i++) {
        int lane = 4 * g + i;
        w[i] = lane < UCIE_NUM_TEST_LANES ? word(r, lane) : 0;
      }
      pt_write_chunk(base, g, r, w);
    }
  }
  pt_wr(base, UCIE_TX_MANUAL_REPEAT_PERIOD, rows);
}

static inline uint32_t pt_captured(uintptr_t base, int lane, int entry) {
  pt_wr(base, UCIE_RX_DATA_LANE, lane);
  pt_wr(base, UCIE_RX_DATA_OFFSET, entry);
  return (uint32_t)pt_rd(base, UCIE_RX_DATA_CHUNK);
}

// A manual pattern word, distinct per row and lane, with the framing waveform
// on valid. The loopback lane's slot is zero because a mainband run reads
// that lane as zero. A loopback-lane run overrides it.
static inline uint32_t pt_pattern_word(int row, int lane) {
  if (lane == UCIE_VALID_LANE) return (uint32_t)UCIE_DEFAULT_VALID;
  if (lane == UCIE_LOOPBACK_LANE) return 0;
  return (uint32_t)(row + 1) * 0x9e3779b9u + 7u * (uint32_t)lane;
}

// ===== Sideband =====

static inline void pt_sb_reset(uintptr_t base) {
  pt_wr(base, UCIE_SIDEBAND_MODE, UCIE_BAND_MODE_MANUAL);
  pt_wr(base, UCIE_SB_RX_RST, 1);
  pt_wait_cycles(64);
}

static inline int pt_sb_send(uintptr_t base, uint64_t packet) {
  if (!pt_wait_eq(base, UCIE_SB_TX_BUSY, 0, "sideband TX idle")) return 0;
  pt_wr(base, UCIE_SB_TX_PACKET, packet);
  pt_wr(base, UCIE_SB_TX_SEND, 1);
  return 1;
}

// Waits for a sideband packet and pops it. Returns 0 on a timeout.
static inline int pt_sb_recv(uintptr_t base, uint64_t *packet) {
  if (!pt_wait_eq(base, UCIE_SB_RX_VALID, 1, "sideband packet")) return 0;
  *packet = pt_rd(base, UCIE_SB_RX_PACKET);
  pt_wr(base, UCIE_SB_RX_POP, 1);
  return 1;
}

// ===== Setup =====

// `setup_ucie` with the sideband receiver cleared, so that a program starts
// from a known state on both bands.
static inline void pt_setup(uintptr_t base) {
  setup_ucie(base);
  pt_sb_reset(base);
}

static inline void pt_begin(const char *port, const char *test) {
  pt_port = port;
  printf("[%s] %s (cycle %lu)\n", port, test, rdcycle());
}

static inline void pt_finish(const char *program) {
  if (pt_failures) {
    printf("%s: %d check(s) FAILED\n", program, pt_failures);
    exit(1);
  }
  printf("%s: all checks passed\n", program);
}

#endif
