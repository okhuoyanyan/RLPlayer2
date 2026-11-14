// Production Library load/add/remove/reload. Never starts scanning or audio.
// All settings, indexes, metadata and fake legacy siblings are private fixtures.
// The child process reloads the saved zero-root state after an actual restart.
#include "net/library.h"
#include "platform/process.h"
#include <chrono>
#include <iostream>
#include <fstream>

namespace fs=std::filesystem;
using rta::net::Library;
static std::string u8(const fs::path& p){return rta::platform::to_utf8(p);}
static void write(const fs::path& p,const std::string& s){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f<<s;}
static void isolate(const fs::path& root,const fs::path& profile){
    fs::create_directories(profile);fs::current_path(root);
    _putenv_s("RLPLAYER_LIBRARY_DIR",u8(profile).c_str());
    _putenv_s("RLPLAYER_PORT","");_putenv_s("RLPLAYER_ISOLATE","0");
    _putenv_s("USERPROFILE",u8(root/"private-user").c_str());
    rta::meta::MetaStore::instance().setBaseDir(u8(root/"private-meta"));
    rta::meta::EntityStore::instance().setBaseDir(u8(root/"private-meta"));
}
int main(int argc,char** argv){
    if(argc==3&&std::string(argv[1])=="--reload-empty"){
        const auto root=rta::platform::from_utf8(argv[2]);isolate(root,root/"profiles"/"added");
        auto& lib=Library::instance();lib.load();
        std::cout<<"RESTART roots="<<lib.roots().size()<<" tracks="<<lib.trackCount()<<" scanning="<<lib.scanning()<<"\n";
        return lib.roots().empty()&&!lib.scanning()&&lib.trackCount()==0?0:1;
    }
    const auto self=fs::absolute(rta::platform::from_utf8(argv[0]));
    const auto root=self.parent_path()/("library-sources336-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);fs::create_directories(root/"fake-old-music");
    const std::string old=u8(root/"fake-old-music");
    // Even the old red revision finds this private migration decoy before its
    // system-Music fallback, so no user settings/directories need to be read.
    write(root/"profiles"/"rlplayer-library.conf",old+"\n");
    auto& lib=Library::instance();int checks=0,failures=0;
    auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;std::cout<<(ok?"PASS ":"FAIL ")<<label<<" roots="<<lib.roots().size()<<"\n";};
    isolate(root,root/"profiles"/"missing");lib.load();
    check(lib.roots().empty(),"isolated missing configuration stays empty");
    write(root/"legacy-parent"/"rlplayer-library.conf",old+"\n");
    isolate(root,root/"legacy-parent"/"new-profile");lib.load();
    check(lib.roots().empty(),"isolated profile never inherits parent legacy roots");
    std::ifstream legacy(root/"legacy-parent"/"rlplayer-library.conf");std::string legacyValue;std::getline(legacy,legacyValue);
    check(legacyValue==old,"fake legacy evidence is unchanged");
    const auto empty=root/"profiles"/"explicit-empty";
    write(empty/"rlplayer-library.conf","# Intentionally empty source list\n");
    isolate(root,empty);lib.load();check(lib.roots().empty(),"explicit empty configuration stays empty");
    lib.load();check(lib.roots().empty(),"empty saved list remains empty after load");
    const auto added=root/"profiles"/"added";
    write(added/"rlplayer-library.conf","# User removed every source\n");
    isolate(root,added);lib.load();
    const auto music=root/fs::path(std::u8string(u8"音乐 目录 🎶"));fs::create_directories(music);
    check(lib.addRoot(u8(music))&&lib.roots()==std::vector<std::string>{u8(music)},"Unicode root can be added");
    check(lib.addRoot(u8(music))&&lib.roots().size()==1,"duplicate source is not duplicated");
    lib.load();check(lib.roots()==std::vector<std::string>{u8(music)},"Unicode source persists through reload");
    check(lib.removeRoot(u8(music))&&lib.roots().empty(),"last source can be removed");
    lib.load();check(lib.roots().empty(),"last-source removal does not restore defaults");
    const auto child=rta::platform::run_hidden('"'+u8(self)+"\" --reload-empty \""+u8(root)+'"',true);
    std::cout<<child.output;
    check(child.started&&child.exit_code==0,"fresh process preserves removed-last-source state");
    const auto explicitProfile=root/"profiles"/"explicit-existing";
    write(explicitProfile/"rlplayer-library.conf",u8(music)+"\n");
    isolate(root,explicitProfile);lib.load();
    check(lib.roots()==std::vector<std::string>{u8(music)},"explicit existing configuration is retained");
    check(!lib.scanning()&&lib.trackCount()==0,"source management does not start scanning");
    std::cout<<"LIBRARY_SOURCES336 checks="<<checks<<" failures="<<failures<<" fixture="<<u8(root)<<"\n";
    return failures?1:0;
}
