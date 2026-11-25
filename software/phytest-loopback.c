// PhyTest over every UCIe port, with each port's bumps looped back onto
// itself: the single-chip harness in simulation, or a loopback board.
//
// Each test leaves the registers it changed as `setup_ucie` left them, so
// they run in any order and any subset.

#include "phytest.h"

#define N_PACKETS 64

static uint64_t zero(int lane) { return 0; }

// ===== LFSR =====

// At the TX queue's reset cap of 16 words, which on the Iris harness, with the
// PHY test clock at the packet clock, is where a cap has the least room.
static void test_lfsr(uintptr_t base) {
  PT_CHECK_EQ("TX queue cap reset value", pt_rd(base, UCIE_TX_QUEUE_LIMIT), 16);
  pt_set_seeds(base, pt_seed, pt_seed);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, N_PACKETS);
  pt_go(base, N_PACKETS);
  pt_check_errors(base, PT_MAINBAND_MASK, zero, "LFSR run");
  pt_check_fifo_flags(base, "LFSR run");
}

// One injected error on a data lane and one on track, counted exactly once
// each. Infinite, and read under `rxPauseCounters`, as a lab run would be.
static int inject_lane;
static uint64_t one_on_injected(int lane) {
  return lane == inject_lane || lane == UCIE_TRACK_LANE;
}

static void test_inject(uintptr_t base) {
  pt_set_seeds(base, pt_seed, pt_seed);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, 0);
  // A strobe before the run hits its first packet.
  inject_lane = 3;
  pt_wr(base, UCIE_TX_INJECT_LANE, inject_lane);
  pt_wr(base, UCIE_TX_INJECT_ERROR, 1);
  pt_go(base, 0);
  pt_wait_ge(base, UCIE_RX_PACKETS_RECEIVED, 16, "RX packets");
  pt_wr(base, UCIE_TX_INJECT_LANE, UCIE_TRACK_LANE);
  pt_wr(base, UCIE_TX_INJECT_ERROR, 1);
  uint64_t r = pt_rd(base, UCIE_RX_PACKETS_RECEIVED);
  pt_wait_ge(base, UCIE_RX_PACKETS_RECEIVED, r + 16, "RX packets");
  pt_wr(base, UCIE_RX_PAUSE_COUNTERS, 1);
  pt_wait_cycles(64);
  pt_check_errors(base, PT_MAINBAND_MASK, one_on_injected, "injected errors");
  pt_wr(base, UCIE_RX_PAUSE_COUNTERS, 0);
  pt_wr(base, UCIE_TX_INJECT_LANE, 0);
  reset_fsms(base);
}

// Scored against its own history with the RX seeds wrong on every lane.
static uint64_t wrong_seed(int lane) { return pt_seed(lane + 7); }

static void test_self_sync(uintptr_t base) {
  pt_set_seeds(base, pt_seed, wrong_seed);
  pt_wr(base, UCIE_RX_SELF_SYNC, 1);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, N_PACKETS);
  pt_go(base, N_PACKETS);
  pt_check_errors(base, PT_MAINBAND_MASK, zero, "self-synchronized run");
  // The early count is still the seeded check, so it sees the wrong seeds.
  PT_CHECK(pt_errors_early(base, 0) > 0,
           "seeded early count read clean against the wrong seed");
  pt_wr(base, UCIE_RX_SELF_SYNC, 0);
}

// Valid moved onto track at both ends, the workaround for a broken valid
// lane. Track then carries valid and is scored against it.
static void test_valid_on_track(uintptr_t base) {
  pt_set_seeds(base, pt_seed, pt_seed);
  pt_wr(base, UCIE_TX_VALID_LANE_SEL, UCIE_TRACK_VALID_LANE_SEL);
  pt_wr(base, UCIE_RX_VALID_LANE_SEL, UCIE_TRACK_VALID_LANE_SEL);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, N_PACKETS);
  pt_go(base, N_PACKETS);
  pt_check_errors(base, PT_MAINBAND_MASK, zero, "valid on track");
  pt_wr(base, UCIE_TX_VALID_LANE_SEL, UCIE_DEFAULT_VALID_LANE_SEL);
  pt_wr(base, UCIE_RX_VALID_LANE_SEL, UCIE_DEFAULT_VALID_LANE_SEL);
}

