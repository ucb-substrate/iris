// Programs the clock dividers in clk-div-ctrl@120000 and checks that every
// hart keeps working, and that its clock really changed, after each change.
//
// Divider registers (8 bits each). A value v divides io.clock by v + 1; 0 passes
// it through undivided (testchipip ClockDivideOrPass):
//   0x120000 uncore (sbus/mbus/cbus/pbus/fbus/d2d)
//   0x120004 ShuttleTile 0 (hart 0)
//   0x120008 ShuttleTile 1 (hart 1)
//   0x12000c RocketTile    (hart 2)
//   0x120010 clock_tap output (checked by the harness's period monitor)
//
// Each hart measures its core clock against the CLINT's mtime. mtime advances
// once every 1000 uncore cycles (timebase 500 kHz at the 500 MHz uncore), so
// for core clock io/(t) and uncore clock io/(u) a hart sees
//   mcycle per mtime tick = 1000 * u / t.
// After each change every hart runs a small memory self-check. Hart 0 drives
// the sequence and is the only hart that prints (over HTIF, which does not
// depend on the uncore clock, unlike the UART).

#include <stdint.h>
#include <stdio.h>
#include <riscv-pk/encoding.h>
#include "mmio.h"

#define NHARTS 3
#define CLKDIV_BASE 0x120000UL
#define CLKDIV_UNCORE (CLKDIV_BASE + 0x00)
#define CLKDIV_TILE(h) (CLKDIV_BASE + 0x04 + 4 * (h))
#define CLKDIV_TAP (CLKDIV_BASE + 0x10)
#define CLINT_MTIME 0x200bff8UL

#define CYCLES_PER_TICK 1000 // undivided core cycles per mtime tick
#define MEASURE_TICKS 4
#define TOLERANCE_PCT 10
#define ACK_TIMEOUT_TICKS 2000
#define BUF_WORDS 1024 // 8 KiB per hart

#define LINE __attribute__((aligned(64)))

typedef struct {
  uint8_t uncore, tile[NHARTS], tap; // divide ratios (1 = undivided)
} clk_config_t;

static const clk_config_t configs[] = {
  {1, {1, 1, 1}, 1},
  // uncore alone
  {2, {1, 1, 1}, 2},
  {3, {1, 1, 1}, 3},
  {7, {1, 1, 1}, 7},
  // one tile at a time
  {1, {2, 1, 1}, 1},
  {1, {1, 3, 1}, 1},
  {1, {1, 1, 7}, 1},
  // mixed
  {1, {2, 3, 7}, 2},
  {3, {7, 2, 3}, 3},
  {2, {3, 7, 2}, 7},
  // back to undivided
  {1, {1, 1, 1}, 1},
};
#define NCONFIGS (sizeof(configs) / sizeof(configs[0]))

static volatile uint64_t epoch LINE;
static struct {
  volatile uint64_t ack;             // epoch this hart last finished
  volatile uint64_t cycles_per_tick; // measured
  volatile uint64_t errors;          // self-check mismatches
} LINE status[NHARTS];
static uint64_t buf[NHARTS][BUF_WORDS] LINE;

static inline uint64_t mtime(void) { return reg_read64(CLINT_MTIME); }

static uint64_t measure_cycles_per_tick(void)
{
  uint64_t t = mtime();
  while (mtime() == t) ; // align to a tick edge
  uint64_t c0 = read_csr(mcycle);
  uint64_t t0 = mtime();
  while (mtime() < t0 + MEASURE_TICKS) ;
  uint64_t c1 = read_csr(mcycle);
  uint64_t t1 = mtime();
  return (c1 - c0) / (t1 - t0);
}

