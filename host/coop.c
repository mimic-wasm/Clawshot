/* (hookshot) Co-op: the session, the network and the proximity chat. See host/coop.h for what it is and who calls it.

   One UDP socket and one thread. The host binds the port (COOP_DEFAULT_PORT unless the address says another) and is the hub: a
   player sends the host where Link is and what the microphone says, the host sends everyone the whole roster ten times a
   second and relays each voice to the players it can be heard by. Nothing is peer to peer, so a joiner needs only the host's
   address, and only the host needs a port opened on a router (or a LAN, or a VPN such as Tailscale, ZeroTier or Radmin).

   Joining: the joiner asks (INFO), is told whether the session has started, wants a key, or is full, and then joins (JOIN). The
   key is a shared password: the joiner sends a SHA-256 of the host's nonce, its own and the key, never the key. A session that
   has started answers every new JOIN with DENY(started); a player who joined before it started keeps their place (a lost
   answer is sent again for the same address). The session's packets carry a random token the host gave at the join. Nothing
   is encrypted: the positions and the voices cross the network as they are.

   Voice: 48 kHz mono from the default microphone (SDL, host/hos_sdl.c), 20 ms Opus frames (VOIP, 24 kbit/s) sent when the
   player talks (voice activity, or a push-to-talk key), decoded per speaker into a small ring that the audio mixer's thread
   drains into the game's mix (coop_voice_mix) with a gain by distance and a pan by direction. Heard only by those on the same
   map, not in a cutscene, within COOP_VOICE_RANGE; the host does not even relay a voice to those out of range.

   The file touches no game memory, so tests/test_coop.py runs it as it is, with a fake microphone (COOP_TEST). */
#define _GNU_SOURCE
#include "coop.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
typedef CRITICAL_SECTION Mu;
typedef HANDLE Thread;
#define SOCK_NONE INVALID_SOCKET
#define sock_close closesocket
#define mu_init(m) InitializeCriticalSection(m)
#define mu_lock(m) EnterCriticalSection(m)
#define mu_unlock(m) LeaveCriticalSection(m)
extern BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG length);   /* (RtlGenRandom, in advapi32) */
static int64_t now_ms(void) { return (int64_t)GetTickCount64(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
typedef pthread_mutex_t Mu;
typedef pthread_t Thread;
#define SOCK_NONE (-1)
#define sock_close close
#define mu_init(m) pthread_mutex_init(m, NULL)
#define mu_lock(m) pthread_mutex_lock(m)
#define mu_unlock(m) pthread_mutex_unlock(m)
static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#endif

#include <opus.h>
#include "hos_sdl.h"   /* (the microphone: hos_sdl_mic_*) */

void hos_log(const char *fmt, ...);

/* ---------------------------------------------------------------------------------------------------------------------- */
/* the protocol */

#define MAGIC 0x5343u   /* "CS" */
#define VERSION 1
#define MAX_PACKET 1400
#define SNAP_MS 100       /* the host's roster to everyone */
#define STATE_MS 100      /* a client's state to the host */
#define RESEND_MS 500     /* a request not answered yet */
#define JOIN_TIMEOUT_MS 10000
#define CLIENT_TIMEOUT_MS 10000   /* the host not heard from */
#define HOST_TIMEOUT_MS 30000     /* a client not heard from */
#define STALE_MS 3000             /* a position older than this is not in the world */
#define FRAME 960                 /* 20 ms at 48 kHz */
#define RING 24000                /* a speaker's decoded voice kept: half a second */
#define PREBUFFER 2400            /* 50 ms before a voice starts playing */
#define HANGOVER_FRAMES 15        /* voice activity stays on 300 ms after the last loud frame */
#define ACTIVITY_PEAK 700         /* (16-bit peak: about -27 dBFS; the room's noise is below) */
#define OPUS_MAX 400

enum { T_INFO_REQ = 1, T_INFO, T_JOIN, T_JOIN_OK, T_DENY, T_STATE, T_SNAP, T_VOICE, T_VOICE_RELAY, T_LEAVE, T_CLOSE };
enum { DENY_STARTED = 1, DENY_KEY, DENY_FULL, DENY_VERSION };
enum { F_VALID = 1, F_CHILD = 2, F_EVENT = 4, F_SPEAKING = 8, F_HOST = 16 };

typedef struct { uint8_t b[MAX_PACKET]; int n; } Pkt;
static void p_init(Pkt *p, int type) {
    p->n = 0;
    p->b[p->n++] = MAGIC & 255, p->b[p->n++] = MAGIC >> 8, p->b[p->n++] = VERSION, p->b[p->n++] = (uint8_t)type;
}
static void p_u8(Pkt *p, unsigned v) { if (p->n < MAX_PACKET) p->b[p->n++] = (uint8_t)v; }
static void p_u16(Pkt *p, unsigned v) { p_u8(p, v & 255), p_u8(p, v >> 8); }
static void p_u32(Pkt *p, uint32_t v) { p_u16(p, v & 0xffff), p_u16(p, v >> 16); }
static void p_f32(Pkt *p, float f) { uint32_t u; memcpy(&u, &f, 4); p_u32(p, u); }
static void p_bytes(Pkt *p, const void *d, int n) { if (p->n + n <= MAX_PACKET) memcpy(p->b + p->n, d, (size_t)n), p->n += n; }
static void p_str(Pkt *p, const char *s, int n) {   /* (n bytes, the end padded with zeros) */
    char t[64] = {0};
    snprintf(t, (size_t)n < sizeof t ? (size_t)n : sizeof t, "%s", s);
    p_bytes(p, t, n);
}
typedef struct { const uint8_t *b; int n, at, bad; } Rd;
static uint8_t r_u8(Rd *r) { if (r->at >= r->n) return r->bad = 1, 0; return r->b[r->at++]; }
static unsigned r_u16(Rd *r) { const unsigned a = r_u8(r); return a | r_u8(r) << 8; }
static uint32_t r_u32(Rd *r) { const uint32_t a = r_u16(r); return a | (uint32_t)r_u16(r) << 16; }
static float r_f32(Rd *r) { const uint32_t u = r_u32(r); float f; memcpy(&f, &u, 4); return f; }
static void r_bytes(Rd *r, void *d, int n) {
    if (r->at + n > r->n) { r->bad = 1, memset(d, 0, (size_t)n); return; }
    memcpy(d, r->b + r->at, (size_t)n), r->at += n;
}
static void r_str(Rd *r, char *out, int n) {   /* (n bytes on the wire, out has n + 1; printable ASCII and UTF-8 only) */
    r_bytes(r, out, n);
    out[n] = 0;
    for (int i = 0; i < n && out[i]; i++)
        if ((unsigned char)out[i] < 32 || out[i] == 127) out[i] = '?';
}

/* ---- SHA-256, for the key's proof ---- */
typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t len; int fill; } Sha;
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}
static void sha_init(Sha *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv), s->len = 0, s->fill = 0;
}
static void sha_update(Sha *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        const size_t take = (size_t)(64 - s->fill) < n ? (size_t)(64 - s->fill) : n;
        memcpy(s->buf + s->fill, p, take), s->fill += (int)take, p += take, n -= take;
        if (s->fill == 64) sha_block(s, s->buf), s->fill = 0;
    }
}
static void sha_final(Sha *s, uint8_t out[32]) {
    const uint64_t bits = s->len * 8;
    const uint8_t pad = 0x80, zero = 0;
    sha_update(s, &pad, 1);
    while (s->fill != 56) sha_update(s, &zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, l, 8);
    for (int i = 0; i < 8; i++)
        out[i * 4] = (uint8_t)(s->h[i] >> 24), out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16), out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8),
        out[i * 4 + 3] = (uint8_t)s->h[i];
}
/* what a joiner sends for the key: SHA-256(host nonce, its own, the key) */
static void key_proof(const uint8_t host_nonce[16], const uint8_t my_nonce[8], const char *key, uint8_t out[32]) {
    Sha s;
    sha_init(&s);
    sha_update(&s, host_nonce, 16), sha_update(&s, my_nonce, 8), sha_update(&s, key, strlen(key));
    sha_final(&s, out);
}

