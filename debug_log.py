import json
log = json.load(open('playout_log.json'))
frames = log['frames']
present = sum(1 for f in frames if f['present'])
print(f'Present: {present}/{len(frames)}')
for f in frames[:10]:
    print(f)
