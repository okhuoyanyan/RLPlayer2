#include "audio/multilevel_wav_stream.h"
#include <iostream>
int main(int argc,char**argv){try{if(argc!=3)return 2;rta::audio::MultilevelWavStream r(std::filesystem::u8path(argv[1]));uint64_t hash=14695981039346656037ull,n=0;while(r.remaining()){auto b=r.read_frames(std::stoul(argv[2]));for(auto v:b.raw){uint32_t u;memcpy(&u,&v,4);for(int k=0;k<4;++k){hash^=uint8_t(u>>(k*8));hash*=1099511628211ull;}}n+=b.frames();}std::cout<<n<<" "<<hash<<"\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 2;}}
