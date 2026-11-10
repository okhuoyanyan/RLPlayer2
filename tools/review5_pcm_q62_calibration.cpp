// Test the same generic exact Q62->DD conversion used by the finite manifest.
#define main manifest_export_main
#include "review5_pcm_manifest.cpp"
#undef main
#include <bit>
int main(int argc,char**argv){if(argc!=2)return 2;std::vector<int64_t>v={INT64_MIN,INT64_MIN+1,-(INT64_C(1)<<53)-1,-INT64_C(4294967297),-1,0,1,INT64_C(4294967297),(INT64_C(1)<<53)+1,INT64_MAX-1,INT64_MAX};uint64_t z=20261103;for(int i=0;i<256;++i){z=z*UINT64_C(6364136223846793005)+1;v.push_back(std::bit_cast<int64_t>(z));}std::ofstream o(argv[1]);o<<std::setprecision(17)<<"[";for(size_t i=0;i<v.size();++i){if(i)o<<",";auto d=q62(v[i]);o<<"{\"q\":"<<v[i]<<",\"hi\":"<<d.h<<",\"lo\":"<<d.l<<"}";}o<<"]";return !o;}