static uint64_t self_check(unsigned h, uint64_t e)
{
  uint64_t seed = 0x9e3779b97f4a7c15ull * (e + 1) + h;
  uint64_t errors = 0;
  for (int i = 0; i < BUF_WORDS; i++)
    buf[h][i] = seed ^ ((uint64_t)i * 0x100000001b3ull);
  asm volatile("fence" ::: "memory");
  // Read back in reverse, then a dependent checksum, so it is not just store
  // forwarding of the last few lines.
  for (int i = BUF_WORDS - 1; i >= 0; i--)
    if (buf[h][i] != (seed ^ ((uint64_t)i * 0x100000001b3ull)))
      errors++;
  uint64_t sum = 0, want = 0;
  for (int i = 0; i < BUF_WORDS; i++) {
    sum = (sum << 1 | sum >> 63) ^ buf[h][i];
    want = (want << 1 | want >> 63) ^ (seed ^ ((uint64_t)i * 0x100000001b3ull));
  }
  return errors + (sum != want);
}

static void run_round(unsigned h, uint64_t e)
{
  status[h].cycles_per_tick = measure_cycles_per_tick();
  status[h].errors = self_check(h, e);
  asm volatile("fence" ::: "memory");
  status[h].ack = e;
}

void __main(void)
{
  unsigned h = read_csr(mhartid);
  uint64_t seen = 0;
  if (h >= NHARTS) for (;;) asm volatile("wfi");
  for (;;) {
    uint64_t e = epoch;
    if (e != seen) {
      asm volatile("fence" ::: "memory");
      run_round(h, e);
      seen = e;
    }
  }
}

// Writes the dividers for c and returns the number of registers that did not
// read back what was written.
static int program(const clk_config_t *c)
{
  int fails = 0;
  reg_write32(CLKDIV_UNCORE, c->uncore - 1);
  for (int h = 0; h < NHARTS; h++)
    reg_write32(CLKDIV_TILE(h), c->tile[h] - 1);
  reg_write32(CLKDIV_TAP, c->tap - 1);
  // Read back each register; the read also orders the writes before the
  // measurement starts.
  if (reg_read32(CLKDIV_UNCORE) != (uint32_t)(c->uncore - 1)) {
    printf("FAIL: uncore divider readback\n");
    fails++;
  }
  for (int h = 0; h < NHARTS; h++) {
    if (reg_read32(CLKDIV_TILE(h)) != (uint32_t)(c->tile[h] - 1)) {
      printf("FAIL: tile %d divider readback\n", h);
      fails++;
    }
  }
  if (reg_read32(CLKDIV_TAP) != (uint32_t)(c->tap - 1)) {
    printf("FAIL: tap divider readback\n");
    fails++;
  }
  // Let the divisor synchronizers and the in-flight divided period settle.
  uint64_t t = mtime();
  while (mtime() < t + 2) ;
  return fails;
}

int main(void)
{
  int fails = 0;
  for (int h = 0; h < NHARTS; h++) status[h].ack = 0;
  epoch = 0;
  asm volatile("fence" ::: "memory");

  for (unsigned n = 0; n < NCONFIGS; n++) {
    const clk_config_t *c = &configs[n];
    uint64_t e = n + 1;
    printf("clock-div config %u: uncore /%u, tiles /%u /%u /%u, tap /%u\n", n, c->uncore,
           c->tile[0], c->tile[1], c->tile[2], c->tap);
    fails += program(c);

    asm volatile("fence" ::: "memory");
    epoch = e;
    asm volatile("fence" ::: "memory");
    run_round(0, e);

    uint64_t t0 = mtime();
    for (int h = 1; h < NHARTS; h++) {
      while (status[h].ack != e && mtime() - t0 < ACK_TIMEOUT_TICKS) ;
    }
    asm volatile("fence" ::: "memory");

    for (int h = 0; h < NHARTS; h++) {
      uint64_t want = (uint64_t)CYCLES_PER_TICK * c->uncore / c->tile[h];
      uint64_t got = status[h].cycles_per_tick;
      int ok = status[h].ack == e && status[h].errors == 0 &&
               got * 100 >= want * (100 - TOLERANCE_PCT) &&
               got * 100 <= want * (100 + TOLERANCE_PCT);
      printf("  hart %d: %lu cycles/tick (want %lu), self-check errors %lu, ack %lu: %s\n", h, got,
             want, status[h].errors, status[h].ack, ok ? "ok" : "FAIL");
      fails += !ok;
    }
  }

  if (fails) {
    printf("clock-div: %d checks FAILED\n", fails);
    return 1;
  }
  printf("clock-div: all checks passed\n");
  return 0;
}
