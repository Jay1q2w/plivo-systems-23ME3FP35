# RUNLOG — Experiment Log

Each row is one run of `python3 run.py`. Record every experiment, including failures.

## Testing Commands

```bash
# Build
make

# Run with profile A (mild: 2% loss, 10-40ms jitter)
python3 run.py --profile profiles/A.json --delay_ms 100 --seed 1

# Run with profile B (moderate: 5% loss, 20-80ms jitter)
python3 run.py --profile profiles/B.json --delay_ms 120 --seed 1
```

## Results

| # | Profile | Seed | delay_ms | Frames | Misses | Miss % | Overhead | Valid | Notes |
|---|---------|------|----------|--------|--------|--------|----------|-------|-------|
| 1 | A | 1 | 100 | | | | | | FEC K=4 + jitter buffer + NACKs — initial test |
| 2 | B | 1 | 120 | | | | | | Same code on moderate profile |
| 3 | A | 1 | 80 | | | | | | Reduce delay target |
| 4 | A | 1 | 60 | | | | | | Aggressive delay reduction |
| 5 | A | 2 | 60 | | | | | | Stability check, different seed |
| 6 | A | 3 | 60 | | | | | | Stability check, different seed |
| 7 | B | 1 | 100 | | | | | | Moderate profile, lower delay |
| 8 | B | 2 | 100 | | | | | | Stability check |
| 9 | B | 1 | 80 | | | | | | Push lower on B |
| 10 | B | 3 | 80 | | | | | | Stability check |

## How to Read the Score Output

```
================ SCORE ================
  frames               : 1500
  deadline misses      : 5  (0.33%)   [cap 1.00%]
  playout delay        : 80 ms   <-- your score if valid; lower wins
  bandwidth overhead   : 1.31x   [cap 2.00x]
  RESULT               : VALID
```

Fill in the table after each run. One change per run; log what you changed and why.
