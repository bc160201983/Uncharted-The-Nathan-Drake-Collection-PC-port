"""Create an isolated manifest overlay for the retained UC2 stage spawn package.

Never writes to the base or patch game trees. This does not create a match or
modify package contents; it exposes one retained package to the native loader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

RELATIVE_MANIFEST = Path('u2data/build/main/pak22_ps4.txt')
RELATIVE_PACKAGE = Path('u2data/build/main/pak22/ps4/stage-multi-pak-ingame10.pak')
NAME = 'stage-multi-pak'
PACKAGE = 'stage-multi-pak-ingame'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def validate_and_append(data):
    text = data.decode('utf-8')
    headers = list(re.finditer(r'(?m)^level-name[ \t]+([^\r\n]+)', text))
    names = [match.group(1).strip() for match in headers]
    if len(names) != len(set(names)):
        raise ValueError('Source manifest contains duplicate level-name declarations')
    if 'stage-multi' not in names:
        raise ValueError('Source manifest has no stage-multi declaration')
    if NAME in names:
        index = names.index(NAME)
        end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
        lines = [line.strip() for line in text[headers[index].start():end].splitlines() if line.strip()]
        expected = [f'level-name {NAME}', f'level-id {NAME}', f'package {PACKAGE}']
        if lines != expected:
            raise ValueError('Existing stage-multi-pak declaration differs from the verified three-line block')
        return data, False
    newline = b'\r\n' if b'\r\n' in data else b'\n'
    separator = b'' if data.endswith(newline * 2) else newline if data.endswith(newline) else newline * 2
    block = newline.join(line.encode('ascii') for line in (
        f'level-name {NAME}', f'level-id {NAME}', f'package {PACKAGE}', ''))
    return data + separator + block, True


def build_overlay(base, patch, overlay):
    base, overlay = Path(base).resolve(), Path(overlay).resolve()
    patch = Path(patch).resolve() if patch else None
    roots = ([patch] if patch else []) + [base]
    if any(overlay == root or overlay.is_relative_to(root) for root in roots):
        raise ValueError('Overlay must be outside both original game roots')
    source = next((root / RELATIVE_MANIFEST for root in roots if (root / RELATIVE_MANIFEST).is_file()), None)
    package = next((root / RELATIVE_PACKAGE for root in roots if (root / RELATIVE_PACKAGE).is_file()), None)
    if source is None or package is None:
        raise ValueError('Effective manifest or retained stage wrapper package is missing')
    original = source.read_bytes()
    package_data = package.read_bytes()
    if len(package_data) < 32 or package_data[:4] != bytes.fromhex('790a0000'):
        raise ValueError('Retained stage package header is not expected PAK magic 0xa79')
    output, appended = validate_and_append(original)
    destination = overlay / RELATIVE_MANIFEST
    resolved_destination = destination.resolve()
    if not resolved_destination.is_relative_to(overlay) or any(resolved_destination.is_relative_to(root) for root in roots):
        raise ValueError('Resolved destination escapes the isolated overlay')
    if destination.exists() and destination.read_bytes() != output:
        raise ValueError('Destination contains a different manifest; refusing to overwrite an unrelated overlay')
    unchanged = destination.exists()
    report = dict(kind='uc2mp-stage-wrapper-manifest-overlay', source=str(source),
                  source_sha256=digest(original), destination=str(destination),
                  overlay_sha256=digest(output), package=str(package),
                  package_sha256=digest(package_data), appended=appended,
                  level_name=NAME, package_name=PACKAGE,
                  source_modified=False, runtime_validated=False)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not unchanged:
        destination.write_bytes(output)
    if source.read_bytes() != original:
        raise RuntimeError('Source changed concurrently; overlay must be regenerated')
    (overlay / 'uc2mp-stage-wrapper-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', '--base-root', type=Path, required=True)
    parser.add_argument('--patch', '--patch-root', type=Path)
    parser.add_argument('--overlay', '--overlay-root', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build_overlay(args.base, args.patch, args.overlay), indent=2))


if __name__ == '__main__':
    main()
