"""Run an independently implemented common-band metric on exported PCM data."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parents[1]
fixture = Path(tempfile.mkdtemp(prefix='rlplayer-cleanup-oracle-')).resolve()
data = fixture/'independent-reference.f64'
subprocess.run([str(Path(sys.argv[1]).resolve()), str(data)], cwd=fixture, check=True)
raise SystemExit(subprocess.run([sys.executable, '-B', str(source/'tests/test_recording_cleanup_oracle.py'), str(data)], cwd=fixture).returncode)
