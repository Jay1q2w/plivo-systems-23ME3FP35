/*
 * RECEIVER — Jitter buffer + XOR FEC recovery + NACK requests
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from sender, via the hostile relay
 *   send 47020  -> harness player (4-byte BE seq + 160-byte payload)
 *   send 47003  -> feedback to sender, via relay (NACKs)
 *
 * Strategy:
 *   1. Jitter buffer: incoming frames are stored by sequence number and
 *      played out at their deadlines (t0 + delay_s + i*20ms).
 *   2. Duplicate detection: frames already stored are silently dropped.
 *   3. FEC recovery: tracks XOR state per group of K frames. When K-1 data
 *      frames plus the parity have arrived, the missing frame is recovered
 *      via XOR and placed in the jitter buffer.
 *   4. NACKs: when a gap is detected (a higher-seq frame arrives before a
 *      lower one), NACKs are sent for the missing frames. Re-NACKs are sent
 *      as playout deadlines approach.
 *
 * Wire format (sender -> receiver):
 *   DATA:       type(0x01) + seq(4B BE) + payload(160B) = 165 bytes
 *   PARITY:     type(0x02) + group_start_seq(4B BE) + xor_payload(160B) = 165 B
 *   RETRANSMIT: type(0x03) + seq(4B BE) + payload(160B) = 165 bytes
 *
 * Wire format (receiver -> sender):
 *   NACK:       type(0x04) + seq(4B BE) = 5 bytes
 *
 * Env vars: T0, DURATION_S, DELAY_MS.
 * Harness kills this process at run end; a forever-loop is fine.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>

/* ── constants ───────────────────────────────────────────────────────── */

#define PAYLOAD_LEN   160
#define HARNESS_HDR   4
#define HARNESS_PKT   164

#define PKT_DATA       0x01
#define PKT_PARITY     0x02
#define PKT_RETRANSMIT 0x03
#define PKT_NACK       0x04

#define FEC_K          4
#define HDR_LEN        5
#define WIRE_PKT       165
#define NACK_PKT_LEN   5

#define MAX_FRAMES     8192
#define MAX_GROUPS     (MAX_FRAMES / FEC_K + 1)
#define MAX_NACK_PER_FRAME 3  /* max NACK retries per frame */

/* ── jitter buffer ───────────────────────────────────────────────────── */

static uint8_t frame_payload[MAX_FRAMES][PAYLOAD_LEN];
static int     frame_received[MAX_FRAMES];

/* ── FEC group state ─────────────────────────────────────────────────── */

typedef struct {
    uint8_t xor_accum[PAYLOAD_LEN];   /* XOR of all received data payloads */
    uint8_t parity_data[PAYLOAD_LEN]; /* parity payload (if received) */
    int     recv_mask;                /* bitmask of received data indices */
    int     recv_count;               /* count of received data frames */
    int     parity_ok;                /* 1 if parity arrived */
} fec_group_t;

static fec_group_t fec[MAX_GROUPS];

/* ── NACK state ──────────────────────────────────────────────────────── */

static int nack_count[MAX_FRAMES];   /* how many NACKs sent for this frame */

/* ── globals set from env ────────────────────────────────────────────── */

static int    total_frames;
static double t0;
static double delay_s;

/* ── sockets ─────────────────────────────────────────────────────────── */

static int fb_fd;                     /* feedback / NACK socket */
static struct sockaddr_in feedback_addr;

/* ── helpers ─────────────────────────────────────────────────────────── */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void xor_bytes(uint8_t *dst, const uint8_t *src, int len) {
    for (int i = 0; i < len; i++)
        dst[i] ^= src[i];
}

static void send_nack(uint32_t seq) {
    uint8_t pkt[NACK_PKT_LEN];
    pkt[0] = PKT_NACK;
    uint32_t ns = htonl(seq);
    memcpy(pkt + 1, &ns, 4);
    sendto(fb_fd, pkt, NACK_PKT_LEN, 0,
           (struct sockaddr *)&feedback_addr, sizeof feedback_addr);
}

/* ── FEC recovery ────────────────────────────────────────────────────── */

static void try_fec_recovery(int gid) {
    if (gid < 0 || gid >= MAX_GROUPS) return;
    fec_group_t *g = &fec[gid];

    /* need parity + exactly K-1 data frames */
    if (!g->parity_ok)             return;
    if (g->recv_count != FEC_K - 1) return;

    /* find the one missing index */
    int missing_idx = -1;
    for (int i = 0; i < FEC_K; i++) {
        if (!(g->recv_mask & (1 << i))) {
            missing_idx = i;
            break;
        }
    }
    if (missing_idx < 0) return;

    int missing_seq = gid * FEC_K + missing_idx;
    if (missing_seq >= total_frames || missing_seq < 0) return;
    if (frame_received[missing_seq]) return;

    /* recovered = parity XOR accumulated_received */
    uint8_t recovered[PAYLOAD_LEN];
    memcpy(recovered, g->parity_data, PAYLOAD_LEN);
    xor_bytes(recovered, g->xor_accum, PAYLOAD_LEN);

    /* insert into jitter buffer */
    memcpy(frame_payload[missing_seq], recovered, PAYLOAD_LEN);
    frame_received[missing_seq] = 1;

    g->recv_mask |= (1 << missing_idx);
    g->recv_count++;
}

/* ── store a data frame ──────────────────────────────────────────────── */

