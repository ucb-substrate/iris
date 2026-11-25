// PhyTest on UCIe0 with its bumps looped back, through the clock setups of
// bring-up: the TX divider, the main clock on each PLL, the PHY test and
// sideband clocks on a PLL, and a PHY test clock too slow for the link, which
// the crossing flags must report.
//
// As in bring-up, every change ends with `pt_setup`, which restarts the block
// on its new clocks.

#include "phytest.h"

#define N_PACKETS 64

// `MainClkSel`, `PhyTestClkSrcSel` and the divider codes, from the clocking
// tile's interface.
#define MAIN_PLL8 0
#define MAIN_PLL12 1
#define MAIN_BYPASS 3
#define TX_DIV1 0
#define TX_DIV2 1
#define TX_DIV4 2
#define SRC_PLL8 0
#define SRC_PLL12 1
#define PHY_TEST_DIV8 4
#define SB_DIV10 2

static void move_main_clk(uintptr_t base, uint64_t sel, uint64_t div) {
  set_main_clk(base, sel, div);
  pt_wait_clk_cfg(base);
  pt_setup(base);
}

// After an apply that moved the PHY test or sideband clock: both must run
// before `pt_setup` touches their register windows, or the bus hangs. If
// either is stopped (a PLL that won't lock), end the apply, put both back on
// their bypass pins, and stop the program.
static void require_clocks(uintptr_t base, const char *what) {
  int busy = !pt_wait_clk_cfg(base);
  uint64_t pt = pt_meter_mhz(base, UCIE_CLK_METER_PHY_TEST, PT_CHIP_MHZ);
  uint64_t sb = pt_meter_mhz(base, UCIE_CLK_METER_SB, PT_CHIP_MHZ);
  if (!busy && pt != 0 && sb != 0) return;
  PT_CHECK(0, "%s: apply busy %d, PHY test %lu MHz, sideband %lu MHz", what,
           busy, pt, sb);
  release_clk_gate(base);
  pt_wr(base, UCIE_PHY_TEST_CLK_BYPASS_EN, 1);
  pt_wr(base, UCIE_SB_CLK_BYPASS_EN, 1);
  apply_clk_cfg(base);
  release_clk_gate(base);
  pt_finish("phytest-clocks");
}

// The packet clocks, TX and looped-back RX, from the clock meters.
static void check_packet_clocks(uintptr_t base, uint64_t mhz) {
  pt_check_meter(base, UCIE_CLK_METER_TX_DIV, mhz, "TX divided");
  pt_check_meter(base, UCIE_CLK_METER_RX_DIV, mhz, "RX divided");
}

static void test_sideband_on_pll(uintptr_t base) {
  // 800 MHz, /10 off PLL8, which this apply turns on if it isn't already.
  pt_wr(base, UCIE_PLL8_EN, 1);
  pt_wr(base, UCIE_SB_CLK_SRC_SEL, SRC_PLL8);
  pt_wr(base, UCIE_SB_CLK_DIV, SB_DIV10);
  pt_wr(base, UCIE_SB_CLK_BYPASS_EN, 0);
  apply_clk_cfg(base);
  require_clocks(base, "sideband onto PLL8 /10");
  pt_setup(base);
  const uint64_t packet = 0x5a5a00000000c10cULL;
  pt_sb_send(base, packet);
  uint64_t got;
  if (pt_sb_recv(base, &got)) PT_CHECK_EQ("sideband packet off PLL8", got, packet);
}

