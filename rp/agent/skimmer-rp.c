/* skimmer-rp — the skimmer's agent on a Red Pitaya STEMlab 122.88-16
 * (docs/REDPITAYA.md). It replaces the two sdr-receiver-hpsdr processes of
 * Pavel Demin's sdr_receiver_hpsdr_122_88 app (red-pitaya-notes, MIT) on the
 * same bitstream:
 *
 *   - one openHPSDR Protocol 1 server per interface (eth0 = DDC 0–7, mvl0 =
 *     DDC 8–15), on the wire the same as Pavel's: discovery, start/stop from
 *     anyone, EP2 rate/receivers/frequencies, EP6 1032-byte packets. CW
 *     Skimmer's HermesIntf and Quisk see no difference. A discovery whose
 *     padding carries "SKRP" (bytes 4–7; byte 3 is part of the command) gets the agent's marker after the standard 20
 *     bytes (version, mode, input mask); a plain one gets Pavel's reply
 *     byte for byte.
 *   - the ADC input of each receiver (IN1/IN2) is a register (rx_sel) the
 *     stock server only writes at start; here `ctl sel` changes it live.
 *   - its own orphan stop: Protocol 1 has no watchdog, so a client that dies
 *     without a stop left the radio streaming to a closed port for good
 *     (2026-11-18, 11-22). With IP_RECVERR the ICMP port-unreachables come
 *     back on the socket; a stream that hears only those for --orphan-s
 *     seconds is stopped. A live client never answers its own stream with
 *     ICMP, so a quiet client (SkimSrv sends its EP2 only once) is safe.
 *   - `ctl status`: a JSON line (board, temperature, load, link, both
 *     streams) for the skimmer's Statistics page, read over SSH.
 *   - for two-antenna phasing (docs/PHASING.md): a marked start whose byte 8
 *     has bit 0 set is PAIRED. When the same client (address) streams paired
 *     from both instances, every FIFO reset — the second start, an overflow
 *     on either side, a new receiver count — resets BOTH FIFO sets back to
 *     back (as rp-spike pair did: lag 0) and starts a new epoch on both.
 *     A marked stream carries, in the C&C bytes of its EP6 frames, the
 *     epoch and the index of the packet's first sample since that reset:
 *       frame 1: 7F 7F 7F 78 epoch idx[47:40] idx[39:32] flags (bit 0 paired)
 *       frame 2: 7F 7F 7F 80 idx[31:24] idx[23:16] idx[15:8] idx[7:0]
 *     so the client aligns A and B by (epoch, index) whatever the packet loss
 *     or the start skew. An unmarked stream keeps Pavel's C&C byte for byte;
 *     a reset of a paired stream never touches a FIFO another client reads.
 *
 * --sim replaces /dev/mem with synthetic FIFOs (a tone a receiver, louder on
 * IN1 than on IN2, and a noise both inputs hear, a function of the sample
 * number) so the rp-agent and rp-two-ip gates run it on the build machine.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/errqueue.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define AGENT_MAJOR 0
#define AGENT_MINOR 6
#define ADC_HZ      122.88e6
#define NRX         8
#define MODE_P1     0

/* ---- the hardware: registers + FIFOs, or a simulation ------------------------ */

typedef struct Inst Inst;

typedef struct {
  int    sim;
  /* sim: one stream clock per instance */
  double t0[2];
  uint64_t taken[2];
  uint64_t base[2];        /* the first sample's index on a common grid    */
  uint64_t n[2][NRX];      /* samples a receiver gave since the reset      */
  volatile int force_overflow[2];   /* ctl sim-overflow                    */
} Hw;

static Hw hw;
static int skrf;                     /* the skimmer's own bitstream (FPGA.md)   */

struct Inst {
  int                k;                    /* 0 = eth0 (DDC 0–7), 1 = mvl0      */
  char               name[32];             /* interface (or address for --sim) */
  char               ifname[IFNAMSIZ];     /* "" = bind by address only        */
  struct sockaddr_in bind_addr;
  volatile uint8_t  *rst;
  volatile uint16_t *rate;
  volatile uint8_t  *sel;
  volatile uint32_t *freq[NRX];
  volatile uint16_t *cntr[NRX];
  volatile uint64_t *data[NRX];
  /* sim registers */
  uint8_t  s_rst, s_sel;
  uint16_t s_rate;
  uint32_t s_freq[NRX];
  /* the server */
  int                sock;
  pthread_mutex_t    lock;
  pthread_t          thread;
  volatile int       enable, active;
  struct sockaddr_in client;
  int                receivers;
  uint32_t           freq_hz[NRX];
  time_t             since;
  uint64_t           packets;
  uint32_t           seq;
  int64_t            fifo_reset_ns;        /* CLOCK_MONOTONIC of the last reset */
  uint64_t           overflows;
  /* the skimmer's client (its start carried "SKRP"): its ADC word counts */
  int                marked;
  uint8_t            base_sel;             /* the manager's inputs (--sel, ctl) */
  struct sockaddr_in adc_from;             /* the last ADC word: who, what     */
  int                adc_have;
  uint8_t            adc_sel;
  /* phasing: a paired stream, its epoch and sample index (marked only) */
  int                paired;
  volatile uint32_t  resets;              /* FIFO resets (under pair_lock)    */
  uint8_t            reset_epoch;         /* the epoch the last reset began   */
  uint32_t           seen_resets;         /* the EP6 thread's view            */
  uint8_t            epoch;
  uint64_t           idx;
  uint64_t           pair_resets;
  /* the skimmer's bitstream: where this stream reads its slots */
  uint64_t           sk_rd;               /* the next hardware index           */
  uint32_t           sk_gen[NRX];         /* the slots' timelines it anchored on */
  /* orphan detection */
  int64_t            unreach_first_ns, unreach_last_ns;
  uint64_t           unreach_count, orphan_stops;
  uint8_t            mac[6];
};

static Inst inst[2];
static int  ninst = 2;
static int  orphan_s = 10;
static const char *ctl_path = "/run/skimmer-rp.sock";

static int64_t mono_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void logf_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "skimmer-rp: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

#define SK_STS_ADDR 0x41000000u      /* the skimmer's bitstream's status words */

/* --fpga: which bitstream the board runs (start.sh loaded it) */
static int fpga_opt = 0;             /* 0 auto, 1 stock, 2 skrf, 3 skrf simulated */

static void hw_map(void) {
  if (fpga_opt == 0 && !hw.sim) {    /* auto: the skimmer's bitstream says SKRF */
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); exit(1); }
    volatile uint32_t *st = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, SK_STS_ADDR);
    fpga_opt = st != MAP_FAILED && st[0] == 0x46524B53u ? 2 : 1;
    if (st != MAP_FAILED) { munmap((void *)st, 4096); }
    close(fd);
  }
  if (fpga_opt >= 2) {               /* registers are shadows; the slots get them */
    for (int k = 0; k < 2; k++) {
      Inst *in = &inst[k];
      in->rst = &in->s_rst;
      in->rate = &in->s_rate;
      in->sel = &in->s_sel;
      for (int i = 0; i < NRX; i++) { in->freq[i] = &in->s_freq[i]; }
    }
    return;
  }
  if (hw.sim) {
    for (int k = 0; k < 2; k++) {
      Inst *in = &inst[k];
      in->rst = &in->s_rst;
      in->rate = &in->s_rate;
      in->sel = &in->s_sel;
      for (int i = 0; i < NRX; i++) { in->freq[i] = &in->s_freq[i]; }
    }
    return;
  }
  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0) { perror("/dev/mem"); exit(1); }
  const long pg = sysconf(_SC_PAGESIZE);
  volatile uint8_t *sts = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x40000000);
  volatile uint8_t *cfg = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x40001000);
  if (sts == MAP_FAILED || cfg == MAP_FAILED) { perror("mmap"); exit(1); }
  for (int k = 0; k < 2; k++) {
    Inst *in = &inst[k];
    const int c0 = k ? 40 : 0, s0 = k ? 28 : 12;
    in->rst = cfg + c0;
    in->rate = (volatile uint16_t *)(cfg + c0 + 4);
    in->sel = cfg + c0 + 6;
    for (int i = 0; i < NRX; i++) {
      in->freq[i] = (volatile uint32_t *)(cfg + c0 + 8 + i * 4);
      in->cntr[i] = (volatile uint16_t *)(sts + s0 + i * 2);
      void *p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     (k ? 0x40020000 : 0x40010000) + i * 0x2000);
      if (p == MAP_FAILED) { perror("mmap fifo"); exit(1); }
      in->data[i] = p;
    }
  }
}

static double rate_hz(const Inst *in) { return ADC_HZ / *in->rate / 2.0; }

/* sim: the sample a stream would give next. */
static uint64_t sim_next(const Inst *in) { return hw.base[in->k] + hw.n[in->k][0]; }

/* One FIFO set's reset at now_ns (sim: its stream clock restarts there and
 * its samples go on from sim_base — a solo reset where it stopped, so a
 * tone stays continuous; a paired one where the further of the two is, so
 * both give the same sample at the same index). */
static void fifo_reset_bookkeeping(Inst *in, int64_t now_ns, uint64_t sim_base);

static void fifo_reset_at(Inst *in, int64_t now_ns, uint64_t sim_base) {
  *in->rst |= 1;
  *in->rst &= ~1;
  fifo_reset_bookkeeping(in, now_ns, sim_base);
}

/* Both FIFO sets at once: both reset bits set, then both released — as
 * rp-spike pair did (lag 0). Releasing one before setting the other left
 * B 5 samples (52 µs at 96 kHz) behind A on rp-f08908 (2026-11-26: the
 * register writes take that long). */
static void fifo_reset_pair(Inst *a, Inst *b, int64_t now_ns, uint64_t sim_base) {
  *a->rst |= 1;
  *b->rst |= 1;
  *a->rst &= ~1;
  *b->rst &= ~1;
  fifo_reset_bookkeeping(a, now_ns, sim_base);
  fifo_reset_bookkeeping(b, now_ns, sim_base);
}

