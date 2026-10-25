"""Verify real native character/camera control in an isolated offscreen instance.

Start it with uc2mp_local_launcher.py --map-test stage-multi --preload-hero
--defer-spawn --stage-spawn --headless --seconds 300 --instance NAME first.
This test moves the character through the runtime's diagnostic pad API.
It does not inject operating-system input or claim network gameplay.
"""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('instance')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', args.instance):
        parser.error('Invalid isolated instance name')
    folder = ROOT / 'work/local-lobby/instances' / args.instance
    log = (folder / 'analysis102/big2-ps4_Shipping/boot.log').read_text(errors='replace')
    if 'GOW3_HEADLESS=1: real offscreen rendering' not in log:
        parser.error('This automated input test requires an offscreen diagnostic instance')
    evidence = folder / 'control-test'
    evidence.mkdir(exist_ok=True)

    def snapshot(name):
        result = subprocess.run([sys.executable, str(ROOT/'tools/uc2mp_runtime_probe.py'),
                                 '--instance', args.instance, '--level', 'stage-multi',
                                 '--out', str(evidence/(name+'.json'))],
                                cwd=ROOT, capture_output=True, text=True, timeout=10)
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        data = json.loads(result.stdout)
        hero = data['hero']
        if not hero.get('valid') or not data['camera'].get('valid'):
            raise RuntimeError('A generation-checked native hero and camera are required')
        state = hero['player_state']['state_machine_at_50'].get('state_name')
        if state not in ('Move', 'IdleAmbient'):
            raise RuntimeError(f'Expected live native locomotion/idle state, got {state}')
        if not data['levels'][0].get('ready_predicate'):
            raise RuntimeError('Native stage Level is not ready')
        return data

    def capture(name, *action):
        subprocess.run([sys.executable, str(ROOT/'tools/uc2mp_frame_capture.py'), args.instance,
                        str(evidence/(name+'.png')), *action], cwd=ROOT, check=True, timeout=15,
                       stdout=subprocess.DEVNULL)

    before = snapshot('before')
    capture('before')
    capture('forward', '--sticks', '128', '0', '128', '128', '--hold', '2', '--settle', '0.6')
    moved = snapshot('after-forward')
    distance = math.dist(before['hero']['local_position4'][:3], moved['hero']['local_position4'][:3])
    if not 0.25 < distance < 30:
        raise RuntimeError(f'Unexpected displacement after native input: {distance}')
    capture('camera', '--sticks', '128', '128', '205', '128', '--hold', '1', '--settle', '0.6')
    rotated = snapshot('after-camera')
    q1, q2 = moved['camera']['quaternion4'], rotated['camera']['quaternion4']
    dot = abs(sum(a*b for a,b in zip(q1,q2)))
    if not math.isfinite(dot) or dot > 0.999:
        raise RuntimeError('Right-stick input did not measurably rotate the native camera')
    result = dict(passed=True, displacement=distance, camera_quaternion_abs_dot=dot,
                  hero_generation=rotated['hero']['generation'],
                  capability='one native locally controlled character; no network gameplay')
    (evidence/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))
    print(f'Evidence: {evidence}')


if __name__ == '__main__':
    main()