// Bursts with the forwarded clock gated between them, as the UCIe controller
// parks it. The RX lanes deserialize only on clock edges, so this stops and
// restarts the real receiver.
// Gating uses the UCIe reset values: no preamble, one packet of postamble.
// The clock parks high and low in turn, then always low. One packet is enough
// after the RX dividers are realigned behind the parked clock and one 40 UI
// gap moves the RX word boundary so every burst ends 23 UI into an RX word
// (ucie/docs/phytest_bringup.md, section 8). With no preamble, the first edge
// after a park must sample the burst's first data UI, so the test centres the
// forwarded clock and restores it afterwards.
static void test_gated_bursts(uintptr_t base) {
  const uint64_t n = 24;
  const uint64_t phase = pt_rd(base, UCIE_CLK_PHASE_SEL);
  PT_CHECK_EQ("preamble reset value", pt_rd(base, UCIE_TX_CLK_PREAMBLE), 0);
  PT_CHECK_EQ("postamble reset value", pt_rd(base, UCIE_TX_CLK_POSTAMBLE),
              32 / UCIE_CLK_POSTAMBLE_UI);
  PT_CHECK_EQ("park alternation reset value",
              pt_rd(base, UCIE_TX_CLK_PARK_ALTERNATE), 1);
  pt_set_seeds(base, pt_seed, pt_seed);
  pt_wr(base, UCIE_RX_BURST_MODE, 1);
  for (int alternate = 1; alternate >= 0; alternate--) {
    pt_wr(base, UCIE_TX_CLK_PARK_ALTERNATE, alternate);
    // Gate only after `pt_arm`, because its RX datapath reset needs clock
    // edges to release. Then realign the dividers behind the parked clock, so
    // the next burst's first edge samples its first data UI.
    pt_arm(base, UCIE_TX_TEST_MODE_LFSR, 1);
    pt_wr(base, UCIE_TX_CLK_GATE, 1);
    pt_wait_cycles(256);
    set_clk_en(base, 0);
    set_rx_clk_en(base, 0);
    set_global_delay(base, PT_CENTRED_DELAY_TAPS);
    reset_dividers(base);
    set_clk_en(base, 1);
    set_rx_clk_en(base, 1);
    pt_wr(base, UCIE_TX_CLK_POSTAMBLE, 40 / UCIE_CLK_POSTAMBLE_UI);
    pt_go(base, 1);
    pt_wr(base, UCIE_TX_CLK_POSTAMBLE, 32 / UCIE_CLK_POSTAMBLE_UI);

    // The bursts. A clock that runs or parks for whole packets moves no word
    // boundary, so the FSMs can reset on a running clock in between.
    pt_wr(base, UCIE_TX_CLK_GATE, 0);
    pt_wr(base, UCIE_TX_BURST_PACKETS, 6);
    pt_wr(base, UCIE_TX_IDLE_PACKETS, 10);
    pt_arm(base, UCIE_TX_TEST_MODE_LFSR, n);
    pt_wr(base, UCIE_TX_CLK_GATE, 1);
    pt_wait_cycles(256);
    uint64_t w0 = pt_rd(base, UCIE_RX_IDLE_WORDS_OBSERVED);
    pt_wait_cycles(1024);
    PT_CHECK_EQ("idle words with the clock parked",
                pt_rd(base, UCIE_RX_IDLE_WORDS_OBSERVED), w0);
    pt_go(base, n);
    const char *what =
        alternate ? "gated bursts, parking high and low" : "gated bursts";
    pt_check_errors(base, PT_MAINBAND_MASK, zero, what);
    pt_check_fifo_flags(base, what);
    pt_wr(base, UCIE_TX_CLK_GATE, 0);
    pt_wr(base, UCIE_TX_BURST_PACKETS, 0);
    pt_wr(base, UCIE_TX_IDLE_PACKETS, 0);
  }
  pt_wr(base, UCIE_TX_CLK_PARK_ALTERNATE, 1);
  pt_wr(base, UCIE_RX_BURST_MODE, 0);
  set_clk_en(base, 0);
  set_rx_clk_en(base, 0);
  pt_wr(base, UCIE_CLK_PHASE_SEL, phase);
  apply_clk_cfg(base);
  pt_wait_clk_cfg(base);
  reset_dividers(base);
  set_clk_en(base, 1);
  set_rx_clk_en(base, 1);
  reset_fsms(base);
}