static void fifo_reset_bookkeeping(Inst *in, int64_t now_ns, uint64_t sim_base) {
  in->fifo_reset_ns = now_ns;
  if (hw.sim) {
    hw.t0[in->k] = now_ns * 1e-9;
    hw.taken[in->k] = 0;
    hw.base[in->k] = sim_base;
    memset(hw.n[in->k], 0, sizeof(hw.n[in->k]));
  }
}

static uint64_t sk_anchor(Inst *in);
static void sk_set_rd(Inst *in, uint64_t at);
static void sk_program(void);
static void sk_settle(Inst *in);

/* The resets, and the epochs they begin. An EP6 thread reads a batch under
 * fifo_rw's read lock (both instances at once); a reset takes it for
 * writing, so it never lands inside a batch: a batch that popped samples
 * after a reset would leave the next one's index off by that many. */
static pthread_mutex_t pair_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t fifo_rw = PTHREAD_RWLOCK_INITIALIZER;
static uint8_t pair_gen;

/* The other instance when it streams paired to the same client address. */
static Inst *partner_of(Inst *in) {
  if (ninst < 2 || !in->marked || !in->paired) { return NULL; }
  Inst *o = &inst[1 - in->k];
  pthread_mutex_lock(&o->lock);
  const int same = o->active && o->marked && o->paired &&
                   o->client.sin_addr.s_addr == in->client.sin_addr.s_addr;
  pthread_mutex_unlock(&o->lock);
  return same ? o : NULL;
}

/* A reset of in's FIFOs (a start, an overflow, a new receiver count) — and
 * its partner's in the same breath when paired: one new epoch for both. */
static void stream_reset(Inst *in) {
  if (skrf) { sk_settle(in); }          /* its slots on their new rate first */
  pthread_mutex_lock(&pair_lock);
  Inst *o = partner_of(in);
  pthread_rwlock_wrlock(&fifo_rw);                        /* no batch runs */
  const uint8_t e = ++pair_gen;
  const int64_t now = mono_ns();
  if (skrf) {
    /* no FIFO to reset: anchor at a common hardware index */
    uint64_t at = sk_anchor(in);
    if (o && sk_anchor(o) > at) { at = sk_anchor(o); }
    sk_set_rd(in, at);
    in->fifo_reset_ns = now;
    if (o) {
      sk_set_rd(o, at);
      o->fifo_reset_ns = now;
    }
  } else {
    uint64_t base = sim_next(in);
    if (o && sim_next(o) > base) { base = sim_next(o); }
    if (o) {
      fifo_reset_pair(in, o, now, base);                  /* at once      */
    } else {
      fifo_reset_at(in, now, base);
    }
  }
  in->reset_epoch = e;
  __atomic_add_fetch(&in->resets, 1, __ATOMIC_SEQ_CST);
  if (o) {
    o->reset_epoch = e;
    __atomic_add_fetch(&o->resets, 1, __ATOMIC_SEQ_CST);
    in->pair_resets++;
    o->pair_resets++;
  }
  pthread_rwlock_unlock(&fifo_rw);
  pthread_mutex_unlock(&pair_lock);
}

/* RX1's FIFO fill, in 32-bit halves as the bitstream counts it. */
static unsigned fifo_count(Inst *in) {
  if (!hw.sim) { return *in->cntr[0]; }
  const double avail = (mono_ns() * 1e-9 - hw.t0[in->k]) * rate_hz(in) - (double)hw.taken[in->k];
  const double halves = avail * 2;
  return halves >= 4095 ? 4095 : halves <= 0 ? 0 : (unsigned)halves;
}

static uint64_t sim_sample(double rate, int rx, int in2, uint64_t t) {
  /* receiver rx: a tone (rx+1) kHz above its centre at true RF, so the wire
   * (RF-inverted) carries its conjugate; IN1 −12 dBFS, IN2 −26 dBFS. Its
   * phase is a function of the sample number t (fifo_reset_at; with
   * --sim-skrf the hardware index): two streams of one instant give the
   * same sample at the same index */
  const double amp = in2 ? 0.05 : 0.25;
  const double f = 1000.0 * (rx + 1);
  const uint64_t per = (uint64_t)llround(rate);          /* whole seconds */
  const double ph = 2 * M_PI * f * (double)(t % per) / rate;
  /* and a noise both inputs hear, a function of the sample number too
   * (-40 dBFS on IN1, half of it on IN2): a common interferer — two streams
   * paired off by a block lose its coherence, aligned ones can null it */
  uint64_t hsh = t * 0x9E3779B97F4A7C15ull;
  hsh ^= hsh >> 31;
  hsh *= 0xBF58476D1CE4E5B9ull;
  hsh ^= hsh >> 29;
  const double nr = ((double)(hsh & 0xFFFFFF) / 8388608.0 - 1.0);
  const double ni = ((double)((hsh >> 24) & 0xFFFFFF) / 8388608.0 - 1.0);
  const double na = in2 ? 0.005 : 0.01;
  const int32_t vi = (int32_t)lrint((amp * cos(ph) + na * nr) * 8388607.0);
  const int32_t vq = (int32_t)lrint((-amp * sin(ph) + na * ni) * 8388607.0);
  uint64_t w = 0;
  uint8_t *b = (uint8_t *)&w;
  b[0] = (uint8_t)(vi >> 16); b[1] = (uint8_t)(vi >> 8); b[2] = (uint8_t)vi;
  b[3] = (uint8_t)(vq >> 16); b[4] = (uint8_t)(vq >> 8); b[5] = (uint8_t)vq;
  return w;
}

/* A 24-bit big-endian I then Q in the first 6 bytes of the word (memory
 * order), as the bitstream's FIFO hands them and the server copies them. */
static uint64_t sim_word(Inst *in, int rx) {
  const uint64_t t = hw.base[in->k] + hw.n[in->k][rx]++;
  return sim_sample(rate_hz(in), rx, (*in->sel >> rx) & 1, t);
}

static uint64_t fifo_read(Inst *in, int rx) {
  if (!hw.sim) { return *in->data[rx]; }
  return sim_word(in, rx);
}

/* How the FIFOs are read (--read): 0 = one 64-bit load a sample (Pavel's
 * server), 2 = NEON 256 bits at the window's base (4 samples a load: the
 * bitstream pops a sample per 64-bit beat whatever the address). Measured on
 * rp-f08908 2026-11-25 (rp-spike burst/rdcost): NEON 128/256 bits give the
 * same spectrum as 64-bit loads, no repeated word; 8 RX × 384 kHz cost 81 %
 * of a core with mode 0, 54 % with 256-bit loads. LDM (32-bit beats) reads
 * garbage — never. */
static int read_mode = 2;
static int use_gso = 1;             /* --gso 0: sendmmsg as Pavel's server */

static void fifo_read_n(Inst *in, int rx, uint64_t *out, int n) {
#if defined(__arm__)
  if (read_mode == 2 && !hw.sim) {
    int k = 0;
    for (; k + 4 <= n; k += 4) {
      __asm__ volatile("vld1.64 {d0-d3}, [%0]\n vst1.64 {d0-d3}, [%1]"
                       : : "r"(in->data[rx]), "r"(out + k) : "d0", "d1", "d2", "d3", "memory");
    }
    for (; k < n; k++) { out[k] = *in->data[rx]; }
    return;
  }
#endif
  for (int k = 0; k < n; k++) { out[k] = fifo_read(in, rx); }
}

static uint32_t phase_word(uint32_t hz) { return (uint32_t)floor(hz / ADC_HZ * (1 << 30) + 0.5); }

/* ---- the skimmer's own bitstream (docs/FPGA.md): slots and the ring ------------- */

/* With fpga/projects/skimmer_rp_122_88 loaded, the same two P1 instances run
 * on its slots: instance k on slots 8k … 8k+7, all at the instance's rate,
 * each receiver's frequency and input its slot's. The FPGA writes every
 * sample as a tagged 64-bit word into a ring in DDR; one reader thread
 * copies the new bursts out (NEON: 245 MB/s uncached, 64-bit loads 66) and
 * files each sample into its slot's buffer at its HARDWARE index — the
 * stage's vector number since the sync, unwrapped. A stream reads its
 * receivers' buffers at one index range, so they are aligned by
 * construction, across instances too; a reset (start, overflow, a new
 * receiver count or rate) re-anchors it — with its partner when paired —
 * at a common index and starts a new epoch, as on the stock FIFOs. */

#define SK_MAGIC     0x46524B53u       /* "SKRF" */
#define SK_CFG       0x40000000u
#define SK_STS       0x41000000u
#define SK_RING      0x1E000000u
#define SK_RING_SIZE (8u << 20)
#define SK_BURST     128u
#define SK_NBURSTS   (SK_RING_SIZE / SK_BURST)
#define SK_MAX       32
#define SK_BUF       (1u << 16)        /* a slot's buffer: 65536 samples      */

typedef struct {
  uint64_t          w[SK_BUF];         /* index i at w[i % SK_BUF]; bytes 6–7 = i's low 16 bits */
  volatile uint64_t wr;                /* the next index to come (all below are in) */
  volatile int      stage;             /* 0 = 384 k … 3 = 48 k; -1 = nothing yet    */
  volatile uint32_t gen;               /* bumps when the timeline restarts          */
} SkSlot;

typedef struct {
  int               on, sim;
  int               nslots, version;
  volatile uint32_t *cfg, *sts;
  volatile uint64_t *ring;
  uint32_t          sim_cfg[64], sim_sts[16];
  uint64_t          *sim_ring;
  SkSlot            *slot;
  pthread_mutex_t   lock;              /* register writes (and the sim's packer)    */
  uint32_t          ctrl;              /* cfg word 0 as written                     */
  uint32_t          rd_burst;
  uint64_t          stage_idx[4];
  int               stage_have[4];
  volatile uint32_t sync_gen;
  /* what the reader saw */
  uint64_t          words, ticks, peak_words, unknown, overruns, syncs;
  unsigned          flags, peak1, peak2, clips;
} Sk;

static Sk sk;

static const uint16_t sk_rate_reg[4] = { 160, 320, 640, 1280 };   /* stage → stock rate register */
static int sk_stage_of(uint16_t rate_reg) {
  for (int r = 0; r < 4; r++) {
    if (sk_rate_reg[r] == rate_reg) { return r; }
  }
  return 3;
}

