/*
 * RECEIVER — Immediate playout + XOR FEC recovery + NACKs
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>

#define PAYLOAD_LEN   160
#define HARNESS_HDR   4
#define HARNESS_PKT   164

#define PKT_DATA       0x01
#define PKT_PARITY     0x02
#define PKT_RETRANSMIT 0x03
#define PKT_NACK       0x04

#define FEC_K          2
#define HDR_LEN        5
#define WIRE_PKT       165
#define NACK_PKT_LEN   5

#define MAX_FRAMES     8192
#define MAX_GROUPS     (MAX_FRAMES / FEC_K + 1)
#define MAX_NACK_PER_FRAME 2

static int     frame_received[MAX_FRAMES];

typedef struct {
    uint8_t xor_accum[PAYLOAD_LEN];
    uint8_t parity_data[PAYLOAD_LEN];
    int     recv_mask;
    int     recv_count;
    int     parity_ok;
} fec_group_t;

static fec_group_t fec[MAX_GROUPS];
static int nack_count[MAX_FRAMES];

static int    total_frames;
static double t0;
static double delay_s;

static int out_fd;
static struct sockaddr_in player;
static int fb_fd;
static struct sockaddr_in feedback_addr;

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void xor_bytes(uint8_t *dst, const uint8_t *src, int len) {
    for (int i = 0; i < len; i++) dst[i] ^= src[i];
}

static void send_nack(uint32_t seq) {
    uint8_t pkt[NACK_PKT_LEN];
    pkt[0] = PKT_NACK;
    uint32_t ns = htonl(seq);
    memcpy(pkt + 1, &ns, 4);
    sendto(fb_fd, pkt, NACK_PKT_LEN, 0, (struct sockaddr *)&feedback_addr, sizeof feedback_addr);
}

static void playout(int seq, const uint8_t *payload) {
    if (seq < 0 || seq >= MAX_FRAMES) return;
    if (frame_received[seq]) return;
    frame_received[seq] = 1;
    
    uint8_t harness_pkt[HARNESS_PKT];
    uint32_t seq_n = htonl((uint32_t)seq);
    memcpy(harness_pkt, &seq_n, 4);
    memcpy(harness_pkt + HARNESS_HDR, payload, PAYLOAD_LEN);
    sendto(out_fd, harness_pkt, HARNESS_PKT, 0, (struct sockaddr *)&player, sizeof player);
}

static void try_fec_recovery(int gid) {
    if (gid < 0 || gid >= MAX_GROUPS) return;
    fec_group_t *g = &fec[gid];
    if (!g->parity_ok || g->recv_count != FEC_K - 1) return;

    int missing_idx = -1;
    for (int i = 0; i < FEC_K; i++) {
        if (!(g->recv_mask & (1 << i))) { missing_idx = i; break; }
    }
    if (missing_idx < 0) return;

    int seq = gid * FEC_K + missing_idx;
    if (seq >= total_frames || frame_received[seq]) return;

    uint8_t recovered[PAYLOAD_LEN];
    memcpy(recovered, g->parity_data, PAYLOAD_LEN);
    xor_bytes(recovered, g->xor_accum, PAYLOAD_LEN);

    playout(seq, recovered);
    g->recv_mask |= (1 << missing_idx);
    g->recv_count++;
}

static void handle_data(int seq, const uint8_t *payload) {
    if (seq < 0 || seq >= MAX_FRAMES || frame_received[seq]) return;
    playout(seq, payload);

    int gid  = seq / FEC_K;
    int fidx = seq % FEC_K;
    if (gid < MAX_GROUPS && !(fec[gid].recv_mask & (1 << fidx))) {
        xor_bytes(fec[gid].xor_accum, payload, PAYLOAD_LEN);
        fec[gid].recv_mask |= (1 << fidx);
        fec[gid].recv_count++;
        try_fec_recovery(gid);
    }
}

int main(void) {
    t0 = atof(getenv("T0") ? getenv("T0") : "0");
    delay_s = atof(getenv("DELAY_MS") ? getenv("DELAY_MS") : "100") / 1000.0;
    total_frames = (int)(atof(getenv("DURATION_S") ? getenv("DURATION_S") : "30") * 50);

    memset(frame_received, 0, sizeof frame_received);
    memset(fec, 0, sizeof fec);
    memset(nack_count, 0, sizeof nack_count);

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr);

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&player, 0, sizeof player);
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&feedback_addr, 0, sizeof feedback_addr);
    feedback_addr.sin_family = AF_INET;
    feedback_addr.sin_port = htons(47003);
    feedback_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t buf[2048];
    double nack_threshold = delay_s * 0.4; 

    for (;;) {
        struct timeval tv = {0, 5000}; 
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        select(in_fd + 1, &rfds, NULL, NULL, &tv);

        while (1) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL);
            if (n < HDR_LEN) break;

            uint8_t type = buf[0];
            uint32_t seq;
            memcpy(&seq, buf + 1, 4);
            seq = ntohl(seq);

            if ((type == PKT_DATA || type == PKT_RETRANSMIT) && n >= WIRE_PKT) {
                handle_data((int)seq, buf + HDR_LEN);
            } else if (type == PKT_PARITY && n >= WIRE_PKT) {
                int gid = (int)seq / FEC_K;
                if (gid >= 0 && gid < MAX_GROUPS && !fec[gid].parity_ok) {
                    memcpy(fec[gid].parity_data, buf + HDR_LEN, PAYLOAD_LEN);
                    fec[gid].parity_ok = 1;
                    try_fec_recovery(gid);
                }
            }
        }

        double now = now_sec();
        for (int i = 0; i < total_frames; i++) {
            if (frame_received[i]) continue;
            double creation_time = t0 + i * 0.020;
            double age = now - creation_time;
            
            if (age > nack_threshold && age < delay_s) {
                if (nack_count[i] == 0 || (nack_count[i] == 1 && age > nack_threshold * 2)) {
                    // send_nack((uint32_t)i); // DISABLED NACKs to save bandwidth
                    nack_count[i]++;
                }
            }
            if (age < nack_threshold) break; 
        }
    }
    return 0;
}
