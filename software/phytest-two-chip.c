// PhyTest between two chips, each running this program. UCIe1 of each chip
// is wired to UCIe0 of the other: the two-chip ring in simulation, or two
// chips on a package.
//
// Each direction is tested in its own phase. A chip starts a TX only after
// the partner reports, over the sideband of the same link, that the RX it
// feeds is armed. Each report is resent until the partner acknowledges it, so
// the chips may start at different times. The programs need nothing else to
// stay in step: no shared clock, no host, no matching code timing.
//
//   my UCIe1 TX --> partner UCIe0 RX     my UCIe0 sideband --> partner UCIe1
//   my UCIe0 TX --> partner UCIe1 RX     my UCIe1 sideband --> partner UCIe0

#include "phytest.h"

#define N_PACKETS 64
#define PERIOD 5
#define N_MANUAL (8 * PERIOD)

#define ARMED(phase) (0x5054000041524d00ULL | (phase))
#define ACKED(phase) (0x5054000041434b00ULL | (phase))
#define IS_MESSAGE(p) \
  (((p) & ~0xffULL) == ARMED(0) || ((p) & ~0xffULL) == ACKED(0))

// Core cycles between resends of an unacknowledged message, and the longest
// a handshake waits for the partner. A message sent before the partner has
// set up its link is lost, so only a resend reaches a partner that started
// late.
#define RESEND_CYCLES 50000ULL
#define HANDSHAKE_TIMEOUT (1000 * PT_TIMEOUT)

// Pops a sideband packet from `base` if one is waiting. A packet that isn't
// one of ours means the receiver came out of reset mid-packet and counts
// bits out of step, so realign it. The sender's next resend gets through.
static int sb_poll(uintptr_t base, uint64_t *packet) {
  if (!pt_rd(base, UCIE_SB_RX_VALID)) return 0;
  *packet = pt_rd(base, UCIE_SB_RX_PACKET);
  pt_wr(base, UCIE_SB_RX_POP, 1);
  if (IS_MESSAGE(*packet)) return 1;
  pt_wr(base, UCIE_SB_RX_RST, 1);
  return 0;
}

// Tells the partner, on the sideband of `rx`'s link, that `rx` is armed, and
// resends until the partner acknowledges on that link. Meanwhile waits on the
// sideband of `tx`'s link for the partner to say the RX that `tx` feeds is
// armed, and acknowledges every copy. Done when both have happened. A packet
// for another phase is a late copy, or the partner running ahead. It is
// dropped; if it matters, a resend brings it back.
static int handshake(uintptr_t rx, uintptr_t tx, uint64_t phase) {
  int acked = 0, armed = 0, sent = 0;
  uint64_t start = rdcycle(), last_send = 0, got;
  while (!(acked && armed)) {
    uint64_t now = rdcycle();
    if (now - start > HANDSHAKE_TIMEOUT) {
      PT_CHECK(0, "partner never %s phase %lu",
               armed ? "acknowledged" : "armed its RX for", phase);
      return 0;
    }
    if (!acked && (!sent || now - last_send > RESEND_CYCLES)) {
      if (!pt_sb_send(rx, ARMED(phase))) return 0;
      last_send = now;
      sent = 1;
    }
    if (sb_poll(rx, &got) && got == ACKED(phase)) acked = 1;
    if (sb_poll(tx, &got) && got == ARMED(phase)) {
      armed = 1;
      if (!pt_sb_send(tx, ACKED(phase))) return 0;
    }
  }
  return 1;
}

