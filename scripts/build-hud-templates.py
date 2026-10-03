"""Create binary font templates from manually labelled, local HUD crops.
This is glyph matching, with no learned detector or external inference service.
"""
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
labels = {'001': '30', '017': '28', '018': '27', '021': '25',
          '022': '24', '026': '21', '029': '20', '030': '18',
          '033': '17', '034': '15', '037': '14', '038': '12'}
templates = []

def add(image, text, source):
    rgb = np.asarray(image.convert('RGB'))
    before = len(templates)
    for threshold in (160, 180, 205):
        mask = rgb.min(axis=2) > threshold
        columns = mask.sum(axis=0) >= 2
        runs, start = [], None
        for x, present in enumerate(list(columns) + [False]):
            if present and start is None:
                start = x
            if not present and start is not None:
                if x - start >= 1:
                    runs.append((start, x))
                start = None
        if len(runs) > len(text):
            runs = runs[-len(text):]
        if len(runs) != len(text):
            continue
        for digit, (left, right) in zip(text, runs):
            glyph = mask[:, left:right]
            rows = np.flatnonzero(glyph.any(axis=1))
            glyph = glyph[rows[0]:rows[-1]+1]
            h, w = glyph.shape
            scaled = np.asarray(Image.fromarray(glyph.astype(np.uint8)*255).resize((20, 32), Image.Resampling.NEAREST)) > 0
            templates.append({'digit': 10 if digit=='*' else int(digit), 'aspect': w/h, 'source': source,
                              'mask': [''.join('1' if v else '0' for v in row) for row in scaled]})
    if len(templates) == before:
        raise ValueError((source, text, 'No clean segmentation'))

for name, text in labels.items():
    image = Image.open(ROOT / f'artifacts/ammo-digits/{name}.png').crop((15,12,74,44))
    add(image, text, f'reference-2024-ammo-{name}')
image = Image.open(ROOT / 'artifacts/assist-2445.png').resize((1920,1080),Image.Resampling.BILINEAR).crop((1724,96,1759,119))
add(image, '916', 'reference-2024-score-916')
image = Image.open(ROOT / 'artifacts/reference-2439.png').resize((1920,1080),Image.Resampling.BILINEAR).crop((1716,101,1755,118))
add(image, '1578', 'reference-2024-score-1578')
for name, text in {'001':'1378','009':'1413','012':'1453','013':'1468','014':'1488','015':'1503','016':'1523','017':'1543'}.items():
    image = Image.open(ROOT / f'artifacts/score-digits/{name}.png').crop((20,2,61,20))
    add(image, text, f'reference-2024-score-{name}')
for time, text in {2438:'844',2440:'916',2445:'916'}.items():
    image = Image.open(ROOT / f'artifacts/assist-original-{time}.png').resize((1920,1080),Image.Resampling.BILINEAR).crop((1724,101,1757,118))
    add(image, text, f'reference-2024-assist-counter-{time}')
image = Image.open(ROOT / 'artifacts/reference-2439.png').resize((1920,1080),Image.Resampling.BILINEAR).crop((1697,101,1716,118))
add(image, '*', 'damage-icon-boundary')
output = {'version': 'apex-zh-2024-v1', 'width': 20, 'height': 32,
          'method': 'binary-glyph-distance', 'templates': templates}
target = ROOT / 'config/hud.zh.v1.json'
target.write_text(json.dumps(output,ensure_ascii=False,indent=2),encoding='utf-8')
print(target, len(templates), 'templates', sorted({x['digit'] for x in templates}))