static void store_data(int seq, const uint8_t *payload) {
    if (seq < 0 || seq >= MAX_FRAMES || seq >= total_frames) return;
    if (frame_received[seq]) return;              /* duplicate */

    memcpy(frame_payload[seq], payload, PAYLOAD_LEN);
    frame_received[seq] = 1;

    /* update FEC group */
    int gid  = seq / FEC_K;
    int fidx = seq % FEC_K;
    if (gid < MAX_GROUPS && !(fec[gid].recv_mask & (1 << fidx))) {
        xor_bytes(fec[gid].xor_accum, payload, PAYLOAD_LEN);
        fec[gid].recv_mask |= (1 << fidx);
        fec[gid].recv_count++;
        try_fec_recovery(gid);
    }
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {

    /* --- parse environment --- */
    const char *t0_str  = getenv("T0");
    const char *dur_str = getenv("DURATION_S");
    const char *dms_str = getenv("DELAY_MS");
    if (!t0_str || !dur_str || !dms_str) {
        fprintf(stderr, "receiver: T0 / DURATION_S / DELAY_MS not set\n");
        return 1;
    }
    t0           = atof(t0_str);
    double dur   = atof(dur_str);
    double d_ms  = atof(dms_str);
    delay_s      = d_ms / 1000.0;
    total_frames = (int)(dur * 1000.0 / 20.0);

    /* --- init buffers --- */
    memset(frame_received, 0, sizeof frame_received);
    memset(fec,            0, sizeof fec);
    memset(nack_count,     0, sizeof nack_count);

    /* --- socket: from relay (port 47002) --- */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof in_addr);
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002");
        return 1;
    }

    /* --- socket: to harness player (port 47020) --- */
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player;
    memset(&player, 0, sizeof player);
    player.sin_family      = AF_INET;
    player.sin_port        = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* --- socket: feedback / NACKs (port 47003) --- */
    fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&feedback_addr, 0, sizeof feedback_addr);
    feedback_addr.sin_family      = AF_INET;
    feedback_addr.sin_port        = htons(47003);
    feedback_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* --- state --- */
    int next_play    = 0;    /* next frame to play out */
    int highest_recv = -1;   /* highest seq received so far */

    uint8_t buf[2048];
    uint8_t harness_pkt[HARNESS_PKT];

    /* ── event loop ──────────────────────────────────────────────────── */
    for (;;) {
        if (next_play >= total_frames) break;

        double now           = now_sec();
        double next_deadline = t0 + delay_s + next_play * 0.020;
        double wait          = next_deadline - now;

        /* clamp select timeout: 0 .. 1ms */
        struct timeval tv;
        if (wait <= 0) {
            tv.tv_sec = 0; tv.tv_usec = 0;
        } else {
            double w = wait < 0.001 ? wait : 0.001;
            tv.tv_sec  = 0;
            tv.tv_usec = (int)(w * 1e6);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        select(in_fd + 1, &rfds, NULL, NULL, &tv);

        /* ── drain all available packets ────────────────────────── */
        for (;;) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf,
                                 MSG_DONTWAIT, NULL, NULL);
            if (n < 0) break;                  /* EAGAIN / no more */
            if (n < HDR_LEN) continue;

            uint8_t type = buf[0];
            uint32_t seq;
            memcpy(&seq, buf + 1, 4);
            seq = ntohl(seq);

            if (n >= WIRE_PKT &&
                (type == PKT_DATA || type == PKT_RETRANSMIT)) {

                store_data((int)seq, buf + HDR_LEN);

                /* gap detection → NACKs */
                if ((int)seq > highest_recv) {
                    int start = highest_recv >= 0 ? highest_recv + 1 : 0;
                    for (int i = start; i < (int)seq; i++) {
                        if (i >= next_play && i < total_frames &&
                            !frame_received[i] &&
                            nack_count[i] < MAX_NACK_PER_FRAME) {
                            send_nack((uint32_t)i);
                            nack_count[i]++;
                        }
                    }
                    highest_recv = (int)seq;
                }
            }
            else if (n >= WIRE_PKT && type == PKT_PARITY) {
                int gid = (int)seq / FEC_K;
                if (gid >= 0 && gid < MAX_GROUPS &&
                    !fec[gid].parity_ok) {
                    memcpy(fec[gid].parity_data,
                           buf + HDR_LEN, PAYLOAD_LEN);
                    fec[gid].parity_ok = 1;
                    try_fec_recovery(gid);
                }
            }
        }

        /* ── playout ─────────────────────────────────────────────── */
        now = now_sec();
        while (next_play < total_frames) {
            double deadline = t0 + delay_s + next_play * 0.020;
            if (now < deadline) break;

            if (frame_received[next_play]) {
                uint32_t seq_n = htonl((uint32_t)next_play);
                memcpy(harness_pkt, &seq_n, 4);
                memcpy(harness_pkt + HARNESS_HDR,
                       frame_payload[next_play], PAYLOAD_LEN);
                sendto(out_fd, harness_pkt, HARNESS_PKT, 0,
                       (struct sockaddr *)&player, sizeof player);
            }
            /* else: deadline miss — nothing to send */
            next_play++;
        }

        /* ── proactive re-NACKs for upcoming frames ──────────────── */
        now = now_sec();
        for (int i = next_play;
             i < total_frames && i < next_play + 20; i++) {
            double dl = t0 + delay_s + i * 0.020;
            double remaining = dl - now;
            if (remaining > delay_s * 0.7) break;  /* too far out */
            if (remaining < 0.010) continue;        /* too late */
            if (!frame_received[i] &&
                nack_count[i] < MAX_NACK_PER_FRAME) {
                send_nack((uint32_t)i);
                nack_count[i]++;
            }
        }
    }

    /* stay alive until harness kills us */
    for (;;) pause();
    return 0;
}
