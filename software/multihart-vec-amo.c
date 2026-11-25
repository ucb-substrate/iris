// Runs on all three Iris harts at once:
//   hart 0, hart 1: ShuttleTile (marchid 34), Saturn with the OPU/MX datapath
//   hart 2:         RocketTile  (marchid 1),  Saturn "DMA" vector unit
//
// Part 1: every hart runs the same RVV kernel (integer MAC, widening add,
// strided load, compare + popcount, reduction, FP32 FMA) on its own buffers in
// DRAM and checks it against a scalar reference. Hart 2 exercises Rocket's
// vector unit specifically.
//
// Part 2: all three harts contend on DRAM (L2-backed) cache lines with AMOs
// (add.d/add.w/or/max/xor/swap) and LR/SC (64- and 32-bit; the 32-bit LR/SC
// shares a line with an AMO counter), then hart 0 checks every final value.
//
// Hart 0 runs main() and is the only hart that prints or exits; harts 1 and 2
// enter __main() (libgloss-htif's secondary-hart entry) and report through
// shared memory. Every wait is bounded, so a hang becomes a nonzero exit.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <riscv-pk/encoding.h>
#include <riscv_vector.h>

#define NHARTS 3
#define ROCKET_HART 2
#define MARCHID_SHUTTLE 34
#define MARCHID_ROCKET 1

#define N 203         // odd, so every strip-mined loop ends on a partial vector
#define AMO_ITERS 64
#define GO_MAGIC 0x5a5a1234u
#define DONE_MAGIC 0xd0e0000000000000ull
#define TIMEOUT_CYCLES 20000000ull
#define LRSC_MAX_RETRIES 100000

#define LINE __attribute__((aligned(64)))

// ---------------------------------------------------------------------------
// Shared state. Every field that more than one hart writes sits on its own
// 64-byte line, except lrsc32, which deliberately shares amo_add64's line so
// AMOs and LR/SC from different harts hit the same line.
// ---------------------------------------------------------------------------
static volatile uint32_t go LINE;
static volatile uint64_t arrive LINE;
static volatile uint64_t done_count LINE;

static struct {
  volatile uint64_t amo_add64;
  volatile uint32_t lrsc32;
} line_a LINE;
static volatile uint32_t amo_add32 LINE;
static volatile uint64_t amo_or LINE;
static volatile int64_t amo_max LINE;
static volatile uint32_t amo_xor LINE;
static volatile uint64_t amo_swap LINE;
static volatile uint64_t lrsc64 LINE;

typedef struct {
  volatile uint64_t marchid;
  volatile uint64_t misa;
  volatile uint64_t vlenb;
  volatile uint64_t vec_errors;
  volatile uint64_t first_bad;    // check id << 32 | element index
  volatile uint64_t swap_sum;     // sum of the values amoswap returned
  volatile uint64_t lrsc_retries; // worst-case SC failures for one update
  volatile uint64_t lrsc_fail;    // an LR/SC loop gave up
  volatile uint64_t done;         // DONE_MAGIC | hartid when the hart finishes
} LINE hart_result_t;
static hart_result_t result[NHARTS];

// Per-hart vector buffers, each hart on its own lines.
static int32_t va[NHARTS][N] LINE;
static int32_t vb[NHARTS][N] LINE;
static int32_t vc[NHARTS][N] LINE;
static int32_t vmac_out[NHARTS][N] LINE;
static uint8_t vu8a[NHARTS][N] LINE;
static uint8_t vu8b[NHARTS][N] LINE;
static uint16_t vwadd_out[NHARTS][N] LINE;
static int32_t vstrided_src[NHARTS][2 * N] LINE;
static int32_t vstrided_out[NHARTS][N] LINE;
static float vfx[NHARTS][N] LINE;
static float vfy[NHARTS][N] LINE;
static uint64_t vred_out[NHARTS] LINE;
static uint64_t vcpop_out[NHARTS] LINE;

