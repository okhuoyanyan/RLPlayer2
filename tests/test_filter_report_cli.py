"""Offline production report CLI regression and shared-coefficient alignment."""
import argparse
import csv
import io
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--exe', required=True, type=Path)
parser.add_argument('--sweep', type=Path)
parser.add_argument('--pipeline-witness', type=Path)
parser.add_argument('--evidence', type=Path)
options = parser.parse_args()
checks = 0

def check(condition, description):
    global checks
    checks += 1
    assert condition, description
    print('PASS', description, flush=True)

def run(args, expected=0):
    result = subprocess.run([str(options.exe), *args], capture_output=True, encoding='utf-8')
    check(result.returncode == expected, f'exit {expected}: {args}')
    if result.returncode != expected:
        print(result.stdout, result.stderr)
    return result

for args in [
    ['--unknown'], ['unexpected.csv'], ['--out'], ['--preset'],
    ['--preset', 'missing-preset'], ['--in-rate', '44100'],
    ['--in-rate', '0', '--out-rate', '0'],
    ['--in-rate', '-1', '--out-rate', '705600'],
    ['--in-rate', '44100junk', '--out-rate', '705600'],
    ['--in-rate', '48000', '--out-rate', '48000'],
    ['--in-rate', '96000', '--out-rate', '48000'],
    ['--in-rate', '44100', '--out-rate', '999999999'],
    ['--coefficients'], ['--fft-factor', '1'], ['--out', ''],
]:
    run(args, 2)
check('QUALITY NOT CERTIFIED' in run(['--help']).stdout, 'help states inventory is not quality certification')

scratch = tempfile.TemporaryDirectory(prefix='filter-report-')
root = options.evidence or Path(scratch.name)
root.mkdir(parents=True, exist_ok=True)
cases = [
    ('poly-sinc-short-lp', 44100, 705600),
    ('apod-20k', 44100, 705600),
    ('poly-sinc-long-ip', 44100, 705600),
    ('poly-sinc-short-2s', 44100, 705600),
    ('poly-sinc-short-lp', 96000, 384000),
    ('poly-sinc-mqa/mp3-mp', 44100, 705600),
]
for preset, source, target in cases:
    label = f'{source}-{target}-{preset}'.replace('/', '_')
    folder = root / label
    folder.mkdir(parents=True, exist_ok=True)
    arguments = ['--preset', preset, '--in-rate', str(source), '--out-rate', str(target)]
    report = folder / 'report.csv'
    coefficient_dir = folder / 'report-coefficients'
    result = run(['--csv', '--out', str(report), '--coefficients', str(coefficient_dir), *arguments])
    check(result.stdout == report.read_text(encoding='utf-8'), 'CSV --out preserves clean machine-readable stdout')
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    check(len(rows) == 1 and rows[0]['name'] == preset, 'exact preset selects one identity')
    row = rows[0]
    check(row['status'] == 'STRUCTURE_ONLY' and row['quality'] == 'NOT_CERTIFIED', 'CSV explicitly declines quality certification')
    check(not {'passbandRippleDb', 'worstStopbandDb', 'imgAtten29kDb', 'prePostRatio', 'peakPos', 'centroidSamples'} & row.keys(), 'obsolete spectral and ringing measurements removed')
    check(int(row['prototype_samples']) > 0 and int(row['T']) > 0, 'actual prototype and stage taps present')
    if preset == 'apod-20k':
        check(row['apodizing'] == 'true' and abs(float(row['apod_f1_hz']) - 18400) < 1e-8 and abs(float(row['apod_f2_hz']) - 21800) < 1e-8, 'production apod F1/F2 retained')
    if preset == 'poly-sinc-short-2s':
        check(row['topology'] == 'cascade' and int(row['stage2_T']) > 0, 'actual cascade and both stage budgets reported')
    if preset == 'poly-sinc-long-ip':
        check(row['configured_phase'] == 'mix', 'long-ip reports production intermediate phase configuration')
    coefficients = coefficient_dir / row['coefficient_file']
    check(coefficients.stat().st_size == int(row['prototype_samples']) * 8, 'coefficient binary length matches reported actual sample count')
    if options.sweep:
        sweep_table = folder / 'sweep.tsv'
        sweep_coefficients = folder / 'sweep-coefficients'
        sweep_result = subprocess.run([str(options.sweep), *arguments, '--structure-only', '--output', str(sweep_table), '--coefficients', str(sweep_coefficients)], capture_output=True, encoding='utf-8')
        (folder / 'sweep.log').write_text(sweep_result.stdout + sweep_result.stderr, encoding='utf-8')
        check(sweep_result.returncode == 0, 'shared filter_sweep structure CLI succeeds')
        sweep_row = next(csv.DictReader((line for line in sweep_table.read_text(encoding='utf-8').splitlines() if not line.startswith('#')), delimiter='\t'))
        for report_key, sweep_key in [('prototype_samples', 'taps'), ('T', 'T'), ('stage2_T', 'stage2_T'), ('L', 'L'), ('M', 'M'), ('prototype_rate', 'prototype_rate'), ('topology', 'topology')]:
            check(row[report_key] == sweep_row[sweep_key], f'report/sweep {report_key} identical')
        check(coefficients.read_bytes() == (sweep_coefficients / sweep_row['coefficient_file']).read_bytes(), 'actual report/sweep coefficient binaries bit-identical')
    if options.pipeline_witness:
        witness = subprocess.run([str(options.pipeline_witness), preset, str(source), str(target), str(coefficients)], capture_output=True, encoding='utf-8')
        (folder / 'pipeline.log').write_text(witness.stdout + witness.stderr, encoding='utf-8')
        check(witness.returncode == 0, f'actual engine::Pipeline impulse agrees: {witness.stdout.strip()}')

result = run(['--markdown', '--preset', 'poly-sinc-short-lp'])
check('STRUCTURE_ONLY / QUALITY NOT CERTIFIED' in result.stdout, 'legacy markdown CLI clearly labels structure-only scope')
check('逐位一致 (0 差)' not in result.stdout and 'filter_sweep --preset poly-sinc-short-lp' in result.stdout, 'unperformed pair-test claim replaced by executable quality command')
run(['--csv', '--preset', 'poly-sinc-short-lp', '--out', str(root)], 2)
print(f'filter_report_cli: {checks} checks, 0 failures', flush=True)

