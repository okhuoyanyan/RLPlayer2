"""Independent numerical audit for test_recording_cleanup's exported f64 fixture.
Uses only the Python standard library; no production DSP or FFT code is imported.
"""
import array
import json
import math
from pathlib import Path
import struct
import sys

p = Path(sys.argv[1])
b = p.read_bytes()
n, = struct.unpack_from('<Q', b)
v = array.array('d')
v.frombytes(b[8:])
assert len(v) == 3 * n
clean, raw, result = (v[i*n:(i+1)*n] for i in range(3))
# Fixed common-band measurement, neither level fitting nor delay searching.
h = []
for j in range(-64, 65):
    sinc = math.sin(2 * math.pi * 20000 / 44100 * j) / (math.pi * j) if j else 40000 / 44100
    h.append(sinc * (0.5 + 0.5 * math.cos(math.pi * j / 64)))
norm = math.fsum(h)
h = [x/norm for x in h]

def mse(y):
    errors = [a-b for a, b in zip(y, clean)]
    total = 0.0
    for i in range(4096, n-4096):
        e = math.fsum(t * errors[i+j-64] for j, t in enumerate(h))
        total += e*e
    return total/(n-8192)

before, after = mse(raw), mse(result)
gain = 10*math.log10(before/after)
assert gain > 1.0, gain
record = {'samples': n, 'sample_rate':44100, 'measurement_band_limit_hz':20000,
          'fixed_time_alignment':True,'no_gain_normalization':True,
          'raw_error_rms': math.sqrt(before),'cleaned_error_rms': math.sqrt(after),
          'error_improvement_db':gain,'passed':True}
print(json.dumps(record, ensure_ascii=False, indent=2))
