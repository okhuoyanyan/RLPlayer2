"""Actual updater subprocess+OS faults; isolated install, no GUI/network/device."""
import pathlib, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='rlplayer-update-failure-') as tmp:
    result = subprocess.run([sys.executable, str(root/'tools/review5_update_rollback_negative.py'), '--probe', sys.argv[1], '--evidence-dir', str(pathlib.Path(tmp)/'run')])
    if result.returncode:
        raise SystemExit(result.returncode)
    print('PASS actual updater failure process cases=5 unlock_retry_processes=2')