static void random_bytes(void *out, size_t n) {
#ifdef _WIN32
    if (SystemFunction036(out, (ULONG)n)) return;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        const size_t got = fread(out, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
#endif
    uint8_t *p = out;   /* (no source of it: the clock, only so a token is not zeros) */
    uint64_t x = (uint64_t)now_ms() * 0x9e3779b97f4a7c15ull ^ (uint64_t)(uintptr_t)out;
    for (size_t i = 0; i < n; i++) x ^= x << 13, x ^= x >> 7, x ^= x << 17, p[i] = (uint8_t)x;
}

/* ---------------------------------------------------------------------------------------------------------------------- */
/* the state */

typedef struct {
    int used;
    char name[COOP_NAME_MAX];
    int valid, child, event, speaking;
    char map[COOP_MAP_MAX];
    float pos[3], yaw;
    int64_t state_at, speak_until;
} Player;

typedef struct {   /* (the host's record of a client: the address and token the roster entry of the same id goes with) */
    struct sockaddr_in addr;
    uint8_t token[8];
    int64_t last_seen;
} Client;

typedef struct {   /* (a speaker's voice, decoded, waiting for the mixer) */
    OpusDecoder *dec;
    int16_t ring[RING];
    int head, fill, primed, have_seq;
    uint16_t last_seq;
} Speaker;

static struct {
    Mu mu;
    int inited;
    /* the player's choices */
    char name[COOP_NAME_MAX], key[COOP_KEY_MAX], address[64];
    int key_on, voice, port;
    char prefs_path[512];
    int (*ptt_probe)(void);
    /* the session */
    int mode, started, self_id, needs_key;
    char message[112], lobby_address[64];
    Player roster[MKWVK_COOP_PLAYERS];
    Client clients[MKWVK_COOP_PLAYERS];
    /* the game's side */
    CoopLocal local;
    int64_t local_at;
    double eye[3], target[3];
    int64_t camera_at;
    float fovy;
    int64_t fov_at;
    /* the thread */
    Thread thread;
    int thread_made;
    volatile int stop;
    volatile int started_flag;   /* (coop_active, without the lock) */
    volatile int fov_wanted;
    sock_t sock;
    /* joining */
    char join_host[64];
    int join_port;
    /* voice */
    Speaker speakers[MKWVK_COOP_PLAYERS];
    int speaking_self;
    int64_t self_speak_until;
} G;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void coop_ctor(void) { mu_init(&G.mu); }   /* (before any thread of the game can call in) */
#endif

static void say(const char *fmt, ...) {   /* (the message, under the lock) */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(G.message, sizeof G.message, fmt, ap);
    va_end(ap);
}

static void clean_name(char *out, const char *in) {   /* (printable ASCII; at most COOP_NAME_MAX - 1 bytes; "Link" when nothing is left) */
    int n = 0;
    for (; in && *in && n < COOP_NAME_MAX - 1; in++)
        if ((unsigned char)*in >= 32 && (unsigned char)*in < 127) out[n++] = *in;
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
    if (!n) snprintf(out, COOP_NAME_MAX, "Link");
}

/* ---- the player's choices, kept between runs (never the key) ---- */
static void prefs_save(void) {
    if (!G.prefs_path[0]) return;
    FILE *f = fopen(G.prefs_path, "wb");
    if (!f) return;
    fprintf(f, "# Co-op choices (the Insert menu's Gameplay tab, Co-op). The key is never kept.\nname=%s\naddress=%s\nkey_on=%d\nvoice=%d\n",
            G.name, G.address, G.key_on, G.voice);
    fclose(f);
}
static void prefs_load(void) {
    if (!G.prefs_path[0]) return;
    FILE *f = fopen(G.prefs_path, "rb");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq++ = 0;
        eq[strcspn(eq, "\r\n")] = 0;
        if (!strcmp(line, "name")) clean_name(G.name, eq);
        else if (!strcmp(line, "address")) snprintf(G.address, sizeof G.address, "%s", eq);
        else if (!strcmp(line, "key_on")) G.key_on = atoi(eq) != 0;
        else if (!strcmp(line, "voice")) G.voice = atoi(eq) < 0 || atoi(eq) > MKWVK_COOP_VOICE_PUSH ? 0 : atoi(eq);
    }
    fclose(f);
}

static void init_once(void) {   /* (G.mu held, or the first call) */
    if (G.inited) return;
    G.inited = 1;
    G.sock = SOCK_NONE;
    G.port = COOP_DEFAULT_PORT;
    G.voice = MKWVK_COOP_VOICE_ACTIVITY;
    const char *user = getenv("USERNAME");
    if (!user || !*user) user = getenv("USER");
    clean_name(G.name, user && *user ? user : "Link");
    prefs_load();
}

/* ---------------------------------------------------------------------------------------------------------------------- */
/* distance, hearing, the picture */

float coop_voice_gain(float d) {
    if (!(d < COOP_VOICE_RANGE)) return 0.0f;
    if (d <= COOP_VOICE_REF) return 1.0f;
    const float k = (COOP_VOICE_RANGE - d) / (COOP_VOICE_RANGE - COOP_VOICE_REF);
    return k * k;
}
static float dist3(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}
/* A player's place now: in a world, seen lately, and not in a cutscene (G.mu held) */
static int in_world(const Player *p, int64_t now) { return p->used && p->valid && !p->event && now - p->state_at < STALE_MS; }
/* Whether dst hears src (G.mu held): both in the world, on the same map, within a little more than the range (the margin: the
   positions are a tenth of a second old) */
static int hears(const Player *src, const Player *dst, int64_t now, float *distance) {
    if (!G.started || !in_world(src, now) || !in_world(dst, now) || strcmp(src->map, dst->map)) return 0;
    const float d = dist3(src->pos, dst->pos);
    if (distance) *distance = d;
    return d < COOP_VOICE_RANGE * 1.1f;
}

/* The local player's entry (own state kept apart: the game's thread writes it) as the roster would hold it (G.mu held) */
static void own_player(Player *p, int64_t now) {
    memset(p, 0, sizeof *p);
    p->used = 1;
    snprintf(p->name, sizeof p->name, "%s", G.name);
    if (G.self_id >= 0 && G.self_id < MKWVK_COOP_PLAYERS && G.roster[G.self_id].used) snprintf(p->name, sizeof p->name, "%s", G.roster[G.self_id].name);
    p->valid = G.local.valid && now - G.local_at < STALE_MS;
    p->child = G.local.child, p->event = G.local.event;
    snprintf(p->map, sizeof p->map, "%s", G.local.map);
    memcpy(p->pos, G.local.pos, sizeof p->pos);
    p->yaw = G.local.yaw;
    p->state_at = now;
    p->speaking = G.speaking_self;
}

