"""Real updater retries an interrupted, uncommitted operation after a sharing lock.

Only disposable sentinel installations are used. No player/audio/server is started.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--evidence-dir', type=Path)
    args = parser.parse_args()
    root = (args.evidence_dir or Path(tempfile.mkdtemp(prefix='rlplayer-intent-recovery-'))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    rows = []
    for kind in ('install-existing', 'install-new', 'delete-existing'):
        case = root / kind
        case.mkdir(exist_ok=False)
        shutil.copy2(args.probe.resolve(), case / 'update_probe.exe')
        backup = case / 'updates/9.9.9/backup'
        backup.mkdir(parents=True)
        target = case / 'rtaweb.exe'
        old, new = b'OLD-INTENT-SENTINEL', b'NEW-INTENT-SENTINEL'
        delete = kind == 'delete-existing'
        saved = backup / ('deleted/rtaweb.exe' if delete else 'rtaweb.exe')
        saved.parent.mkdir(parents=True, exist_ok=True)
        if kind != 'install-new':
            saved.write_bytes(old)
        if not delete:
            target.write_bytes(new)
        operation = ('delete:' if delete else 'install:') + 'rtaweb.exe'
        ledger = case / 'updates/pending.txt'
        ledger.write_text('phase=applying\nversion=9.9.9\nfrom=3.2.2\nfile=rtaweb.exe\n'
                          f'operation={operation}\noperationExisted={0 if kind == "install-new" else 1}\n', encoding='utf8')
        protected = case / 'rlplayer-settings.conf'
        protected.write_bytes(b'PRIVATE-SENTINEL-NO-REAL-SETTINGS')
        locked = saved if delete else target
        # Share-read allows the updater to inspect the sentinel, but denies replace/delete.
        handle = kernel.CreateFileW(str(locked), 0x80000000, 0 if delete else 1, None, 3, 0x80, None)
        if handle in (None, ctypes.c_void_p(-1).value):
            raise RuntimeError(f'Could not lock private fixture: {ctypes.get_last_error()}')
        env = {**os.environ, 'RLPLAYER_UPDATE_DIR': str(case), 'RTA_NO_DEVICE': '1',
               'RTA_NET_TESTS': '0', 'RLPLAYER_UPDATE_ALLOW_LOCAL': '1',
               'RLPLAYER_UPDATE_MANIFEST': 'http://127.0.0.1:1/api/v1/update/windows/latest',
               'APPDATA': str(case/'profile'), 'LOCALAPPDATA': str(case/'profile')}
        try:
            first = subprocess.run([str(case/'update_probe.exe'), '--rollback', 'INTENT_LOCKED'],
                                   cwd=case, env=env, capture_output=True, timeout=20)
        finally:
            kernel.CloseHandle(handle)
        first_ledger = ledger.read_text(encoding='utf8')
        (case/'first.log').write_bytes(first.stdout + first.stderr)
        retry = subprocess.run([str(case/'update_probe.exe'), '--rollback', 'INTENT_UNLOCKED'],
                               cwd=case, env=env, capture_output=True, timeout=20)
        (case/'retry.log').write_bytes(retry.stdout + retry.stderr)
        restored = (not target.exists()) if kind == 'install-new' else target.exists() and target.read_bytes() == old
        row = {'case': kind, 'first_exit': first.returncode,
               'intent_retained': f'operation={operation}\n' in first_ledger,
               'failed_phase': 'phase=rollback_failed\n' in first_ledger,
               'retry_exit': retry.returncode, 'restored': restored,
               'final_phase': 'phase=rolled_back\n' in ledger.read_text(encoding='utf8'),
               'protected_unchanged': protected.read_bytes() == b'PRIVATE-SENTINEL-NO-REAL-SETTINGS'}
        row['pass'] = (row['first_exit'] != 0 and row['intent_retained'] and row['failed_phase']
                       and row['retry_exit'] == 0 and row['restored'] and row['final_phase']
                       and row['protected_unchanged'])
        rows.append(row)
    (root/'results.json').write_text(json.dumps(rows, indent=2), encoding='utf8')
    print(json.dumps(rows, indent=2))
    return 0 if all(row['pass'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
