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
 *
 * --sim replaces /dev/mem with synthetic FIFOs (a tone a receiver, louder on
 * IN1 than on IN2) so the rp-agent gate runs it on the build machine.
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
#define AGENT_MINOR 2
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
  double phase[2][NRX];
} Hw;

static Hw hw;

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

static void hw_map(void) {
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

static void fifo_reset(Inst *in) {
  *in->rst |= 1;
  *in->rst &= ~1;
  in->fifo_reset_ns = mono_ns();
  if (hw.sim) {
    hw.t0[in->k] = in->fifo_reset_ns * 1e-9;
    hw.taken[in->k] = 0;
  }
}

/* RX1's FIFO fill, in 32-bit halves as the bitstream counts it. */
static unsigned fifo_count(Inst *in) {
  if (!hw.sim) { return *in->cntr[0]; }
  const double avail = (mono_ns() * 1e-9 - hw.t0[in->k]) * rate_hz(in) - (double)hw.taken[in->k];
  const double halves = avail * 2;
  return halves >= 4095 ? 4095 : halves <= 0 ? 0 : (unsigned)halves;
}

/* A 24-bit big-endian I then Q in the first 6 bytes of the word (memory
 * order), as the bitstream's FIFO hands them and the server copies them. */
static uint64_t sim_word(Inst *in, int rx) {
  /* receiver rx: a tone (rx+1) kHz above its centre at true RF, so the wire
   * (RF-inverted) carries its conjugate; IN1 −12 dBFS, IN2 −26 dBFS */
  const double amp = ((*in->sel >> rx) & 1) ? 0.05 : 0.25;
  double *ph = &hw.phase[in->k][rx];
  *ph += 2 * M_PI * 1000.0 * (rx + 1) / rate_hz(in);
  if (*ph > M_PI) { *ph -= 2 * M_PI; }
  const int32_t vi = (int32_t)lrint(amp * cos(*ph) * 8388607.0);
  const int32_t vq = (int32_t)lrint(-amp * sin(*ph) * 8388607.0);
  uint64_t w = 0;
  uint8_t *b = (uint8_t *)&w;
  b[0] = (uint8_t)(vi >> 16); b[1] = (uint8_t)(vi >> 8); b[2] = (uint8_t)vi;
  b[3] = (uint8_t)(vq >> 16); b[4] = (uint8_t)(vq >> 8); b[5] = (uint8_t)vq;
  return w;
}

static uint64_t fifo_read(Inst *in, int rx) {
  if (!hw.sim) { return *in->data[rx]; }
  return sim_word(in, rx);
}

static uint32_t phase_word(uint32_t hz) { return (uint32_t)floor(hz / ADC_HZ * (1 << 30) + 0.5); }

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
  uint8_t data[NRX][4096];
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
  int header_offset = 0;
  in->seq = 0;
  fifo_reset(in);
  while (in->enable) {
    const int receivers = in->receivers;
    const int size = receivers * 6 + 2, n = 504 / size, m = 256 / n;
    unsigned c = fifo_count(in);
    if (c >= 2048) {
      in->overflows++;
      fifo_reset(in);
      continue;
    }
    if (c < (unsigned)(m * n * 4)) {
      usleep(1000);
      continue;
    }
    for (int i = 0; i < m * n * 2; i++) {
      for (int r = 0; r < NRX; r++) {                /* all 8 drained in lockstep */
        const uint64_t w = fifo_read(in, r);
        memcpy(data[r] + i * 8, &w, 8);
      }
    }
    if (hw.sim) { hw.taken[in->k] += (uint64_t)(m * n * 2); }
    for (int i = 0; i < m; i++) {
      const uint32_t be = htonl(in->seq++);
      memcpy(buffer + i * 1032 + 4, &be, 4);
    }
    int data_offset = 0;
    for (int i = 0; i < m * 2; i++) {
      uint8_t *p = buffer + i * 516 - i % 2 * 4 + 8;
      memcpy(p, header + header_offset, 8);
      header_offset = header_offset >= 32 ? 0 : header_offset + 8;
      p += 8;
      memset(p, 0, 504);
      for (int j = 0; j < n; j++) {
        for (int r = 0; r < receivers; r++) { memcpy(p + r * 6, data[r] + data_offset, 6); }
        data_offset += 8;
        p += size;
      }
    }
    const int sent = sendmmsg(in->sock, dgram, m, 0);
    if (sent > 0) { in->packets += (uint64_t)sent; }
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
    *in->sel = in->base_sel;
  }
}

