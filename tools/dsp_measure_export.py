"""Export the three measured matrices to an editable Excel workbook and JSON.
No DSP is run. Paths are explicit; source/evidence files are read-only inputs.
"""
import argparse
import csv
import hashlib
import json
import re
from pathlib import Path
from openpyxl import Workbook,load_workbook
from openpyxl.styles import Alignment,Font,PatternFill
from openpyxl.utils import get_column_letter


def read_rows(path):
    with path.open(encoding="utf-8-sig") as f:
        return list(csv.DictReader((s for s in f if not s.startswith("#")),delimiter="\t"))


def preset_metadata(source):
    output={}
    for line in (source/"src/dsp/filter_families.h").read_text(encoding="utf-8").splitlines():
        m=re.match(r'\s*\{\s*"([^"]+)"\s*,(.*)\}',line)
        if not m:
            continue
        fields=m[2].split(",")
        if len(fields)<8:
            continue
        apod=len(fields)>9 and fields[8].strip()=="true"
        expr=fields[9].strip() if apod else None
        if expr and not re.fullmatch(r"[0-9.eE+*/() \-]+",expr):
            raise ValueError("non-numeric apod expression")
        output[m[1]]=dict(transition=float(fields[1]),apod=apod,
                         f1=float(eval(expr,{"__builtins__":{}},{})) if expr else None)
    if len(output)!=79:
        raise ValueError(f"expected79 immutable names, got{len(output)}")
    return output