// ===== Manual patterns and the capture SRAM =====

#define PERIOD 5
static const int checked_lanes[] = {0, 9, UCIE_TRACK_LANE - 3, UCIE_VALID_LANE,
                                    UCIE_TRACK_LANE};
#define N_CHECKED (sizeof(checked_lanes) / sizeof(checked_lanes[0]))

static void check_capture(uintptr_t base, int first, const char *what) {
  for (unsigned i = 0; i < N_CHECKED; i++) {
    int lane = checked_lanes[i];
    for (int e = 0; e < UCIE_SRAM_PACKETS; e++) {
      uint32_t got = pt_captured(base, lane, e);
      uint32_t want = pt_pattern_word((first + e) % PERIOD, lane);
      PT_CHECK(got == want, "%s: lane %d entry %d read 0x%x, sent 0x%x", what,
               lane, e, got, want);
    }
  }
}

static void test_manual_capture(uintptr_t base) {
  reset_fsms(base);
  pt_load_pattern(base, PERIOD, pt_pattern_word);
  pt_arm(base, UCIE_TX_TEST_MODE_MANUAL, UCIE_SRAM_PACKETS);
  pt_go(base, UCIE_SRAM_PACKETS);
  check_capture(base, 0, "manual capture");

  // From packet `start` on, past the first lap of the SRAM.
  const int start = UCIE_SRAM_PACKETS + 7;
  pt_wr(base, UCIE_RX_CAPTURE_START, start);
  pt_arm(base, UCIE_TX_TEST_MODE_MANUAL, start + UCIE_SRAM_PACKETS);
  pt_go(base, start + UCIE_SRAM_PACKETS);
  check_capture(base, start, "capture start");
  pt_wr(base, UCIE_RX_CAPTURE_START, 0);
  pt_wr(base, UCIE_TX_MANUAL_REPEAT_PERIOD, 0);
}

// An error-triggered capture. Two lanes send the same stream, so one shows
// what the other should have received. The SRAM must hold exactly the one
// flipped bit.
#define TWIN 0
#define VICTIM 1
static uint64_t twin_seed(int lane) {
  return pt_seed(lane == VICTIM ? TWIN : lane);
}
static uint64_t one_on_victim(int lane) { return lane == VICTIM; }

