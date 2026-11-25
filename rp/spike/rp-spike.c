/* rp-spike.c — F0 measurements on the STOCK sdr_receiver_hpsdr_122_88
 * bitstream of a Red Pitaya STEMlab 122.88-16 (docs/REDPITAYA.md).
 *
 * Questions it answers, nothing more:
 *   1. does rx_rate below 160 (80 → 768 kHz, 40 → 1.536 MS/s) give valid IQ,
 *      and can the ARM read the FIFOs at that rate (overflows, CPU)?
 *   2. are the two instances (eth0 = DDC 0–7, mvl0 = DDC 8–15) coherent:
 *      a constant phase and a known sample offset after a common reset?
 *   3. how many Mb/s of UDP can the board send (sendmmsg, 1032-byte packets)?
 *
 * Register map = Pavel Demin's sdr-receiver-hpsdr.c (MIT) for this bitstream:
 *   cfg 0x40001000: eth0 rst +0, rate +4 (u16), sel +6 (u8), freq[8] +8;
 *                   mvl0 rst +40, rate +44, sel +46, freq[8] +48
 *   sts 0x40000000: eth0 cntr[8] +12 (u16), mvl0 cntr[8] +28
 *   FIFOs: eth0 0x40010000 + i·0x2000, mvl0 0x40020000 + i·0x2000 (u64 reads)
 *
 * Every command that writes a register refuses an instance whose
 * sdr-receiver-hpsdr still runs (a client may be streaming from it).
 * Run from /tmp: a reboot of the board brings back its own start.sh.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ADC_HZ 122.88e6

typedef struct {
  const char        *iface;
  volatile uint8_t  *rst;
  volatile uint16_t *rate;
  volatile uint8_t  *sel;
  volatile uint32_t *freq[8];
  volatile uint16_t *cntr[8];
  volatile uint64_t *data[8];
} Inst;

static Inst inst[2];

static double now_s(clockid_t id) {
  struct timespec t;
  clock_gettime(id, &t);
  return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static void map_all(int need_data) {
  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0) { perror("/dev/mem"); exit(1); }
  const long pg = sysconf(_SC_PAGESIZE);
  volatile uint8_t *sts = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x40000000);
  volatile uint8_t *cfg = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x40001000);
  if (sts == MAP_FAILED || cfg == MAP_FAILED) { perror("mmap"); exit(1); }
  for (int k = 0; k < 2; k++) {
    Inst *in = &inst[k];
    const int c0 = k ? 40 : 0, s0 = k ? 28 : 12;
    in->iface = k ? "mvl0" : "eth0";
    in->rst = cfg + c0;
    in->rate = (volatile uint16_t *)(cfg + c0 + 4);
    in->sel = cfg + c0 + 6;
    for (int i = 0; i < 8; i++) {
      in->freq[i] = (volatile uint32_t *)(cfg + c0 + 8 + i * 4);
      in->cntr[i] = (volatile uint16_t *)(sts + s0 + i * 2);
      if (need_data) {
        void *p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                       (k ? 0x40020000 : 0x40010000) + i * 0x2000);
        if (p == MAP_FAILED) { perror("mmap fifo"); exit(1); }
        in->data[i] = p;
      }
    }
  }
}

/* The stock server for iface still runs: TRUE (and its pid). */
static int server_runs(const char *iface, int *pid_out) {
  DIR *d = opendir("/proc");
  struct dirent *e;
  int found = 0;
  while (d && (e = readdir(d)) && !found) {
    char path[300], buf[512];
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) continue;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) continue;
    buf[n] = 0;
    const char *arg0 = buf, *arg1 = buf + strlen(buf) + 1;
    if (arg1 < buf + n && strstr(arg0, "sdr-receiver-hpsdr") && strcmp(arg1, iface) == 0) {
      found = 1;
      if (pid_out) *pid_out = atoi(e->d_name);
    }
  }
  if (d) closedir(d);
  return found;
}

static Inst *writable(const char *iface) {
  Inst *in = strcmp(iface, "mvl0") == 0 ? &inst[1] : strcmp(iface, "eth0") == 0 ? &inst[0] : NULL;
  if (!in) { fprintf(stderr, "interface eth0 or mvl0\n"); exit(2); }
  int pid = 0;
  if (server_runs(in->iface, &pid)) {
    fprintf(stderr, "sdr-receiver-hpsdr %s still runs (pid %d) — stop that one first; "
                    "this program never writes a register a server owns\n", in->iface, pid);
    exit(3);
  }
  return in;
}

static uint32_t phase_word(double hz) { return (uint32_t)floor(hz / ADC_HZ * (1 << 30) + 0.5); }