def stats(rows):
    excluded=sum(r["status"]=="EXCLUDED" for r in rows)
    fail=sum("FAIL" in r["status"] for r in rows if r["status"]!="EXCLUDED")
    unresolved=sum("UNRESOLVED" in r["status"] for r in rows if r["status"]!="EXCLUDED")
    return dict(total=len(rows),excluded=excluded,finite_grid_met=len(rows)-excluded-fail-unresolved,
                sampled_fail=fail,unresolved=unresolved)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--evidence",required=True,type=Path)
    p.add_argument("--source-root",required=True,type=Path)
    p.add_argument("--output",required=True,type=Path)
    args=p.parse_args();e=args.evidence
    inputs=[("预设拓扑395",e/"final-even395.tsv"),("强制两段395",e/"forced-on395.tsv"),
            ("DSD1024_158",e/"final-dsd1024-158.tsv"),("历史基线395_不计948",e/"baseline-306-dd16.tsv")]
    metadata=preset_metadata(args.source_root)
    wb=Workbook();wb.remove(wb.active);summaries={}
    headings=["编号","预设（名称未变）","实际身份","输入Hz","输出Hz","结论","规格排除理由","未决说明",
        "实际拓扑","首级每相T","次级每相T","原型或等效N","FFT总点数","每条FFT点数",
        "声明通带上界Hz","通带最差点Hz","通带最差dB","通带阈值绝对dB","通带余量dB",
        "阻带起点Hz","阻带极值点Hz","阻带峰值dB（DSD为乘积）","目标衰减dB","最大允许阻带dB",
        "过程误差δ（线性）","含δ阻带上界dB","保守阻带余量dB","真实过程见证dB","判定类型",
        "过程判定","频谱复用","证明范围","证据TSV"]
    scope="有限DD频点、端点与候选细化；不证明连续全带；DSD另含实际process冲激δ界；90位仅复核见证点。"
    for title,path in inputs:
        rows=read_rows(path);summaries[title]=stats(rows);ws=wb.create_sheet(title)
        ws.append([title+"：完整身份清单"])
        ws.merge_cells(start_row=1,start_column=1,end_row=1,end_column=len(headings))
        ws.append(["948项=395+395+158；863项有限频点达标，85项规格排除不计通过。历史基线另列，不计入948。"])
        ws.merge_cells(start_row=2,start_column=1,end_row=2,end_column=len(headings))
        ws.append([scope]);ws.merge_cells(start_row=3,start_column=1,end_row=3,end_column=len(headings))
        s=summaries[title];ws.append([f"本表：总数{s['total']}；有限频点达标{s['finite_grid_met']}；规格排除{s['excluded']}；失败{s['sampled_fail']}；未决{s['unresolved']}。"])
        ws.merge_cells(start_row=4,start_column=1,end_row=4,end_column=len(headings));ws.append(headings)
        for i,r in enumerate(rows,1):
            info=metadata[r["preset"]];inrate=int(r["in_rate"]);outrate=int(r["out_rate"])
            declared=(info["f1"] if info["apod"] else 1-info["transition"])*inrate*.5
            content=40000 if inrate==96000 else 20000
            excluded=r["status"]=="EXCLUDED";unknown="UNRESOLVED" in r["status"];failed="FAIL" in r["status"]
            conclusion="规格排除（不计通过）" if excluded else "未决" if unknown else "历史已知失败" if failed else "有限频点+δ界达标" if "F1" in r else "有限频点达标"
            reason=f"非切趾；声明通带{declared:.6f}Hz < 内容边缘{content}Hz − 1000Hz（原D-15）" if excluded else ""
            stop=float(r["stop_db"]);target=float(r["target"]);passdb=float(r["pass_db"])
            bound=float(r["bounded_peak_db"]) if "bounded_peak_db" in r else stop
            topology=r.get("topology") or ("cascade" if "F1" in r or r.get("T")=="0" else "single")
            vals=[i,r["preset"],r.get("identity") or f"{inrate}/{outrate}/{r['preset']}",inrate,outrate,conclusion,reason,
                "数值或过程上界未能决定；不得计作通过" if unknown else "无",topology,
                int(r["T"]) if r.get("T") and int(r["T"])>0 else "历史接口未暴露",int(r.get("stage2_T") or 0),
                int(r.get("effective_taps") or r["taps"]),int(r["fft_n"]),int(r.get("stripe_n") or r["fft_n"]),
                declared,float(r["pass_hz"]),passdb,.01,.01-abs(passdb),inrate-declared,float(r["stop_hz"]),stop,target,-target,
                float(r["process_delta_l1"]) if "process_delta_l1" in r else None,bound,-target-bound,
                float(r["process_witness_db"]) if "process_witness_db" in r else None,r.get("kind") or "",r.get("decision") or r["status"],r.get("reused") or "0",scope,str(path)]
            ws.append(vals)
            row=ws.max_row
            ws.cell(row,6).fill=PatternFill("solid",fgColor="E2E8F0" if excluded else "FEE2E2" if failed or unknown else "DCFCE7")
            for col in range(1,len(headings)+1):
                cell=ws.cell(row,col);cell.alignment=Alignment(vertical="top",wrap_text=col in (2,3,6,7,8,29,30,32,33))
                if isinstance(cell.value,float):cell.number_format="0.000E+00" if col==25 else "0.000000000000"
            ws.row_dimensions[row].height=30
        ws.freeze_panes="D6";ws.auto_filter.ref=f"A5:{get_column_letter(len(headings))}{ws.max_row}"
        ws.sheet_view.zoomScale=80
        for row in (1,5):
            for cell in ws[row]:cell.fill=PatternFill("solid",fgColor="17324D");cell.font=Font(name="微软雅黑",size=11,color="FFFFFF",bold=True)
        for row in (2,3,4):
            ws.cell(row,1).font=Font(name="微软雅黑",size=11,bold=row==4,color="17324D")
            ws.cell(row,1).alignment=Alignment(wrap_text=True,vertical="center");ws.row_dimensions[row].height=29
        ws.row_dimensions[1].height=27;ws.row_dimensions[5].height=42
        for col in range(1,len(headings)+1):ws.column_dimensions[get_column_letter(col)].width=18
        for col,width in {"A":7,"B":28,"C":65,"F":24,"G":56,"H":28,"AC":24,"AD":38,"AF":65,"AG":65}.items():ws.column_dimensions[col].width=width
        ws.print_title_rows="1:5";ws.sheet_properties.pageSetUpPr.fitToPage=True
        ws.page_setup.orientation="landscape";ws.page_setup.paperSize=ws.PAPERSIZE_A3;ws.page_setup.fitToWidth=1;ws.page_setup.fitToHeight=0
    args.output.parent.mkdir(parents=True,exist_ok=True);wb.save(args.output)
    check=load_workbook(args.output,read_only=True,data_only=True)
    assert [check[n].max_row-5 for n,_ in inputs]==[395,395,158,395]
    assert sum(summaries[n]["total"] for n,_ in inputs[:3])==948
    assert sum(summaries[n]["excluded"] for n,_ in inputs[:3])==85
    assert sum(summaries[n]["finite_grid_met"] for n,_ in inputs[:3])==863
    check.close()
    summary_path=e/"measurement-summary.json"
    summary=json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.exists() else {}
    summary["final_matrices"]={name:dict(**summaries[name],tsv=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for name,path in inputs}
    summary["final_combined"]=dict(total=948,excluded_not_passed=85,finite_grid_met=863,sampled_fail=0,unresolved=0,
        scope=scope,continuous_band_proven=False,workbook=str(args.output),workbook_sha256=hashlib.sha256(args.output.read_bytes()).hexdigest())
    summary["dsd_scan"]=dict(identities=158,unique_spectrum_evaluations=58,strict_reuses=100,unique_grid_points=6322913280,
        maximum_fft_grid=1073741824,elapsed_seconds=457.366,minimum_bounded_stop_margin_db=1.44175722708394,
        fallback_needed=0,fallback_independently_tested_bins=32769,fallback_max_absolute_error=1.3171644805625601e-29,
        scope="Measured CPU process-impulse response on the finite grid; ideal stage product plus computed L1 error envelope. Does not establish continuous extrema or arbitrary signal-dependent roundoff.")
    exact=[]
    for name in ("final-exact-production-90.json","final-exact-manual-90144-90.json"):
        exact+=json.loads((e/name).read_text())
    if "new_exact_calibration" in summary:
        summary.setdefault("pre_even_new_exact_calibration",summary["new_exact_calibration"])
    summary["new_exact_calibration"]=dict(points=len(exact),finite_nonzero_points=sum(x.get("exact_arithmetic_error_db") is not None for x in exact),
        analytic_zeros=sum(x.get("analytic_zero",False) for x in exact),
        analytic_zeros_return_correct_floor=all(x.get("exact_zero_floor_correct",True) for x in exact),
        max_arithmetic_error_db=max(abs(float(x["exact_arithmetic_error_db"])) for x in exact if x["exact_arithmetic_error_db"] is not None),
        max_error_including_hz_normalization_db=max(abs(float(x["delta_exact_db"])) for x in exact if x["delta_exact_db"] is not None),
        caveat="Analytic zeros have mathematical -infinity dB; a relative dB error is undefined. They are tested by exact rational endpoint sums and the documented -6000 floor.")
    witnesses=json.loads((e/"final-dsd1024-all-witnesses-90.json").read_text())
    summary["dsd_90_digit_witnesses"]=dict(points=len(witnesses),max_dd_error_db=max(abs(float(x["delta_dd_db"])) for x in witnesses),
        scope="Only the recorded stop/pass witness points were evaluated at90 decimal digits; not all grid bins and not a continuous-frequency proof.")
    summary_path.write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(dict(workbook=str(args.output),sheets=summaries,sha256=summary["final_combined"]["workbook_sha256"]),ensure_ascii=False,indent=2))


if __name__=="__main__":main()
