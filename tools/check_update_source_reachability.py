"""Read-only, bounded public update-source reachability. Never download/install an update."""
import argparse,datetime,json,pathlib,re,time,urllib.request,urllib.parse
p=argparse.ArgumentParser();p.add_argument('--evidence-dir',type=pathlib.Path,required=True);a=p.parse_args();a.evidence_dir.mkdir(parents=True,exist_ok=False)
source=pathlib.Path(__file__).resolve().parents[1];text=(source/'src/net/update_check.h').read_text(encoding='utf-8');url=re.search(r'kDefaultManifestUrl\[\]\s*=\s*"([^"]+)"',text).group(1)
def public(value):
 u=urllib.parse.urlsplit(value);return urllib.parse.urlunsplit((u.scheme,u.netloc,u.path,'<redacted>' if u.query else '',u.fragment))
def request(opener,url,method,limit):
 start=time.monotonic();row={'url':public(url),'method':method,'max_body_bytes':limit}
 try:
  headers={'User-Agent':'RLPlayer-ReadOnly-Update-Audit/1','Accept-Encoding':'identity'}
  if method=='GET' and limit==1024:headers['Range']='bytes=0-1023'
  with opener.open(urllib.request.Request(url,headers=headers,method=method),timeout=12) as r:
   body=r.read(limit+1) if method!='HEAD' else b''
   row.update(status=r.status,final_url=public(r.url),content_length=r.headers.get('Content-Length'),content_range=r.headers.get('Content-Range'),bytes_read=min(len(body),limit),body_over_limit=len(body)>limit)
  return row,body[:limit]
 except Exception as e:row.update(error=type(e).__name__+': '+str(e));return row,b''
 finally:row['elapsed_seconds']=round(time.monotonic()-start,4)
reports=[]
for mode in ['system_default','direct_no_env_proxy']:
 opener=urllib.request.build_opener() if mode=='system_default' else urllib.request.build_opener(urllib.request.ProxyHandler({}))
 row,body=request(opener,url,'GET',512*1024);row['connection_mode']=mode;reports.append(row)
 if row.get('status')!=200 or row.get('body_over_limit'):continue
 try:manifest=json.loads(body.decode('utf-8-sig'))
 except Exception as e:row['json_error']=str(e);continue
 (a.evidence_dir/(mode+'-manifest.json')).write_bytes(body);row['manifest_version']=manifest.get('version');assets=[]
 for value in [manifest.get('download_url'),manifest.get('url'),manifest.get('downloadUrl'),manifest.get('full',{}).get('url') if isinstance(manifest.get('full'),dict) else None]:
  if isinstance(value,str) and value not in assets and urllib.parse.urlsplit(value).hostname in ['github.com','raw.githubusercontent.com']:assets.append(value)
 for asset in assets[:2]:
  check,_=request(opener,asset,'HEAD',0);check['connection_mode']=mode;reports.append(check)
  small,_=request(opener,asset,'GET',1024);small['connection_mode']=mode;reports.append(small)
summary={'checked_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'default_manifest_from_source':url,'scope':'This Windows host only at this time. Direct_no_env_proxy bypasses environment proxies; not proof of every mainland ISP. HEAD or <=1025 response bytes per asset; no package installation, publication or credentials.','requests':reports}
(a.evidence_dir/'reachability.json').write_text(json.dumps(summary,indent=2),encoding='utf-8');print(json.dumps(summary,indent=2));raise SystemExit(0 if any(r.get('status')==200 and r.get('manifest_version') for r in reports) else 1)
