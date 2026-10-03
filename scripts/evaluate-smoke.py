"""Evaluate the explicitly labelled local smoke cases, never a claimed holdout."""
import json
import math
from pathlib import Path
import numpy as np

root = Path(__file__).resolve().parent.parent
cases = [
    ('knock-verified', [('knockdown', 2436.65)], 1),
    ('assist-verified', [('assist', 2445.0)], 0),
    ('downed-replay', [], 0),
]
rows = []
tp = fp = fn = saved = false_saved = 0
for name, truth, expected_clips in cases:
    observations = [json.loads(s) for s in (root / f'artifacts/{name}.jsonl').read_text(encoding='utf-8').splitlines()]
    predicted = [e for o in observations for e in o['events'] if e['kind'] != 'squad_wipe']
    unused = set(range(len(predicted)))
    matched = 0
    for kind, time in truth:
        hit = next((i for i in unused if predicted[i]['kind'] == kind and abs(predicted[i]['time'] - time) <= 1), None)
        if hit is not None:
            matched += 1
            unused.remove(hit)
    clips = json.loads((root / f'artifacts/{name}.jsonl.clips.json').read_text(encoding='utf-8'))['clips']
    tp += matched
    fp += len(unused)
    fn += len(truth) - matched
    saved += len(clips)
    false_saved += max(0, len(clips) - expected_clips)
    rows.append({'case': name, 'observations': len(observations), 'trueResults': truth, 'detectedResults': predicted,
                 'clipCount': len(clips), 'expectedClipCount': expected_clips})
spectra = {}
for name in ('game', 'desktop'):
    samples = np.fromfile(root / f'artifacts/final-{name}-audio.f32', dtype=np.float32)
    spectra[name] = {str(f): float(abs(np.mean(samples * np.exp(-2j * np.pi * f * np.arange(len(samples)) / 48000)))) for f in (440, 880)}
leak = {'gameContainsOtherDb': 20 * math.log10(max(spectra['game']['880'], 1e-12) / spectra['game']['440']),
        'otherContainsGameDb': 20 * math.log10(max(spectra['desktop']['440'], 1e-12) / spectra['desktop']['880'])}
streams = json.loads((root / 'artifacts/final-streams.json').read_text(encoding='utf-8-sig'))['streams']
starts = [float(s['start_time']) for s in streams]
report = {'qualification': 'small calibration smoke set; not independent holdout; not current Apex live validation',
          'truePositives': tp, 'falsePositives': fp, 'falseNegatives': fn,
          'precision': tp / (tp + fp) if tp + fp else None,
          'recall': tp / (tp + fn) if tp + fn else None,
          'savedClips': saved, 'falseSavedClips': false_saved,
          'falseClipRatio': false_saved / saved if saved else None,
          'cases': rows, 'toneMagnitude': spectra, 'toneLeakDb': leak,
          'streamStartDifferenceMs': (max(starts) - min(starts)) * 1000,
          'ruleScenarios': 17, 'nativeScenarios': 5,
          'longRunAvDrift': 'not measured', 'gameFpsImpact': 'not measured', 'currentHud': 'not validated'}
target = root / 'artifacts/validation-summary.json'
target.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(report, ensure_ascii=False, indent=2))
