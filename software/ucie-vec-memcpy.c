// two-chip UCIe RVV memcpy test
//
// hart 0/1 are the Shuttle/OPU cores 
// hart 2 is the Rocket tile with Saturn DMA
// BENCH_HART_ID picks which hart runs this test

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <riscv-pk/encoding.h>
#include <riscv_vector.h>
#include "mmio.h"
#include "router.h"
#include "ucie.h"
#include "rvv_memcpy.h"

#define UCIE0_REG_BASE 0x200000UL
#define UCIE1_REG_BASE 0x208000UL
#define SCRATCHPAD_BASE 0x08000000UL
#define OFFCHIP_OFFSET  0x800000000L

// Logical memcpy size per run. Tunable: -DCOPY_BYTES=<n>.
#ifndef COPY_BYTES
#define COPY_BYTES 256
#endif

// Which hart runs the test. Hart 2 is the Saturn-DMA-only rocket tile added
// by IrisConfig's "Saturn DMA" line. Tunable: -DBENCH_HART_ID=<n>.
#ifndef BENCH_HART_ID
#define BENCH_HART_ID 2
#endif

// Aligned to the 32-byte beat so the natural-alignment path is exercised.
static uint8_t src[COPY_BYTES] __attribute__((aligned(64)));
static uint8_t dst[COPY_BYTES] __attribute__((aligned(64)));

// Chip 2's local view of the done flag: chip 1 writes 1 here over UCIe;
// chip 2 polls it locally. In the MBus scratchpad so it stays off the cache.
static volatile uint64_t * const done_flag =
    (volatile uint64_t *)SCRATCHPAD_BASE;

void __main(void)
{
  if (read_csr(mhartid) != BENCH_HART_ID) { while (1); }

  uint64_t my_chip_id = reg_read64(CHIP_ID_ADDR);
  printf("Chip %lu: starting (COPY_BYTES=%d)\n",
         (unsigned long)my_chip_id, COPY_BYTES);

  // Both chips know the same deterministic source pattern.
  for (int i = 0; i < COPY_BYTES; i++) src[i] = (uint8_t)(i & 0xff);

  if (my_chip_id == 1) {
    // ============================================================
    // Sender: chip 1's UCIe0/port 0 -> chip 2. RVV-memcpy src into chip 2's
    // `dst`, then raise the flag.
    // ============================================================
    setup_ucie(UCIE0_REG_BASE);
    reg_write64(UCIE0_REG_BASE + UCIE_MAINBAND_MODE, UCIE_BAND_MODE_TL);
    program_router(0, 2, 0);

    uint64_t peer_chip_id = 2;
    void *remote_dst =
        (void *)((uint8_t *)dst + peer_chip_id * OFFCHIP_OFFSET);
    volatile uint64_t *remote_done_flag =
        (volatile uint64_t *)(SCRATCHPAD_BASE +
                              peer_chip_id * OFFCHIP_OFFSET);

    // Make the source writes visible before the vector store issues.
    __sync_synchronize();

    uint64_t t0 = read_csr(mcycle);
    memcpy_vec(remote_dst, src, COPY_BYTES);
    asm volatile("fence");
    uint64_t t1 = read_csr(mcycle);

    uint64_t cycles = t1 - t0;
    printf("Chip 1: RVV memcpy %d bytes in %lu cycles (%lu cycles/byte)\n",
           COPY_BYTES, (unsigned long)cycles,
           (unsigned long)(cycles / COPY_BYTES));

    // Order the dst writes ahead of the flag store.
    __sync_synchronize();
    *remote_done_flag = 1;
    printf("Chip 1: flag raised, exiting\n");
  } else if (my_chip_id == 2) {
    // ============================================================
    // Verifier: chip 2's UCIe1/port 1 -> chip 1. Wait for flag, then compare
    // `dst` to the pattern.
    // ============================================================
    setup_ucie(UCIE1_REG_BASE);
    reg_write64(UCIE1_REG_BASE + UCIE_MAINBAND_MODE, UCIE_BAND_MODE_TL);
    program_router(0, 1, 1);

    *done_flag = 0;
    __sync_synchronize();

    printf("Chip 2: waiting for chip 1\n");
    uint64_t t_wait_start = read_csr(mcycle);
    while (*done_flag == 0);
    uint64_t t_wait_end = read_csr(mcycle);
    __sync_synchronize();
    printf("Chip 2: flag observed after %lu cycles\n",
           (unsigned long)(t_wait_end - t_wait_start));

    uint64_t t0 = read_csr(mcycle);
    for (int i = 0; i < COPY_BYTES; i++) {
      if (dst[i] != (uint8_t)(i & 0xff)) {
        printf("Chip 2: FAIL at byte %d: got 0x%02x, want 0x%02x\n",
               i, dst[i], (uint8_t)(i & 0xff));
        exit(1);
      }
    }
    uint64_t t1 = read_csr(mcycle);
    printf("Chip 2: verified %d bytes locally in %lu cycles\n",
           COPY_BYTES, (unsigned long)(t1 - t0));
    printf("Chip 2: passed\n");
  } else {
    printf("Chip %lu: unexpected chip_id (expected 1 or 2)\n",
           (unsigned long)my_chip_id);
    exit(1);
  }

  exit(0);
}

int main(void)
{
  __main();
  return 0;
}