static void test_error_capture(uintptr_t base) {
  const uint64_t post = 4;
  pt_set_seeds(base, twin_seed, twin_seed);
  pt_wr(base, UCIE_RX_CAPTURE_MODE, UCIE_CAPTURE_MODE_ERROR);
  pt_wr(base, UCIE_RX_CAPTURE_POST_TRIGGER, post);
  pt_wr(base, UCIE_RX_TRIGGER_LANE_MASK, PT_ALL_LANES_MASK);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, 0);
  pt_go(base, 0);
  pt_wait_ge(base, UCIE_RX_PACKETS_RECEIVED, 2 * UCIE_SRAM_PACKETS,
             "RX packets");
  PT_CHECK_EQ("triggered before any error",
              pt_rd(base, UCIE_RX_CAPTURE_TRIGGERED), 0);
  // Read before the strobe: the injected packet can arrive before a read
  // issued after it returns.
  uint64_t r = pt_rd(base, UCIE_RX_PACKETS_RECEIVED);
  pt_wr(base, UCIE_TX_INJECT_LANE, VICTIM);
  pt_wr(base, UCIE_TX_INJECT_ERROR, 1);
  pt_wait_ge(base, UCIE_RX_PACKETS_RECEIVED, r + 2 * UCIE_SRAM_PACKETS,
             "RX packets");
  pt_wr(base, UCIE_RX_PAUSE_COUNTERS, 1);
  if (pt_wait_eq(base, UCIE_RX_CAPTURE_TRIGGERED, 1, "capture trigger")) {
    uint64_t t = pt_rd(base, UCIE_RX_CAPTURE_TRIGGER_PACKET);
    PT_CHECK(t >= r, "trigger packet %lu is before the injection at %lu", t, r);
    for (int e = 0; e < UCIE_SRAM_PACKETS; e++) {
      uint32_t diff = pt_captured(base, TWIN, e) ^ pt_captured(base, VICTIM, e);
      uint32_t want = (uint64_t)e == t % UCIE_SRAM_PACKETS ? 1 : 0;
      PT_CHECK(diff == want, "entry %d: twin lanes differ by 0x%x, expected 0x%x",
               e, diff, want);
    }
  }
  pt_check_errors(base, PT_MAINBAND_MASK, one_on_victim, "error capture run");
  pt_wr(base, UCIE_RX_PAUSE_COUNTERS, 0);
  // Back to the reset values: half the SRAM after the trigger, every lane.
  pt_wr(base, UCIE_RX_CAPTURE_MODE, UCIE_CAPTURE_MODE_FIRST);
  pt_wr(base, UCIE_RX_CAPTURE_POST_TRIGGER, UCIE_SRAM_PACKETS / 2);
  pt_wr(base, UCIE_RX_TRIGGER_LANE_MASK, (1ULL << UCIE_NUM_TEST_LANES) - 1);
  pt_wr(base, UCIE_TX_INJECT_LANE, 0);
  reset_fsms(base);
}

// The RX starting on whatever arrives, with nothing on valid.
static void test_immediate_start(uintptr_t base) {
  pt_wr(base, UCIE_RX_DATA_MODE, UCIE_DATA_MODE_INFINITE);
  reset_fsms(base);
  pt_wait_cycles(512);
  PT_CHECK_EQ("packets with the TX idle", pt_rd(base, UCIE_RX_PACKETS_RECEIVED),
              0);
  pt_wr(base, UCIE_RX_START_MODE, UCIE_RX_START_MODE_IMMEDIATE);
  pt_wait_ge(base, UCIE_RX_PACKETS_RECEIVED, 8, "immediate-start packets");
  pt_wr(base, UCIE_RX_START_MODE, UCIE_RX_START_MODE_VALID);
  reset_fsms(base);
}

// ===== Track =====

static void count_track(uintptr_t base, uint64_t word, uint64_t *ones,
                        uint64_t *zeros, uint64_t *even) {
  pt_wr(base, UCIE_TX_TRACK_WORD, word);
  pt_wait_cycles(256);
  pt_wr(base, UCIE_TRACK_CNT_RST, 1);
  pt_wait_cycles(2048);
  pt_wr(base, UCIE_TRACK_CNT_HOLD, 1);
  pt_wait_cycles(64);
  *ones = pt_rd(base, UCIE_TRACK_ONES);
  *zeros = pt_rd(base, UCIE_TRACK_ZEROS);
  *even = pt_rd(base, UCIE_TRACK_ONES_EVEN);
  pt_wr(base, UCIE_TRACK_CNT_HOLD, 0);
}

static void test_track(uintptr_t base) {
  uint64_t ones, zeros, even;
  pt_wr(base, UCIE_TX_TRACK_CLK, 1);
  // Sixteen ones in a row: half on even UIs whatever the RX word boundary.
  count_track(base, 0x0000ffffULL, &ones, &zeros, &even);
  PT_CHECK(ones > 0 && ones == zeros && 2 * even == ones,
           "track 0x0000ffff: ones %lu zeros %lu even ones %lu", ones, zeros,
           even);
  // A clock: every one on an even UI, or every one on an odd UI.
  count_track(base, UCIE_DEFAULT_TRACK, &ones, &zeros, &even);
  PT_CHECK(ones > 0 && ones == zeros && (even == ones || even == 0),
           "track clock: ones %lu zeros %lu even ones %lu", ones, zeros, even);
  printf("[%s] track clock lands on %s UIs\n", pt_port,
         even == ones ? "even" : "odd");
  pt_wr(base, UCIE_TX_TRACK_CLK, 0);
  pt_wr(base, UCIE_TX_TRACK_WORD, UCIE_DEFAULT_TRACK);
}

