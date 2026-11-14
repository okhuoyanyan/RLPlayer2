"""Execute real launcher startup decisions; native full-process proof is separate."""
import pathlib, subprocess, sys, tempfile, os
root = pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT', pathlib.Path(__file__).resolve().parents[1]))
source = (root/'tools/launcher.cpp').read_text(encoding='utf-8')
start = source.index('    if (!open_webview_window(g_port)', source.index('int WINAPI wWinMain'))
end = source.index('\n    // 261024-05 #1 #4：消息循环结束', start)
fragment = source[start:end]
code = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
static bool g_exiting=false;
static int g_port=19321,mode=0,web=0,browser=0,get=0;
static bool open_webview_window(int){++web;if(mode==0){g_exiting=true;return false;}return mode==1;}
static void open_browser(int){++browser;}
static void logf(const wchar_t*,...){ }
static int test_GetMessageW(MSG*,HWND,UINT,UINT){++get;return 0;}
#define GetMessageW test_GetMessageW
static void run(){
''' + fragment + r'''
}
int main(){int checks=0,failed=0;auto ck=[&](bool b,const char*s){++checks;failed+=!b;printf("%s %s\n",b?"PASS":"FAIL",s);};
for(mode=0;mode<3;++mode){g_exiting=false;web=browser=get=0;run();ck(web==1,"actual startup invokes embedded WebView once");if(mode==0){ck(g_exiting,"nested-pump exit remains persistent");ck(browser==0,"initialization exit never opens Edge fallback");ck(get==0,"consumed WM_QUIT cannot block in GetMessage");}else{ck(browser==(mode==2),"normal native/fallback behavior remains");ck(get==1,"non-exiting startup enters message loop");}}
printf("checks=%d failures=%d\n",checks,failed);return failed?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='rlplayer-launcher-exit-') as tmp:
    path = pathlib.Path(tmp)
    (path/'test.cpp').write_text(code, encoding='utf-8')
    subprocess.run([sys.argv[1], '-std=c++20', '-O1', str(path/'test.cpp'), '-o', str(path/'test.exe'), '-luser32'], check=True)
    raise SystemExit(subprocess.run([str(path/'test.exe')], cwd=path).returncode)