static void sk_burst_copy(uint64_t *dst, const volatile uint64_t *src) {
#if defined(__arm__)
  if (!sk.sim) {
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
    return;
  }
#endif
  for (unsigned k = 0; k < SK_BURST / 8; k++) { dst[k] = src[k]; }
}

/* ---- SKRS: the skimmer's own stream (docs/FPGA.md, "The ARM side") -------------
 * P1 cannot mix rates and has 8 receivers an IP. A client that sends SKRC
 * datagrams to port 1025 of eth0 gets up to every slot, each at its own
 * rate, as SKRS datagrams — one slot a datagram:
 *   SKRC: "SKRC" ver(1)=1 cmd(1): 1 = start (n(1), then n × { id(1),
 *         hz(4, BE), rate(1: 0 = 48 … 3 = 384 k), input(1: 1|2) }),
 *         2 = stop, 3 = keep-alive (every second; 5 s without one = stop).
 *   SKRS: "SKRS" ver(1)=1 id(1) rate(1) flags(1: bit 0 samples lost
 *         before this one, bit 1 IN1 clipped, bit 2 IN2 clipped — since
 *         the last datagram of that id) epoch(4, BE: the syncs) index(8,
 *         BE: the hardware index of the first sample) count(2, BE), then
 *         count × 6 bytes I/Q (24-bit BE, RF-inverted as P1's).
 * Two ids at one rate share the hardware timeline: one index is one
 * instant (a phased pair). The slots come from 16 … N-1 first, then from
 * an instance no P1 client streams on; while SKRS holds an instance's
 * slots that instance answers busy and ignores a start. */

#define SKRS_PORT  1025
#define SKRS_MAXN  240                 /* samples a datagram: 22 + 1440 bytes  */
#define SKRS_HDR   22

typedef struct {
  uint8_t  id, stage, in2;
  uint32_t hz;
  int      slot;
  uint64_t rd;                         /* the next hardware index to send     */
  uint32_t gen;
  int      lost;
  uint64_t packets;
} SkrsStream;

typedef struct {
  int                sock;
  pthread_mutex_t    lock;
  int                on;
  struct sockaddr_in to;
  int64_t            last_ka_ns;
  int                n;
  SkrsStream         st[SK_MAX];
  uint32_t           holds;            /* slot mask SKRS holds                */
  uint64_t           starts, timeouts;
} Skrs;

static Skrs skrs = { .sock = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

/* Does SKRS hold any of instance k's slots? (that instance is busy) */
static int skrs_holds_inst(int k) {
  return skrf && (skrs.holds & (0xffu << (8 * k))) != 0;
}

/* Every slot's register from both instances' state and SKRS's: rate,
 * input, frequency, on (the receivers a streaming instance reads, SKRS's
 * streams); then one apply, so the new frequencies take effect on one
 * clock. */
static void sk_program(void) {
  if (!sk.on) { return; }
  pthread_mutex_lock(&sk.lock);
  uint32_t ins = 0, ena = 0, mask[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < skrs.n && skrs.on; i++) {
    const SkrsStream *x = &skrs.st[i];
    sk.cfg[11 + x->slot] = (uint32_t)llround(x->hz / ADC_HZ * 4294967296.0);
    ena |= 1u << x->slot;
    mask[x->stage] |= 1u << x->slot;
    if (x->in2) { ins |= 1u << x->slot; }
  }
  for (int k = 0; k < ninst; k++) {
    Inst *in = &inst[k];
    const int stage = sk_stage_of(*in->rate);
    for (int r = 0; r < NRX; r++) {
      const int s = 8 * k + r;
      if (s >= sk.nslots || ((skrs.holds >> s) & 1)) { continue; }
      sk.cfg[11 + s] = (uint32_t)llround(in->freq_hz[r] / ADC_HZ * 4294967296.0);
      if (!in->active || r >= in->receivers) { continue; }
      ena |= 1u << s;
      mask[stage] |= 1u << s;
      if ((*in->sel >> r) & 1) { ins |= 1u << s; }
    }
  }
  sk.cfg[1] = ins;
  sk.cfg[2] = ena;
  for (int r = 0; r < 4; r++) { sk.cfg[3 + r] = mask[r]; }
  sk.cfg[0] = sk.ctrl | 2u;          /* apply: the rising edge */
  sk.cfg[0] = sk.ctrl;
  pthread_mutex_unlock(&sk.lock);
}

/* The sync: every NCO, CIC, filter and the packer held and released, the
 * DMA writer back to the ring's start. Every slot's timeline restarts. */
static void sk_sync(void) {
  pthread_mutex_lock(&sk.lock);
  sk.cfg[0] = sk.ctrl = 0;
  usleep(1000);
  if (sk.sim) {
    memset(sk.sim_ring, 0, SK_RING_SIZE);
    sk.sim_sts[2] = 0;
  } else {
    for (uint32_t i = 0; i < SK_RING_SIZE / 8; i++) { sk.ring[i] = 0; }
  }
  sk.cfg[7] = 0;                      /* unity gain: stock's levels (FPGA.md)   */
  sk.cfg[8] = SK_RING;
  sk.rd_burst = 0;
  memset(sk.stage_idx, 0, sizeof(sk.stage_idx));
  memset(sk.stage_have, 0, sizeof(sk.stage_have));
  for (int s = 0; s < sk.nslots; s++) {
    sk.slot[s].stage = -1;
    sk.slot[s].wr = 0;
    __atomic_add_fetch(&sk.slot[s].gen, 1, __ATOMIC_SEQ_CST);
  }
  sk.cfg[0] = sk.ctrl = 4u;           /* DMA on (non-cacheable: /dev/mem)       */
  usleep(1000);
  sk.cfg[0] = sk.ctrl = 5u;           /* run: the sync                          */
  sk.syncs++;
  __atomic_add_fetch(&sk.sync_gen, 1, __ATOMIC_SEQ_CST);
  pthread_mutex_unlock(&sk.lock);
}

/* One word from the ring. */
static void sk_word(uint64_t w) {
  const unsigned tag = (unsigned)(w >> 56);
  if ((tag >> 6) == 1) {
    const unsigned s = (unsigned)(w >> 48) & 0x1f, r = (tag >> 4) & 3;
    const unsigned vc = (tag & 0xf) | (unsigned)((w >> 53) & 7) << 4;
    if (s >= (unsigned)sk.nslots) { sk.unknown++; return; }
    uint64_t full;
    if (!sk.stage_have[r]) {
      full = vc;
      sk.stage_have[r] = 1;
    } else {
      full = sk.stage_idx[r] + ((vc - (unsigned)sk.stage_idx[r]) & 127u);
    }
    sk.stage_idx[r] = full;
    SkSlot *x = &sk.slot[s];
    if (x->stage != (int)r) {         /* a new rate: a new timeline             */
      x->stage = (int)r;
      x->wr = full;
      __atomic_add_fetch(&x->gen, 1, __ATOMIC_SEQ_CST);
    }
    x->w[full % SK_BUF] = (w & 0x0000ffffffffffffull) | (full & 0xffff) << 48;
    if (full >= x->wr) { __atomic_store_n(&x->wr, full + 1, __ATOMIC_RELEASE); }
    sk.words++;
  } else if (tag == 0xff) {
    sk.ticks++;
    sk.flags |= (unsigned)(w >> 48) & 0xff;
  } else if (tag == 0xfe) {
    sk.peak_words++;
    sk.peak1 = (unsigned)((w & 0xff) << 8 | ((w >> 8) & 0xff));
    sk.peak2 = (unsigned)(((w >> 16) & 0xff) << 8 | ((w >> 24) & 0xff));
    sk.clips |= (unsigned)(w >> 32) & 3;
    sk.flags |= (unsigned)(w >> 48) & 0xff;
  } else {
    sk.unknown++;
  }
}

static void *sk_reader(void *arg) {
  (void)arg;
  uint64_t b[SK_BURST / 8] __attribute__((aligned(16)));
  for (;;) {
    const uint32_t wr = (sk.sim ? sk.sim_sts[2] : sk.sts[2]) & 0xffff;
    /* the writer's counter moves at the address phase: the last two bursts
       may still be in flight */
    uint32_t avail = (wr - sk.rd_burst - 2) & (SK_NBURSTS - 1);
    if (avail > SK_NBURSTS - 512) {
      if (avail < SK_NBURSTS - 2) {   /* not "just behind": it lapped us       */
        sk.overruns++;
        logf_("the ring ran over (the reader fell 8 MB behind) — a new sync");
        sk_sync();
      }
      usleep(500);
      continue;
    }
    if (!avail) { usleep(1000); continue; }
    while (avail--) {
      sk_burst_copy(b, (sk.sim ? (volatile uint64_t *)sk.sim_ring : sk.ring) + sk.rd_burst * (SK_BURST / 8));
      for (unsigned k = 0; k < SK_BURST / 8; k++) { sk_word(b[k]); }
      sk.rd_burst = (sk.rd_burst + 1) & (SK_NBURSTS - 1);
    }
  }
  return NULL;
}

/* --sim-skrf: the FPGA's packer as a thread — every stage's vectors at its
 * rate, the slots in its mask as words (the stock sim's tone and noise, a
 * function of the hardware index), tick and peak words, 128-byte bursts. */
static uint64_t sim_sample(double rate, int rx, int in2, uint64_t t);

static void *sk_sim_thread(void *arg) {
  (void)arg;
  uint32_t seen = 0;
  double t0 = 0;
  uint64_t v[4] = { 0, 0, 0, 0 }, ticks = 0;
  uint64_t burst[SK_BURST / 8];
  unsigned nb = 0;
  for (;;) {
    usleep(1000);
    pthread_mutex_lock(&sk.lock);
    const uint32_t g = __atomic_load_n(&sk.sync_gen, __ATOMIC_SEQ_CST);
    if (g != seen) {                  /* a sync: the packer starts over         */
      seen = g;
      t0 = mono_ns() * 1e-9;
      memset(v, 0, sizeof(v));
      ticks = 0;
      nb = 0;
    }
    if (!(sk.sim_cfg[0] & 1)) { pthread_mutex_unlock(&sk.lock); continue; }
    const double t = mono_ns() * 1e-9 - t0;
    for (int r = 0; r < 4; r++) {
      const double rate = 384000.0 / (1 << r);
      const uint64_t want = (uint64_t)(t * rate);
      for (; v[r] < want; v[r]++) {
        const uint32_t mask = sk.sim_cfg[3 + r], ins = sk.sim_cfg[1];
        for (int s = 0; s < sk.nslots; s++) {
          if (!((mask >> s) & 1)) { continue; }
          const uint64_t p = sim_sample(rate, s % 8, (ins >> s) & 1, v[r]);
          const unsigned vc = (unsigned)(v[r] & 127);
          burst[nb++] = p | (uint64_t)((1u << 6) | ((unsigned)r << 4) | (vc & 15)) << 56 |
                        (uint64_t)(((vc >> 4) << 5) | (unsigned)s) << 48;
          if (nb == SK_BURST / 8) {
            const uint32_t at = sk.sim_sts[2] & (SK_NBURSTS - 1);
            memcpy(sk.sim_ring + at * (SK_BURST / 8), burst, SK_BURST);
            __atomic_store_n(&sk.sim_sts[2], (at + 1) & 0xffff, __ATOMIC_RELEASE);
            nb = 0;
          }
        }
        if (r == 3) {
          ticks++;
          burst[nb++] = 0xffull << 56 | ((ticks >> 40) & 0xff) | ((ticks >> 32) & 0xff) << 8 |
                        ((ticks >> 24) & 0xff) << 16 | ((ticks >> 16) & 0xff) << 24 |
                        ((ticks >> 8) & 0xff) << 32 | (ticks & 0xff) << 40;
          if (nb == SK_BURST / 8) {
            const uint32_t at = sk.sim_sts[2] & (SK_NBURSTS - 1);
            memcpy(sk.sim_ring + at * (SK_BURST / 8), burst, SK_BURST);
            __atomic_store_n(&sk.sim_sts[2], (at + 1) & 0xffff, __ATOMIC_RELEASE);
            nb = 0;
          }
        }
      }
    }
    pthread_mutex_unlock(&sk.lock);
  }
  return NULL;
}

static void sk_init(int sim) {
  sk.on = skrf = 1;
  sk.sim = sim;
  pthread_mutex_init(&sk.lock, NULL);
  if (sim) {
    sk.cfg = sk.sim_cfg;
    sk.sts = sk.sim_sts;
    sk.sim_ring = calloc(1, SK_RING_SIZE);
    sk.ring = sk.sim_ring;
    sk.sim_sts[0] = SK_MAGIC;
    sk.sim_sts[1] = 24u << 24 | 24u << 16 | 3u;
  } else {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); exit(1); }
    sk.cfg = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SK_CFG);
    sk.sts = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SK_STS);
    sk.ring = mmap(NULL, SK_RING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SK_RING);
    close(fd);
    if (sk.cfg == MAP_FAILED || sk.sts == MAP_FAILED || sk.ring == MAP_FAILED) { perror("mmap skrf"); exit(1); }
    if (sk.sts[0] != SK_MAGIC) {
      logf_("--fpga skrf, but the FPGA does not say SKRF (%08x)", sk.sts[0]);
      exit(1);
    }
  }
  sk.nslots = (int)(sk.sts[1] >> 24);
  sk.version = (int)(sk.sts[1] & 0xffff);
  if (sk.nslots > SK_MAX) { sk.nslots = SK_MAX; }
  sk.slot = calloc((size_t)sk.nslots, sizeof(SkSlot));
  if (!sk.slot || (sim && !sk.sim_ring)) { perror("calloc"); exit(1); }
  for (int s = 0; s < sk.nslots; s++) { sk.cfg[11 + s] = 0; }
  sk_sync();
  pthread_t t;
  if (pthread_create(&t, NULL, sk_reader, NULL) == 0) { pthread_detach(t); }
  if (sim && pthread_create(&t, NULL, sk_sim_thread, NULL) == 0) { pthread_detach(t); }
}