// ===== Loopback lane, debug lane, sideband =====

static uint32_t loopback_word(int row, int lane) {
  // Bit 0 set on the first row: the RX frames on the first one.
  if (lane == UCIE_LOOPBACK_LANE) return ((uint32_t)(row + 1) * 0x9e3779b9u) | 1;
  return 0;
}

static void test_loopback_lane(uintptr_t base) {
  const int n = 2 * UCIE_SRAM_PACKETS;
  reset_fsms(base);
  pt_load_pattern(base, PERIOD, loopback_word);
  pt_wr(base, UCIE_TEST_TARGET, UCIE_TEST_TARGET_LOOPBACK);
  // Every target change needs a datapath reset; `reset_fsms` does one.
  pt_wr(base, UCIE_TX_TEST_MODE, UCIE_TX_TEST_MODE_MANUAL);
  pt_wr(base, UCIE_TX_DATA_MODE, UCIE_DATA_MODE_FINITE);
  pt_wr(base, UCIE_RX_DATA_MODE, UCIE_DATA_MODE_FINITE);
  pt_wr(base, UCIE_TX_PACKETS_TO_SEND, n);
  pt_wr(base, UCIE_RX_PACKETS_TO_RECEIVE, n);
  reset_fsms(base);
  pt_wait_cycles(256);
  pt_go(base, n);
  for (int e = 0; e < UCIE_SRAM_PACKETS; e++) {
    uint32_t got = pt_captured(base, UCIE_LOOPBACK_LANE, e);
    uint32_t want = loopback_word(e % PERIOD, UCIE_LOOPBACK_LANE);
    PT_CHECK(got == want, "loopback lane entry %d read 0x%x, sent 0x%x", e, got,
             want);
  }
  PT_CHECK_EQ("loopback RX FIFO overflow",
              pt_rd(base, UCIE_LOOPBACK_RX_FIFO_OVERFLOW), 0);
  PT_CHECK_EQ("loopback TX FIFO underflow",
              pt_rd(base, UCIE_LOOPBACK_TX_FIFO_UNDERFLOW), 0);
  pt_wr(base, UCIE_TEST_TARGET, UCIE_TEST_TARGET_MAINBAND);
  pt_wr(base, UCIE_TX_MANUAL_REPEAT_PERIOD, 0);
  reset_fsms(base);
}

// The observation bumps, which `setup_ucie` leaves off. Only this test turns
// them on and drives each: the TX data debug lane runs, `mux0` watches the TX
// divided clock, and `mux1` watches RX data lane 0, which gets traffic. The
// simulation harness checks every bump toggled; on the bench, probe them with
// a scope now. The debug lane only drives a bump, so software can check only
// that it runs to completion without starving.
static void test_observation(uintptr_t base) {
  enable_debug_drivers(base, 1);
  pt_wr(base, UCIE_DEBUG_MUX_SEL, UCIE_DEBUG_MUX_TX_DIV_CLK);
  pt_wr(base, UCIE_DEBUG_MUX_SEL + UCIE_DEBUG_MUX_SEL_WIDTH,
        UCIE_DEBUG_MUX_RX_DATA_LSB);
  pt_wr(base, UCIE_DEBUG_TX_TEST_MODE, UCIE_TX_TEST_MODE_LFSR);
  pt_wr(base, UCIE_DEBUG_TX_DATA_MODE, UCIE_DATA_MODE_FINITE);
  pt_wr(base, UCIE_DEBUG_TX_PACKETS_TO_SEND, N_PACKETS);
  pt_wr(base, UCIE_DEBUG_TX_FSM_RST, 1);
  pt_wr(base, UCIE_DEBUG_TX_EXECUTE, 1);
  pt_wait_eq(base, UCIE_DEBUG_TX_TEST_STATE, UCIE_TX_TEST_STATE_DONE,
             "debug TX run");
  PT_CHECK_EQ("debug TX packets sent", pt_rd(base, UCIE_DEBUG_TX_PACKETS_SENT),
              N_PACKETS);
  PT_CHECK_EQ("debug TX FIFO underflow",
              pt_rd(base, UCIE_TX_DEBUG_FIFO_UNDERFLOW), 0);
  pt_wr(base, UCIE_DEBUG_TX_FSM_RST, 1);
  pt_lfsr_check(base, N_PACKETS, "LFSR run with the observation drivers on");
  pt_wr(base, UCIE_DEBUG_MUX_SEL, 0);
  pt_wr(base, UCIE_DEBUG_MUX_SEL + UCIE_DEBUG_MUX_SEL_WIDTH, 0);
  enable_debug_drivers(base, 0);
}