// One direction: an LFSR run from `tx` here to the partner, while `rx` here
// takes the partner's run the same way round.
static void lfsr_phase(uintptr_t tx, uintptr_t rx, uintptr_t idle_rx,
                       uint64_t phase, const char *what) {
  pt_set_seeds(tx, pt_seed, pt_seed);
  pt_set_seeds(rx, pt_seed, pt_seed);
  pt_arm(rx, UCIE_TX_TEST_MODE_LFSR, N_PACKETS);
  pt_arm(tx, UCIE_TX_TEST_MODE_LFSR, N_PACKETS);
  if (!handshake(rx, tx, phase)) return;
  pt_wr(tx, UCIE_TX_EXECUTE, 1);
  pt_wait_eq(tx, UCIE_TX_TEST_STATE, UCIE_TX_TEST_STATE_DONE, "TX run");
  pt_wait_ge(rx, UCIE_RX_PACKETS_RECEIVED, N_PACKETS, "RX packets");
  PT_CHECK_EQ("RX packets received", pt_rd(rx, UCIE_RX_PACKETS_RECEIVED),
              N_PACKETS);
  pt_check_errors(rx, PT_MAINBAND_MASK, pt_zero, what);
  pt_check_fifo_flags(rx, what);
  pt_check_fifo_flags(tx, what);
  // The other link carried nothing this phase, so its RX framed nothing:
  // each RX hears only the TX it is wired to.
  PT_CHECK_EQ("packets on the link with no traffic",
              pt_rd(idle_rx, UCIE_RX_PACKETS_RECEIVED), 0);
}

// The reference the RX scores against: the partner's pattern, with one word
// wrong by two bits on purpose, so the check has to find exactly those.
#define WRONG_ROW 3
#define WRONG_LANE 2
static uint32_t reference_word(int row, int lane) {
  uint32_t w = pt_pattern_word(row, lane);
  return row == WRONG_ROW && lane == WRONG_LANE ? w ^ 0x5 : w;
}
static uint64_t manual_errors(int lane) {
  return lane == WRONG_LANE ? 2 * (N_MANUAL / PERIOD) : 0;
}

// A manual pattern from UCIe1 here, scored by the partner's UCIe0 against
// the copy in its own TX pattern SRAM.
static void manual_check_phase(uintptr_t tx, uintptr_t rx, uint64_t phase) {
  reset_fsms(tx);
  reset_fsms(rx);
  pt_load_pattern(tx, PERIOD, pt_pattern_word);
  pt_load_pattern(rx, PERIOD, reference_word);
  pt_wr(rx, UCIE_RX_MANUAL_CHECK, 1);
  pt_arm(rx, UCIE_TX_TEST_MODE_MANUAL, N_MANUAL);
  pt_arm(tx, UCIE_TX_TEST_MODE_MANUAL, N_MANUAL);
  if (handshake(rx, tx, phase)) {
    pt_wr(tx, UCIE_TX_EXECUTE, 1);
    pt_wait_eq(tx, UCIE_TX_TEST_STATE, UCIE_TX_TEST_STATE_DONE, "TX run");
    pt_wait_ge(rx, UCIE_RX_PACKETS_RECEIVED, N_MANUAL, "RX packets");
    pt_wait_cycles(256);
    pt_check_errors(rx, PT_MAINBAND_MASK, manual_errors, "manual pattern check");
  }
  pt_wr(rx, UCIE_RX_MANUAL_CHECK, 0);
  pt_wr(rx, UCIE_TX_MANUAL_REPEAT_PERIOD, 0);
  pt_wr(tx, UCIE_TX_MANUAL_REPEAT_PERIOD, 0);
}

int main(void) {
  const uintptr_t u0 = UCIE0_REG_BASE, u1 = UCIE1_REG_BASE;

  pt_begin("both", "setup");
  pt_setup(u0);
  pt_setup(u1);

  pt_begin("ucie1->partner ucie0", "LFSR");
  lfsr_phase(u1, u0, u1, 1, "LFSR into ucie0");

  pt_begin("ucie0->partner ucie1", "LFSR");
  lfsr_phase(u0, u1, u0, 2, "LFSR into ucie1");

  pt_begin("ucie1->partner ucie0", "manual pattern check");
  manual_check_phase(u1, u0, 3);

  pt_finish("phytest-two-chip");
  return 0;
}
