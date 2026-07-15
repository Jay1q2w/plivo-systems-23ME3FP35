/*
 * SENDER — XOR FEC (K=4) + NACK-based retransmission
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i at t0 + i*20ms
 *                  (format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink toward the receiver
 *   bind 47004  <- feedback from receiver via relay (NACKs)
 *
 * Strategy:
 *   1. Every frame received from harness is immediately forwarded as a DATA
 *      packet with a 1-byte type prefix (165 bytes total).
 *   2. Frames are grouped into FEC groups of K=4. After every K data frames,
 *      an XOR parity packet is sent so the receiver can recover any single
 *      loss within the group without a round-trip.
 *   3. A send-history ring buffer stores recent frames. When a NACK arrives
 *      from the receiver, the requested frame is retransmitted.
 *
 * Wire format (sender -> receiver):
 *   DATA:       type(0x01) + seq(4B BE) + payload(160B) = 165 bytes
 *   PARITY:     type(0x02) + group_start_seq(4B BE) + xor_payload(160B) = 165 B
 *   RETRANSMIT: type(0x03) + seq(4B BE) + payload(160B) = 165 bytes
 *
 * Wire format (receiver -> sender):
 *   NACK:       type(0x04) + seq(4B BE) = 5 bytes
 *
 * Env vars: T0, DURATION_S, DELAY_MS (available but not required by sender).
 * The harness kills this process at run end; a forever-loop is fine.
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdint.h>

/* ── constants ───────────────────────────────────────────────────────── */

#define PAYLOAD_LEN   160
#define HARNESS_HDR   4
#define HARNESS_PKT   164     /* 4 + 160 */

#define PKT_DATA       0x01
#define PKT_PARITY     0x02
#define PKT_RETRANSMIT 0x03
#define PKT_NACK       0x04

#define FEC_K          2      /* parity group size */
#define HDR_LEN        5      /* type(1) + seq(4) */
#define WIRE_PKT       165    /* 5 + 160 */
#define NACK_PKT_LEN   5      /* type(1) + seq(4) */

#define HISTORY_SIZE   4096   /* ring-buffer slots for retransmission */

/* ── send history (ring buffer of original harness frames) ───────── */

static uint8_t  history_data[HISTORY_SIZE][HARNESS_PKT];
static uint32_t history_seq[HISTORY_SIZE];     /* stored seq for validation */
static int      history_valid[HISTORY_SIZE];

/* ── helpers ─────────────────────────────────────────────────────────── */

static void xor_bytes(uint8_t *dst, const uint8_t *src, int len) {
    for (int i = 0; i < len; i++)
        dst[i] ^= src[i];
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {

    /* --- socket: harness source (port 47010) --- */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof in_addr);
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }

    /* --- socket: feedback / NACKs (port 47004) --- */
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr;
    memset(&fb_addr, 0, sizeof fb_addr);
    fb_addr.sin_family      = AF_INET;
    fb_addr.sin_port        = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof fb_addr) < 0) {
        perror("bind 47004");
        return 1;
    }

    /* --- socket: outgoing to relay (port 47001) --- */
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay;
    memset(&relay, 0, sizeof relay);
    relay.sin_family      = AF_INET;
    relay.sin_port        = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* --- FEC state --- */
    uint8_t parity[PAYLOAD_LEN];
    memset(parity, 0, PAYLOAD_LEN);
    int      group_count = 0;   /* frames accumulated in current group */
    uint32_t group_start = 0;   /* seq of first frame in current group */

    memset(history_valid, 0, sizeof history_valid);

    uint8_t buf[2048];
    uint8_t wire[WIRE_PKT];
    int maxfd = in_fd > fb_fd ? in_fd : fb_fd;

    /* ── event loop ──────────────────────────────────────────────────── */
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        FD_SET(fb_fd, &rfds);

        struct timeval tv = {0, 5000};          /* 5 ms timeout */
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) continue;

        /* ── incoming frame from harness ─────────────────────────── */
        if (FD_ISSET(in_fd, &rfds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n < HARNESS_PKT) continue;

            uint32_t seq;
            memcpy(&seq, buf, 4);
            seq = ntohl(seq);
            uint8_t *payload = buf + HARNESS_HDR;

            /* save in ring buffer for possible retransmission */
            int slot = (int)(seq % HISTORY_SIZE);
            memcpy(history_data[slot], buf, HARNESS_PKT);
            history_seq[slot]   = seq;
            history_valid[slot] = 1;

            /* build & send DATA packet */
            wire[0] = PKT_DATA;
            uint32_t seq_n = htonl(seq);
            memcpy(wire + 1, &seq_n, 4);
            memcpy(wire + HDR_LEN, payload, PAYLOAD_LEN);
            sendto(out_fd, wire, WIRE_PKT, 0,
                   (struct sockaddr *)&relay, sizeof relay);

            /* XOR into parity accumulator */
            if (group_count == 0) {
                memset(parity, 0, PAYLOAD_LEN);
                group_start = seq;
            }
            xor_bytes(parity, payload, PAYLOAD_LEN);
            group_count++;

            /* emit parity after K data frames */
            if (group_count == FEC_K) {
                wire[0] = PKT_PARITY;
                uint32_t gs = htonl(group_start);
                memcpy(wire + 1, &gs, 4);
                memcpy(wire + HDR_LEN, parity, PAYLOAD_LEN);
                sendto(out_fd, wire, WIRE_PKT, 0,
                       (struct sockaddr *)&relay, sizeof relay);
                group_count = 0;
            }
        }

        /* ── NACK from receiver (via relay) ──────────────────────── */
        if (FD_ISSET(fb_fd, &rfds)) {
            ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n < NACK_PKT_LEN) continue;
            if (buf[0] != PKT_NACK) continue;

            uint32_t req_seq;
            memcpy(&req_seq, buf + 1, 4);
            req_seq = ntohl(req_seq);

            int slot = (int)(req_seq % HISTORY_SIZE);
            if (history_valid[slot] && history_seq[slot] == req_seq) {
                /* retransmit the frame */
                wire[0] = PKT_RETRANSMIT;
                uint32_t rs = htonl(req_seq);
                memcpy(wire + 1, &rs, 4);
                memcpy(wire + HDR_LEN,
                       history_data[slot] + HARNESS_HDR, PAYLOAD_LEN);
                sendto(out_fd, wire, WIRE_PKT, 0,
                       (struct sockaddr *)&relay, sizeof relay);
            }
        }
    }
    return 0;
}
