"""Independent arbitrary-precision reference over exported *binary64* coefficients.

python tools/dsp_measure_reference.py --table cal.tsv --coefficients coefficients --output reference.json
Uses exact float->mpf conversion, true mp.pi, 90 decimal digits by default.
For a dense table checks both violating witnesses; --all-witnesses also checks
the sampled stop maximum and pass worst point for every selected identity.
This confirms finite points only, not a continuous-band maximum.
"""
import argparse
import csv
import hashlib
import json
import struct
from fractions import Fraction
from pathlib import Path
import mpmath as mp

def exact_endpoint(kernel, frequency, rate):
    twice=Fraction.from_float(float(frequency))*2/Fraction(rate)
    if twice.denominator!=1:
        return None
    alternating=bool(twice.numerator&1)
    rational=sum((Fraction.from_float(float(v))*(-1 if alternating and (i&1) else 1)
                  for i,v in enumerate(kernel)),Fraction(0))
    return mp.mpf(rational.numerator)/rational.denominator


def stage_reference(row,args):
    """Ideal transfer product of the real two stage kernels, independently."""
    stem=f'{row["in_rate"]}-{row["out_rate"]}-{row["preset"]}'
    blobs=[(args.coefficients/(stem+suffix+".f64")).read_bytes() for suffix in ("-stage1","-stage2")]
    kernels=[[mp.mpf(v) for v in struct.unpack("<"+"d"*(len(b)//8),b)] for b in blobs]
    fs1=int(row.get("stage1_rate") or int(row["in_rate"])*int(row["F1"]))
    fs2=int(row.get("stage2_rate") or row["out_rate"])
    witnesses=[]
    if "frequency_hz" in row:
        witnesses=[("CALIBRATION",row["frequency_hz"],row["dd_db"])]
    else:
        if args.all_witnesses or float(row["product_peak_db"])>-float(row["target"]):
            witnesses.append(("STOP",row["stop_hz"],row["product_peak_db"]))
        if args.all_witnesses or abs(float(row["pass_db"]))>.01:
            witnesses.append(("PASS",row["pass_hz"],row["pass_db"]))
    results=[]
    for kind,freq,dd in witnesses:
        product=mp.mpc(1)
        for h,fs in zip(kernels,(fs1,fs2)):
            rotation=mp.exp(-2*mp.j*mp.pi*mp.mpf(float(freq))/fs)
            z=mp.mpc(1);total=mp.mpc(0)
            for x in h:
                total+=x*z
                z*=rotation
            product*=total
        db=20*mp.log10(abs(product))
        results.append(dict(identity=row["identity"],kind=kind,frequency_hz=freq,
            dd_db=dd,db90=str(db),delta_dd_db=str(mp.mpf(float(dd))-db),
            digits=args.digits,reference="real-stage ideal transfer product; process envelope is separate",
            coefficient_sha256=[hashlib.sha256(b).hexdigest() for b in blobs]))
    return results


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--table", action="append", required=True, type=Path)
    p.add_argument("--coefficients", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--digits", type=int, default=90)
    p.add_argument("--preset")
    p.add_argument("--all-witnesses", action="store_true")
    args = p.parse_args()
    if not 70 <= args.digits <= 500:
        p.error("--digits must be 70..500")
    mp.mp.dps = args.digits
    results = []
    for table in args.table:
        with table.open(encoding="utf-8-sig") as f:
            rows = list(csv.DictReader((s for s in f if not s.startswith("#")), delimiter="\t"))
        for row in rows:
            if args.preset and row["preset"] != args.preset:
                continue
            if row.get("status") == "EXCLUDED":
                continue
            if "stage1_rate" in row or "F1" in row:
                for item in stage_reference(row,args):
                    results.append(item)
                    print(json.dumps(item),flush=True)
                    args.output.parent.mkdir(parents=True,exist_ok=True)
                    args.output.write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding="utf-8")
                continue
            name = row.get("coefficient_file") or f'{row["in_rate"]}-{row["out_rate"]}-{row["preset"]}.f64'
            witnesses = []
            if "frequency_hz" in row:
                witnesses.append(("CALIBRATION", row["frequency_hz"], row["dd_db"]))
            else:
                if args.all_witnesses or float(row["stop_db"]) > -float(row["target"]):
                    witnesses.append(("STOP", row["stop_hz"], row["stop_db"]))
                if args.all_witnesses or abs(float(row["pass_db"])) > .01:
                    witnesses.append(("PASS", row["pass_hz"], row["pass_db"]))
            if not witnesses:
                continue
            raw = (args.coefficients / name).read_bytes()
            values = [mp.mpf(v) for v in struct.unpack("<" + "d" * (len(raw)//8), raw)]
            for kind, frequency, dd in witnesses:
                # Parsing to float reproduces the C++ binary64 Hz input exactly.
                hz = mp.mpf(float(frequency)); fs = mp.mpf(int(row["prototype_rate"]))
                rotation = mp.exp(-2 * mp.j * mp.pi * hz / fs)
                z = mp.mpc(1); total = mp.mpc(0)
                norm = mp.mpf(2*float(frequency)/int(row["prototype_rate"]))
                norm_rotation = mp.exp(-mp.j*mp.pi*norm)
                nz = mp.mpc(1); norm_total = mp.mpc(0)
                for h in values:
                    total += h*z
                    z *= rotation
                    if "exact_db" in row:
                        norm_total += h*nz
                        nz *= norm_rotation
                physical_endpoint=exact_endpoint(values,float(frequency),int(row["prototype_rate"]))
                normalized_endpoint=exact_endpoint(values,float(norm),2)
                if physical_endpoint is not None:total=mp.mpc(physical_endpoint)
                if normalized_endpoint is not None:norm_total=mp.mpc(normalized_endpoint)
                analytic_zero=total==0
                db = mp.mpf(-6000) if analytic_zero else 20*mp.log10(abs(total))
                item = dict(identity=row["identity"], taps=len(values), kind=kind,
                            frequency_hz=frequency, digits=args.digits, db90=str(db),
                            dd_db=dd, delta_dd_db=None if analytic_zero else str(mp.mpf(float(dd))-db),
                            coefficient_sha256=hashlib.sha256(raw).hexdigest())
                if "exact_db" in row:
                    item["exact_db"] = row["exact_db"]
                    item["delta_exact_db"] = None if analytic_zero else str(mp.mpf(float(row["exact_db"]))-db)
                    norm_zero=norm_total==0
                    norm_db = mp.mpf(-6000) if norm_zero else 20*mp.log10(abs(norm_total))
                    item["binary_normalized_frequency"] = str(norm)
                    item["normalized_reference_db90"] = str(norm_db)
                    item["exact_arithmetic_error_db"] = None if norm_zero else str(mp.mpf(float(row["exact_db"]))-norm_db)
                    item["analytic_zero"] = bool(norm_zero)
                    if norm_zero:
                        item["mathematical_db"]="-inf"
                        item["exact_zero_floor_correct"]=float(row["exact_db"])==-6000
                else:
                    item["target_db"] = row["target"]
                    item["strict_failure_confirmed"] = bool(db > -mp.mpf(row["target"]) if kind == "STOP" else abs(db) > mp.mpf(".01"))
                results.append(item)
                print(json.dumps(item), flush=True)
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    if not results:
        p.error("zero reference witnesses selected")
    finite_errors=[abs(float(x["delta_dd_db"])) for x in results if x["delta_dd_db"] is not None]
    print("POINTS", len(results), "ANALYTIC_ZEROS",sum(x.get("analytic_zero",False) for x in results),"MAX_FINITE_DD_ERROR_DB", max(finite_errors,default=0), flush=True)


if __name__ == "__main__":
    main()