/* An instance's receivers' buffers: the samples every one of them has from
 * in->sk_rd on (avail), or -1 when one of them restarted its timeline (a
 * new rate, a sync) or fell a buffer behind (an overflow): reset. */
static int sk_avail(Inst *in, int receivers, uint64_t *avail) {
  const int stage = sk_stage_of(*in->rate);
  uint64_t a = UINT64_MAX;
  for (int r = 0; r < receivers; r++) {
    SkSlot *x = &sk.slot[8 * in->k + r];
    if (x->stage != stage) { *avail = 0; return 0; }   /* not at this rate yet */
    if (x->gen != in->sk_gen[r]) { return -2; }
    const uint64_t w = __atomic_load_n(&x->wr, __ATOMIC_ACQUIRE);
    const uint64_t have = w > in->sk_rd ? w - in->sk_rd : 0;
    if (have > SK_BUF - 8192) { return -1; }
    if (have < a) { a = have; }
  }
  *avail = a == UINT64_MAX ? 0 : a;
  return 0;
}

/* A reset right after a new rate or receiver count waits (≤ 100 ms) for its
 * slots' first words at that rate: anchored before, the stream would only
 * meet the new timeline later and reset again. */
static void sk_settle(Inst *in) {
  const int stage = sk_stage_of(*in->rate);
  for (int tries = 0; tries < 200; tries++) {
    int ok = 1;
    for (int r = 0; r < in->receivers && ok; r++) {
      ok = sk.slot[8 * in->k + r].stage == stage && sk.slot[8 * in->k + r].wr > 0;
    }
    if (ok) { return; }
    usleep(500);
  }
}

/* The common index a reset anchors at: past every receiver's newest sample. */
static uint64_t sk_anchor(Inst *in) {
  uint64_t a = 0;
  for (int r = 0; r < in->receivers; r++) {
    const SkSlot *x = &sk.slot[8 * in->k + r];
    if (x->wr > a) { a = x->wr; }
  }
  return a;
}

static void sk_set_rd(Inst *in, uint64_t at) {
  in->sk_rd = at;
  for (int r = 0; r < NRX; r++) {
    if (8 * in->k + r < sk.nslots) { in->sk_gen[r] = sk.slot[8 * in->k + r].gen; }
  }
}

static void skrs_stop_locked(const char *why) {
  if (!skrs.on) { return; }
  char a[32];
  inet_ntop(AF_INET, &skrs.to.sin_addr, a, sizeof(a));
  logf_("SKRS: %s:%u's %d streams stopped (%s)", a, ntohs(skrs.to.sin_port), skrs.n, why);
  skrs.on = 0;
  skrs.n = 0;
  skrs.holds = 0;
}

/* A start: the slots for n streams, or 0 when they do not fit. */
static int skrs_start_locked(const struct sockaddr_in *from, const uint8_t *b, ssize_t len) {
  const int n = b[6];
  if (n < 1 || n > sk.nslots || len < 7 + 7 * n) { return 0; }
  /* free slots: 16 … first, then an idle instance's */
  int order[SK_MAX], no = 0;
  for (int sl = 16; sl < sk.nslots; sl++) { order[no++] = sl; }
  for (int k = 0; k < ninst; k++) {
    if (inst[k].active) { continue; }
    for (int r = 0; r < NRX && 8 * k + r < sk.nslots; r++) { order[no++] = 8 * k + r; }
  }
  if (ninst < 2) {
    for (int r = 0; r < NRX && 8 + r < sk.nslots; r++) { order[no++] = 8 + r; }
  }
  if (no < n) { return 0; }
  skrs.to = *from;
  skrs.n = n;
  skrs.holds = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t *e = b + 7 + 7 * i;
    SkrsStream *x = &skrs.st[i];
    memset(x, 0, sizeof(*x));
    x->id = e[0];
    x->hz = (uint32_t)e[1] << 24 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 8 | e[4];
    if (x->hz > 61440000u) { x->hz = 61440000u; }
    x->stage = (uint8_t)(3 - (e[5] & 3));      /* 0 = 48 k on the wire, stage 3 */
    x->in2 = e[6] == 2;
    x->slot = order[i];
    x->rd = 0;
    x->gen = 0;
    skrs.holds |= 1u << x->slot;
  }
  skrs.on = 1;
  skrs.last_ka_ns = mono_ns();
  skrs.starts++;
  char a[32];
  inet_ntop(AF_INET, &from->sin_addr, a, sizeof(a));
  logf_("SKRS: %d streams to %s:%u", n, a, ntohs(from->sin_port));
  return 1;
}