static inline uint64_t hartid(void) { return read_csr(mhartid); }
static inline uint64_t cycles(void) { return read_csr(mcycle); }

// ---------------------------------------------------------------------------
// Part 1: vector kernel and its scalar reference
// ---------------------------------------------------------------------------

static void init_vectors(unsigned h)
{
  uint32_t x = 0x9e3779b9u * (h + 1);
  for (int i = 0; i < N; i++) {
    x = x * 1664525u + 1013904223u;
    va[h][i] = (int32_t)(x >> 7) - (1 << 23);
    vb[h][i] = (int32_t)((x >> 3) & 0xffff) - 0x8000;
    vc[h][i] = (int32_t)(x ^ (h << 28));
    vu8a[h][i] = (uint8_t)(x >> 11);
    vu8b[h][i] = (uint8_t)(x >> 19);
    // Small integers: every FP32 product and sum below is exact.
    vfx[h][i] = (float)((int)(x % 37) - 18);
    vfy[h][i] = (float)((int)((x >> 8) % 23) - 11);
    vmac_out[h][i] = 0;
    vwadd_out[h][i] = 0;
    vstrided_out[h][i] = 0;
  }
  for (int i = 0; i < 2 * N; i++)
    vstrided_src[h][i] = (int32_t)(0x1000 * h + 3 * i + 7);
}

static void run_vector_kernel(unsigned h)
{
  // vmacc: out = a * b + c (e32, LMUL=2)
  for (size_t i = 0, vl; i < N; i += vl) {
    vl = __riscv_vsetvl_e32m2(N - i);
    vint32m2_t a = __riscv_vle32_v_i32m2(&va[h][i], vl);
    vint32m2_t b = __riscv_vle32_v_i32m2(&vb[h][i], vl);
    vint32m2_t c = __riscv_vle32_v_i32m2(&vc[h][i], vl);
    c = __riscv_vmacc_vv_i32m2(c, a, b, vl);
    __riscv_vse32_v_i32m2(&vmac_out[h][i], c, vl);
  }
  // vwaddu: u8 + u8 -> u16 (e8 -> e16 widening)
  for (size_t i = 0, vl; i < N; i += vl) {
    vl = __riscv_vsetvl_e8m1(N - i);
    vuint8m1_t a = __riscv_vle8_v_u8m1(&vu8a[h][i], vl);
    vuint8m1_t b = __riscv_vle8_v_u8m1(&vu8b[h][i], vl);
    __riscv_vse16_v_u16m2(&vwadd_out[h][i], __riscv_vwaddu_vv_u16m2(a, b, vl), vl);
  }
  // vlse32: every other element of the strided source
  for (size_t i = 0, vl; i < N; i += vl) {
    vl = __riscv_vsetvl_e32m1(N - i);
    vint32m1_t s = __riscv_vlse32_v_i32m1(&vstrided_src[h][2 * i], 2 * sizeof(int32_t), vl);
    __riscv_vse32_v_i32m1(&vstrided_out[h][i], s, vl);
  }
  // vredsum of (a ^ b), and vcpop of (a < b)
  uint64_t red = 0, pop = 0;
  for (size_t i = 0, vl; i < N; i += vl) {
    vl = __riscv_vsetvl_e32m4(N - i);
    vint32m4_t a = __riscv_vle32_v_i32m4(&va[h][i], vl);
    vint32m4_t b = __riscv_vle32_v_i32m4(&vb[h][i], vl);
    vint32m4_t x = __riscv_vxor_vv_i32m4(a, b, vl);
    vint32m1_t acc = __riscv_vmv_s_x_i32m1(0, 1);
    acc = __riscv_vredsum_vs_i32m4_i32m1(x, acc, vl);
    red += (uint32_t)__riscv_vmv_x_s_i32m1_i32(acc);
    vbool8_t lt = __riscv_vmslt_vv_i32m4_b8(a, b, vl);
    pop += __riscv_vcpop_m_b8(lt, vl);
  }
  vred_out[h] = red;
  vcpop_out[h] = pop;
  // vfmacc: y = 3 * x + y (FP32, LMUL=1)
  for (size_t i = 0, vl; i < N; i += vl) {
    vl = __riscv_vsetvl_e32m1(N - i);
    vfloat32m1_t x = __riscv_vle32_v_f32m1(&vfx[h][i], vl);
    vfloat32m1_t y = __riscv_vle32_v_f32m1(&vfy[h][i], vl);
    y = __riscv_vfmacc_vf_f32m1(y, 3.0f, x, vl);
    __riscv_vse32_v_f32m1(&vfy[h][i], y, vl);
  }
  asm volatile("fence" ::: "memory");
}

