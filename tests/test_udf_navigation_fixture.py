"""Build numeric filesystem fixtures and exercise the actual bounded UDF reader."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parents[1]
fixture = Path(tempfile.mkdtemp(prefix='rlplayer-udf-boundaries-')).resolve()
subprocess.run([sys.executable, '-B', str(source/'tests/make_udf_fixture.py'), str(fixture)], check=True)
raise SystemExit(subprocess.run([str(Path(sys.argv[1]).resolve()), str(fixture)], cwd=fixture).returncode)