/* The control socket: starts, stops, keep-alives; the 5-s timeout. */
static void *skrs_ctl_thread(void *arg) {
  (void)arg;
  for (;;) {
    struct pollfd p = { .fd = skrs.sock, .events = POLLIN };
    const int pr = poll(&p, 1, 500);
    pthread_mutex_lock(&skrs.lock);
    if (skrs.on && mono_ns() - skrs.last_ka_ns > 5000000000LL) {
      skrs.timeouts++;
      skrs_stop_locked("no keep-alive for 5 s");
      pthread_mutex_unlock(&skrs.lock);
      sk_program();
      continue;
    }
    pthread_mutex_unlock(&skrs.lock);
    if (pr <= 0 || !(p.revents & POLLIN)) { continue; }
    uint8_t b[512];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const ssize_t len = recvfrom(skrs.sock, b, sizeof(b), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (len < 6 || memcmp(b, "SKRC", 4) != 0 || b[4] != 1) { continue; }
    int changed = 0;
    pthread_mutex_lock(&skrs.lock);
    const int same = skrs.on && from.sin_addr.s_addr == skrs.to.sin_addr.s_addr &&
                     from.sin_port == skrs.to.sin_port;
    if (b[5] == 1) {                     /* start (or a new set from the same client) */
      if (skrs.on && !same) {
        /* one SKRS client at a time: a second one is refused (no reply) */
      } else {
        skrs_stop_locked("a new start");
        uint8_t ack[8] = { 'S', 'K', 'R', 'C', 1, 0x81, 0, 0 };
        ack[6] = (uint8_t)skrs_start_locked(&from, b, len);
        sendto(skrs.sock, ack, sizeof(ack), 0, (struct sockaddr *)&from, fl);
        changed = 1;
      }
    } else if (b[5] == 2 && same) {
      skrs_stop_locked("the client's stop");
      changed = 1;
    } else if (b[5] == 3 && same) {
      skrs.last_ka_ns = mono_ns();
    }
    pthread_mutex_unlock(&skrs.lock);
    if (changed) { sk_program(); }
  }
  return NULL;
}

/* The sender: every stream's new samples, SKRS_MAXN a datagram, a slot's
 * datagrams of one round in one GSO send. */
static void *skrs_send_thread(void *arg) {
  (void)arg;
  static uint8_t buf[8 * (SKRS_HDR + 6 * SKRS_MAXN)];
  const size_t dsize = SKRS_HDR + 6 * SKRS_MAXN;
  for (;;) {
    int sent_any = 0;
    pthread_mutex_lock(&skrs.lock);
    for (int i = 0; i < skrs.n && skrs.on; i++) {
      SkrsStream *x = &skrs.st[i];
      SkSlot *sl = &sk.slot[x->slot];
      if (sl->stage != x->stage || sl->wr == 0) { continue; }      /* not yet at its rate */
      if (sl->gen != x->gen) {                       /* (re)anchor on its timeline */
        x->gen = sl->gen;
        x->rd = sl->wr;
        continue;
      }
      const uint64_t wr = __atomic_load_n(&sl->wr, __ATOMIC_ACQUIRE);
      uint64_t have = wr > x->rd ? wr - x->rd : 0;
      if (have > SK_BUF - 8192) {                    /* fell behind: skip ahead */
        x->rd = wr - 4096;
        x->lost = 1;
        have = 4096;
      }
      int m = 0;
      while (have >= SKRS_MAXN && m < 8) {
        uint8_t *d = buf + (size_t)m * dsize;
        memcpy(d, "SKRS", 4);
        d[4] = 1;
        d[5] = x->id;
        d[6] = (uint8_t)(3 - x->stage);
        d[7] = (uint8_t)((x->lost ? 1 : 0) | ((sk.flags >> 1) & 3) << 1);
        x->lost = 0;
        const uint32_t ep = sk.sync_gen;
        d[8] = (uint8_t)(ep >> 24); d[9] = (uint8_t)(ep >> 16); d[10] = (uint8_t)(ep >> 8); d[11] = (uint8_t)ep;
        for (int b = 0; b < 8; b++) { d[12 + b] = (uint8_t)(x->rd >> (56 - 8 * b)); }
        d[20] = SKRS_MAXN >> 8;
        d[21] = SKRS_MAXN & 0xff;
        uint8_t *q = d + SKRS_HDR;
        for (int k = 0; k < SKRS_MAXN; k++, q += 6) {
          const uint64_t idx = x->rd + (uint64_t)k;
          const uint64_t w = sl->w[idx % SK_BUF];
          if ((w >> 48) == (idx & 0xffff)) { memcpy(q, &w, 6); } else { memset(q, 0, 6); }
        }
        x->rd += SKRS_MAXN;
        have -= SKRS_MAXN;
        m++;
      }
      if (!m) { continue; }
      struct sockaddr_in to = skrs.to;
      int done = 0;
      if (use_gso && m > 1) {
        char cbuf[CMSG_SPACE(sizeof(uint16_t))];
        memset(cbuf, 0, sizeof(cbuf));
        struct iovec iov = { buf, (size_t)m * dsize };
        struct msghdr mh = { .msg_name = &to, .msg_namelen = sizeof(to), .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_UDP;
        cm->cmsg_type = UDP_SEGMENT;
        cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
        const uint16_t seg = (uint16_t)dsize;
        memcpy(CMSG_DATA(cm), &seg, sizeof(seg));
        done = sendmsg(skrs.sock, &mh, 0) >= 0;
      }
      for (int k = 0; !done && k < m; k++) {
        sendto(skrs.sock, buf + (size_t)k * dsize, dsize, 0, (struct sockaddr *)&to, sizeof(to));
      }
      x->packets += (uint64_t)m;
      sent_any = 1;
    }
    pthread_mutex_unlock(&skrs.lock);
    if (!sent_any) { usleep(1000); }
  }
  return NULL;
}

static void skrs_init(const Inst *a) {
  skrs.sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (skrs.sock < 0) { perror("socket"); return; }
  int yes = 1, big = 4 << 20;
  setsockopt(skrs.sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(skrs.sock, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
  struct sockaddr_in b = a->bind_addr;
  b.sin_port = htons((uint16_t)(ntohs(a->bind_addr.sin_port) + 1));     /* 1025 beside 1024 */
  if (a->ifname[0]) {
    setsockopt(skrs.sock, SOL_SOCKET, SO_BINDTODEVICE, a->ifname, strlen(a->ifname) + 1);
  }
  if (bind(skrs.sock, (struct sockaddr *)&b, sizeof(b)) < 0) {
    perror("SKRS bind");
    close(skrs.sock);
    skrs.sock = -1;
    return;
  }
  pthread_t t;
  if (pthread_create(&t, NULL, skrs_ctl_thread, NULL) == 0) { pthread_detach(t); }
  if (pthread_create(&t, NULL, skrs_send_thread, NULL) == 0) { pthread_detach(t); }
}

/* n samples of receiver r from in->sk_rd on, as the stock FIFO's words (the
 * first 6 bytes I then Q, big-endian 24 bits); a sample the ring lost is 0. */
static void sk_read(Inst *in, int r, uint64_t *out, int n) {
  const SkSlot *x = &sk.slot[8 * in->k + r];
  for (int k = 0; k < n; k++) {
    const uint64_t i = in->sk_rd + (uint64_t)k;
    const uint64_t w = x->w[i % SK_BUF];
    out[k] = (w >> 48) == (i & 0xffff) ? w : 0;
  }
}


/* ---- EP6: the stream ----------------------------------------------------------- */

static void *ep6_thread(void *arg) {
  Inst *in = arg;
  static const uint8_t header[40] = {
    127, 127, 127, 0, 0, 33, 17, 25,
    127, 127, 127, 8, 0, 0, 0, 0,
    127, 127, 127, 16, 0, 0, 0, 0,
    127, 127, 127, 24, 0, 0, 0, 0,
    127, 127, 127, 32, 66, 66, 66, 66,
  };
  static __thread uint64_t data[NRX][512];
  uint8_t buffer[25 * 1032];
  struct iovec iov[25];
  struct mmsghdr dgram[25];
  memset(dgram, 0, sizeof(dgram));
  pthread_mutex_lock(&in->lock);
  struct sockaddr_in to = in->client;
  pthread_mutex_unlock(&in->lock);
  for (int i = 0; i < 25; i++) {
    *(uint32_t *)(buffer + i * 1032) = 0x0601feef;
    iov[i].iov_base = buffer + i * 1032;
    iov[i].iov_len = 1032;
    dgram[i].msg_hdr.msg_iov = &iov[i];
    dgram[i].msg_hdr.msg_iovlen = 1;
    dgram[i].msg_hdr.msg_name = &to;
    dgram[i].msg_hdr.msg_namelen = sizeof(to);
  }
  /* GSO: the m datagrams of a batch as one send (UDP_SEGMENT, kernel 4.18+),
   * cut into 1032-byte datagrams by the kernel — each identical to one
   * sendmmsg would send; refused once, sendmmsg from then on */
  int gso = use_gso;
  char cbuf[CMSG_SPACE(sizeof(uint16_t))];
  struct iovec giov = { buffer, 0 };
  struct msghdr gmsg = { .msg_name = &to, .msg_namelen = sizeof(to), .msg_iov = &giov, .msg_iovlen = 1,
                         .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
  memset(cbuf, 0, sizeof(cbuf));
  struct cmsghdr *cm = CMSG_FIRSTHDR(&gmsg);
  cm->cmsg_level = SOL_UDP;
  cm->cmsg_type = UDP_SEGMENT;
  cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
  const uint16_t seg = 1032;
  memcpy(CMSG_DATA(cm), &seg, sizeof(seg));
  int header_offset = 0;
  in->seq = 0;
  sk_program();                                  /* its slots on (skrf)     */
  stream_reset(in);
  int was = in->receivers;
  const int marked = in->marked;
  while (in->enable) {
    const int receivers = in->receivers;
    const int size = receivers * 6 + 2, n = 504 / size, m = 256 / n;
    const uint32_t r0 = __atomic_load_n(&in->resets, __ATOMIC_SEQ_CST);
    if (r0 != in->seen_resets) {                 /* a reset began an epoch */
      in->seen_resets = r0;
      in->epoch = in->reset_epoch;
      in->idx = 0;
    }
    /* only the receivers asked for are read (the rest overflow on their
     * own — F0: that stalls nobody); a new count realigns them all */
    if (receivers != was) {
      was = receivers;
      sk_program();
      stream_reset(in);
      continue;
    }
    if (skrf) {
      pthread_rwlock_rdlock(&fifo_rw);
      if (__atomic_load_n(&in->resets, __ATOMIC_SEQ_CST) != in->seen_resets) {
        pthread_rwlock_unlock(&fifo_rw);
        continue;
      }
      uint64_t avail = 0;
      int st = sk_avail(in, receivers, &avail);
      if (hw.force_overflow[in->k]) {
        hw.force_overflow[in->k] = 0;
        st = -1;
      }
      if (st < 0) {                    /* fell a buffer behind, or a new timeline */
        pthread_rwlock_unlock(&fifo_rw);
        if (st == -1) {
          in->overflows++;
          logf_("%s: overflow — RX1's slot %d at %llu, the stream at %llu (%d receivers)", in->name,
                8 * in->k, (unsigned long long)sk.slot[8 * in->k].wr, (unsigned long long)in->sk_rd,
                receivers);
        }
        stream_reset(in);
        continue;
      }
      const uint64_t need = (uint64_t)(m * n * 2);
      if (avail < need) {
        pthread_rwlock_unlock(&fifo_rw);
        long us = (long)((double)(need - avail) * 1e6 / rate_hz(in));
        usleep((useconds_t)(us < 200 ? 200 : us > 20000 ? 20000 : us));
        continue;
      }
      for (int r = 0; r < receivers; r++) { sk_read(in, r, data[r], m * n * 2); }
      in->sk_rd += need;
      pthread_rwlock_unlock(&fifo_rw);
    } else {
    pthread_rwlock_rdlock(&fifo_rw);
    if (__atomic_load_n(&in->resets, __ATOMIC_SEQ_CST) != in->seen_resets) {
      pthread_rwlock_unlock(&fifo_rw);           /* reset meanwhile: adopt it */
      continue;
    }
    unsigned c = fifo_count(in);
    if (c >= 2048 || (hw.sim && hw.force_overflow[in->k])) {
      pthread_rwlock_unlock(&fifo_rw);
      hw.force_overflow[in->k] = 0;
      in->overflows++;
      stream_reset(in);
      continue;
    }
    const unsigned need = (unsigned)(m * n * 4);
    if (c < need) {                    /* sleep until the batch is due, not 1 ms polls */
      pthread_rwlock_unlock(&fifo_rw);
      long us = (long)((need - c) / 2 * 1e6 / rate_hz(in));
      usleep((useconds_t)(us < 200 ? 200 : us > 20000 ? 20000 : us));
      continue;
    }
    for (int r = 0; r < receivers; r++) { fifo_read_n(in, r, data[r], m * n * 2); }
    if (hw.sim) { hw.taken[in->k] += (uint64_t)(m * n * 2); }
    pthread_rwlock_unlock(&fifo_rw);
    }
    for (int i = 0; i < m; i++) {
      const uint32_t be = htonl(in->seq++);
      memcpy(buffer + i * 1032 + 4, &be, 4);
    }
    int data_offset = 0;
    for (int i = 0; i < m * 2; i++) {
      uint8_t *p = buffer + i * 516 - i % 2 * 4 + 8;
      if (marked) {
        /* the epoch and the packet's first sample (docs/PHASING.md) */
        const uint64_t ix = in->idx + (uint64_t)(i / 2) * 2 * n;
        const uint8_t h[8] = { 127, 127, 127,
                               i % 2 ? 0x80 : 0x78,
                               i % 2 ? (uint8_t)(ix >> 24) : in->epoch,
                               i % 2 ? (uint8_t)(ix >> 16) : (uint8_t)(ix >> 40),
                               i % 2 ? (uint8_t)(ix >> 8) : (uint8_t)(ix >> 32),
                               i % 2 ? (uint8_t)ix : (uint8_t)(in->paired ? 1 : 0) };
        memcpy(p, h, 8);
      } else {
        memcpy(p, header + header_offset, 8);
        header_offset = header_offset >= 32 ? 0 : header_offset + 8;
      }
      p += 8;
      memset(p, 0, 504);
      for (int j = 0; j < n; j++) {
        for (int r = 0; r < receivers; r++) { memcpy(p + r * 6, &data[r][data_offset], 6); }
        data_offset++;
        p += size;
      }
    }
    int sent;
    if (gso) {
      giov.iov_len = (size_t)m * 1032;
      sent = sendmsg(in->sock, &gmsg, 0) < 0 ? -1 : m;
      if (sent < 0 && (errno == EINVAL || errno == ENOPROTOOPT || errno == EIO || errno == EOPNOTSUPP)) {
        logf_("%s: UDP GSO refused (%s) — sendmmsg from now on", in->name, strerror(errno));
        gso = use_gso = 0;
        sent = sendmmsg(in->sock, dgram, m, 0);
      }
    } else {
      sent = sendmmsg(in->sock, dgram, m, 0);
    }
    if (sent > 0) { in->packets += (uint64_t)sent; }
    in->idx += (uint64_t)m * 2 * n;
  }
  in->active = 0;
  return NULL;
}

static void stream_stop(Inst *in) {
  if (!in->active) { return; }
  in->enable = 0;
  pthread_join(in->thread, NULL);
  in->active = 0;
  if (in->marked) {                 /* the skimmer's inputs end with its stream */
    in->marked = 0;
    in->paired = 0;
    *in->sel = in->base_sel;
  }
  sk_program();                     /* its slots off (skrf)                     */
}

static void stream_start(Inst *in, const struct sockaddr_in *from, int marked, int paired) {
  stream_stop(in);
  pthread_mutex_lock(&in->lock);
  in->marked = marked;
  in->paired = marked && paired;
  in->client = *from;
  in->since = time(NULL);
  in->packets = 0;
  in->unreach_first_ns = in->unreach_last_ns = 0;
  in->unreach_count = 0;
  pthread_mutex_unlock(&in->lock);
  in->enable = 1;
  in->active = 1;
  if (pthread_create(&in->thread, NULL, ep6_thread, in) != 0) {
    perror("pthread_create");
    in->active = in->enable = 0;
  }
}

/* The ADC word (C0 = 0x1C: C1 RX1..RX4, C2 RX5..RX8, 2 bits each, 1 = IN2)
 * from from: taken when from is the marked client streaming now; kept for
 * a marked start from that address that follows (the client configures
 * before it starts). Another client's word changes nothing — its inputs
 * are the manager's (ctl sel), not its own. */
static void adc_word(Inst *in, const uint8_t *f, const struct sockaddr_in *from) {
  uint8_t sel = 0;
  for (int r = 0; r < 8; r++) {
    if (((f[1 + r / 4] >> (2 * (r % 4))) & 3) == 1) { sel |= (uint8_t)(1u << r); }
  }
  in->adc_from = *from;
  in->adc_sel = sel;
  in->adc_have = 1;
  if (in->active && in->marked && from->sin_addr.s_addr == in->client.sin_addr.s_addr &&
      from->sin_port == in->client.sin_port) {
    *in->sel = sel;
    sk_program();
  }
}

/* An instance's IPv4 now (network order; 0 = none): its interface's (DHCP
 * may change it), or the address it is bound to (--sim). */
static uint32_t inst_ipv4(const Inst *in) {
  if (!in->ifname[0]) { return in->bind_addr.sin_addr.s_addr; }
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) { return 0; }
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", in->ifname);
  uint32_t a = 0;
  if (ioctl(s, SIOCGIFADDR, &ifr) == 0) { a = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr; }
  close(s);
  return a;
}

/* One EP2 control frame (C0 at frame[0]), as Pavel's process_ep2. */
static void ep2_frame(Inst *in, const uint8_t *f, const struct sockaddr_in *from) {
  if (f[0] == 0x1C || f[0] == 0x1D) {
    adc_word(in, f, from);
    return;
  }
  static const int rx_of[38] = {
    [4] = 1, [5] = 1, [6] = 2, [7] = 2, [8] = 3, [9] = 3, [10] = 4, [11] = 4,
    [12] = 5, [13] = 5, [14] = 6, [15] = 6, [16] = 7, [17] = 7, [36] = 8, [37] = 8,
  };
  if (f[0] == 0 || f[0] == 1) {
    in->receivers = ((f[4] >> 3) & 7) + 1;
    static const uint16_t reg[4] = { 1280, 640, 320, 160 };
    const uint16_t was = *in->rate;
    *in->rate = reg[f[1] & 3];
    if (skrf && *in->rate != was) { sk_program(); }   /* its slots to that stage */
    return;
  }
  if (f[0] >= 38 || !rx_of[f[0]]) { return; }
  uint32_t hz;
  memcpy(&hz, f + 1, 4);
  hz = ntohl(hz);
  if (hz > 61440000u) { return; }
  const int rx = rx_of[f[0]] - 1;
  const uint32_t was = in->freq_hz[rx];
  in->freq_hz[rx] = hz;
  *in->freq[rx] = phase_word(hz);
  if (skrf && hz != was) { sk_program(); }
}

/* An ICMP error queued on the socket: port unreachable from our client
 * counts towards the orphan stop. */
static void read_errqueue(Inst *in) {
  for (;;) {
    uint8_t buf[64], cbuf[512];
    struct sockaddr_in from;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = { .msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &iov,
                          .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
    if (recvmsg(in->sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) { return; }
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level != IPPROTO_IP || c->cmsg_type != IP_RECVERR) { continue; }
      const struct sock_extended_err *ee = (const void *)CMSG_DATA(c);
      if (ee->ee_origin != SO_EE_ORIGIN_ICMP || ee->ee_type != 3 || ee->ee_code != 3) { continue; }
      pthread_mutex_lock(&in->lock);
      const int ours = in->active && from.sin_addr.s_addr == in->client.sin_addr.s_addr &&
                       from.sin_port == in->client.sin_port;
      if (ours) {
        const int64_t now = mono_ns();
        if (!in->unreach_first_ns || now - in->unreach_last_ns > 3000000000LL) {
          in->unreach_first_ns = now;               /* a fresh run of them */
        }
        in->unreach_last_ns = now;
        in->unreach_count++;
      }
      pthread_mutex_unlock(&in->lock);
    }
  }
}

static void check_orphan(Inst *in) {
  if (!in->active || !in->unreach_first_ns) { return; }
  const int64_t now = mono_ns();
  if (now - in->unreach_last_ns > 3000000000LL) {   /* the run ended: not dead */
    in->unreach_first_ns = 0;
    return;
  }
  if (in->unreach_last_ns - in->unreach_first_ns >= (int64_t)orphan_s * 1000000000LL) {
    char a[32];
    inet_ntop(AF_INET, &in->client.sin_addr, a, sizeof(a));
    logf_("%s: %s:%u answers its stream only with port-unreachable for %d s — "
          "an orphan, stopped", in->name, a, ntohs(in->client.sin_port), orphan_s);
    stream_stop(in);
    in->orphan_stops++;
    in->unreach_first_ns = 0;
  }
}

static int open_socket(Inst *in) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) { perror("socket"); return -1; }
  int yes = 1, big = 2 << 20;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
  setsockopt(s, IPPROTO_IP, IP_RECVERR, &yes, sizeof(yes));
  if (in->ifname[0]) {
    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, in->ifname, strlen(in->ifname) + 1) < 0) {
      perror("SO_BINDTODEVICE");
      close(s);
      return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, in->ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0) { memcpy(in->mac, ifr.ifr_hwaddr.sa_data, 6); }
  }
  if (bind(s, (struct sockaddr *)&in->bind_addr, sizeof(in->bind_addr)) < 0) {
    perror("bind");
    close(s);
    return -1;
  }
  return s;
}

