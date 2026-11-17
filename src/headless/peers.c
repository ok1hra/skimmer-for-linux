/* peers.c — see peers.h. Part of skimmer-for-linux. GPL-3.0-or-later.
 *
 * One thread runs a private GMainContext; every peer's connect, reads,
 * login timer and retry timer are sources on it, so a peer's state needs a
 * lock only where json() reads it from outside.
 */
#include "peers.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#include "engine/callsign.h"
#include "json.h"

#define RETRY_FIRST_S  5
#define RETRY_MAX_S    60
#define LOGIN_WAIT_MS  1500        /* no prompt by then: log in anyway        */
#define RX_MAX         8192        /* a line longer than this is junk         */

typedef struct _SkimHdPeers SkimHdPeers;

typedef struct {
  SkimHdPeers       *owner;
  char              *spec;          /* as given                              */
  char              *host;
  guint16            port;
  GSocketConnection *conn;
  GCancellable      *cancel;
  GString           *rx;
  char               buf[2048];
  gboolean           logged_in;
  guint              login_timer, retry_timer;
  guint              retry_s;
  /* read by json() from other threads: under owner->lock */
  char               state[96];
  gboolean           connected;
  guint64            spots, lines;
} Peer;

struct _SkimHdPeers {
  GMainContext  *ctx;
  GMainLoop     *loop;
  GThread       *thread;
  GPtrArray     *peers;             /* Peer*                                 */
  char          *login;
  SkimHdPeerSpot cb;
  gpointer       user;
  GMutex         lock;
  volatile gint  stopping;
};

static void peer_connect(Peer *p);
static gboolean start_cb(gpointer data);

/* A timer of ours: it lives on the module's context, not the default one. */
static void timer_stop(SkimHdPeers *s, guint *id) {
  if (!*id) { return; }
  GSource *src = g_main_context_find_source_by_id(s->ctx, *id);
  if (src) { g_source_destroy(src); }
  *id = 0;
}

static void set_state(Peer *p, gboolean connected, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
static void set_state(Peer *p, gboolean connected, const char *fmt, ...) {
  char s[96];
  va_list ap;
  va_start(ap, fmt);
  g_vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  g_mutex_lock(&p->owner->lock);
  const gboolean changed = strcmp(s, p->state) != 0;
  g_strlcpy(p->state, s, sizeof(p->state));
  p->connected = connected;
  g_mutex_unlock(&p->owner->lock);
  if (changed) { g_message("peer %s: %s", p->spec, s); }
}

/* ---- parsing ---------------------------------------------------------------------- */

static gboolean is_num(const char *t, double *v) {
  char *end = NULL;
  *v = g_ascii_strtod(t, &end);
  return end != t && *end == '\0';
}

gboolean skim_hd_peer_parse(const char *line, char *call, gsize call_size, char *mode,
                            gsize mode_size, double *freq_hz, double *snr_db,
                            double *speed) {
  while (*line == ' ' || *line == '\a') { line++; }
  if (g_ascii_strncasecmp(line, "DX de ", 6) != 0) { return FALSE; }
  const char *colon = strchr(line + 6, ':');     /* "SPOTTER-#:" may touch the kHz */
  if (!colon) { return FALSE; }
  char **t = g_strsplit_set(colon + 1, " \t\r\n", -1);
  GPtrArray *tok = g_ptr_array_new();
  for (char **q = t; *q; q++) {
    if (**q) { g_ptr_array_add(tok, *q); }
  }
  gboolean ok = FALSE;
  double khz = 0, v = 0;
  if (tok->len >= 3 && is_num(tok->pdata[0], &khz) && khz > 100 && khz < 500000) {
    const char *c = tok->pdata[1];
    char up[24];
    g_strlcpy(up, c, sizeof(up));
    for (char *x = up; *x; x++) { *x = g_ascii_toupper(*x); }
    gboolean cq = FALSE;
    *snr_db = 0;
    *speed = 0;
    g_strlcpy(mode, "CW", mode_size);
    for (guint i = 2; i < tok->len; i++) {
      const char *w = tok->pdata[i];
      if (strcmp(w, "CQ") == 0) { cq = TRUE; }
      if (i == 2 && g_ascii_isalpha(w[0]) && strcmp(w, "CQ") != 0) {
        g_strlcpy(mode, w, mode_size);
      }
      if (i + 1 < tok->len && is_num(w, &v)) {
        const char *u = tok->pdata[i + 1];
        if (g_ascii_strcasecmp(u, "dB") == 0) { *snr_db = v; }
        if (g_ascii_strcasecmp(u, "WPM") == 0 || g_ascii_strcasecmp(u, "BPS") == 0 ||
            g_ascii_strcasecmp(u, "BPM") == 0) {
          *speed = v;
        }
      }
    }
    if (cq && skim_callsign_is_valid(up)) {
      g_strlcpy(call, up, call_size);
      *freq_hz = khz * 1000.0;
      ok = TRUE;
    }
  }
  g_ptr_array_free(tok, TRUE);
  g_strfreev(t);
  return ok;
}

/* ---- one peer (module thread) --------------------------------------------------- */

static void peer_drop(Peer *p, const char *why) {
  timer_stop(p->owner, &p->login_timer);
  if (p->conn) {
    g_io_stream_close(G_IO_STREAM(p->conn), NULL, NULL);
    g_clear_object(&p->conn);
  }
  g_clear_object(&p->cancel);
  g_string_truncate(p->rx, 0);
  p->logged_in = FALSE;
  if (g_atomic_int_get(&p->owner->stopping)) { return; }
  p->retry_s = p->retry_s ? MIN(p->retry_s * 2, RETRY_MAX_S) : RETRY_FIRST_S;
  set_state(p, FALSE, "retrying in %u s (%s)", p->retry_s, why);
  GSource *t = g_timeout_source_new_seconds(p->retry_s);
  g_source_set_callback(t, start_cb, p, NULL);
  p->retry_timer = g_source_attach(t, p->owner->ctx);
  g_source_unref(t);
}

static void peer_send(Peer *p, const char *s) {
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(p->conn));
  g_output_stream_write_all(out, s, strlen(s), NULL, NULL, NULL);
}