int main(void) {
  const uintptr_t base = UCIE0_REG_BASE;

  pt_begin("ucie0", "bypass clock /1");
  pt_setup(base);
  pt_check_meter(base, UCIE_CLK_METER_PHY_TEST, 500, "PHY test on its bypass pin");
  pt_check_meter(base, UCIE_CLK_METER_SB, 800, "sideband on its bypass pin");
  check_packet_clocks(base, 500);
  pt_lfsr_check(base, N_PACKETS, "bypass /1");

  pt_begin("ucie0", "bypass clock /2");
  move_main_clk(base, MAIN_BYPASS, TX_DIV2);
  check_packet_clocks(base, 250);
  pt_lfsr_check(base, N_PACKETS, "bypass /2");

  pt_begin("ucie0", "bypass clock /4");
  move_main_clk(base, MAIN_BYPASS, TX_DIV4);
  check_packet_clocks(base, 125);
  pt_lfsr_check(base, N_PACKETS, "bypass /4");

  // PLL8 up, and the PHY test clock onto it, /8 = 1 GHz. The apply
  // holds the PHY test clock's gate shut until PLL8 has locked.
  pt_begin("ucie0", "PHY test clock onto PLL8 /8");
  use_internal_clk(base, SRC_PLL8, PHY_TEST_DIV8);
  require_clocks(base, "PHY test clock onto PLL8 /8");
  pt_wait_eq(base, UCIE_PLL_LOCK_OBSERVED, 1, "PLL8 to lock");
  pt_setup(base);
  pt_check_meter(base, UCIE_CLK_METER_PHY_TEST, 1000, "PHY test on PLL8 /8");
  pt_lfsr_check(base, N_PACKETS, "PHY test clock on PLL8, main on bypass");

  pt_begin("ucie0", "main clock onto PLL8 /1");
  move_main_clk(base, MAIN_PLL8, TX_DIV1);
  check_packet_clocks(base, 500);
  pt_lfsr_check(base, N_PACKETS, "main on PLL8 /1");

  // The TX queue capped below its reset value of 16, for the lowest latency,
  // with the PHY test clock at twice the packet clock. The closer the two
  // clocks, the larger the cap must be. At equal rates, 8 runs the queue dry
  // and 10 is the smallest that works.
  pt_begin("ucie0", "TX queue capped at 8 words");
  pt_wr(base, UCIE_TX_QUEUE_LIMIT, 8);
  pt_lfsr_check(base, N_PACKETS, "TX queue capped at 8 words");
  pt_wr(base, UCIE_TX_QUEUE_LIMIT, 16);

  pt_begin("ucie0", "main clock onto PLL12 /1");
  move_main_clk(base, MAIN_PLL12, TX_DIV1);
  check_packet_clocks(base, 750);
  pt_lfsr_check(base, N_PACKETS, "main on PLL12 /1");

  // The PHY test clock back on its bypass pin, which the harness runs at
  // 500 MHz. The 12 GHz lanes move a word every 1.33 ns, so the TX queue runs
  // dry partway through the run and its underflow flag must be set. The PHY
  // sends an idle word for each missing word and keeps the forwarded clock
  // running. The looped-back RX still takes a word every packet clock, faster
  // than the PHY test clock can drain, so the RX queue overflows too. The two
  // packet clocks meter the same.
  pt_begin("ucie0", "PHY test clock too slow for the link");
  pt_wr(base, UCIE_PHY_TEST_CLK_BYPASS_EN, 1);
  apply_clk_cfg(base);
  pt_wait_clk_cfg(base);
  pt_setup(base);
  pt_set_seeds(base, pt_seed, pt_seed);
  pt_arm(base, UCIE_TX_TEST_MODE_LFSR, N_PACKETS);
  pt_wr(base, UCIE_TX_EXECUTE, 1);
  pt_wait_eq(base, UCIE_TX_TEST_STATE, UCIE_TX_TEST_STATE_DONE,
             "TX run to finish");
  pt_wait_cycles(1024);
  PT_CHECK_EQ("TX test FIFO underflow with a slow PHY test clock",
              pt_rd(base, UCIE_TX_TEST_FIFO_UNDERFLOW), 1);
  PT_CHECK_EQ("RX test FIFO overflow with a slow PHY test clock",
              pt_rd(base, UCIE_RX_TEST_FIFO_OVERFLOW), 1);
  {
    uint64_t tx = pt_meter_mhz(base, UCIE_CLK_METER_TX_DIV, PT_CHIP_MHZ);
    uint64_t rx = pt_meter_mhz(base, UCIE_CLK_METER_RX_DIV, PT_CHIP_MHZ);
    PT_CHECK(rx * 50 >= tx * 49 && rx * 50 <= tx * 51,
             "RX packet clock %lu MHz, TX %lu MHz", rx, tx);
  }

  // And back: a datapath reset on a fast enough clock clears both.
  pt_begin("ucie0", "PHY test clock back onto PLL8 /8");
  pt_wr(base, UCIE_PHY_TEST_CLK_BYPASS_EN, 0);
  apply_clk_cfg(base);
  require_clocks(base, "PHY test clock back onto PLL8 /8");
  move_main_clk(base, MAIN_PLL8, TX_DIV1);
  pt_lfsr_check(base, N_PACKETS, "after the slow PHY test clock");

  pt_begin("ucie0", "sideband clock onto PLL8 /10");
  test_sideband_on_pll(base);
  pt_check_meter(base, UCIE_CLK_METER_SB, 800, "sideband on PLL8 /10");
  PT_CHECK_EQ("PLL lock lost", pt_rd(base, UCIE_PLL_LOCK_LOST), 0);

  pt_finish("phytest-clocks");
  return 0;
}