// ===== Per-lane sources and error diagnostics =====

#define FIXED_LANE 5
#define INVERTED_LANE 7

static void set_lane_sources(uintptr_t base, int tx, int rx, uint64_t fixed,
                             uint64_t inverted, uint64_t word) {
  if (tx) {
    pt_wr(base, UCIE_TX_LANE_FIXED, fixed);
    pt_wr(base, UCIE_TX_LANE_INVERT, inverted);
    pt_wr(base, UCIE_TX_FIXED_WORD, word);
  }
  if (rx) {
    pt_wr(base, UCIE_RX_LANE_FIXED, fixed);
    pt_wr(base, UCIE_RX_LANE_INVERT, inverted);
    pt_wr(base, UCIE_RX_FIXED_WORD, word);
  }
}

static uint64_t err_class_sum(uintptr_t base, int rise) {
  return rise ? pt_rd(base, UCIE_RX_ERR_RISE_ONE) +
                    pt_rd(base, UCIE_RX_ERR_RISE_ZERO)
              : pt_rd(base, UCIE_RX_ERR_FALL_ONE) +
                    pt_rd(base, UCIE_RX_ERR_FALL_ZERO);
}

static uint64_t err_positions(uintptr_t base, int lane) {
  pt_wr(base, UCIE_RX_ERR_POS_LANE, lane);
  return pt_rd(base, UCIE_RX_ERR_POS_MASK);
}

// Victims and aggressors. First one lane is held at a static level and one is
// inverted, and the RX expects the same, so the run is clean. Then the RX
// reads the inverted lane as plain, so every bit is wrong, half on each
// sampling edge.
static void test_lane_sources(uintptr_t base) {
  const uint64_t fixed = 1ULL << FIXED_LANE;
  const uint64_t inverted = (1ULL << INVERTED_LANE) | (1ULL << UCIE_TRACK_LANE);
  set_lane_sources(base, 1, 1, fixed, inverted, 0);
  pt_lfsr_check(base, N_PACKETS, "fixed and inverted lanes, RX matching");
  PT_CHECK_EQ("lanes flagged errored", pt_rd(base, UCIE_RX_LANE_ERRORED), 0);

  set_lane_sources(base, 0, 1, fixed, 1ULL << UCIE_TRACK_LANE, 0);
  pt_wr(base, UCIE_RX_ERR_CLASS_LANE, INVERTED_LANE);
  pt_set_seeds(base, pt_seed, pt_seed);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, N_PACKETS);
  pt_go(base, N_PACKETS);
  PT_CHECK_EQ("errors on the inverted lane read plain",
              pt_errors(base, INVERTED_LANE), 32 * N_PACKETS);
  PT_CHECK_EQ("those errors on rising-edge UIs", err_class_sum(base, 1),
              16 * N_PACKETS);
  PT_CHECK_EQ("those errors on falling-edge UIs", err_class_sum(base, 0),
              16 * N_PACKETS);
  PT_CHECK_EQ("lanes flagged errored", pt_rd(base, UCIE_RX_LANE_ERRORED),
              1ULL << INVERTED_LANE);
  PT_CHECK_EQ("error positions on the inverted lane",
              err_positions(base, INVERTED_LANE), 0xffffffffULL);
  set_lane_sources(base, 1, 1, 0, 0, 0);
  pt_wr(base, UCIE_RX_ERR_CLASS_LANE, 0);
}

