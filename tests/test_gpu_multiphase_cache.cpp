// Developer-only cache regression; build with HIP or the CPU runtime fixture.
// Includes the actual production implementation; no test ABI in shipped DLLs.
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
static std::vector<const void*> phaseSources;
static int phaseUploads = 0;
static bool failPhaseCopy = false;
static bool failAllocation = false;
template<class T> static hipError_t countedMalloc(T** p, size_t bytes) {
    if (failAllocation) { failAllocation = false; return hipErrorOutOfMemory; }
    return hipMalloc(p, bytes);
}
static hipError_t countedCopy(void* dst, const void* src, size_t bytes, hipMemcpyKind kind) {
    if (kind == hipMemcpyHostToDevice &&
        std::find(phaseSources.begin(), phaseSources.end(), src) != phaseSources.end()) {
        ++phaseUploads;
        if (failPhaseCopy) {
            failPhaseCopy = false;
            if (bytes >= 16) (void)hipMemcpy(dst, src, bytes / 2, kind);
            return hipErrorInvalidValue;
        }
    }
    return hipMemcpy(dst, src, bytes, kind);
}
#define hipMalloc countedMalloc
#define hipMemcpy countedCopy
#include "../src/gpu/gpu_resample_hip.cpp"
#undef hipMalloc
#undef hipMemcpy

static int errors = 0;
static void check(bool pass, const char* message) {
    if (!pass) { ++errors; std::printf("FAIL %s\n", message); }
}
__global__ void constant_probe(double* p) { p[threadIdx.x] = 1.25; }
static bool probe() {
    double* p = nullptr; double out[4]{};
    if (hipMalloc(&p, sizeof(out)) != hipSuccess) return false;
    (void)hipGetLastError();
    hipLaunchKernelGGL(constant_probe, dim3(1), dim3(4), 0, 0, p);
    auto launch = hipGetLastError(); auto sync = hipDeviceSynchronize();
    auto copy = hipMemcpy(out, p, sizeof(out), hipMemcpyDeviceToHost); hipFree(p);
    std::printf("PROBE launch=%d sync=%d copy=%d values=%.2f,%.2f,%.2f,%.2f\n",
        (int)launch, (int)sync, (int)copy, out[0], out[1], out[2], out[3]);
    return launch == hipSuccess && sync == hipSuccess && copy == hipSuccess &&
        out[0] == 1.25 && out[1] == 1.25 && out[2] == 1.25 && out[3] == 1.25;
}
static int run(const std::vector<double>& phases, bool compact = true) {
    const int L = 2, T = (int)phases.size() / L;
    // Powers of two make multiply/add exactly representable on both targets.
    std::vector<double> hist(2 * 32); for (size_t i=0;i<hist.size();++i) hist[i]=(double)(i % 11)*0.125;
    double a[16]{}, b[16]{}; double* planes[]{a,b}; double inter[32]{};
    int bases[16], indices[16]; for(int n=0;n<16;++n){bases[n]=n/2;indices[n]=n%2;}
    int rc = compact ? rta_gpu_resample_compact_planar(hist.data(),2,32,phases.data(),L,1,T,0,0,16,16,planes)
        : rta_gpu_resample(hist.data(),2,32,phases.data(),L,T,bases,indices,16,inter);
    if (rc != 16) return rc;
    for(int c=0;c<2;++c) for(int n=0;n<16;++n) {
        double expected=0; for(int u=0;u<std::min(n/2+1,T);++u) expected+=phases[(n%2)*T+u]*hist[c*32+n/2-u];
        double actual=compact?planes[c][n]:inter[n*2+c];
        check(std::memcmp(&actual,&expected,sizeof(double))==0,"output equals scalar reference");
    }
    return rc;
}
int main() {
    check(probe(),"constant kernel executes and reads back four constants");
    if(errors) return 1;
    std::vector<double> a(8,0.25),b(8,0.5),c(8,0.75),big(128,0.125);
    phaseSources={a.data(),b.data(),c.data(),big.data()};
    rta_gpu_resample_invalidate(); phaseUploads=0;
    for(int i=0;i<8;++i) check(run(i%2?b:a)==16,"ABAB compact succeeds");
    std::printf("ABAB calls=8 phase_uploads=%d expected=2\n",phaseUploads);
    check(phaseUploads==2,"ABAB retains both coefficient tables");
    check(run(a,false)==16,"legacy ABI output");
    check(phaseUploads==2,"both ABIs share coefficient identity");
    a[0]=0.375; check(run(a)==16,"same pointer changed content");
    check(phaseUploads==3,"content mutation uploads again");
    rta_gpu_resample_invalidate(); int before=phaseUploads;
    check(run(a)==16&&run(b)==16,"invalidate reruns");
    check(phaseUploads==before+2,"invalidate clears all slots");
    before=phaseUploads;
    check(run(a)==16&&run(c)==16&&run(a)==16,"LRU reuse");
    check(phaseUploads==before+1,"recent A retained when C evicts B");
    check(run(b)==16,"evicted B restored"); check(phaseUploads==before+2,"evicted B upload");
    failPhaseCopy=true; c[1]=0.875;
    check(run(c)<0,"partial phase upload failure returned");
    before=phaseUploads;
    check(run(c)==16,"partial phase upload retry correct");
    check(phaseUploads==before+1,"partial copy cannot publish stale key");
    failAllocation=true; check(run(big)<0,"allocation failure returned");
    check(run(c)==16,"allocation failure preserves existing cached data");
    check(run(big)==16,"allocation growth retry correct");
    std::printf("CACHE_RESULT errors=%d phase_uploads=%d\n",errors,phaseUploads);
    return errors?1:0;
}
