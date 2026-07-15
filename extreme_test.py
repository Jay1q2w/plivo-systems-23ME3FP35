import subprocess
import json
import re

TESTS = [
    ("A", 100, 1),
    ("A", 80, 2),
    ("A", 60, 3),
    ("B", 140, 1),
    ("B", 120, 2),
    ("B", 100, 3)
]

def run_test(profile, delay, seed):
    cmd = [
        "python3", "run.py", 
        "--profile", f"profiles/{profile}.json", 
        "--delay_ms", str(delay), 
        "--seed", str(seed),
        "--duration", "15" # 15 seconds to speed up testing
    ]
    print(f"Running Profile {profile}, Delay {delay}ms, Seed {seed}...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    # Parse output
    output = result.stdout
    miss_match = re.search(r"deadline misses\s+:\s+\d+\s+\(([\d.]+)%\)", output)
    overhead_match = re.search(r"bandwidth overhead\s+:\s+([\d.]+)x", output)
    valid_match = re.search(r"RESULT\s+:\s+(VALID|INVALID)", output)
    
    miss_rate = miss_match.group(1) if miss_match else "N/A"
    overhead = overhead_match.group(1) if overhead_match else "N/A"
    valid = valid_match.group(1) if valid_match else "ERROR"
    
    return {
        "profile": profile,
        "delay": delay,
        "seed": seed,
        "miss_rate": miss_rate,
        "overhead": overhead,
        "valid": valid,
        "output": output
    }

print(f"{'Profile':<10} | {'Delay':<7} | {'Seed':<5} | {'Miss %':<8} | {'Overhead':<10} | {'Result':<10}")
print("-" * 65)

for p, d, s in TESTS:
    res = run_test(p, d, s)
    print(f"{res['profile']:<10} | {res['delay']:<7} | {res['seed']:<5} | {res['miss_rate']:<8} | {res['overhead']:<10} | {res['valid']:<10}")
