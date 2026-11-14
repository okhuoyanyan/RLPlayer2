#include "dsp/resampler.h"
#include <cstdio>
int main() { std::printf("%s\n",rta::dsp::filter_cache::key(rta::dsp::filter_cache::signature({4096,.125,180},"round")).c_str()); }
