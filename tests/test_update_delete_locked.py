"""Real updater must never commit a deletion that Windows sharing locks denied.

Only private sentinel installations and CLI processes are used. The generated
helper covers the exceptional Win32 case where deletion reports failure after
the source has already disappeared; it still calls the production move helper.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HELPER = r'''
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
static int mode = 0;
static std::wstring source;
static BOOL WINAPI denied_move(LPCWSTR, LPCWSTR, DWORD) {
    SetLastError(ERROR_NOT_SAME_DEVICE); return FALSE;
}
static BOOL WINAPI reported_delete_failure(LPCWSTR path) {
    if (mode == 0) ::DeleteFileW(path);
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}
static DWORD WINAPI query_attributes(LPCWSTR path) {
    if (mode == 2 && source == path) {
        SetLastError(ERROR_ACCESS_DENIED); return INVALID_FILE_ATTRIBUTES;
    }
    return ::GetFileAttributesW(path);
}
#define MoveFileExW denied_move
#define DeleteFileW reported_delete_failure
#define GetFileAttributesW query_attributes
#include "platform/fs.h"
#undef MoveFileExW
#undef DeleteFileW
#undef GetFileAttributesW
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::u8path(argv[1]);
    int failures = 0;
    for (mode = 0; mode != 4; ++mode) {
        const auto dir = root / std::to_string(mode);
        std::filesystem::create_directories(dir);
        const auto from = dir / "source.bin", to = dir / "destination.bin";
        source = from.native();
        if (mode != 3) std::ofstream(from, std::ios::binary) << "ORIGINAL";
        else std::ofstream(to, std::ios::binary) << "KEEP-DESTINATION";
        const bool result = rta::platform::move_or_copy_file(from, to);
        const DWORD error = GetLastError();
        std::ifstream input(to, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(input)), {});
        const bool sourceExists = GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool ok = mode == 0 ? result && !sourceExists && data == "ORIGINAL"
            : mode == 3 ? !result && !sourceExists && data == "KEEP-DESTINATION"
            : !result && sourceExists && data == "ORIGINAL" && error == ERROR_ACCESS_DENIED;
        std::printf("[%s] fallback mode=%d result=%d source_exists=%d error=%lu\n",
                    ok ? "PASS" : "FAIL", mode, result, sourceExists, error);
        failures += !ok;
    }
    return failures ? 1 : 0;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--compiler', type=Path,
                        default=Path('C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'))
    args = parser.parse_args()
    source_root = Path(__file__).resolve().parents[1]
    root = (args.evidence_dir or Path(tempfile.mkdtemp(prefix='rl-update-delete-'))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    rows = []
    for action, sharing in (('delete', None), ('delete', 1), ('install', 1),
                            ('delete', 0), ('install', 0)):
        label = action + ('-normal' if sharing is None else '-share-' + str(sharing))
        case = root / label
        case.mkdir(exist_ok=False)
        shutil.copy2(args.probe.resolve(), case / 'update_probe.exe')
        target = case / 'rtaweb.exe'
        old, new = b'PRIVATE-ORIGINAL-BYTES', b'PRIVATE-REPLACEMENT-BYTES'
        target.write_bytes(old)
        protected = case / 'rlplayer-settings.conf'
        protected.write_bytes(b'PRIVATE-USER-SETTINGS-SENTINEL')
        stage = case / 'updates/9.9.9/staging'
        stage.mkdir(parents=True)
        deleting = action == 'delete'
        if not deleting:
            import hashlib
            (stage / target.name).write_bytes(new)
            marker = f'file={target.name}\t{len(new)}\t{hashlib.sha256(new).hexdigest()}\n'
        else:
            marker = f'delete={target.name}\n'
        (stage / 'complete.txt').write_text('version=9.9.9\n' + marker, encoding='utf8')
        ledger = case / 'updates/pending.txt'
        ledger.write_text('phase=ready\nversion=9.9.9\nfrom=3.2.2\n' +
                          ('delete=' if deleting else 'file=') + target.name + '\n', encoding='utf8')
        backup = case / 'updates/9.9.9/backup' / ('deleted/rtaweb.exe' if deleting else 'rtaweb.exe')
        env = {**os.environ, 'RLPLAYER_UPDATE_DIR': str(case), 'RTA_NO_DEVICE': '1',
               'RTA_NET_TESTS': '0', 'RLPLAYER_UPDATE_ALLOW_LOCAL': '1',
               'RLPLAYER_UPDATE_MANIFEST': 'http://127.0.0.1:1/api/v1/update/windows/latest',
               'APPDATA': str(case/'profile'), 'LOCALAPPDATA': str(case/'profile')}

        def run(mode: str, name: str) -> int:
            result = subprocess.run([str(case/'update_probe.exe'), mode], cwd=case, env=env,
                                    capture_output=True, timeout=20)
            (case/(name+'.log')).write_bytes(result.stdout + result.stderr)
            return result.returncode

        def phase() -> str:
            return next((line[6:] for line in ledger.read_text(encoding='utf8').splitlines()
                         if line.startswith('phase=')), '')

        handle = None
        if sharing is not None:
            handle = kernel.CreateFileW(str(target), 0x80000000, sharing, None, 3, 0x80, None)
            if handle in (None, ctypes.c_void_p(-1).value):
                raise RuntimeError('cannot lock private sentinel: ' + str(ctypes.get_last_error()))
        try:
            first = run('--apply', 'first')
            initial_phase = phase()
            original_retained = target.is_file() and target.read_bytes() == old if sharing != 0 else target.is_file()
            backup_retained = backup.is_file() and backup.read_bytes() == old
            intent_retained = ('operation=' + action + ':rtaweb.exe\n') in ledger.read_text(encoding='utf8')
            failed_retry = run('--rollback', 'still-locked-rollback') if sharing == 1 else None
        finally:
            if handle is not None:
                kernel.CloseHandle(handle)
        row = {'case': label, 'first_exit': first, 'initial_phase': initial_phase,
               'original_retained_during_failure': original_retained,
               'original_backup_retained': backup_retained, 'intent_retained': intent_retained}
        if sharing is None:
            row['initial_pass'] = first == 0 and initial_phase == 'applied' and not target.exists() and backup_retained
        elif sharing == 1:
            row['initial_pass'] = (first != 0 and initial_phase == 'rollback_failed' and
                                   original_retained and backup_retained and intent_retained and failed_retry != 0)
            row['unlocked_rollback_exit'] = run('--rollback', 'unlocked-rollback')
            row['unlocked_original_restored'] = target.read_bytes() == old
        else:
            row['initial_pass'] = first != 0 and initial_phase == 'apply_failed' and target.read_bytes() == old and not backup.exists()
        if sharing is not None:
            row['retry_apply_exit'] = run('--apply', 'unlocked-retry-apply')
            row['retry_applied_correctly'] = (not target.exists()) if deleting else target.read_bytes() == new
        row['final_rollback_exit'] = run('--rollback', 'final-rollback')
        row['final_original_restored'] = target.is_file() and target.read_bytes() == old and phase() == 'rolled_back'
        row['protected_unchanged'] = protected.read_bytes() == b'PRIVATE-USER-SETTINGS-SENTINEL'
        row['pass'] = (row['initial_pass'] and row['final_rollback_exit'] == 0 and
                       row['final_original_restored'] and row['protected_unchanged'] and
                       row.get('unlocked_rollback_exit', 0) == 0 and
                       row.get('unlocked_original_restored', True) and
                       row.get('retry_apply_exit', 0) == 0 and row.get('retry_applied_correctly', True))
        rows.append(row)

    helper = root / 'fallback-edge.cpp'
    helper.write_text(HELPER, encoding='utf8')
    executable = root / 'fallback-edge.exe'
    build = subprocess.run([str(args.compiler), '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS',
                            '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-I'+str(source_root/'src'),
                            str(helper), '-o', str(executable)], capture_output=True, timeout=60)
    (root/'fallback-build.log').write_bytes(build.stdout + build.stderr)
    if build.returncode:
        raise RuntimeError('private fallback helper failed to compile; see fallback-build.log')
    edge = subprocess.run([str(executable), str(root/'fallback-cases')], capture_output=True, timeout=20)
    (root/'fallback-edge.log').write_bytes(edge.stdout + edge.stderr)
    rows.append({'case': 'fallback-api-edge-cases', 'exit': edge.returncode, 'pass': edge.returncode == 0})
    (root/'results.json').write_text(json.dumps(rows, indent=2), encoding='utf8')
    print(json.dumps(rows, indent=2))
    return 0 if all(row['pass'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
