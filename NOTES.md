# NOTES — Design Summary

## Design

The system uses a dual-recovery strategy: **XOR-based Forward Error Correction (FEC)** as the primary loss-recovery mechanism, and **NACK-based retransmission** as a fallback. The sender groups every 4 consecutive frames and emits one XOR parity packet per group, costing ~31% bandwidth overhead (well within the 2.0× cap). The receiver maintains a **jitter buffer** indexed by sequence number with deadline-driven playout, and performs FEC recovery the instant it holds K−1 data frames plus the parity for any group. A lightweight NACK protocol (5 bytes per request) handles the rare case of 2+ losses within a single FEC group: NACKs are sent on gap detection and re-sent proactively as deadlines approach, giving up to 3 attempts per frame.

## Recommended Grading Delay

**`--delay_ms 100`** — this absorbs the worst-case FEC recovery latency ((K−1)×20 ms + max relay delay) across all tested profiles while keeping miss rate well under 1%.

## What Breaks It

Sustained burst losses longer than K frames within one FEC group overwhelm XOR parity (it can only recover 1 per group), and if the NACK round-trip exceeds the remaining playout budget, those frames are irrecoverable misses. Delay spikes exceeding `DELAY_MS` minus the FEC recovery window cause unconditional misses regardless of redundancy. At very low delay settings (<60 ms), there is insufficient time for either FEC parity arrival or NACK round-trips, so the system degrades to baseline behavior.
