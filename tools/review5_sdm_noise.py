"""Independent coherent-tone audio-band measurement, not a 280 dB FIR proof.

Consumes signed one-bit samples only; imports no production DSP. The periodic
Hann tone occupies exactly three DFT bins. The same code is first calibrated
against an ideal coherent sine and a known -100 dB relative in-band spur.
"""
import argparse,csv,json,math,pathlib
import numpy as np

def measure(y,rate,k):
    n=len(y);w=.5-.5*np.cos(2*np.pi*np.arange(n)/n)
    z=np.fft.rfft(y*w);p=np.abs(z)**2
    frequencies=np.fft.rfftfreq(n,1/rate)
    band=(frequencies>=20)&(frequencies<=20000)
    signal=np.abs(np.arange(len(z))-k)<=1
    excluded=np.abs(np.arange(len(z))-k)<=2
    norm=2/(n*np.sum(w*w))
    signal_power=np.sum(p[signal])*norm
    noise_power=np.sum(p[band&~excluded])*norm
    return {"amplitude":float(2*abs(z[k])/np.sum(w)),
            "signal_power":float(signal_power),"noise_power":float(noise_power),
            "sndr_db":float(10*np.log10(signal_power/noise_power)),
            "noise_dbfs_sine_reference":float(10*np.log10(noise_power/.5))}

def main():
    ap=argparse.ArgumentParser();ap.add_argument("directory",type=pathlib.Path);args=ap.parse_args()
    n=1<<18;rate=2822400;k=93;t=np.arange(n)
    ideal=.62*np.sin(2*np.pi*k*t/n)
    calibration=measure(ideal,rate,k)
    spur=measure(ideal+.62e-5*np.sin(2*np.pi*377*t/n),rate,k)
    assert abs(calibration["amplitude"]-.62)<1e-12
    assert abs(calibration["signal_power"]-.62**2/2)<1e-12
    assert calibration["sndr_db"]>230
    assert abs(spur["sndr_db"]-100)<1e-5
    rows=[];failures=[]
    for record in csv.DictReader((args.directory/"index.csv").open()):
        y=np.fromfile(args.directory/record["file"],dtype=np.int8)
        assert len(y)==int(record["frames"]) and np.all((y==1)|(y==-1))
        record["input_amplitude"]=float(record["amplitude"])
        record.update(measure(y.astype(np.float64),int(record["rate"]),int(record["bin"])))
        record["tone_hz"]=int(record["bin"])*int(record["rate"])/len(y)
        record["gain_db"]=20*math.log10(record["amplitude"]/record["input_amplitude"])
        if abs(record["gain_db"])>.01 or any(int(record[k]) for k in ("overloads","clamps","nonfinite")):
            failures.append(record["file"])
        rows.append(record)
    result={"status":"PASS" if not failures else "FAIL","cells":len(rows),"failures":failures,
            "measurement":"20 Hz–20 kHz periodic-Hann integrated residual; exclude fundamental ±2 bins; coherent tones; no acoustic/electrical claim",
            "calibration_ideal":calibration,"calibration_minus100db_spur":spur,"rows":rows}
    (args.directory/"analysis.json").write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding="utf-8")
    with (args.directory/"analysis.csv").open("w",newline="",encoding="utf-8-sig") as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    print(json.dumps({k:v for k,v in result.items() if k!="rows"},ensure_ascii=False))
    for family in ("dsd","ec"):
        for rate in (2822400,45158400):
            group=[r for r in rows if r["family"]==family and int(r["rate"])==rate]
            print(family,rate,"min_sndr",min(r["sndr_db"] for r in group),"max_noise_dbfs",max(r["noise_dbfs_sine_reference"] for r in group),"max_gain_error",max(abs(r["gain_db"]) for r in group))
    return bool(failures)
if __name__=="__main__":raise SystemExit(main())
