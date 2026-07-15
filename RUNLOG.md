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
| 1 | A | 1 | 100 | 1500 | 2 | 0.13% | 1.55x | VALID | Pure FEC K=2 — initial safe test |
| 2 | A | 2 | 80 | 1500 | 0 | 0.00% | 1.55x | VALID | Reduce delay target |
| 3 | A | 3 | 60 | 1500 | 4 | 0.27% | 1.55x | VALID | Absolute minimum physical delay for A |
| 4 | A | 42 | 40 | 1500 | 59 | 3.93% | 1.55x | INVALID | Pushed below minimum bound (60ms) to verify theoretical limit |
| 5 | B | 1 | 140 | 1500 | 12 | 0.80% | 1.55x | VALID | Test on moderate profile |
| 6 | B | 2 | 120 | 1500 | 4 | 0.27% | 1.55x | VALID | Reduce delay target on B |
| 7 | B | 3 | 100 | 1500 | 6 | 0.40% | 1.55x | VALID | Absolute minimum physical delay for B |
| 8 | B | 3 | 80 | 1500 | 34 | 2.27% | 1.55x | INVALID | Pushed below minimum bound (100ms) to verify theoretical limit |

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
