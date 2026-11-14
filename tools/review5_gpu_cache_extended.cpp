// Extend the existing real HIP cache fault test without modifying production.
#define main original_cache_test_main
#include "../tests/test_gpu_multiphase_cache.cpp"
#undef main
static size_t extra_checks=0,compared=0,real_calls=0,mutants_rejected=0;
void extra(bool v,const char* name){++extra_checks;check(v,name);}
int main(){int old=original_cache_test_main();if(old)return old;
 for(auto ratio:{std::pair{2,3},std::pair{3,2},std::pair{4,3},std::pair{5,4}})for(int channels:{1,2,3,6}){
  const int L=ratio.first,M=ratio.second,T=7,H=37,N=(H*L+M-1)/M;std::vector<double>h(L*T),ph(L*T),hist(channels*H);
  for(int i=0;i<L*T;++i)h[i]=(i%9-4)/64.;for(int p=0;p<L;++p)for(int u=0;u<T;++u)ph[p*T+u]=h[p+u*L];
  for(int c=0;c<channels;++c)for(int j=0;j<H;++j)hist[c*H+j]=((j*3+c*5)%17-8)/16.;phaseSources.push_back(ph.data());
  std::vector<std::vector<double>>expected(channels,std::vector<double>(N));
  // Independent zero-insert/convolve/decimate expression, not phase-major
  // indexing or a call to another GPU/production resampler implementation.
  for(int c=0;c<channels;++c)for(int k=0;k<N;++k)for(int j=0;j<H;++j){int index=k*M-j*L;if(index>=0&&index<L*T)expected[c][k]+=hist[c*H+j]*h[index];}
  for(int chunk:{N,1,7,13}){
   std::vector<std::vector<double>>y(channels,std::vector<double>(N));std::vector<double*>ptr(channels);
   for(int k=0;k<N;k+=chunk){int n=std::min(chunk,N-k);for(int c=0;c<channels;++c)ptr[c]=y[c].data()+k;int got=rta_gpu_resample_compact_planar(hist.data(),channels,H,ph.data(),L,M,T,k,0,n,n,ptr.data());++real_calls;extra(got==n,"real compact rational block executes");}
   for(int c=0;c<channels;++c)for(int k=0;k<N;++k){++compared;extra(std::memcmp(&y[c][k],&expected[c][k],8)==0,"real rational phase/block matches independent direct convolution");}
  }
  if(channels==1){
   for(int mode:{0,1}){
    std::vector<double>bad=ph,y(N);if(mode==0)std::reverse(bad.begin(),bad.end());phaseSources.push_back(bad.data());double*ptr[]={y.data()};
    if(mode==0){int got=rta_gpu_resample_compact_planar(hist.data(),1,H,bad.data(),L,M,T,0,0,N,N,ptr);++real_calls;extra(got==N,"mutant layout still really executes");}
    else for(int k=0;k<N;k+=7){int n=std::min(7,N-k);ptr[0]=y.data()+k;int got=rta_gpu_resample_compact_planar(hist.data(),1,H,ph.data(),L,M,T,0,0,n,n,ptr);++real_calls;extra(got==n,"mutant reset clock still really executes");}
    bool differs=false;for(int k=0;k<N;++k)differs|=std::memcmp(&y[k],&expected[0][k],8)!=0;extra(differs,"independent oracle rejects wrong layout or per-block clock restart");mutants_rejected+=differs;
   }
  }
 }
 std::printf("EXTENDED_CACHE checks=%zu compared=%zu real_calls=%zu mutants_rejected=%zu errors=%d audio_device_calls=0\n",extra_checks,compared,real_calls,mutants_rejected,errors);return errors?1:0;}