static void reset(Inst *in) {
  *in->rst |= 1;
  *in->rst &= ~1;
}

/* One FIFO word → I, Q (the 6 bytes the server copies to the wire as
 * I[3] Q[3], 24-bit big-endian; the wire is RF-inverted, kept raw here). */
static void word_iq(uint64_t w, float *i, float *q) {
  const uint8_t *b = (const uint8_t *)&w;
  int32_t vi = (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8) >> 8;
  int32_t vq = (int32_t)((uint32_t)b[3] << 24 | (uint32_t)b[4] << 16 | (uint32_t)b[5] << 8) >> 8;
  *i = (float)vi / 8388608.0f;
  *q = (float)vq / 8388608.0f;
}

typedef struct {
  uint64_t got, overflows, waits;
} Drain;

/* Read what FIFO rx of in holds into out (NULL = discard); cntr counts
 * 32-bit halves, the server resets the FIFO at ≥ 2048 (about to overflow). */
static size_t drain(Inst *in, int rx, float *out, size_t room, Drain *st) {
  uint16_t c = *in->cntr[rx];
  if (c >= 2048) {
    st->overflows++;
    reset(in);
    return 0;
  }
  size_t n = c / 2;
  if (n == 0) { st->waits++; return 0; }
  if (out && n > room) n = room;
  for (size_t k = 0; k < n; k++) {
    uint64_t w = *in->data[rx];
    if (out) word_iq(w, &out[2 * k], &out[2 * k + 1]);
  }
  st->got += n;
  return n;
}

static void write_cf32(const char *path, const float *iq, size_t n, double rate, double centre) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); exit(1); }
  fwrite(iq, sizeof(float) * 2, n, f);
  fclose(f);
  char meta[512];
  snprintf(meta, sizeof(meta), "%s.meta", path);
  f = fopen(meta, "w");
  if (f) {
    fprintf(f, "rate=%.3f\ncenter_hz=%.0f\nformat=cf32 raw (wire orientation: RF-inverted, not conjugated)\n",
            rate, centre);
    fclose(f);
  }
}

static double rate_of(unsigned reg) { return ADC_HZ / reg / 2.0; } /* CIC ÷reg, FIR ÷2 */

static void cmd_info(void) {
  map_all(0);
  for (int k = 0; k < 2; k++) {
    Inst *in = &inst[k];
    int pid = 0, run = server_runs(in->iface, &pid);
    printf("%s: server %s", in->iface, run ? "runs" : "stopped");
    if (run) printf(" (pid %d)", pid);
    printf(", rate reg %u (%.0f S/s), sel 0x%02x, cntr", *in->rate, rate_of(*in->rate), *in->sel);
    for (int i = 0; i < 8; i++) printf(" %u", *in->cntr[i]);
    printf("\n  freq:");
    for (int i = 0; i < 8; i++) printf(" %.0f", *in->freq[i] * ADC_HZ / (1 << 30));
    printf("\n");
  }
}

/* capture IFACE RATE_REG FREQ_HZ SECONDS OUT [INPUT] — RX1 of IFACE. */
static void cmd_capture(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "capture IFACE RATE_REG FREQ_HZ SECONDS OUT [1|2]\n"); exit(2); }
  map_all(1);
  Inst *in = writable(argv[1]);
  const unsigned reg = (unsigned)atoi(argv[2]);
  const double hz = atof(argv[3]), secs = atof(argv[4]);
  const int input = argc > 6 ? atoi(argv[6]) : 1;
  const double rate = rate_of(reg);
  const size_t want = (size_t)(rate * secs);
  float *iq = malloc(want * 2 * sizeof(float));
  if (!iq) { perror("malloc"); exit(1); }
  *in->rate = (uint16_t)reg;
  *in->sel = input == 2 ? 0xff : 0x00;
  for (int i = 0; i < 8; i++) *in->freq[i] = phase_word(hz);
  usleep(10000);
  reset(in);
  Drain st = { 0 };
  size_t have = 0;
  const double t0 = now_s(CLOCK_MONOTONIC), c0 = now_s(CLOCK_PROCESS_CPUTIME_ID);
  while (have < want) {
    size_t n = drain(in, 0, iq + 2 * have, want - have, &st);
    if (st.overflows) break;                          /* the stream has a hole */
    have += n;
    if (!n) usleep(200);
  }
  const double dt = now_s(CLOCK_MONOTONIC) - t0, cpu = now_s(CLOCK_PROCESS_CPUTIME_ID) - c0;
  printf("%s rate reg %u: %zu samples in %.3f s = %.1f S/s (nominal %.1f), overflows %llu, "
         "cpu %.0f %%\n", in->iface, reg, have, dt, have / dt, rate,
         (unsigned long long)st.overflows, 100.0 * cpu / dt);
  if (have) write_cf32(argv[5], iq, have, rate, hz);
  free(iq);
}