static void *server_thread(void *arg) {
  Inst *in = arg;
  uint8_t reply[20] = { 0xef, 0xfe, 2, 0, 0, 0, 0, 0, 0, 25, 1, 'R', '_', 'P', 'I', 'T', 'A', 'Y', 'A', 8 };
  memcpy(reply + 3, in->mac, 6);
  for (;;) {
    struct pollfd p = { .fd = in->sock, .events = POLLIN };
    const int pr = poll(&p, 1, 1000);
    if (pr > 0 && (p.revents & POLLERR)) { read_errqueue(in); }
    check_orphan(in);
    if (pr <= 0 || !(p.revents & POLLIN)) { continue; }
    uint8_t buf[1032];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const ssize_t n = recvfrom(in->sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n < 4) { continue; }                     /* errors (ECONNREFUSED…) too */
    uint32_t code;
    memcpy(&code, buf, 4);
    switch (code) {
    case 0x0201feef:
      if (n >= 1032) {
        ep2_frame(in, buf + 11, &from);
        ep2_frame(in, buf + 523, &from);
      }
      break;
    case 0x0002feef: {
      uint8_t out[60];
      memset(out, 0, sizeof(out));
      memcpy(out, reply, 20);
      out[2] = (uint8_t)(2 + (in->active || skrs_holds_inst(in->k) ? 1 : 0));
      if (n >= 8 && memcmp(buf + 4, "SKRP", 4) == 0) {   /* our client asks (byte 3 stays 0: it is part of the command) */
        memcpy(out + 0x14, "SKRP", 4);
        out[0x18] = AGENT_MAJOR;
        out[0x19] = AGENT_MINOR;
        out[0x1A] = MODE_P1;
        out[0x1B] = *in->sel;
        out[0x1C] = (uint8_t)in->k;
        if (ninst > 1) {                         /* the other instance's IPv4 */
          const uint32_t peer = inst_ipv4(&inst[1 - in->k]);
          memcpy(out + 0x1D, &peer, 4);
        }
        /* the skimmer's bitstream: its slots, and SKRS's port (FPGA.md) */
        out[0x21] = skrf ? (uint8_t)sk.nslots : 0;
        if (skrf && skrs.sock >= 0) {
          const uint16_t sp = (uint16_t)(ntohs(in->bind_addr.sin_port) + 1);
          out[0x22] = (uint8_t)(sp >> 8);
          out[0x23] = (uint8_t)sp;
        }
      }
      sendto(in->sock, out, sizeof(out), 0, (struct sockaddr *)&from, fl);
      break;
    }
    case 0x0004feef:
      stream_stop(in);
      break;
    case 0x0104feef:
    case 0x0204feef:
    case 0x0304feef: {
      /* marked: "SKRP" in bytes 4–7; paired: byte 8 bit 0 (docs/PHASING.md) */
      const int mk = n >= 8 && memcmp(buf + 4, "SKRP", 4) == 0;
      if (skrs_holds_inst(in->k)) { break; }     /* its slots stream as SKRS */
      stream_start(in, &from, mk, mk && n >= 9 && (buf[8] & 1));
      if (in->marked && in->adc_have && in->adc_from.sin_addr.s_addr == from.sin_addr.s_addr &&
          in->adc_from.sin_port == from.sin_port) {
        *in->sel = in->adc_sel;
        sk_program();
      }
      break;
    }
    default:
      break;
    }
  }
  return NULL;
}