int coop_marks(MkwVkCoopMark *out, int max, float aspect) {
    int n = 0;
    mu_lock(&G.mu);
    const int64_t now = now_ms();
    Player me;
    own_player(&me, now);
    if (G.mode >= MKWVK_COOP_HOSTING && G.started && in_world(&me, now) && now - G.camera_at < 1000 && aspect > 0.2f) {
        /* the camera's basis: forward to the point it looks at; right = forward x (0, 1, 0), as the free camera's rows have it
           (host/hos_main.c oot_build_free_view); up = right x forward. A camera looking straight up or down has no right */
        double f[3], r[3], u[3];
        for (int i = 0; i < 3; i++) f[i] = G.target[i] - G.eye[i];
        double len = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        const double flat = sqrt(f[0] * f[0] + f[2] * f[2]);
        if (len > 1e-6 && flat > 1e-6 * len) {
            for (int i = 0; i < 3; i++) f[i] /= len;
            r[0] = -f[2] / (flat / len), r[1] = 0, r[2] = f[0] / (flat / len);   /* (f is unit now: flat / len is its flat part) */
            u[0] = r[1] * f[2] - r[2] * f[1], u[1] = r[2] * f[0] - r[0] * f[2], u[2] = r[0] * f[1] - r[1] * f[0];
            const double fovy = G.fovy > 0.1f && G.fovy < 3.0f && now - G.fov_at < 5000 ? G.fovy : 1.0;
            const double t = tan(fovy * 0.5);
            for (int i = 0; i < MKWVK_COOP_PLAYERS && n < max; i++) {
                const Player *p = &G.roster[i];
                if (i == G.self_id || !in_world(p, now) || strcmp(p->map, me.map)) continue;
                const double head = p->child ? 1.2 : 1.8;   /* (the name over the head: Link's height, in metres) */
                const double d[3] = {p->pos[0] - G.eye[0], p->pos[1] + head - G.eye[1], p->pos[2] - G.eye[2]};
                const double x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2], y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2],
                             z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
                MkwVkCoopMark *m = &out[n];
                memset(m, 0, sizeof *m);
                snprintf(m->name, sizeof m->name, "%s", p->name);
                m->distance = dist3(p->pos, me.pos);
                m->speaking = (uint8_t)(p->speaking || now < p->speak_until);
                double nx = 0, ny = 0;
                int off = z <= 0.05;
                if (!off) {
                    nx = x / (z * t * aspect), ny = y / (z * t);
                    off = fabs(nx) > 1.0 || fabs(ny) > 1.0;
                }
                if (off) {   /* (on the edge, in the direction it lies: one behind the camera on the side he is to) */
                    double dx = z <= 0.05 ? x : nx, dy = z <= 0.05 ? y : ny;
                    const double l = sqrt(dx * dx + dy * dy);
                    if (l < 1e-6) dx = 0, dy = -1; else dx /= l, dy /= l;
                    const double k = fmin(fabs(dx) > 1e-6 ? 0.9 / fabs(dx) : 1e9, fabs(dy) > 1e-6 ? 0.9 / fabs(dy) : 1e9);
                    nx = dx * k, ny = dy * k;
                    m->offscreen = 1;
                }
                m->x = (float)(0.5 + 0.5 * nx), m->y = (float)(0.5 - 0.5 * ny);
                n++;
            }
        }
    }
    mu_unlock(&G.mu);
    return n;
}

/* ---------------------------------------------------------------------------------------------------------------------- */
/* what the game gives */

void coop_set_local(const CoopLocal *l) {
    mu_lock(&G.mu);
    G.local = *l;
    G.local.map[COOP_MAP_MAX - 1] = 0;
    G.local_at = now_ms();
    mu_unlock(&G.mu);
}
void coop_set_camera(const double eye[3], const double target[3]) {
    mu_lock(&G.mu);
    memcpy(G.eye, eye, sizeof G.eye), memcpy(G.target, target, sizeof G.target);
    G.camera_at = now_ms();
    mu_unlock(&G.mu);
}
int coop_wants_fov(void) { return G.fov_wanted; }
void coop_note_fov(float fovy) {
    mu_lock(&G.mu);
    G.fovy = fovy, G.fov_at = now_ms();
    mu_unlock(&G.mu);
}
int coop_active(void) { return G.started_flag; }
void coop_set_prefs_path(const char *path) {
    mu_lock(&G.mu);
    snprintf(G.prefs_path, sizeof G.prefs_path, "%s", path ? path : "");
    G.inited = 0;
    init_once();
    mu_unlock(&G.mu);
}
void coop_set_ptt_probe(int (*probe)(void)) { G.ptt_probe = probe; }

/* ---------------------------------------------------------------------------------------------------------------------- */
/* voice */

static void speaker_free(Speaker *s) {
    if (s->dec) opus_decoder_destroy(s->dec);
    memset(s, 0, sizeof *s);
}
/* A voice packet from player id, decoded into its ring (G.mu held) */
static void voice_in(int id, unsigned seq, const uint8_t *data, int len) {
    if (id < 0 || id >= MKWVK_COOP_PLAYERS || id == G.self_id || len <= 0 || len > OPUS_MAX) return;
    Speaker *s = &G.speakers[id];
    if (!s->dec) {
        int err = 0;
        s->dec = opus_decoder_create(48000, 1, &err);
        if (!s->dec) return;
    }
    int16_t pcm[FRAME];
    const int gap = s->have_seq ? (int)(uint16_t)(seq - s->last_seq) : 1;
    if (s->have_seq && (gap == 0 || gap > 32768)) return;   /* (a repeat, or older than one played already) */
    if (gap > 8) opus_decoder_ctl(s->dec, OPUS_RESET_STATE);   /* (a new burst of speech) */
    int lost = gap == 2 || gap == 3 ? gap - 1 : 0;           /* (a frame or two missing: made up) */
    while (lost-- > 0) {
        const int n = opus_decode(s->dec, NULL, 0, pcm, FRAME, 0);
        for (int i = 0; i < n; i++) {
            const int at = (s->head + s->fill) % RING;
            s->ring[at] = pcm[i];
            if (s->fill < RING) s->fill++; else s->head = (s->head + 1) % RING;
        }
    }
    const int n = opus_decode(s->dec, data, len, pcm, FRAME, 0);
    if (n <= 0) return;
    s->have_seq = 1, s->last_seq = (uint16_t)seq;
    for (int i = 0; i < n; i++) {
        const int at = (s->head + s->fill) % RING;
        s->ring[at] = pcm[i];
        if (s->fill < RING) s->fill++; else s->head = (s->head + 1) % RING;
    }
    if (s->fill >= PREBUFFER) s->primed = 1;
    G.roster[id].speak_until = now_ms() + 350;
}

