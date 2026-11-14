#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <cstdint>
#include <limits>
namespace rta::audio {
// WASAPI positions use opaque driver units. Only position/frequency is seconds.
// https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition
inline bool clock_position_frames(IAudioClock* clock,uint32_t rate,unsigned long long& frames) {
    if(!clock||!rate)return false;
    UINT64 frequency=0,position=0,qpc=0;
    if(FAILED(clock->GetFrequency(&frequency))||!frequency||
       FAILED(clock->GetPosition(&position,&qpc)))return false;
    constexpr auto maximum=(std::numeric_limits<uint64_t>::max)();
    const uint64_t whole=position/frequency,remainder=position%frequency;
    if(whole>maximum/rate)return false;
    uint64_t fraction=0;
    if(remainder<=maximum/rate)fraction=remainder*rate/frequency;
    else {
        // Exact floor(remainder*rate/frequency), without a 96-bit overflow.
        // This slow path handles exotic clock units, never individual samples.
        uint64_t residual=0;
        for(int bit=31;bit>=0;--bit){
            fraction*=2;
            if(residual>=frequency-residual){residual-=frequency-residual;++fraction;}
            else residual+=residual;
            if((rate>>bit)&1u){
                if(residual>=frequency-remainder){residual-=frequency-remainder;++fraction;}
                else residual+=remainder;
            }
        }
    }
    const uint64_t base=whole*rate;
    if(fraction>maximum-base)return false;
    frames=base+fraction;return true;
}
}