static void peer_login(Peer *p) {
  if (p->logged_in || !p->conn) { return; }
  p->logged_in = TRUE;
  char *l = g_strdup_printf("%s\r\n", p->owner->login);
  peer_send(p, l);
  g_free(l);
  set_state(p, TRUE, "logged in as %s", p->owner->login);
  p->retry_s = 0;
}

static gboolean login_timeout(gpointer data) {
  Peer *p = data;
  p->login_timer = 0;
  peer_login(p);
  return G_SOURCE_REMOVE;
}

static void peer_line(Peer *p, const char *line) {
  char call[24], mode[12];
  double f, snr, speed;
  g_mutex_lock(&p->owner->lock);
  p->lines++;
  g_mutex_unlock(&p->owner->lock);
  if (!skim_hd_peer_parse(line, call, sizeof(call), mode, sizeof(mode), &f, &snr, &speed)) {
    return;
  }
  g_mutex_lock(&p->owner->lock);
  p->spots++;
  g_mutex_unlock(&p->owner->lock);
  p->owner->cb(p->spec, call, mode, f, snr, speed, p->owner->user);
}

static void peer_read(Peer *p);

static void read_done(GObject *src, GAsyncResult *res, gpointer data) {
  Peer *p = data;
  GError *err = NULL;
  const gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &err);
  if (n <= 0) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      peer_drop(p, n == 0 ? "closed by the peer" : err->message);
    }
    g_clear_error(&err);
    return;
  }
  g_string_append_len(p->rx, p->buf, n);
  char *nl;
  while ((nl = strchr(p->rx->str, '\n')) != NULL) {
    *nl = '\0';
    char *line = g_strdup(p->rx->str);
    g_string_erase(p->rx, 0, (gssize)(nl - p->rx->str) + 1);
    if (!p->logged_in) {
      char *low = g_ascii_strdown(line, -1);
      if (strstr(low, "call") && strchr(low, ':')) { peer_login(p); }
      g_free(low);
    } else {
      peer_line(p, line);
    }
    g_free(line);
  }
  /* the login prompt has no line end: "Please enter your call: " */
  if (!p->logged_in && p->rx->len) {
    char *low = g_ascii_strdown(p->rx->str, -1);
    if (strstr(low, "call") && strrchr(low, ':')) {
      g_string_truncate(p->rx, 0);
      peer_login(p);
    }
    g_free(low);
  }
  if (p->rx->len > RX_MAX) { g_string_truncate(p->rx, 0); }
  peer_read(p);
}

static void peer_read(Peer *p) {
  GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(p->conn));
  g_input_stream_read_async(in, p->buf, sizeof(p->buf), G_PRIORITY_DEFAULT, p->cancel,
                            read_done, p);
}

static void connect_done(GObject *src, GAsyncResult *res, gpointer data) {
  Peer *p = data;
  GError *err = NULL;
  GSocketConnection *c = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, &err);
  g_object_unref(src);
  if (!c) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { peer_drop(p, err->message); }
    g_clear_error(&err);
    return;
  }
  p->conn = c;
  /* the client's timeout was for the connect: a feed may stay quiet for long */
  GSocket *sock = g_socket_connection_get_socket(c);
  g_socket_set_timeout(sock, 0);
  g_socket_set_keepalive(sock, TRUE);
  set_state(p, TRUE, "connected, logging in");
  GSource *t = g_timeout_source_new(LOGIN_WAIT_MS);
  g_source_set_callback(t, login_timeout, p, NULL);
  p->login_timer = g_source_attach(t, p->owner->ctx);
  g_source_unref(t);
  peer_read(p);
}