/* ---- status ---------------------------------------------------------------------- */

static int read_file(const char *path, char *buf, size_t room) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) { return -1; }
  ssize_t n = read(fd, buf, room - 1);
  close(fd);
  if (n < 0) { return -1; }
  buf[n] = 0;
  return (int)n;
}

static double read_num(const char *path, double dflt) {
  char b[64];
  return read_file(path, b, sizeof(b)) > 0 ? atof(b) : dflt;
}

/* The Zynq's XADC temperature, °C (NaN = none). */
static double fpga_temp(void) {
  const char *d = "/sys/bus/iio/devices/iio:device0/in_temp0_";
  char p[128];
  snprintf(p, sizeof(p), "%sraw", d);
  const double raw = read_num(p, NAN);
  snprintf(p, sizeof(p), "%soffset", d);
  const double off = read_num(p, NAN);
  snprintf(p, sizeof(p), "%sscale", d);
  const double sc = read_num(p, NAN);
  return isnan(raw) || isnan(off) || isnan(sc) ? NAN : (raw + off) * sc / 1000.0;
}

/* hw_rev=… from the board's EEPROM (the factory environment at 0x1800). */
static void board_rev(char *out, size_t room) {
  snprintf(out, room, "%s", hw.sim || sk.sim ? "SIM" : "?");
  int fd = open("/sys/bus/i2c/devices/0-0050/eeprom", O_RDONLY);
  if (fd < 0) { return; }
  char env[1024];
  ssize_t n = pread(fd, env, sizeof(env) - 1, 6144);
  close(fd);
  if (n <= 0) { return; }
  env[n] = 0;
  for (ssize_t i = 0; i < n;) {
    const char *s = env + i;
    const size_t l = strnlen(s, (size_t)(n - i));
    if (strncmp(s, "hw_rev=", 7) == 0) {
      snprintf(out, room, "%.*s", (int)(l - 7 < 63 ? l - 7 : 63), s + 7);
      return;
    }
    i += (ssize_t)l + 1;
  }
}

static void json_str(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { fputc('\\', f); }
    if ((unsigned char)*s >= 0x20) { fputc(*s, f); }
  }
  fputc('"', f);
}

static void status_json(FILE *f) {
  char rev[64], b[256];
  board_rev(rev, sizeof(rev));
  const double t = fpga_temp();
  double l1 = 0, l5 = 0, l15 = 0, up = 0;
  if (read_file("/proc/loadavg", b, sizeof(b)) > 0) { sscanf(b, "%lf %lf %lf", &l1, &l5, &l15); }
  if (read_file("/proc/uptime", b, sizeof(b)) > 0) { up = atof(b); }
  long mem_total = 0, mem_avail = 0;
  FILE *mi = fopen("/proc/meminfo", "r");
  if (mi) {
    char line[128];
    while (fgets(line, sizeof(line), mi)) {
      sscanf(line, "MemTotal: %ld", &mem_total);
      sscanf(line, "MemAvailable: %ld", &mem_avail);
    }
    fclose(mi);
  }
  fprintf(f, "{\"agent\":\"%d.%d\",\"read_mode\":%d,\"gso\":%s,\"board\":", AGENT_MAJOR, AGENT_MINOR,
          read_mode, use_gso ? "true" : "false");
  json_str(f, rev);
  fprintf(f, ",\"mode\":\"p1\",\"sim\":%s", hw.sim || sk.sim ? "true" : "false");
  if (skrf) {
    fprintf(f, ",\"fpga\":{\"bitstream\":\"skrf\",\"version\":\"%d.%d\",\"slots\":%d,\"ticks\":%llu,"
               "\"words\":%llu,\"unknown\":%llu,\"overruns\":%llu,\"syncs\":%llu,\"flags\":%u,"
               "\"peak_in1\":%u,\"peak_in2\":%u,\"clips\":%u}",
            (sk.version >> 8) & 0xff, sk.version & 0xff, sk.nslots, (unsigned long long)sk.ticks,
            (unsigned long long)sk.words, (unsigned long long)sk.unknown, (unsigned long long)sk.overruns,
            (unsigned long long)sk.syncs, sk.flags, sk.peak1, sk.peak2, sk.clips);
    pthread_mutex_lock(&skrs.lock);
    char a[32] = "";
    if (skrs.on) { inet_ntop(AF_INET, &skrs.to.sin_addr, a, sizeof(a)); }
    fprintf(f, ",\"skrs\":{\"streaming\":%s,\"client\":\"%s\",\"starts\":%llu,\"timeouts\":%llu,\"streams\":[",
            skrs.on ? "true" : "false", a, (unsigned long long)skrs.starts, (unsigned long long)skrs.timeouts);
    for (int i = 0; i < skrs.n && skrs.on; i++) {
      const SkrsStream *x = &skrs.st[i];
      fprintf(f, "%s{\"id\":%u,\"slot\":%d,\"hz\":%u,\"rate\":%u,\"input\":%d,\"packets\":%llu}", i ? "," : "",
              x->id, x->slot, x->hz, 384000u >> x->stage, x->in2 ? 2 : 1, (unsigned long long)x->packets);
    }
    fprintf(f, "]}");
    pthread_mutex_unlock(&skrs.lock);
  } else {
    fprintf(f, ",\"fpga\":{\"bitstream\":\"stock\"}");
  }
  if (isnan(t)) { fprintf(f, ",\"fpga_c\":null"); } else { fprintf(f, ",\"fpga_c\":%.1f", t); }
  fprintf(f, ",\"load\":[%.2f,%.2f,%.2f],\"uptime_s\":%.0f,\"mem_kb\":%ld,\"mem_avail_kb\":%ld",
          l1, l5, l15, up, mem_total, mem_avail);
  fprintf(f, ",\"orphan_s\":%d,\"inst\":[", orphan_s);
  for (int k = 0; k < ninst; k++) {
    Inst *in = &inst[k];
    char a[32] = "", p[160];
    pthread_mutex_lock(&in->lock);
    if (in->active) { inet_ntop(AF_INET, &in->client.sin_addr, a, sizeof(a)); }
    fprintf(f, "%s{\"iface\":", k ? "," : "");
    json_str(f, in->name);
    fprintf(f, ",\"ddc\":\"%d-%d\",\"streaming\":%s,\"client\":", k * 8, k * 8 + 7,
            in->active ? "true" : "false");
    json_str(f, a);
    fprintf(f, ",\"client_port\":%u,\"since\":%ld,\"rate\":%.0f,\"receivers\":%d,\"sel\":%u,"
               "\"packets\":%llu,\"overflows\":%llu,\"orphan_stops\":%llu,\"fifo_reset_ns\":%lld,"
               "\"marked\":%s,\"paired\":%s,\"epoch\":%u,\"pair_resets\":%llu,\"freq\":[",
            in->active ? ntohs(in->client.sin_port) : 0, in->active ? (long)in->since : 0L,
            rate_hz(in), in->receivers, *in->sel, (unsigned long long)in->packets,
            (unsigned long long)in->overflows, (unsigned long long)in->orphan_stops,
            (long long)in->fifo_reset_ns, in->active && in->marked ? "true" : "false",
            in->active && in->paired ? "true" : "false", in->epoch,
            (unsigned long long)in->pair_resets);
    for (int i = 0; i < NRX; i++) { fprintf(f, "%s%u", i ? "," : "", in->freq_hz[i]); }
    fprintf(f, "]");
    pthread_mutex_unlock(&in->lock);
    if (in->ifname[0]) {
      snprintf(p, sizeof(p), "/sys/class/net/%s/statistics/tx_bytes", in->ifname);
      const double txb = read_num(p, 0);
      snprintf(p, sizeof(p), "/sys/class/net/%s/statistics/tx_packets", in->ifname);
      const double txp = read_num(p, 0);
      /* a macvlan's link speed is its parent's (eth0) */
      snprintf(p, sizeof(p), "/sys/class/net/%.15s/speed", k ? "eth0" : in->ifname);
      fprintf(f, ",\"tx_bytes\":%.0f,\"tx_packets\":%.0f,\"link_mbps\":%.0f", txb, txp, read_num(p, -1));
    }
    fprintf(f, "}");
  }
  fprintf(f, "]}\n");
}