void coop_voice_mix(int16_t *pcm, int channels, int frames) {
    if (!G.started_flag || (channels != 2 && channels != 6) || frames <= 0) return;
    mu_lock(&G.mu);
    const int64_t now = now_ms();
    Player me;
    own_player(&me, now);
    /* the listener's right, from the camera: the direction a voice comes from against it */
    double rx = 1, rz = 0;
    int have_dir = 0;
    if (now - G.camera_at < 1000) {
        const double fx = G.target[0] - G.eye[0], fz = G.target[2] - G.eye[2], l = sqrt(fx * fx + fz * fz);
        if (l > 1e-6) rx = -fz / l, rz = fx / l, have_dir = 1;   /* (forward x up: the same right as coop_marks) */
    }
    for (int id = 0; id < MKWVK_COOP_PLAYERS; id++) {
        Speaker *s = &G.speakers[id];
        if (!s->dec || id == G.self_id) continue;
        if (s->fill == 0) {   /* (it ran dry: it waits for the prebuffer again) */
            s->primed = 0;
            continue;
        }
        if (!s->primed) {
            if (s->fill < PREBUFFER) continue;
            s->primed = 1;
        }
        if (s->fill < frames) continue;
        float distance = 0;
        const float gain = hears(&G.roster[id], &me, now, &distance) ? coop_voice_gain(distance) : 0.0f;
        float left = gain, right = gain;
        if (gain > 0 && have_dir && distance > 1.0f) {   /* (a pan by the side it lies on, never fully to one ear) */
            const double dx = G.roster[id].pos[0] - me.pos[0], dz = G.roster[id].pos[2] - me.pos[2], l = sqrt(dx * dx + dz * dz);
            const double side = l > 1e-6 ? (dx * rx + dz * rz) / l : 0;
            const double p = side * 0.7;
            left = gain * (float)sqrt((1 - p) * 0.5) * 1.4142f, right = gain * (float)sqrt((1 + p) * 0.5) * 1.4142f;
        }
        for (int i = 0; i < frames; i++) {
            const int16_t v = s->ring[(s->head + i) % RING];
            if (left > 0 || right > 0) {
                int l = pcm[i * channels] + (int)(v * left), r = pcm[i * channels + 1] + (int)(v * right);
                pcm[i * channels] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                pcm[i * channels + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            }
        }
        s->head = (s->head + frames) % RING, s->fill -= frames;
    }
    mu_unlock(&G.mu);
}

/* The microphone and the encoder, on the net thread */
typedef struct {
    void *mic;
    OpusEncoder *enc;
    int16_t accum[FRAME];
    int have;
    int hang, talking;
    uint16_t seq;
    int64_t retry_at;
} Capture;

static void capture_close(Capture *c) {
    if (c->mic) hos_sdl_mic_close(c->mic);
    if (c->enc) opus_encoder_destroy(c->enc);
    memset(c, 0, sizeof *c);
}

static int ptt_down(void) { return G.ptt_probe ? G.ptt_probe() : 0; }

typedef void (*VoiceSink)(const uint8_t *opus, int len, uint16_t seq);

/* Reads what the microphone has and hands each 20 ms of speech, encoded, to send; mode: MKWVK_COOP_VOICE_* */
static void capture_tick(Capture *c, int mode, int64_t now, VoiceSink send) {
    if (mode == MKWVK_COOP_VOICE_OFF) {
        if (c->mic || c->enc) capture_close(c);
        return;
    }
    if (!c->mic) {
        if (now < c->retry_at) return;
        c->mic = hos_sdl_mic_open(0, 48000, 1, 0x8010);
        if (!c->mic) {
            c->retry_at = now + 3000;
            return;
        }
        int err = 0;
        c->enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
        if (!c->enc) {
            capture_close(c);
            c->retry_at = now + 3000;
            return;
        }
        opus_encoder_ctl(c->enc, OPUS_SET_BITRATE(24000));
        opus_encoder_ctl(c->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(c->enc, OPUS_SET_INBAND_FEC(0));
        hos_log("coop: microphone open for proximity chat (%s)", mode == MKWVK_COOP_VOICE_PUSH ? "push to talk" : "voice activity");
    }
    for (int guard = 0; guard < 16; guard++) {
        const int avail = hos_sdl_mic_available(c->mic);
        if (avail < 2) break;
        const int want = (FRAME - c->have) * 2;
        const int got = hos_sdl_mic_read(c->mic, (uint8_t *)c->accum + c->have * 2, avail < want ? avail : want);
        if (got <= 0) break;
        c->have += got / 2;
        if (c->have < FRAME) continue;
        c->have = 0;
        int peak = 0;
        for (int i = 0; i < FRAME; i++) {
            const int a = c->accum[i] < 0 ? -c->accum[i] : c->accum[i];
            if (a > peak) peak = a;
        }
        int talk;
        if (mode == MKWVK_COOP_VOICE_PUSH) {
            talk = ptt_down();
            c->hang = talk ? 3 : c->hang > 0 ? c->hang - 1 : 0, talk = talk || c->hang > 0;
        } else {
            if (peak >= ACTIVITY_PEAK) c->hang = HANGOVER_FRAMES;
            else if (c->hang > 0) c->hang--;
            talk = c->hang > 0;
        }
        if (talk) {
            uint8_t packet[OPUS_MAX];
            const int n = opus_encode(c->enc, c->accum, FRAME, packet, sizeof packet);
            if (n > 0) send(packet, n, c->seq++);
        }
        c->talking = talk;
    }
}

/* ---------------------------------------------------------------------------------------------------------------------- */
/* the network */

static int addr_same(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static void send_to(const Pkt *p, const struct sockaddr_in *to) {
    if (G.sock != SOCK_NONE) sendto(G.sock, (const char *)p->b, p->n, 0, (const struct sockaddr *)to, sizeof *to);
}

static sock_t open_socket(int port) {   /* (non-blocking UDP, bound to port when it is not 0) */
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_NONE) return s;
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL off = FALSE;   /* (a datagram to a port nobody listens on is answered by an ICMP error: Windows would end the next receive with it) */
    DWORD bytes = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (port) {
        const int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET, a.sin_addr.s_addr = htonl(INADDR_ANY), a.sin_port = htons((uint16_t)port);
        if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
            sock_close(s);
            return SOCK_NONE;
        }
    }
    return s;
}

/* This PC's address on its network, for the host to give to friends (a UDP socket "connected" sends nothing; the system
   says which address it would have used) */
static void local_address(char *out, size_t cap, int port) {
    snprintf(out, cap, "127.0.0.1:%d", port);
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_NONE) return;
    struct sockaddr_in far_away, mine;
    memset(&far_away, 0, sizeof far_away);
    far_away.sin_family = AF_INET, far_away.sin_port = htons(9), far_away.sin_addr.s_addr = htonl(0xC0000201u);   /* 192.0.2.1 (TEST-NET-1) */
#ifdef _WIN32
    int len = sizeof mine;
#else
    socklen_t len = sizeof mine;
#endif
    if (connect(s, (struct sockaddr *)&far_away, sizeof far_away) == 0 && getsockname(s, (struct sockaddr *)&mine, &len) == 0 &&
        mine.sin_addr.s_addr != 0) {
        char ip[64];
        if (inet_ntop(AF_INET, &mine.sin_addr, ip, sizeof ip)) snprintf(out, cap, "%s:%d", ip, port);
    }
    sock_close(s);
}