static void peer_connect(Peer *p) {
  p->retry_timer = 0;
  set_state(p, FALSE, "connecting to %s:%u", p->host, p->port);
  p->cancel = g_cancellable_new();
  GSocketClient *sc = g_socket_client_new();
  g_socket_client_set_timeout(sc, 10);
  g_socket_client_connect_to_host_async(sc, p->host, p->port, p->cancel, connect_done, p);
}

/* the first connect and every retry */
static gboolean start_cb(gpointer data) {
  peer_connect(data);
  return G_SOURCE_REMOVE;
}

static void peer_free(gpointer data) {
  Peer *p = data;
  g_free(p->spec);
  g_free(p->host);
  g_string_free(p->rx, TRUE);
  g_free(p);
}

/* ---- module ------------------------------------------------------------------------ */

static gpointer peers_thread(gpointer data) {
  SkimHdPeers *s = data;
  g_main_context_push_thread_default(s->ctx);
  g_main_loop_run(s->loop);
  g_main_context_pop_thread_default(s->ctx);
  return NULL;
}

SkimHdPeers *skim_hd_peers_new(const char *const *hosts, const char *login,
                               SkimHdPeerSpot cb, gpointer user) {
  SkimHdPeers *s = g_new0(SkimHdPeers, 1);
  s->ctx = g_main_context_new();
  s->loop = g_main_loop_new(s->ctx, FALSE);
  s->peers = g_ptr_array_new_with_free_func(peer_free);
  s->login = g_strdup(login);
  s->cb = cb;
  s->user = user;
  g_mutex_init(&s->lock);
  for (const char *const *h = hosts; h && *h; h++) {
    if (!**h) { continue; }
    Peer *p = g_new0(Peer, 1);
    p->owner = s;
    p->spec = g_strdup(*h);
    p->rx = g_string_new(NULL);
    const char *colon = strrchr(*h, ':');
    guint64 port = 0;
    if (colon && strchr(*h, ':') == colon && g_ascii_string_to_unsigned(colon + 1, 10, 1, 65535,
                                                                        &port, NULL)) {
      p->host = g_strndup(*h, (gsize)(colon - *h));
      p->port = (guint16)port;
    } else {
      p->host = g_strdup(*h);
      p->port = SKIM_HD_PEER_PORT;
    }
    g_strlcpy(p->state, "starting", sizeof(p->state));
    g_ptr_array_add(s->peers, p);
    GSource *t = g_idle_source_new();
    g_source_set_callback(t, start_cb, p, NULL);
    g_source_attach(t, s->ctx);
    g_source_unref(t);
  }
  s->thread = g_thread_new("skim-peers", peers_thread, s);
  return s;
}

static gboolean stop_cb(gpointer data) {
  SkimHdPeers *s = data;
  for (guint i = 0; i < s->peers->len; i++) {
    Peer *p = s->peers->pdata[i];
    if (p->cancel) { g_cancellable_cancel(p->cancel); }
    timer_stop(s, &p->retry_timer);
    timer_stop(s, &p->login_timer);
    if (p->conn) {
      g_io_stream_close(G_IO_STREAM(p->conn), NULL, NULL);
      g_clear_object(&p->conn);
    }
  }
  g_main_loop_quit(s->loop);
  return G_SOURCE_REMOVE;
}

void skim_hd_peers_free(SkimHdPeers *s) {
  if (!s) { return; }
  g_atomic_int_set(&s->stopping, 1);
  g_main_context_invoke(s->ctx, stop_cb, s);
  g_thread_join(s->thread);
  /* the cancelled operations still hold their callbacks: let them run out */
  while (g_main_context_iteration(s->ctx, FALSE)) {}
  for (guint i = 0; i < s->peers->len; i++) {
    g_clear_object(&((Peer *)s->peers->pdata[i])->cancel);
  }
  g_ptr_array_free(s->peers, TRUE);
  g_main_loop_unref(s->loop);
  g_main_context_unref(s->ctx);
  g_mutex_clear(&s->lock);
  g_free(s->login);
  g_free(s);
}

char *skim_hd_peers_json(SkimHdPeers *s) {
  GString *o = g_string_new("[");
  g_mutex_lock(&s->lock);
  for (guint i = 0; i < s->peers->len; i++) {
    const Peer *p = s->peers->pdata[i];
    g_string_append_printf(o, "%s{\"host\":\"", i ? "," : "");
    skim_json_append(o, p->spec);
    g_string_append(o, "\",\"state\":\"");
    skim_json_append(o, p->state);
    g_string_append_printf(o, "\",\"connected\":%s,\"spots\":%" G_GUINT64_FORMAT
                           ",\"lines\":%" G_GUINT64_FORMAT "}",
                           p->connected ? "true" : "false", p->spots, p->lines);
  }
  g_mutex_unlock(&s->lock);
  g_string_append_c(o, ']');
  return g_string_free(o, FALSE);
}