/* ---- ctl: a unix socket, one command a connection -------------------------------- */

static void ctl_handle(int c) {
  char line[256];
  ssize_t n = read(c, line, sizeof(line) - 1);
  if (n <= 0) { return; }
  line[n] = 0;
  line[strcspn(line, "\r\n")] = 0;
  FILE *f = fdopen(dup(c), "w");
  if (!f) { return; }
  char which;
  unsigned mask;
  if (strcmp(line, "status") == 0) {
    status_json(f);
  } else if (strcmp(line, "version") == 0) {
    fprintf(f, "skimmer-rp %d.%d\n", AGENT_MAJOR, AGENT_MINOR);
  } else if (sscanf(line, "sel %c %i", &which, &mask) == 2 && (which == 'a' || which == 'b') &&
             mask <= 0xff && (which == 'a' || ninst > 1)) {
    Inst *in = &inst[which == 'b'];
    *in->sel = (uint8_t)mask;          /* the manager's word: live, and the base */
    in->base_sel = (uint8_t)mask;
    sk_program();
    logf_("%s: inputs 0x%02x (bit = receiver on IN2)", in->name, mask);
    fprintf(f, "ok\n");
  } else if ((hw.sim || sk.sim) && sscanf(line, "sim-overflow %c", &which) == 1 &&
             (which == 'a' || (which == 'b' && ninst > 1))) {
    hw.force_overflow[which == 'b'] = 1;   /* --sim: as if its FIFO ran over */
    fprintf(f, "ok\n");
  } else {
    fprintf(f, "error: unknown command (status | version | sel a|b MASK)\n");
  }
  fclose(f);
}

static void *ctl_thread(void *arg) {
  (void)arg;
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = { .sun_family = AF_UNIX };
  snprintf(a.sun_path, sizeof(a.sun_path), "%s", ctl_path);
  unlink(ctl_path);
  if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
    perror(ctl_path);
    return NULL;
  }
  chmod(ctl_path, 0600);                     /* root (over SSH) only */
  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) { continue; }
    ctl_handle(c);
    close(c);
  }
  return NULL;
}

static int ctl_client(const char *path, int argc, char **argv) {
  char line[256] = "";
  for (int i = 0; i < argc; i++) {
    strncat(line, argv[i], sizeof(line) - strlen(line) - 2);
    if (i + 1 < argc) { strncat(line, " ", sizeof(line) - strlen(line) - 1); }
  }
  strncat(line, "\n", sizeof(line) - strlen(line) - 1);
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = { .sun_family = AF_UNIX };
  snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
  if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
    fprintf(stderr, "skimmer-rp: %s: %s (is the agent running?)\n", path, strerror(errno));
    return 1;
  }
  if (write(s, line, strlen(line)) < 0) { return 1; }
  shutdown(s, SHUT_WR);
  char buf[4096];
  ssize_t n;
  int bad = 0;
  while ((n = read(s, buf, sizeof(buf))) > 0) {
    if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) { return 1; }
    if (strncmp(buf, "error", 5) == 0) { bad = 1; }
  }
  close(s);
  return bad;
}

/* ---- main ------------------------------------------------------------------------ */

/* "eth0" (an interface, port 1024) or "127.0.0.2:5000" (an address, --sim). */
static void parse_bind(Inst *in, const char *spec) {
  snprintf(in->name, sizeof(in->name), "%s", spec);
  in->bind_addr.sin_family = AF_INET;
  in->bind_addr.sin_port = htons(1024);
  in->bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  char host[64];
  snprintf(host, sizeof(host), "%s", spec);
  char *colon = strchr(host, ':');
  if (colon) {
    *colon = 0;
    in->bind_addr.sin_port = htons((uint16_t)atoi(colon + 1));
  }
  if (inet_pton(AF_INET, host, &in->bind_addr.sin_addr) != 1) {
    in->bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    snprintf(in->ifname, sizeof(in->ifname), "%.15s", host);
  }
}

static void usage(void) {
  fprintf(stderr,
          "skimmer-rp serve [--sim | --sim-skrf] [--fpga auto|stock|skrf] [--bind A[,B]] [--sel A,B]\n"
          "                 [--ctl PATH] [--orphan-s N] [--read 0|2] [--gso 0|1]\n"
          "   --fpga: the bitstream start.sh loaded (auto: SKRF in its status word = the skimmer's)\n"
          "   --read 0: one 64-bit load a sample (default 2: NEON, 4 a load); --gso 0: no UDP GSO\n"
          "   A, B: an interface (eth0, mvl0 — port 1024) or addr:port (--sim)\n"
          "   --sel: each instance's input mask (bit r set = receiver r on IN2)\n"
          "skimmer-rp ctl [--ctl PATH] status | version | sel a|b MASK\n");
  exit(2);
}

int main(int argc, char **argv) {
  if (argc < 2) { usage(); }
  if (strcmp(argv[1], "ctl") == 0) {
    int i = 2;
    if (i + 1 < argc && strcmp(argv[i], "--ctl") == 0) {
      ctl_path = argv[i + 1];
      i += 2;
    }
    if (i >= argc) { usage(); }
    return ctl_client(ctl_path, argc - i, argv + i);
  }
  if (strcmp(argv[1], "serve") != 0) { usage(); }
  const char *binds = "eth0,mvl0";
  unsigned sel[2] = { 0, 0 };
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--sim") == 0) { hw.sim = 1; }
    else if (strcmp(argv[i], "--sim-skrf") == 0) { fpga_opt = 3; }
    else if (strcmp(argv[i], "--fpga") == 0 && i + 1 < argc) {
      const char *v = argv[++i];
      fpga_opt = !strcmp(v, "stock") ? 1 : !strcmp(v, "skrf") ? 2 : !strcmp(v, "auto") ? 0 : -1;
      if (fpga_opt < 0) { usage(); }
    }
    else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) { binds = argv[++i]; }
    else if (strcmp(argv[i], "--read") == 0 && i + 1 < argc) { read_mode = atoi(argv[++i]) == 2 ? 2 : 0; }
    else if (strcmp(argv[i], "--gso") == 0 && i + 1 < argc) { use_gso = atoi(argv[++i]) != 0; }
    else if (strcmp(argv[i], "--sel") == 0 && i + 1 < argc) {
      if (sscanf(argv[++i], "%i,%i", &sel[0], &sel[1]) < 1) { usage(); }
    } else if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) { ctl_path = argv[++i]; }
    else if (strcmp(argv[i], "--orphan-s") == 0 && i + 1 < argc) { orphan_s = atoi(argv[++i]); }
    else { usage(); }
  }
  signal(SIGPIPE, SIG_IGN);
  char spec[128];
  snprintf(spec, sizeof(spec), "%s", binds);
  char *second = strchr(spec, ',');
  if (second) { *second++ = 0; }
  ninst = second ? 2 : 1;
  for (int k = 0; k < 2; k++) {
    inst[k].k = k;
    pthread_mutex_init(&inst[k].lock, NULL);
    inst[k].receivers = 1;
  }
  parse_bind(&inst[0], spec);
  if (second) { parse_bind(&inst[1], second); }
  hw_map();
  if (fpga_opt >= 2) { sk_init(fpga_opt == 3); }
  for (int k = 0; k < 2; k++) {                     /* Pavel's defaults */
    for (int i = 0; i < NRX; i++) {
      *inst[k].freq[i] = phase_word(600000);
      inst[k].freq_hz[i] = 600000;
    }
    *inst[k].rate = 1280;
    *inst[k].sel = (uint8_t)sel[k];
    inst[k].base_sel = (uint8_t)sel[k];
  }
  sk_program();
  for (int k = 0; k < ninst; k++) {
    inst[k].sock = open_socket(&inst[k]);
    if (inst[k].sock < 0) { return 1; }
  }
  if (skrf) { skrs_init(&inst[0]); }
  pthread_t t;
  if (pthread_create(&t, NULL, ctl_thread, NULL) == 0) { pthread_detach(t); }
  for (int k = 1; k < ninst; k++) {
    if (pthread_create(&t, NULL, server_thread, &inst[k]) == 0) { pthread_detach(t); }
  }
  logf_("%d.%d serving %s%s%s (%s), orphan stop after %d s", AGENT_MAJOR, AGENT_MINOR,
        inst[0].name, ninst > 1 ? ", " : "", ninst > 1 ? inst[1].name : "",
        skrf ? (sk.sim ? "the skimmer's bitstream, simulated" : "the skimmer's bitstream (SKRF)")
             : hw.sim ? "simulated FIFOs" : "/dev/mem, the stock bitstream", orphan_s);
  server_thread(&inst[0]);
  return 0;
}