/* host or host:port, the port a number; the default port when it has none. 0 when it does not parse */
static int split_address(const char *text, char *host, size_t cap, int *port) {
    char t[96];
    snprintf(t, sizeof t, "%s", text);
    for (char *p = t; *p == ' '; ) memmove(p, p + 1, strlen(p));   /* (leading spaces) */
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') t[--n] = 0;
    if (!n) return 0;
    *port = COOP_DEFAULT_PORT;
    char *colon = strrchr(t, ':');
    if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
        const long v = atol(colon + 1);
        if (v < 1 || v > 65535) return 0;
        *port = (int)v;
        *colon = 0;
    }
    if (!t[0] || strlen(t) >= cap) return 0;
    snprintf(host, cap, "%s", t);
    return 1;
}

/* ---- the host's side ---- */

static int free_id(void) {   /* (G.mu held) */
    for (int i = 1; i < MKWVK_COOP_PLAYERS; i++)
        if (!G.roster[i].used) return i;
    return -1;
}
static int player_count(void) {
    int n = 0;
    for (int i = 0; i < MKWVK_COOP_PLAYERS; i++) n += G.roster[i].used;
    return n;
}
static void unique_name(char *name, int self) {   /* (a second "Link" is "Link 2") */
    char base[COOP_NAME_MAX];
    snprintf(base, sizeof base, "%s", name);
    for (int k = 2; k < 100; k++) {
        int clash = 0;
        for (int i = 0; i < MKWVK_COOP_PLAYERS; i++)
            if (i != self && G.roster[i].used && !strcmp(G.roster[i].name, name)) clash = 1;
        if (!clash) return;
        char suffix[8];
        snprintf(suffix, sizeof suffix, " %d", k);
        snprintf(name, COOP_NAME_MAX, "%.*s%s", COOP_NAME_MAX - 1 - (int)strlen(suffix), base, suffix);
    }
}

static uint8_t host_nonce[16];

static void put_player(Pkt *p, int id, const Player *pl, int64_t now) {
    p_u8(p, id);
    p_u8(p, (pl->valid && now - pl->state_at < STALE_MS ? F_VALID : 0) | (pl->child ? F_CHILD : 0) | (pl->event ? F_EVENT : 0) |
                (pl->speaking || now < pl->speak_until ? F_SPEAKING : 0) | (id == 0 ? F_HOST : 0));
    p_str(p, pl->name, COOP_NAME_MAX);
    p_str(p, pl->map, COOP_MAP_MAX);
    for (int i = 0; i < 3; i++) p_f32(p, pl->pos[i]);
    p_f32(p, pl->yaw);
}

static void host_send_snapshots(int64_t now) {   /* (G.mu held) */
    Pkt p;
    p_init(&p, T_SNAP);
    p_u8(&p, G.started);
    int count = 0;
    const int at = p.n;
    p_u8(&p, 0);
    Player me;
    own_player(&me, now);
    snprintf(G.roster[0].name, COOP_NAME_MAX, "%s", G.name);
    G.roster[0].used = 1, G.roster[0].valid = me.valid, G.roster[0].child = me.child, G.roster[0].event = me.event;
    memcpy(G.roster[0].map, me.map, sizeof me.map), memcpy(G.roster[0].pos, me.pos, sizeof me.pos), G.roster[0].yaw = me.yaw;
    G.roster[0].state_at = now, G.roster[0].speaking = G.speaking_self;
    for (int i = 0; i < MKWVK_COOP_PLAYERS; i++)
        if (G.roster[i].used) put_player(&p, i, &G.roster[i], now), count++;
    p.b[at] = (uint8_t)count;
    for (int i = 1; i < MKWVK_COOP_PLAYERS; i++)
        if (G.roster[i].used) send_to(&p, &G.clients[i].addr);
}

static void host_drop(int id, const char *why) {   /* (G.mu held) */
    if (id <= 0 || id >= MKWVK_COOP_PLAYERS || !G.roster[id].used) return;
    hos_log("coop: %s left (%s)", G.roster[id].name, why);
    memset(&G.roster[id], 0, sizeof G.roster[id]);
    memset(&G.clients[id], 0, sizeof G.clients[id]);
    speaker_free(&G.speakers[id]);
}

static int client_of(const struct sockaddr_in *from, int id, const uint8_t token[8]) {   /* (G.mu held: the roster id, or -1) */
    if (id < 1 || id >= MKWVK_COOP_PLAYERS || !G.roster[id].used) return -1;
    if (!addr_same(from, &G.clients[id].addr) || memcmp(token, G.clients[id].token, 8)) return -1;
    return id;
}

static void send_deny(const struct sockaddr_in *to, int why) {
    Pkt p;
    p_init(&p, T_DENY);
    p_u8(&p, why);
    send_to(&p, to);
}

static void host_voice_out(const uint8_t *opus, int len, uint16_t seq);

static void host_packet(const uint8_t *buf, int n, const struct sockaddr_in *from, int64_t now) {   /* (G.mu held) */
    Rd r = {buf, n, 0, 0};
    if (r_u16(&r) != MAGIC) return;
    const int version = r_u8(&r), type = r_u8(&r);
    if (version != VERSION) {
        if (type == T_INFO_REQ || type == T_JOIN) send_deny(from, DENY_VERSION);
        return;
    }
    if (type == T_INFO_REQ) {
        const uint32_t echo = r_u32(&r);
        if (r.bad) return;
        Pkt p;
        p_init(&p, T_INFO);
        p_u32(&p, echo);
        p_u8(&p, G.started);
        p_u8(&p, G.key_on);
        p_u8(&p, player_count());
        p_u8(&p, MKWVK_COOP_PLAYERS);
        p_bytes(&p, host_nonce, 16);
        p_str(&p, G.name, COOP_NAME_MAX);
        send_to(&p, from);
    } else if (type == T_JOIN) {
        char name[COOP_NAME_MAX + 1];
        uint8_t cnonce[8], proof[32];
        r_str(&r, name, COOP_NAME_MAX);
        r_bytes(&r, cnonce, 8);
        r_bytes(&r, proof, 32);
        if (r.bad) return;
        int id = -1;
        for (int i = 1; i < MKWVK_COOP_PLAYERS; i++)   /* (the answer was lost: the same player asks again, whatever has happened since) */
            if (G.roster[i].used && addr_same(from, &G.clients[i].addr)) id = i;
        if (id < 0) {
            if (G.started) {
                send_deny(from, DENY_STARTED);
                return;
            }
            if (G.key_on) {
                uint8_t want[32];
                key_proof(host_nonce, cnonce, G.key, want);
                uint8_t diff = 0;
                for (int i = 0; i < 32; i++) diff |= want[i] ^ proof[i];
                if (diff) {
                    send_deny(from, DENY_KEY);
                    return;
                }
            }
            id = free_id();
            if (id < 0) {
                send_deny(from, DENY_FULL);
                return;
            }
            memset(&G.roster[id], 0, sizeof G.roster[id]);
            G.roster[id].used = 1;
            char clean[COOP_NAME_MAX];
            clean_name(clean, name);
            unique_name(clean, id);
            snprintf(G.roster[id].name, sizeof G.roster[id].name, "%s", clean);
            G.clients[id].addr = *from;
            random_bytes(G.clients[id].token, 8);
            hos_log("coop: %s joined as player %d", G.roster[id].name, id);
        }
        G.clients[id].last_seen = now;
        Pkt p;
        p_init(&p, T_JOIN_OK);
        p_u8(&p, id);
        p_bytes(&p, G.clients[id].token, 8);
        p_str(&p, G.roster[id].name, COOP_NAME_MAX);
        send_to(&p, from);
    } else if (type == T_STATE || type == T_VOICE || type == T_LEAVE) {
        const int id0 = r_u8(&r);
        uint8_t token[8];
        r_bytes(&r, token, 8);
        const int id = r.bad ? -1 : client_of(from, id0, token);
        if (id < 0) return;
        G.clients[id].last_seen = now;
        Player *pl = &G.roster[id];
        if (type == T_LEAVE) {
            host_drop(id, "left");
        } else if (type == T_STATE) {
            const int flags = r_u8(&r);
            char map[COOP_MAP_MAX + 1];
            r_str(&r, map, COOP_MAP_MAX);
            float pos[3];
            for (int i = 0; i < 3; i++) pos[i] = r_f32(&r);
            const float yaw = r_f32(&r);
            if (r.bad) return;
            pl->valid = (flags & F_VALID) != 0, pl->child = (flags & F_CHILD) != 0, pl->event = (flags & F_EVENT) != 0;
            pl->speaking = (flags & F_SPEAKING) != 0;
            memcpy(pl->map, map, COOP_MAP_MAX), pl->map[COOP_MAP_MAX - 1] = 0;
            for (int i = 0; i < 3; i++) pl->pos[i] = isfinite(pos[i]) ? pos[i] : 0;
            pl->yaw = isfinite(yaw) ? yaw : 0;
            pl->state_at = now;
        } else if (G.started) {   /* (a voice: to the players who can hear it, the host included) */
            const unsigned seq = r_u16(&r);
            const int len = r_u8(&r);
            if (r.bad || len <= 0 || len > OPUS_MAX || r.at + len > r.n) return;
            const uint8_t *opus = buf + r.at;
            pl->speak_until = now + 350;
            Pkt p;
            p_init(&p, T_VOICE_RELAY);
            p_u8(&p, id), p_u16(&p, seq), p_u8(&p, len > 255 ? 255 : len), p_bytes(&p, opus, len);
            for (int to = 1; to < MKWVK_COOP_PLAYERS; to++)
                if (to != id && G.roster[to].used && hears(pl, &G.roster[to], now, NULL)) send_to(&p, &G.clients[to].addr);
            Player me;
            own_player(&me, now);
            if (hears(pl, &me, now, NULL)) voice_in(id, seq, opus, len);
        }
    }
}