/* readout IFACE RATE_REG NRX SECONDS — can the ARM keep NRX FIFOs empty? */
static void cmd_readout(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "readout IFACE RATE_REG NRX SECONDS\n"); exit(2); }
  map_all(1);
  Inst *in = writable(argv[1]);
  const unsigned reg = (unsigned)atoi(argv[2]);
  const int nrx = atoi(argv[3]);
  const double secs = atof(argv[4]);
  *in->rate = (uint16_t)reg;
  for (int i = 0; i < 8; i++) *in->freq[i] = phase_word(7.0e6 + i * 1e6);
  usleep(10000);
  reset(in);
  Drain st[8] = { { 0 } };
  const double t0 = now_s(CLOCK_MONOTONIC), c0 = now_s(CLOCK_PROCESS_CPUTIME_ID);
  while (now_s(CLOCK_MONOTONIC) - t0 < secs) {
    size_t any = 0;
    /* the server reads all FIFOs in lockstep on RX1's count; so do we */
    uint16_t c = *in->cntr[0];
    if (c >= 2048) {
      st[0].overflows++;
      reset(in);
      continue;
    }
    size_t n = c / 2;
    for (int r = 0; r < nrx; r++) {
      for (size_t k = 0; k < n; k++) (void)*in->data[r];
      st[r].got += n;
      any += n;
    }
    if (!any) usleep(200);
  }
  const double dt = now_s(CLOCK_MONOTONIC) - t0, cpu = now_s(CLOCK_PROCESS_CPUTIME_ID) - c0;
  printf("%s rate reg %u (%.0f S/s) × %d RX: %.1f S/s per RX, overflows %llu, cpu %.0f %%\n",
         in->iface, reg, rate_of(reg), nrx, st[0].got / dt, (unsigned long long)st[0].overflows,
         100.0 * cpu / dt);
}

/* pair RATE_REG FREQ_HZ SECONDS OUTA OUTB [RESETS] — RX1 of both instances
 * on IN1 at one frequency, reset back to back; RESETS captures in a row
 * (OUTA/OUTB get .N appended) tell whether the phase after a reset repeats. */
static void cmd_pair(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "pair RATE_REG FREQ_HZ SECONDS OUTA OUTB [RESETS]\n"); exit(2); }
  map_all(1);
  Inst *a = writable("eth0"), *b = writable("mvl0");
  const unsigned reg = (unsigned)atoi(argv[1]);
  const double hz = atof(argv[2]), secs = atof(argv[3]);
  const int resets = argc > 6 ? atoi(argv[6]) : 1;
  const double rate = rate_of(reg);
  const size_t want = (size_t)(rate * secs);
  float *ia = malloc(want * 2 * sizeof(float)), *ib = malloc(want * 2 * sizeof(float));
  if (!ia || !ib) { perror("malloc"); exit(1); }
  for (int r = 0; r < resets; r++) {
    for (int k = 0; k < 2; k++) {
      Inst *in = k ? b : a;
      *in->rate = (uint16_t)reg;
      *in->sel = 0x00;
      for (int i = 0; i < 8; i++) *in->freq[i] = phase_word(hz);
    }
    usleep(10000);
    *a->rst |= 1;                                     /* both, back to back */
    *b->rst |= 1;
    *a->rst &= ~1;
    *b->rst &= ~1;
    Drain sa = { 0 }, sb = { 0 };
    size_t ha = 0, hb = 0;
    while ((ha < want || hb < want) && !sa.overflows && !sb.overflows) {
      size_t n = 0;
      if (ha < want) { size_t m = drain(a, 0, ia + 2 * ha, want - ha, &sa); ha += m; n += m; }
      if (hb < want) { size_t m = drain(b, 0, ib + 2 * hb, want - hb, &sb); hb += m; n += m; }
      if (!n) usleep(200);
    }
    char pa[256], pb[256];
    snprintf(pa, sizeof(pa), resets > 1 ? "%s.%d" : "%s", argv[4], r);
    snprintf(pb, sizeof(pb), resets > 1 ? "%s.%d" : "%s", argv[5], r);
    printf("pair %d: eth0 %zu, mvl0 %zu samples, overflows %llu/%llu\n", r, ha, hb,
           (unsigned long long)sa.overflows, (unsigned long long)sb.overflows);
    write_cf32(pa, ia, ha, rate, hz);
    write_cf32(pb, ib, hb, rate, hz);
  }
  free(ia);
  free(ib);
}

