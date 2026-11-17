"""Copy exactly the reviewed runtime files; never select the newest host Edge."""
from __future__ import annotations
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import stat


def load_pin(path):
    data = json.loads(pathlib.Path(path).read_text(encoding='utf-8-sig'))
    if data.get('schema') != 1 or not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', data.get('runtimeVersion', '')):
        raise ValueError('Invalid runtime pin version/schema')
    if not re.fullmatch(r'\d+\.\d+\.\d+', data.get('baselineRelease', '')) or not data.get('files'):
        raise ValueError('Runtime pin requires baseline release and files')
    seen = set()
    for entry in data['files']:
        name = entry.get('path', '')
        parts = name.split('/')
        if (not name or '\\' in name or re.search(r'[:*?\x00-\x1f]', name) or
                any(p in ('', '.', '..') or p[-1:] in ('.', ' ') or re.match(r'(?i)^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)', p) for p in parts)):
            raise ValueError('Unsafe runtime path: ' + name)
        if name.casefold() in seen:
            raise ValueError('Duplicate runtime path: ' + name)
        seen.add(name.casefold())
        if not isinstance(entry.get('bytes'), int) or entry['bytes'] < 0 or not re.fullmatch(r'[0-9a-f]{64}', entry.get('sha256', '')):
            raise ValueError('Invalid runtime identity: ' + name)
    if 'msedgewebview2.exe' not in seen:
        raise ValueError('Runtime executable absent from pin')
    return data


def digest(path):
    with pathlib.Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def reject_reparse_ancestors(path):
    # Do not resolve first: resolving would hide a symlink/junction in a parent.
    absolute = pathlib.Path(path).absolute()
    for candidate in (absolute, *absolute.parents):
        try:
            identity = candidate.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(identity.st_mode) or getattr(identity, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Runtime path must not contain a reparse point: ' + str(candidate))


def check_file(root, entry):
    path = root / entry['path']
    reject_reparse_ancestors(path)
    if not path.is_file() or path.stat().st_size != entry['bytes'] or digest(path) != entry['sha256']:
        raise ValueError('Pinned runtime content mismatch: ' + str(path))


def copy_pinned_runtime(pin_path, candidates, output):
    pin = load_pin(pin_path)
    output = pathlib.Path(output)
    reject_reparse_ancestors(output)
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError('Runtime output must be absent or empty')
    selected = None
    errors = []
    for candidate in candidates:
        candidate = pathlib.Path(candidate)
        try:
            for entry in pin['files']:
                check_file(candidate, entry)
            selected = candidate
            break
        except (OSError, ValueError) as error:
            errors.append(str(error))
    if selected is None:
        raise ValueError('No verified pinned runtime available. ' + '; '.join(errors))
    output.mkdir(parents=True, exist_ok=True)
    for entry in pin['files']:
        target = output / entry['path']
        reject_reparse_ancestors(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(selected / entry['path'], target)
        check_file(output, entry)
    return {'result': 'PASS_PINNED_RUNTIME', 'runtimeVersion': pin['runtimeVersion'],
            'baselineRelease': pin['baselineRelease'], 'source': str(selected),
            'files': len(pin['files']), 'bytes': sum(x['bytes'] for x in pin['files']),
            'pinSHA256': digest(pin_path), 'output': str(output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pin', required=True)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--candidate', action='append', default=[])
    parser.add_argument('--output', required=True)
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    pin = load_pin(args.pin)
    root = pathlib.Path(args.source_root).resolve()
    candidates = [pathlib.Path(x) for x in args.candidate]
    # A prior official package is preferred to the machine's Evergreen runtime.
    for parent in (root, *root.parents):
        candidates.append(parent / 'dist' / ('RLPlayer-' + pin['baselineRelease'] + '-win64') / 'WebView2Runtime')
    candidates.append(pathlib.Path('C:/Program Files (x86)/Microsoft/EdgeWebView/Application') / pin['runtimeVersion'])
    proof = copy_pinned_runtime(args.pin, candidates, args.output)
    pathlib.Path(args.report).write_text(json.dumps(proof, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(proof, ensure_ascii=False))


if __name__ == '__main__':
    main()