static void host_voice_out(const uint8_t *opus, int len, uint16_t seq) {   /* (G.mu held) the host's own voice, to those in range */
    if (!G.started) return;
    const int64_t now = now_ms();
    Player me;
    own_player(&me, now);
    Pkt p;
    p_init(&p, T_VOICE_RELAY);
    p_u8(&p, 0), p_u16(&p, seq), p_u8(&p, len), p_bytes(&p, opus, len);
    for (int to = 1; to < MKWVK_COOP_PLAYERS; to++)
        if (G.roster[to].used && hears(&me, &G.roster[to], now, NULL)) send_to(&p, &G.clients[to].addr);
}

/* ---- a client's side ---- */

static int my_token_set;
static uint8_t my_token[8];
static struct sockaddr_in server;

static void client_voice_out(const uint8_t *opus, int len, uint16_t seq) {   /* (G.mu held) */
    Pkt p;
    p_init(&p, T_VOICE);
    p_u8(&p, G.self_id), p_bytes(&p, my_token, 8), p_u16(&p, seq), p_u8(&p, len), p_bytes(&p, opus, len);
    send_to(&p, &server);
}

static void client_send_state(void) {   /* (G.mu held) */
    const int64_t now = now_ms();
    Player me;
    own_player(&me, now);
    Pkt p;
    p_init(&p, T_STATE);
    p_u8(&p, G.self_id), p_bytes(&p, my_token, 8);
    p_u8(&p, (me.valid ? F_VALID : 0) | (me.child ? F_CHILD : 0) | (me.event ? F_EVENT : 0) | (G.speaking_self ? F_SPEAKING : 0));
    p_str(&p, me.map, COOP_MAP_MAX);
    for (int i = 0; i < 3; i++) p_f32(&p, me.pos[i]);
    p_f32(&p, me.yaw);
    send_to(&p, &server);
}

typedef struct { int phase; uint32_t echo; int64_t began, last_send, last_heard; uint8_t cnonce[8], hnonce[16]; int have_info; } JoinState;

/* One packet from the host (G.mu held). Returns 1 when the session is over: the thread ends */
static int client_packet(JoinState *js, const uint8_t *buf, int n, const struct sockaddr_in *from, int64_t now) {
    if (!addr_same(from, &server)) return 0;
    Rd r = {buf, n, 0, 0};
    if (r_u16(&r) != MAGIC) return 0;
    const int version = r_u8(&r), type = r_u8(&r);
    if (version != VERSION) return 0;
    js->last_heard = now;
    if (type == T_INFO && js->phase == 0) {
        const uint32_t echo = r_u32(&r);
        const int started = r_u8(&r), needs_key = r_u8(&r), count = r_u8(&r), max = r_u8(&r);
        r_bytes(&r, js->hnonce, 16);
        char host_name[COOP_NAME_MAX + 1];
        r_str(&r, host_name, COOP_NAME_MAX);
        if (r.bad || echo != js->echo) return 0;
        G.needs_key = needs_key;
        if (started) return say("%s's session has already started: nobody new can join.", host_name), 1;
        if (needs_key && (!G.key_on || !G.key[0])) return say("%s's session needs a key. Turn Key on and type it, then join again.", host_name), 1;
        if (count >= max) return say("%s's session is full.", host_name), 1;
        js->phase = 1, js->last_send = 0, js->have_info = 1;
    } else if (type == T_JOIN_OK && js->phase == 1) {
        G.self_id = r_u8(&r);
        r_bytes(&r, my_token, 8);
        char name[COOP_NAME_MAX + 1];
        r_str(&r, name, COOP_NAME_MAX);
        if (r.bad || G.self_id < 1 || G.self_id >= MKWVK_COOP_PLAYERS) return 0;
        my_token_set = 1;
        js->phase = 2;
        G.mode = MKWVK_COOP_JOINED;
        say("Connected. Waiting for the host to start the session.");
        hos_log("coop: joined as player %d (%s)", G.self_id, name);
    } else if (type == T_DENY && js->phase < 2) {
        const int why = r_u8(&r);
        if (why == DENY_STARTED) say("The session has already started: nobody new can join.");
        else if (why == DENY_KEY) say("Wrong key.");
        else if (why == DENY_FULL) say("The session is full.");
        else say("The host runs another version of the game.");
        return 1;
    } else if (type == T_SNAP && js->phase == 2) {
        const int started = r_u8(&r), count = r_u8(&r);
        Player incoming[MKWVK_COOP_PLAYERS];
        int seen[MKWVK_COOP_PLAYERS] = {0};
        memset(incoming, 0, sizeof incoming);
        for (int i = 0; i < count && !r.bad; i++) {
            const int id = r_u8(&r), flags = r_u8(&r);
            Player pl;
            memset(&pl, 0, sizeof pl);
            r_str(&r, pl.name, COOP_NAME_MAX);
            r_str(&r, pl.map, COOP_MAP_MAX);
            for (int k = 0; k < 3; k++) pl.pos[k] = r_f32(&r);
            pl.yaw = r_f32(&r);
            if (r.bad || id >= MKWVK_COOP_PLAYERS) break;
            pl.used = 1, pl.valid = (flags & F_VALID) != 0, pl.child = (flags & F_CHILD) != 0, pl.event = (flags & F_EVENT) != 0;
            pl.speaking = (flags & F_SPEAKING) != 0, pl.state_at = now;
            for (int k = 0; k < 3; k++) if (!isfinite(pl.pos[k])) pl.pos[k] = 0;
            if (id != G.self_id) pl.speak_until = G.roster[id].speak_until;
            incoming[id] = pl, seen[id] = 1;
        }
        if (r.bad) return 0;
        for (int id = 0; id < MKWVK_COOP_PLAYERS; id++) {
            if (seen[id]) G.roster[id] = incoming[id];
            else if (G.roster[id].used) memset(&G.roster[id], 0, sizeof G.roster[id]), speaker_free(&G.speakers[id]);
        }
        if (started && !G.started) {
            G.started = 1, G.started_flag = 1, G.fov_wanted = 1;
            say("The session has started.");
            hos_log("coop: the host started the session");
        }
    } else if (type == T_VOICE_RELAY && js->phase == 2 && G.started) {
        const int id = r_u8(&r);
        const unsigned seq = r_u16(&r);
        const int len = r_u8(&r);
        if (r.bad || len <= 0 || r.at + len > r.n) return 0;
        voice_in(id, seq, buf + r.at, len);
    } else if (type == T_CLOSE) {
        say("The host closed the session.");
        return 1;
    }
    return 0;
}

