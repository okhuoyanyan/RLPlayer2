#include "net/update_check.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
using namespace rta::update;
namespace fs=std::filesystem;
static int checks=0;
static void require(bool ok,const char* msg){++checks;if(!ok)throw std::runtime_error(msg);}
static std::string utf8(const fs::path& p){auto s=p.u8string();return std::string(reinterpret_cast<const char*>(s.data()),s.size());}
int main(){fs::path root;try{
 root=fs::temp_directory_path()/fs::u8path("rl-update-unicode-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 const char* names[]={"ascii-parent/ascii.txt","中文父目录/中文文档.txt","日本語ディレクトリ/日本語文書.txt","emoji-🎵-parent/file-🎧.txt","中文_日本語_🎵/文档_文書_🎧.txt"};
 const std::string expected="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
 for(auto name:names){const auto file=root/fs::u8path(name);fs::create_directories(file.parent_path());{std::ofstream out(file,std::ios::binary);out<<"abc";require(bool(out),"fixture write");}
  std::string digest,err;require(sha256_file(utf8(file),digest,err),"UTF8 file SHA open");require(digest==expected,"official abc SHA vector");
  auto v=verify_downloaded_file(utf8(file),expected,3);require(v.ok,"UTF8 verification and size accepted");
  v=verify_downloaded_file(utf8(file),expected,4);require(!v.ok&&v.reason==VerifyFailReason::SizeMismatch,"UTF8 wrong size rejected distinctly");
  {std::fstream out(file,std::ios::binary|std::ios::in|std::ios::out);out<<'z';}
  v=verify_downloaded_file(utf8(file),expected,3);require(!v.ok&&v.reason==VerifyFailReason::Sha256Mismatch,"same size one byte mutation rejected");
 }
 fs::remove_all(root);std::cout<<"PASS "<<checks<<" / FAIL 0\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<" after "<<checks<<" checks\n";if(!root.empty())fs::remove_all(root);return 1;}}