/* blast IP PORT MBPS SECONDS [IFACE_IP] — 1032-byte UDP datagrams in
 * batches of 25 (as the server sends), seq in bytes 4..7 (big-endian). */
static void cmd_blast(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "blast IP PORT MBPS SECONDS [SRC_IP]\n"); exit(2); }
  const double mbps = atof(argv[3]), secs = atof(argv[4]);
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) { perror("socket"); exit(1); }
  int big = 4 << 20;
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
  if (argc > 5) {
    struct sockaddr_in src = { .sin_family = AF_INET };
    inet_pton(AF_INET, argv[5], &src.sin_addr);
    if (bind(s, (struct sockaddr *)&src, sizeof(src)) < 0) { perror("bind"); exit(1); }
  }
  struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])) };
  if (inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1) { fprintf(stderr, "bad IP\n"); exit(2); }
  enum { BATCH = 25, SIZE = 1032 };
  static uint8_t buf[BATCH][SIZE];
  struct iovec iov[BATCH];
  struct mmsghdr msg[BATCH];
  memset(msg, 0, sizeof(msg));
  for (int i = 0; i < BATCH; i++) {
    buf[i][0] = 0xef; buf[i][1] = 0xfe; buf[i][2] = 0x01; buf[i][3] = 0x06;
    iov[i].iov_base = buf[i];
    iov[i].iov_len = SIZE;
    msg[i].msg_hdr.msg_iov = &iov[i];
    msg[i].msg_hdr.msg_iovlen = 1;
    msg[i].msg_hdr.msg_name = &dst;
    msg[i].msg_hdr.msg_namelen = sizeof(dst);
  }
  const double pps = mbps * 1e6 / ((SIZE + 42) * 8.0);
  uint32_t seq = 0;
  uint64_t sent = 0, failed = 0;
  const double t0 = now_s(CLOCK_MONOTONIC), c0 = now_s(CLOCK_PROCESS_CPUTIME_ID);
  for (;;) {
    const double t = now_s(CLOCK_MONOTONIC) - t0;
    if (t >= secs) break;
    if (sent + BATCH > t * pps) { usleep(100); continue; }
    for (int i = 0; i < BATCH; i++) { uint32_t be = htonl(seq++); memcpy(buf[i] + 4, &be, 4); }
    int n = sendmmsg(s, msg, BATCH, 0);
    if (n < 0) { failed++; seq -= BATCH; usleep(100); continue; }
    sent += (uint64_t)n;
    if (n < BATCH) seq -= (uint32_t)(BATCH - n);
  }
  const double dt = now_s(CLOCK_MONOTONIC) - t0, cpu = now_s(CLOCK_PROCESS_CPUTIME_ID) - c0;
  printf("blast: %llu datagrams in %.2f s = %.0f pkt/s = %.1f Mb/s on the wire "
         "(asked %.1f), send errors %llu, cpu %.0f %%\n", (unsigned long long)sent, dt, sent / dt,
         sent / dt * (SIZE + 42) * 8 / 1e6, mbps, (unsigned long long)failed, 100.0 * cpu / dt);
}

int main(int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[1], "info") == 0) { cmd_info(); return 0; }
  if (argc >= 2 && strcmp(argv[1], "capture") == 0) { cmd_capture(argc - 1, argv + 1); return 0; }
  if (argc >= 2 && strcmp(argv[1], "readout") == 0) { cmd_readout(argc - 1, argv + 1); return 0; }
  if (argc >= 2 && strcmp(argv[1], "pair") == 0) { cmd_pair(argc - 1, argv + 1); return 0; }
  if (argc >= 2 && strcmp(argv[1], "blast") == 0) { cmd_blast(argc - 1, argv + 1); return 0; }
  fprintf(stderr,
          "rp-spike info                                   registers of both instances (read-only)\n"
          "rp-spike capture IFACE RATE_REG HZ SECS OUT [IN] RX1 IQ to OUT (cf32 raw + .meta)\n"
          "rp-spike readout IFACE RATE_REG NRX SECS        FIFO readout: overflows, CPU\n"
          "rp-spike pair RATE_REG HZ SECS OUTA OUTB [N]    both instances, common reset\n"
          "rp-spike blast IP PORT MBPS SECS [SRC_IP]       UDP ceiling\n"
          "RATE_REG: 1280/640/320/160 = 48/96/192/384 kHz (stock); 80/40 = the question\n");
  return 2;
}