static void stream_start(Inst *in, const struct sockaddr_in *from) {
  stream_stop(in);
  pthread_mutex_lock(&in->lock);
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
  }
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
    *in->rate = reg[f[1] & 3];
    return;
  }
  if (f[0] >= 38 || !rx_of[f[0]]) { return; }
  uint32_t hz;
  memcpy(&hz, f + 1, 4);
  hz = ntohl(hz);
  if (hz > 61440000u) { return; }
  const int rx = rx_of[f[0]] - 1;
  in->freq_hz[rx] = hz;
  *in->freq[rx] = phase_word(hz);
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
      out[2] = (uint8_t)(2 + (in->active ? 1 : 0));
      if (n >= 8 && memcmp(buf + 4, "SKRP", 4) == 0) {   /* our client asks (byte 3 stays 0: it is part of the command) */
        memcpy(out + 0x14, "SKRP", 4);
        out[0x18] = AGENT_MAJOR;
        out[0x19] = AGENT_MINOR;
        out[0x1A] = MODE_P1;
        out[0x1B] = *in->sel;
        out[0x1C] = (uint8_t)in->k;
      }
      sendto(in->sock, out, sizeof(out), 0, (struct sockaddr *)&from, fl);
      break;
    }
    case 0x0004feef:
      stream_stop(in);
      break;
    case 0x0104feef:
    case 0x0204feef:
    case 0x0304feef:
      stream_start(in, &from);
      in->marked = n >= 8 && memcmp(buf + 4, "SKRP", 4) == 0;
      if (in->marked && in->adc_have && in->adc_from.sin_addr.s_addr == from.sin_addr.s_addr &&
          in->adc_from.sin_port == from.sin_port) {
        *in->sel = in->adc_sel;
      }
      break;
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
  snprintf(out, room, "%s", hw.sim ? "SIM" : "?");
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
  fprintf(f, "{\"agent\":\"%d.%d\",\"board\":", AGENT_MAJOR, AGENT_MINOR);
  json_str(f, rev);
  fprintf(f, ",\"mode\":\"p1\",\"sim\":%s", hw.sim ? "true" : "false");
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
               "\"packets\":%llu,\"overflows\":%llu,\"orphan_stops\":%llu,\"fifo_reset_ns\":%lld,\"freq\":[",
            in->active ? ntohs(in->client.sin_port) : 0, in->active ? (long)in->since : 0L,
            rate_hz(in), in->receivers, *in->sel, (unsigned long long)in->packets,
            (unsigned long long)in->overflows, (unsigned long long)in->orphan_stops,
            (long long)in->fifo_reset_ns);
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
    logf_("%s: inputs 0x%02x (bit = receiver on IN2)", in->name, mask);
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
          "skimmer-rp serve [--sim] [--bind A[,B]] [--sel A,B] [--ctl PATH] [--orphan-s N]\n"
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
    else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) { binds = argv[++i]; }
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
  for (int k = 0; k < 2; k++) {                     /* Pavel's defaults */
    for (int i = 0; i < NRX; i++) {
      *inst[k].freq[i] = phase_word(600000);
      inst[k].freq_hz[i] = 600000;
    }
    *inst[k].rate = 1280;
    *inst[k].sel = (uint8_t)sel[k];
    inst[k].base_sel = (uint8_t)sel[k];
  }
  for (int k = 0; k < ninst; k++) {
    inst[k].sock = open_socket(&inst[k]);
    if (inst[k].sock < 0) { return 1; }
  }
  pthread_t t;
  if (pthread_create(&t, NULL, ctl_thread, NULL) == 0) { pthread_detach(t); }
  for (int k = 1; k < ninst; k++) {
    if (pthread_create(&t, NULL, server_thread, &inst[k]) == 0) { pthread_detach(t); }
  }
  logf_("%d.%d serving %s%s%s (%s), orphan stop after %d s", AGENT_MAJOR, AGENT_MINOR,
        inst[0].name, ninst > 1 ? ", " : "", ninst > 1 ? inst[1].name : "",
        hw.sim ? "simulated FIFOs" : "/dev/mem", orphan_s);
  server_thread(&inst[0]);
  return 0;
}
