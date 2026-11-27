/* skrf — measurements on the skimmer's own bitstream (docs/FPGA.md, S3).
 *
 *   skrf info                       what the FPGA says (needs our bitstream)
 *   skrf run SECS OUTPREFIX|- [cached] SLOT...   (- = count only)
 *        SLOT = HZ:RATE_K:IN[:SHIFT]   e.g. 7030000:96:1 (SHIFT: that
 *        rate's output shift, 5 bits signed, < 0 = left; default 0)
 *        configures the slots (slot s = the s-th argument), syncs, reads
 *        the ring for SECS and writes OUTPREFIX-sS.cf32 (+ .meta) for each
 *        slot, then prints words per slot, ticks, flags, peaks, the CPU the
 *        reading took and how far behind the writer the reader fell.
 *   skrf tone HZ AMP                the OUT1 test tone (AMP 0–32767, 0 = off)
 *
 * The bitstream is loaded by the caller (cat X.bit > /dev/xdevcfg) with the
 * agent stopped; loading Pavel's back (or a reboot) restores the stock board.
 * Build: rp/build-arm.sh rp/spike/skrf.c skrf.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define ADC_HZ 122.88e6
#define CFG_BASE 0x40000000u
#define STS_BASE 0x41000000u
#define RING_BASE 0x1E000000u
#define RING_SIZE (8u << 20)
#define BURST 128u
#define MAX_SLOTS 32

static volatile uint32_t *cfg, *sts;
static volatile uint64_t *ring;

static void die(const char *m) { perror(m); exit(1); }

static void map_regs(int need_ring) {
  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0) die("/dev/mem");
  cfg = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CFG_BASE);
  sts = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, STS_BASE);
  if (cfg == MAP_FAILED || sts == MAP_FAILED) die("mmap regs");
  if (need_ring) {
    ring = mmap(NULL, RING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, RING_BASE);
    if (ring == MAP_FAILED) die("mmap ring");
  }
  close(fd);
  if (sts[0] != 0x46524B53u) {
    fprintf(stderr, "not the skimmer's bitstream (sts[0] = %08x)\n", sts[0]);
    exit(2);
  }
}

static int nslots(void) { return sts[1] >> 24; }

static double now_s(clockid_t id) {
  struct timespec ts;
  clock_gettime(id, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void cmd_info(void) {
  map_regs(0);
  uint32_t v = sts[1];
  printf("SKRF %u.%u, %u slots, CIC input %u bits\n", (v >> 8) & 0xff, v & 0xff, v >> 24,
         (v >> 16) & 0xff);
  printf("ctrl %08x, writer at burst %u, flags %02x, peaks IN1 %u IN2 %u, ticks %llu\n", cfg[0],
         sts[2] & 0xffff, sts[3] & 0xff, sts[4] & 0xffff, sts[4] >> 16,
         (unsigned long long)sts[5] | (unsigned long long)sts[6] << 32);
}

static void cmd_tone(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "skrf tone HZ AMP\n"); exit(1); }
  map_regs(0);
  double hz = atof(argv[2]);
  cfg[9] = (uint32_t)llround(hz / ADC_HZ * 4294967296.0);
  cfg[10] = (uint32_t)atoi(argv[3]) & 0xffff;
  printf("OUT1: %.0f Hz, amplitude %u\n", hz, cfg[10]);
}

typedef struct {
  double hz;
  int rate_k, in, shift, ri;
  FILE *f;
  long words, vc_jumps;
  unsigned last_vc;
  float buf[2 * 4096];
  int nb;
} Slot;

static int rate_index(int k) {
  switch (k) {
  case 384: return 0;
  case 192: return 1;
  case 96: return 2;
  case 48: return 3;
  }
  return -1;
}

static void flush_slot(Slot *s) {
  if (s->nb) fwrite(s->buf, sizeof(float), 2 * s->nb, s->f);
  s->nb = 0;
}

static void cmd_run(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "skrf run SECS OUTPREFIX [cached] HZ:RATE_K:IN[:SHIFT]...\n"); exit(1); }
  double secs = atof(argv[2]);
  const char *prefix = argv[3];
  const int nosave = !strcmp(prefix, "-"); /* "-": count, keep nothing */
  int a = 4, cached = 0;
  if (!strcmp(argv[a], "cached")) { cached = 1; a++; }
  map_regs(1);
  int n = nslots(), ns = argc - a;
  if (ns > n) { fprintf(stderr, "%d slots asked, the bitstream has %d\n", ns, n); exit(1); }
  static Slot sl[MAX_SLOTS];
  uint32_t ins = 0, ena = 0, mask[4] = {0}, shifts = 0;
  int shift_of[4] = {0, 0, 0, 0}; /* unity gain (bitstream 0.2) */
  for (int s = 0; s < ns; s++) {
    Slot *x = &sl[s];
    x->shift = 99;
    if (sscanf(argv[a + s], "%lf:%d:%d:%d", &x->hz, &x->rate_k, &x->in, &x->shift) < 3 ||
        (x->ri = rate_index(x->rate_k)) < 0 || (x->in != 1 && x->in != 2)) {
      fprintf(stderr, "bad slot %s\n", argv[a + s]);
      exit(1);
    }
    if (x->shift != 99) shift_of[x->ri] = x->shift; /* < 0: a left shift */
    ena |= 1u << s;
    if (x->in == 2) ins |= 1u << s;
    mask[x->ri] |= 1u << s;
    if (nosave) continue;
    char path[512];
    snprintf(path, sizeof path, "%s-s%d.cf32", prefix, s);
    if (!(x->f = fopen(path, "wb"))) die(path);
    snprintf(path, sizeof path, "%s-s%d.cf32.meta", prefix, s);
    FILE *m = fopen(path, "w");
    if (!m) die(path);
    fprintf(m, "rate=%d\ncenter_hz=%.0f\ninput=%d\nsource=skrf\n", x->rate_k * 1000, x->hz, x->in);
    fclose(m);
  }
  for (int r = 0; r < 4; r++) shifts |= (uint32_t)(shift_of[r] & 0x1f) << (8 * r);

  /* hold everything, configure, then DMA on and run (the sync) */
  cfg[0] = 0;
  usleep(1000);
  cfg[1] = ins;
  cfg[2] = ena;
  for (int r = 0; r < 4; r++) cfg[3 + r] = mask[r];
  cfg[7] = shifts;
  cfg[8] = RING_BASE;
  for (int s = 0; s < n; s++)
    cfg[11 + s] = s < ns ? (uint32_t)llround(sl[s].hz / ADC_HZ * 4294967296.0) : 0;
  memset((void *)ring, 0, RING_SIZE);
  cfg[0] = 4u | (cached ? 8u : 0u);
  usleep(1000);
  cfg[0] = 5u | (cached ? 8u : 0u);

  double t0 = now_s(CLOCK_MONOTONIC), c0 = now_s(CLOCK_PROCESS_CPUTIME_ID);
  uint32_t rd = 0; /* next burst to read */
  const uint32_t nbursts = RING_SIZE / BURST;
  long ticks = 0, peaks = 0, bad = 0, laps = 0;
  unsigned maxlag = 0, flags = 0, pk1 = 0, pk2 = 0;
  while (now_s(CLOCK_MONOTONIC) - t0 < secs) {
    uint32_t wr = sts[2] & 0xffff;
    /* the writer's counter moves at the address phase: the last two
       bursts may still be in flight */
    uint32_t avail = (wr - rd - 2) & (nbursts - 1);
    if (avail > nbursts / 2) avail = 0; /* just behind the writer */
    if (avail > maxlag) maxlag = avail;
    if (avail > nbursts - 64) laps++;
    if (!avail) { usleep(500); continue; }
    while (avail--) {
      /* one burst out of the uncached ring with NEON (245 MB/s against
         66 MB/s for 64-bit loads, measured), then parse it cached */
      uint64_t b[BURST / 8] __attribute__((aligned(16)));
      const volatile uint64_t *src = ring + rd * (BURST / 8);
      uint64_t *dst = b;
      __asm__ volatile("vld1.64 {d0-d3}, [%0]!\n\t"
                       "vld1.64 {d4-d7}, [%0]!\n\t"
                       "vld1.64 {d16-d19}, [%0]!\n\t"
                       "vld1.64 {d20-d23}, [%0]\n\t"
                       "vst1.64 {d0-d3}, [%1]!\n\t"
                       "vst1.64 {d4-d7}, [%1]!\n\t"
                       "vst1.64 {d16-d19}, [%1]!\n\t"
                       "vst1.64 {d20-d23}, [%1]\n\t"
                       : "+r"(src), "+r"(dst)
                       :
                       : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d16", "d17", "d18", "d19",
                         "d20", "d21", "d22", "d23", "memory");
      for (int k = 0; k < (int)(BURST / 8); k++) {
        uint64_t w = b[k];
        unsigned tag = w >> 56, slot = (w >> 48) & 0x1f;
        /* 0.3 and later: tag 01rrvvvv; 0.1/0.2: 0x10 + r */
        int fmt3 = (tag >> 6) == 1;
        if (fmt3 || (tag >= 0x10 && tag <= 0x13)) {
          if (!fmt3) slot = (w >> 48) & 0xff;
          if (slot >= (unsigned)ns) { bad++; continue; }
          Slot *x = &sl[slot];
          if (fmt3) { /* the vector number, mod 128 = the sample index */
            unsigned vc = (tag & 0xf) | ((w >> 53) & 7) << 4;
            if (x->words && vc != ((x->last_vc + 1) & 127)) x->vc_jumps++;
            x->last_vc = vc;
          }
          int32_t i = (int32_t)((uint32_t)((w & 0xff) << 24 | ((w >> 8) & 0xff) << 16 | ((w >> 16) & 0xff) << 8)) >> 8;
          int32_t q = (int32_t)((uint32_t)(((w >> 24) & 0xff) << 24 | ((w >> 32) & 0xff) << 16 | ((w >> 40) & 0xff) << 8)) >> 8;
          x->words++;
          if (nosave) continue;
          x->buf[2 * x->nb] = i / 8388608.0f;
          x->buf[2 * x->nb + 1] = q / 8388608.0f;
          if (++x->nb == 4096) flush_slot(x);
        } else if (tag == 0xff) {
          ticks++;
          flags |= slot;
        } else if (tag == 0xfe) {
          peaks++;
          unsigned p1 = (w & 0xff) << 8 | ((w >> 8) & 0xff), p2 = ((w >> 16) & 0xff) << 8 | ((w >> 24) & 0xff);
          if (p1 > pk1) pk1 = p1;
          if (p2 > pk2) pk2 = p2;
        } else
          bad++;
      }
      rd = (rd + 1) & (nbursts - 1);
    }
  }
  double el = now_s(CLOCK_MONOTONIC) - t0, cpu = now_s(CLOCK_PROCESS_CPUTIME_ID) - c0;
  cfg[0] = 0;
  for (int s = 0; s < ns; s++) {
    if (!nosave) {
      flush_slot(&sl[s]);
      fclose(sl[s].f);
    }
    printf("slot %d: %.0f Hz %d k IN%d — %ld samples, %.1f/s (%.4f of the rate), %ld index jumps\n", s,
           sl[s].hz, sl[s].rate_k, sl[s].in, sl[s].words, sl[s].words / el,
           sl[s].words / el / (sl[s].rate_k * 1000.0), sl[s].vc_jumps);
  }
  printf("%.1f s: %ld ticks (%.1f/s), %ld peak words, %ld unknown words, flags %02x (sts %02x)\n", el,
         ticks, ticks / el, peaks, bad, flags, sts[3] & 0xff);
  printf("peaks IN1 %u IN2 %u (of 32768); reader CPU %.1f %% of a core, max lag %u bursts, %ld near-laps\n",
         pk1, pk2, 100.0 * cpu / el, maxlag, laps);
}