// One injected error, found by lane, by UI and as exactly one classified
// error. Which edge it lands on depends on where the RX framed.
static void test_error_location(uintptr_t base) {
  const int lane = 3;
  pt_set_seeds(base, pt_seed, pt_seed);
  pt_wr(base, UCIE_RX_ERR_CLASS_LANE, lane);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, N_PACKETS);
  pt_wr(base, UCIE_TX_INJECT_LANE, lane);
  pt_wr(base, UCIE_TX_INJECT_ERROR, 1);
  pt_go(base, N_PACKETS);
  PT_CHECK_EQ("injected error count", pt_errors(base, lane), 1);
  PT_CHECK_EQ("lanes flagged errored", pt_rd(base, UCIE_RX_LANE_ERRORED),
              1ULL << lane);
  // Bit 0 of the packet, the bit injection flips.
  PT_CHECK_EQ("error positions", err_positions(base, lane), 1);
  PT_CHECK_EQ("classified errors",
              err_class_sum(base, 1) + err_class_sum(base, 0), 1);
  pt_wr(base, UCIE_RX_ERR_CLASS_LANE, 0);
  pt_wr(base, UCIE_TX_INJECT_LANE, 0);
}

// The clock meters, which read the harness's clocks: an 8 GHz main clock
// (500 MHz packet clocks), and the PHY test and sideband clocks on their
// bypass pins at 500 and 800 MHz.
static void test_meters(uintptr_t base) {
  pt_check_meter(base, UCIE_CLK_METER_PHY_TEST, 500, "PHY test");
  pt_check_meter(base, UCIE_CLK_METER_SB, 800, "sideband");
  pt_check_meter(base, UCIE_CLK_METER_TX_DIV, 500, "TX divided");
  pt_check_meter(base, UCIE_CLK_METER_RX_DIV, 500, "RX divided");
  PT_CHECK_EQ("controller RX FIFO overflow",
              pt_rd(base, UCIE_CONTROLLER_RX_FIFO_OVERFLOW), 0);
  PT_CHECK_EQ("PLL lock lost", pt_rd(base, UCIE_PLL_LOCK_LOST), 0);
}

static void test_sideband(uintptr_t base) {
  const uint64_t packets[] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
  pt_sb_reset(base);
  for (int i = 0; i < 2; i++) pt_sb_send(base, packets[i]);
  for (int i = 0; i < 2; i++) {
    uint64_t got;
    if (pt_sb_recv(base, &got)) PT_CHECK_EQ("sideband packet", got, packets[i]);
  }
  pt_wait_cycles(64);
  PT_CHECK_EQ("sideband RX empty", pt_rd(base, UCIE_SB_RX_VALID), 0);
  PT_CHECK_EQ("sideband RX overflow", pt_rd(base, UCIE_SB_RX_OVERFLOW), 0);
}

typedef struct {
  const char *name;
  void (*fn)(uintptr_t);
} test_t;

static const test_t tests[] = {
    {"clock meters", test_meters},
    {"LFSR run", test_lfsr},
    {"error injection", test_inject},
    {"self-synchronized check", test_self_sync},
    {"valid moved onto track", test_valid_on_track},
    {"gated-clock bursts", test_gated_bursts},
    {"manual pattern and capture", test_manual_capture},
    {"error-triggered capture", test_error_capture},
    {"immediate start", test_immediate_start},
    {"track counters", test_track},
    {"loopback lane", test_loopback_lane},
    {"per-lane pattern sources", test_lane_sources},
    {"error location and class", test_error_location},
    {"observation bumps and the TX debug lane", test_observation},
    {"sideband", test_sideband},
};

int main(void) {
  const struct {
    const char *name;
    uintptr_t base;
  } ports[] = {{"ucie0", UCIE0_REG_BASE}, {"ucie1", UCIE1_REG_BASE}};

  for (int p = 0; p < 2; p++) {
    pt_begin(ports[p].name, "setup");
    pt_setup(ports[p].base);
    for (unsigned t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
      pt_begin(ports[p].name, tests[t].name);
      tests[t].fn(ports[p].base);
    }
  }
  pt_finish("phytest-loopback");
  return 0;
}