// The reference must stay scalar: keep GCC from auto-vectorizing it.
#define SCALAR __attribute__((noinline, optimize("no-tree-vectorize", "no-tree-slp-vectorize")))

static SCALAR void note_error(unsigned h, uint64_t check, uint64_t idx)
{
  if (result[h].vec_errors++ == 0)
    result[h].first_bad = (check << 32) | idx;
}

static SCALAR uint64_t f32_bits(float f)
{
  union { float f; uint32_t u; } c = { .f = f };
  return c.u;
}

// Recomputes the expected values from init_vectors' generator, so it never
// reads an input the kernel could have clobbered.
static SCALAR void check_vectors(unsigned h)
{
  uint32_t x = 0x9e3779b9u * (h + 1);
  uint32_t red = 0;
  uint64_t pop = 0;
  for (int i = 0; i < N; i++) {
    x = x * 1664525u + 1013904223u;
    int32_t a = (int32_t)(x >> 7) - (1 << 23);
    int32_t b = (int32_t)((x >> 3) & 0xffff) - 0x8000;
    int32_t c = (int32_t)(x ^ (h << 28));
    uint8_t u8a = (uint8_t)(x >> 11);
    uint8_t u8b = (uint8_t)(x >> 19);
    float fx = (float)((int)(x % 37) - 18);
    float fy = (float)((int)((x >> 8) % 23) - 11);

    if ((uint32_t)vmac_out[h][i] != (uint32_t)a * (uint32_t)b + (uint32_t)c)
      note_error(h, 1, i);
    if (vwadd_out[h][i] != (uint16_t)u8a + (uint16_t)u8b)
      note_error(h, 2, i);
    if (vstrided_out[h][i] != (int32_t)(0x1000 * h + 3 * (2 * i) + 7))
      note_error(h, 3, i);
    if (f32_bits(vfy[h][i]) != f32_bits(3.0f * fx + fy))
      note_error(h, 5, i);
    red += (uint32_t)(a ^ b);
    pop += a < b;
  }
  // The kernel sums per-strip 32-bit reductions into 64 bits; compare mod 2^32.
  if ((uint32_t)vred_out[h] != red)
    note_error(h, 4, 0);
  if (vcpop_out[h] != pop)
    note_error(h, 6, 0);
}

// ---------------------------------------------------------------------------
// Part 2: AMOs and LR/SC
// ---------------------------------------------------------------------------