/* ---- the thread ---- */

static void leave_session(void) {   /* (G.mu held: what the session leaves behind) */
    for (int i = 0; i < MKWVK_COOP_PLAYERS; i++) speaker_free(&G.speakers[i]);
    memset(G.roster, 0, sizeof G.roster), memset(G.clients, 0, sizeof G.clients);
    G.mode = MKWVK_COOP_IDLE, G.started = 0, G.started_flag = 0, G.fov_wanted = 0, G.self_id = 0, G.needs_key = 0;
    G.speaking_self = 0, my_token_set = 0;
    G.lobby_address[0] = 0;
}

static void sink_to_host(const uint8_t *o, int n, uint16_t s) { host_voice_out(o, n, s); }
static void sink_to_server(const uint8_t *o, int n, uint16_t s) { client_voice_out(o, n, s); }

#ifdef _WIN32
static DWORD WINAPI net_main(void *arg)
#else
static void *net_main(void *arg)
#endif
{
    const int hosting = arg != NULL;
    Capture cap;
    memset(&cap, 0, sizeof cap);
    JoinState js;
    memset(&js, 0, sizeof js);
    int64_t next_snap = 0, next_state = 0;
    uint8_t buf[MAX_PACKET];
    int ended = 0;

    mu_lock(&G.mu);
    const int64_t began = now_ms();
    if (!hosting) {   /* (the host's name, looked up here: it can take a while) */
        char host[64];
        int port = 0;
        snprintf(host, sizeof host, "%s", G.join_host);
        port = G.join_port;
        mu_unlock(&G.mu);
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
        char service[8];
        snprintf(service, sizeof service, "%d", port);
        const int rc = getaddrinfo(host, service, &hints, &res);
        mu_lock(&G.mu);
        if (rc != 0 || !res) {
            say("Could not find \"%s\". Check the address.", host);
            ended = 1;
        } else {
            memcpy(&server, res->ai_addr, sizeof server);
            freeaddrinfo(res);
            random_bytes(js.cnonce, 8);
            random_bytes(&js.echo, 4);
            js.began = js.last_heard = began;
        }
    }
    while (!G.stop && !ended) {
        mu_unlock(&G.mu);
#ifdef _WIN32
        WSAPOLLFD pfd = {G.sock, POLLRDNORM, 0};
        WSAPoll(&pfd, 1, 5);
#else
        struct pollfd pfd = {G.sock, POLLIN, 0};
        poll(&pfd, 1, 5);
#endif
        mu_lock(&G.mu);
        const int64_t now = now_ms();
        for (int guard = 0; guard < 128; guard++) {
            struct sockaddr_in from;
#ifdef _WIN32
            int fl = sizeof from;
#else
            socklen_t fl = sizeof from;
#endif
            const int got = (int)recvfrom(G.sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (got <= 0) break;
            if (hosting) host_packet(buf, got, &from, now);
            else if (client_packet(&js, buf, got, &from, now)) { ended = 1; break; }
        }
        if (ended) break;
        if (hosting) {
            if (now >= next_snap) next_snap = now + SNAP_MS, host_send_snapshots(now);
            for (int i = 1; i < MKWVK_COOP_PLAYERS; i++)
                if (G.roster[i].used && now - G.clients[i].last_seen > HOST_TIMEOUT_MS) host_drop(i, "no answer");
        } else if (js.phase < 2) {   /* (asking, then joining: each sent again until it is answered) */
            if (now - js.began > JOIN_TIMEOUT_MS) {
                say("The host did not answer. Check the address, and that its port is open.");
                ended = 1;
                break;
            }
            if (now - js.last_send >= RESEND_MS) {
                js.last_send = now;
                Pkt p;
                if (js.phase == 0) {
                    p_init(&p, T_INFO_REQ);
                    p_u32(&p, js.echo);
                } else {
                    p_init(&p, T_JOIN);
                    p_str(&p, G.name, COOP_NAME_MAX);
                    p_bytes(&p, js.cnonce, 8);
                    uint8_t proof[32] = {0};
                    if (G.key_on) key_proof(js.hnonce, js.cnonce, G.key, proof);
                    p_bytes(&p, proof, 32);
                }
                send_to(&p, &server);
            }
        } else {
            if (now >= next_state) next_state = now + STATE_MS, client_send_state();
            if (now - js.last_heard > CLIENT_TIMEOUT_MS) {
                say("Lost the connection to the host.");
                ended = 1;
                break;
            }
        }
        /* the voice: only in a session that has started */
        if (G.started && (!hosting ? js.phase == 2 : 1)) {
            const int before = cap.talking;
            capture_tick(&cap, G.voice, now, hosting ? sink_to_host : sink_to_server);
            G.speaking_self = cap.talking;
            if (cap.talking) G.self_speak_until = now + 350;
            (void)before;
        } else if (cap.mic || cap.enc) {
            capture_close(&cap);
            G.speaking_self = 0;
        }
    }
    /* the end: the others told (a few times: a datagram can be lost), the socket closed */
    if (hosting) {
        Pkt p;
        p_init(&p, T_CLOSE);
        for (int k = 0; k < 3; k++)
            for (int i = 1; i < MKWVK_COOP_PLAYERS; i++)
                if (G.roster[i].used) send_to(&p, &G.clients[i].addr);
    } else if (my_token_set) {
        Pkt p;
        p_init(&p, T_LEAVE);
        p_u8(&p, G.self_id), p_bytes(&p, my_token, 8);
        for (int k = 0; k < 3; k++) send_to(&p, &server);
    }
    capture_close(&cap);
    if (G.sock != SOCK_NONE) sock_close(G.sock), G.sock = SOCK_NONE;
    const int asked = G.stop;
    leave_session();
    if (asked && G.message[0] == 0) say("Left the session.");
    mu_unlock(&G.mu);
    hos_log("coop: the session ended");
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void join_thread(void) {   /* (not under G.mu: the thread takes it to end) */
    if (!G.thread_made) return;
#ifdef _WIN32
    WaitForSingleObject(G.thread, 3000);
    CloseHandle(G.thread);
#else
    pthread_join(G.thread, NULL);
#endif
    G.thread_made = 0;
}

static int start_thread(int hosting) {
#ifdef _WIN32
    G.thread = CreateThread(NULL, 0, net_main, hosting ? (void *)1 : NULL, 0, NULL);
    G.thread_made = G.thread != NULL;
#else
    G.thread_made = pthread_create(&G.thread, NULL, net_main, hosting ? (void *)1 : NULL) == 0;
#endif
    return G.thread_made;
}

static int net_up(void) {
#ifdef _WIN32
    static int up;
    if (!up) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return 0;
        up = 1;
    }
#endif
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------------- */
/* the menu's side */

static void stop_session(void) {   /* (not under the lock) */
    mu_lock(&G.mu);
    const int running = G.mode != MKWVK_COOP_IDLE;
    G.stop = 1;
    if (running) G.message[0] = 0;
    mu_unlock(&G.mu);
    join_thread();
    mu_lock(&G.mu);
    G.stop = 0;
    mu_unlock(&G.mu);
}

int coop_command(int command, const char *text, int value) {
    int ok = 1;
    if (command == MKWVK_COOP_LEAVE) {
        stop_session();
        return 1;
    }
    if (command == MKWVK_COOP_HOST || command == MKWVK_COOP_JOIN) {
        mu_lock(&G.mu);
        const int busy = G.mode != MKWVK_COOP_IDLE;
        mu_unlock(&G.mu);
        if (busy) return 0;
        join_thread();   /* (one that ended on its own) */
    }
    mu_lock(&G.mu);
    init_once();
    switch (command) {
    case MKWVK_COOP_NAME: clean_name(G.name, text), prefs_save(); break;
    case MKWVK_COOP_KEY: snprintf(G.key, sizeof G.key, "%s", text ? text : ""); break;
    case MKWVK_COOP_KEY_ON: G.key_on = value != 0, prefs_save(); break;
    case MKWVK_COOP_ADDRESS: snprintf(G.address, sizeof G.address, "%s", text ? text : ""), prefs_save(); break;
    case MKWVK_COOP_VOICE: G.voice = value < 0 || value > MKWVK_COOP_VOICE_PUSH ? 0 : value, prefs_save(); break;
    case MKWVK_COOP_START:
        if (G.mode == MKWVK_COOP_HOSTING && !G.started) {
            G.started = 1, G.started_flag = 1, G.fov_wanted = 1;
            say("Started. Nobody new can join now.");
            hos_log("coop: started with %d players", player_count());
        } else ok = 0;
        break;
    case MKWVK_COOP_HOST:
    case MKWVK_COOP_JOIN: {
        const int hosting = command == MKWVK_COOP_HOST;
        G.message[0] = 0;
        if (!net_up()) { say("The network could not start."); ok = 0; break; }
        static int exit_hooked;
        if (!exit_hooked) exit_hooked = 1, atexit(coop_shutdown);   /* (the others told when the game ends) */
        if (G.key_on && !G.key[0]) { say("Type a key, or turn Key off."); ok = 0; break; }
        if (hosting) {
            const char *env = getenv("HOS_COOP_PORT");   /* (another port than the default: a router's forward, a second copy on one PC) */
            G.port = env && atoi(env) > 0 && atoi(env) < 65536 ? atoi(env) : COOP_DEFAULT_PORT;
            G.sock = open_socket(G.port);
            if (G.sock == SOCK_NONE) { say("Port %d is not free. Another game or another copy has it.", G.port); ok = 0; break; }
            memset(G.roster, 0, sizeof G.roster), memset(G.clients, 0, sizeof G.clients);
            G.roster[0].used = 1;
            snprintf(G.roster[0].name, COOP_NAME_MAX, "%s", G.name);
            G.self_id = 0, G.started = 0;
            random_bytes(host_nonce, sizeof host_nonce);
            local_address(G.lobby_address, sizeof G.lobby_address, G.port);
            G.mode = MKWVK_COOP_HOSTING;
            say("Hosting. Give your friends the address, then press Start when everyone is in.");
        } else {
            if (!split_address(G.address, G.join_host, sizeof G.join_host, &G.join_port)) { say("Type the host's address first."); ok = 0; break; }
            G.sock = open_socket(0);
            if (G.sock == SOCK_NONE) { say("The network could not start."); ok = 0; break; }
            memset(G.roster, 0, sizeof G.roster), memset(G.clients, 0, sizeof G.clients);
            G.started = 0, G.self_id = 0, my_token_set = 0;
            G.mode = MKWVK_COOP_JOINING;
            say("Joining %s ...", G.address);
        }
        G.stop = 0;
        mu_unlock(&G.mu);
        const int made = start_thread(hosting);
        mu_lock(&G.mu);
        if (!made) {
            if (G.sock != SOCK_NONE) sock_close(G.sock), G.sock = SOCK_NONE;
            leave_session();
            say("The co-op thread could not start.");
            ok = 0;
        } else {
            hos_log("coop: %s", hosting ? "hosting" : "joining");
        }
        break;
    }
    default: ok = 0;
    }
    mu_unlock(&G.mu);
    return ok;
}

void coop_status(MkwVkCoopStatus *out) {
    memset(out, 0, sizeof *out);
    mu_lock(&G.mu);
    init_once();
    const int64_t now = now_ms();
    out->mode = G.mode, out->started = G.started, out->key_on = G.key_on, out->key_set = G.key[0] != 0, out->needs_key = G.needs_key;
    out->voice = G.voice, out->speaking = G.speaking_self, out->port = G.port;
    snprintf(out->message, sizeof out->message, "%s", G.message);
    snprintf(out->name, sizeof out->name, "%s", G.name);
    snprintf(out->address, sizeof out->address, "%s", G.address);
    snprintf(out->lobby_address, sizeof out->lobby_address, "%s", G.lobby_address);
    Player me;
    own_player(&me, now);
    int n = 0;
    if (G.mode == MKWVK_COOP_HOSTING || G.mode == MKWVK_COOP_JOINED) {
        for (int i = 0; i < MKWVK_COOP_PLAYERS; i++) {
            if (!G.roster[i].used) continue;
            const int self = i == G.self_id;
            const Player *p = self ? &me : &G.roster[i];
            MkwVkCoopPlayer *o = &out->players[n++];
            snprintf(o->name, sizeof o->name, "%s", G.roster[i].name);
            snprintf(o->map, sizeof o->map, "%s", p->map);
            o->host = i == 0, o->self = (uint8_t)self;
            o->speaking = (uint8_t)(self ? G.speaking_self : (p->speaking || now < p->speak_until));
            o->in_world = (uint8_t)(self ? in_world(&me, now) : in_world(p, now));
        }
        if (!n) {   /* (joined, and the host's first roster not here yet) */
            MkwVkCoopPlayer *o = &out->players[n++];
            snprintf(o->name, sizeof o->name, "%s", G.name);
            o->self = 1;
        }
    }
    out->count = n;
    mu_unlock(&G.mu);
}

void coop_shutdown(void) {
    if (!G.inited) return;
    stop_session();
}