/* rdbw [sync]: how fast the ARM reads the ring's memory (uncached via
   /dev/mem): plain 64-bit loads, and NEON 32-byte loads; "sync" opens
   /dev/mem with O_SYNC. Needs no bitstream: the ring is plain DDR. */
static void cmd_rdbw(int argc, char **argv) {
  int fd = open("/dev/mem", O_RDWR | (argc > 2 && !strcmp(argv[2], "sync") ? O_SYNC : 0));
  if (fd < 0) die("/dev/mem");
  volatile uint64_t *m = mmap(NULL, RING_SIZE, PROT_READ, MAP_SHARED, fd, RING_BASE);
  if (m == MAP_FAILED) die("mmap ring");
  for (int mode = 0; mode < 2; mode++) {
    double t0 = now_s(CLOCK_MONOTONIC);
    uint64_t acc = 0;
    size_t bytes = 0;
    while (now_s(CLOCK_MONOTONIC) - t0 < 1.0) {
      const size_t n = RING_SIZE / 8;
      if (mode == 0) {
        for (size_t i = 0; i < n; i++) acc += m[i];
      } else {
        for (size_t i = 0; i < n; i += 4) {
          const volatile uint64_t *p = m + i;
          __asm__ volatile("vld1.64 {d0-d3}, [%0]" : : "r"(p) : "d0", "d1", "d2", "d3", "memory");
        }
      }
      bytes += RING_SIZE;
    }
    double dt = now_s(CLOCK_MONOTONIC) - t0;
    printf("%s: %.1f MB/s (%llx)\n", mode ? "NEON 32-byte loads" : "64-bit loads", bytes / dt / 1e6,
           (unsigned long long)acc);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "skrf info | run SECS OUTPREFIX [cached] SLOT... | tone HZ AMP\n");
    return 1;
  }
  if (!strcmp(argv[1], "info")) cmd_info();
  else if (!strcmp(argv[1], "run")) cmd_run(argc, argv);
  else if (!strcmp(argv[1], "tone")) cmd_tone(argc, argv);
  else if (!strcmp(argv[1], "rdbw")) cmd_rdbw(argc, argv);
  else { fprintf(stderr, "unknown command %s\n", argv[1]); return 1; }
  return 0;
}