static inline uint64_t amoadd_d(volatile uint64_t *p, uint64_t v)
{
  uint64_t old;
  asm volatile("amoadd.d.aqrl %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
  return old;
}

static inline uint32_t amoadd_w(volatile uint32_t *p, uint32_t v)
{
  uint32_t old;
  asm volatile("amoadd.w %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
  return old;
}

static inline void amoor_d(volatile uint64_t *p, uint64_t v)
{
  asm volatile("amoor.d zero, %1, (%0)" : : "r"(p), "r"(v) : "memory");
}

static inline void amomax_d(volatile int64_t *p, int64_t v)
{
  asm volatile("amomax.d zero, %1, (%0)" : : "r"(p), "r"(v) : "memory");
}

static inline void amoxor_w(volatile uint32_t *p, uint32_t v)
{
  asm volatile("amoxor.w zero, %1, (%0)" : : "r"(p), "r"(v) : "memory");
}

static inline uint64_t amoswap_d(volatile uint64_t *p, uint64_t v)
{
  uint64_t old;
  asm volatile("amoswap.d.aq %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
  return old;
}

// Adds v with an LR/SC loop. Returns the number of failed SCs, or -1 if it
// gave up after LRSC_MAX_RETRIES.
static inline int64_t lrsc_add_d(volatile uint64_t *p, uint64_t v)
{
  for (int64_t tries = 0; tries < LRSC_MAX_RETRIES; tries++) {
    uint64_t old, fail;
    asm volatile(
        "lr.d.aq %0, (%2)\n"
        "add %0, %0, %3\n"
        "sc.d.rl %1, %0, (%2)\n"
        : "=&r"(old), "=&r"(fail)
        : "r"(p), "r"(v)
        : "memory");
    if (fail == 0) return tries;
  }
  return -1;
}

static inline int64_t lrsc_add_w(volatile uint32_t *p, uint32_t v)
{
  for (int64_t tries = 0; tries < LRSC_MAX_RETRIES; tries++) {
    uint64_t old, fail;
    asm volatile(
        "lr.w.aq %0, (%2)\n"
        "addw %0, %0, %3\n"
        "sc.w.rl %1, %0, (%2)\n"
        : "=&r"(old), "=&r"(fail)
        : "r"(p), "r"(v)
        : "memory");
    if (fail == 0) return tries;
  }
  return -1;
}

static uint64_t swap_value(unsigned h, unsigned i) { return ((uint64_t)(h + 1) << 32) | (i + 1); }
static uint32_t xor_value(unsigned h, unsigned i) { return 0x9e3779b9u * (h * AMO_ITERS + i + 1); }

static void run_amo(unsigned h)
{
  // Start barrier, so the harts really contend.
  amoadd_d(&arrive, 1);
  uint64_t t0 = cycles();
  while (arrive < NHARTS) {
    if (cycles() - t0 > TIMEOUT_CYCLES) { result[h].lrsc_fail = 2; return; }
  }

  uint64_t swap_sum = 0, worst = 0;
  for (unsigned i = 0; i < AMO_ITERS; i++) {
    amoadd_d(&line_a.amo_add64, 1);
    int64_t r = lrsc_add_w(&line_a.lrsc32, 1);
    if (r < 0) { result[h].lrsc_fail = 1; break; }
    if ((uint64_t)r > worst) worst = r;
    amoadd_w(&amo_add32, 3);
    amoor_d(&amo_or, 1ull << (h * 16 + (i & 15)));
    amomax_d(&amo_max, (int64_t)(h * 1000 + i) - 500);
    amoxor_w(&amo_xor, xor_value(h, i));
    swap_sum += amoswap_d(&amo_swap, swap_value(h, i));
    r = lrsc_add_d(&lrsc64, (uint64_t)h + 1);
    if (r < 0) { result[h].lrsc_fail = 1; break; }
    if ((uint64_t)r > worst) worst = r;
  }
  result[h].swap_sum = swap_sum;
  result[h].lrsc_retries = worst;
}

// ---------------------------------------------------------------------------
// Per-hart body and hart 0's checker
// ---------------------------------------------------------------------------

static void hart_body(unsigned h)
{
  result[h].marchid = read_csr(marchid);
  result[h].misa = read_csr(misa);
  result[h].vlenb = read_csr(vlenb);
  init_vectors(h);
  asm volatile("fence" ::: "memory");
  run_vector_kernel(h);
  check_vectors(h);
  run_amo(h);
  asm volatile("fence" ::: "memory");
  result[h].done = DONE_MAGIC | h;
  amoadd_d(&done_count, 1);
}

void __main(void)
{
  unsigned h = hartid();
  if (h >= NHARTS) { for (;;) asm volatile("wfi"); }
  while (go != GO_MAGIC) ;
  asm volatile("fence" ::: "memory");
  hart_body(h);
  for (;;) asm volatile("wfi");
}

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
  // BSS is zero, but be explicit: these are the values the checks assume.
  arrive = 0;
  done_count = 0;
  line_a.amo_add64 = 0;
  line_a.lrsc32 = 0;
  amo_add32 = 0;
  amo_or = 0;
  amo_max = -1000000;
  amo_xor = 0;
  amo_swap = 0x5555;
  lrsc64 = 0;
  for (int h = 0; h < NHARTS; h++) {
    result[h].vec_errors = 0;
    result[h].lrsc_fail = 0;
    result[h].done = 0;
  }
  asm volatile("fence" ::: "memory");
  go = GO_MAGIC;
  asm volatile("fence" ::: "memory");

  hart_body(0);

  uint64_t t0 = cycles();
  while (done_count < NHARTS) {
    if (cycles() - t0 > TIMEOUT_CYCLES) break;
  }
  asm volatile("fence" ::: "memory");

  for (int h = 0; h < NHARTS; h++) {
    uint64_t want_arch = h == ROCKET_HART ? MARCHID_ROCKET : MARCHID_SHUTTLE;
    printf("hart %d: marchid=%lu misa=0x%lx vlenb=%lu vec_errors=%lu lrsc_worst_retries=%lu done=0x%lx\n",
           h, result[h].marchid, result[h].misa, result[h].vlenb, result[h].vec_errors,
           result[h].lrsc_retries, result[h].done);
    CHECK(result[h].done == (DONE_MAGIC | h), "hart %d did not finish", h);
    CHECK(result[h].marchid == want_arch, "hart %d marchid %lu, want %lu", h, result[h].marchid, want_arch);
    CHECK(result[h].misa & (1ull << ('V' - 'A')), "hart %d misa has no V", h);
    CHECK(result[h].vec_errors == 0, "hart %d: %lu vector mismatches, first check %lu index %lu", h,
          result[h].vec_errors, result[h].first_bad >> 32, result[h].first_bad & 0xffffffff);
    CHECK(result[h].lrsc_fail == 0, "hart %d LR/SC or barrier gave up (%lu)", h, result[h].lrsc_fail);
  }

  uint64_t want_or = 0, swap_written = 0x5555, swap_returned = 0;
  uint32_t want_xor = 0;
  for (unsigned h = 0; h < NHARTS; h++) {
    want_or |= 0xffffull << (h * 16);
    for (unsigned i = 0; i < AMO_ITERS; i++) {
      want_xor ^= xor_value(h, i);
      swap_written += swap_value(h, i);
    }
    swap_returned += result[h].swap_sum;
  }
  printf("amo_add64=%lu lrsc32=%u amo_add32=%u amo_or=0x%lx amo_max=%ld amo_xor=0x%x lrsc64=%lu\n",
         line_a.amo_add64, line_a.lrsc32, amo_add32, amo_or, amo_max, amo_xor, lrsc64);
  CHECK(line_a.amo_add64 == NHARTS * AMO_ITERS, "amo_add64");
  CHECK(line_a.lrsc32 == NHARTS * AMO_ITERS, "lrsc32");
  CHECK(amo_add32 == 3 * NHARTS * AMO_ITERS, "amo_add32");
  CHECK(amo_or == want_or, "amo_or want 0x%lx", want_or);
  CHECK(amo_max == (int64_t)((NHARTS - 1) * 1000 + AMO_ITERS - 1) - 500, "amo_max");
  CHECK(amo_xor == want_xor, "amo_xor want 0x%x", want_xor);
  // Every value swapped in is either returned to some hart or still there.
  CHECK(swap_returned + amo_swap == swap_written, "amo_swap conservation");
  CHECK(lrsc64 == (uint64_t)AMO_ITERS * (1 + 2 + 3), "lrsc64");

  if (fails) {
    printf("multihart-vec-amo: %d checks FAILED\n", fails);
    return 1;
  }
  printf("multihart-vec-amo: all checks passed\n");
  return 0;
}
