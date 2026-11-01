#include "audio/rate_family_policy.h"
#include <cstdio>
#include <string>
using namespace rta::audio;
static int total=0, fails=0;
static void ck(bool condition,const char* name) {
    ++total; if(!condition){++fails;std::printf("FAIL %s\n",name);}
}
int main() {
    for(auto value:{"prefer","strict","compatible"})
        ck(valid_rate_family_policy(value) && std::string(rate_family_policy_name(parse_rate_family_policy(value))) == value,"settings roundtrip");
    ck(!valid_rate_family_policy("unknown") && parse_rate_family_policy("unknown") == RateFamilyPolicy::Prefer,"invalid configuration never enables compatibility");
    for(auto r:{11025u,22050u,44100u,88200u,176400u,352800u,705600u,1411200u}) ck(rate_family(r)==44100,"44.1 exact series");
    for(auto r:{8000u,12000u,16000u,24000u,32000u,48000u,64000u,96000u,192000u,384000u,768000u}) ck(rate_family(r)==48000,"48 exact series including 32k");
    for(auto r:{0u,12345u,44000u,7056000u,UINT32_MAX}) ck(rate_family(r)==0,"nonstandard/ambiguous rate is unknown");
    auto d=decide_pcm_rate(44100,192000,{44100,48000,96000},true);
    ck(d.ok && d.rate==44100,"lower same-family beats foreign high rate");
    d=decide_pcm_rate(176400,44100,{44100,176400,352800},true);
    ck(d.ok && d.rate==176400,"no needless source downsample");
    d=decide_pcm_rate(44100,96000,{88200,96000},true);
    ck(d.ok && d.rate==88200,"exact foreign target never overrides source family");
    d=decide_pcm_rate(32000,44100,{44100,96000},true);
    ck(d.ok && d.rate==96000 && !d.crossFamily,"32k source to 96k same-family");
    d=decide_pcm_rate(44100,44100,{48000,96000},true);
    ck(!d.ok && std::string(d.reason).find("compatible")!=std::string::npos,"cross-only device instructs compatibility setting");
    d=decide_pcm_rate(44100,96000,{48000,96000},false);
    ck(d.ok && d.rate==96000 && d.crossFamily && std::string(d.reason).find("明确指定")!=std::string::npos,"manual preferred cross exact and visible");
    ck(!decide_pcm_rate(44100,96000,{48000,96000},false,RateFamilyPolicy::Strict).ok,"manual strict cross rejected");
    ck(!decide_pcm_rate(44100,88200,{48000,96000},false,RateFamilyPolicy::Compatible).ok,"compatibility never changes explicit unsupported clock");
    d=decide_pcm_rate(44100,44100,{48000,96000},true,RateFamilyPolicy::Compatible);
    ck(d.ok && d.rate==48000 && d.crossFamily,"compatible fallback marked cross");
    d=decide_pcm_rate(44100,96000,{44100,96000},true,RateFamilyPolicy::Compatible);
    ck(d.ok && d.rate==44100,"compatibility still prefers source family");
    ck(!decide_pcm_rate(44100,44100,{},true).ok,"empty capability table fails");
    ck(!decide_pcm_rate(0,48000,{48000},true).ok,"missing source must not infer target family");
    ck(!decide_pcm_rate(44100,0,{44100},true).ok,"missing target fails");
    for(auto policy:{RateFamilyPolicy::Prefer,RateFamilyPolicy::Strict,RateFamilyPolicy::Compatible}) {
        ck(validate_pcm_rate(12345,12345,true,policy).ok,"unknown family same clock safe");
        ck(decide_pcm_rate(12345,48000,{12345,48000},true,policy).rate==12345,"unknown source exact clock preferred");
    }
    ck(!validate_pcm_rate(12345,48000,false,RateFamilyPolicy::Strict).ok,"strict unknown source changed clock blocked");
    ck(!validate_pcm_rate(44100,12345,false,RateFamilyPolicy::Strict).ok,"strict unknown destination blocked");
    ck(!validate_pcm_rate(12345,48000,true).ok,"prefer automatic unknown conversion blocked");
    ck(validate_pcm_rate(12345,48000,false).crossFamily,"prefer manual unknown conversion explicit warning");
    // Broad invariant sweep: independently check every returned rate belongs to
    // capabilities, explicit rate never changes, and automatic never abandons an
    // available source family. This catches exact-target-before-family mistakes.
    for(auto src:{11025u,22050u,32000u,44100u,48000u,96000u,176400u,12345u})
    for(auto wanted:{22050u,32000u,44100u,48000u,88200u,96000u,192000u})
    for(auto policy:{RateFamilyPolicy::Prefer,RateFamilyPolicy::Strict,RateFamilyPolicy::Compatible})
    for(auto caps:{std::vector<uint32_t>{44100,48000,88200,96000},std::vector<uint32_t>{48000,96000},std::vector<uint32_t>{22050,44100},std::vector<uint32_t>{12345,32000}}) {
        auto a=decide_pcm_rate(src,wanted,caps,true,policy);
        ck(!a.ok || std::find(caps.begin(),caps.end(),a.rate)!=caps.end(),"result belongs to device capabilities");
        bool hasSame=false;
        for(auto r:caps) if(r==src || (rate_family(src) && rate_family(r)==rate_family(src))) hasSame=true;
        ck(!hasSame || (a.ok && (a.rate==src || (rate_family(src) && rate_family(a.rate)==rate_family(src)))),"automatic available source family always wins");
        auto m=decide_pcm_rate(src,wanted,caps,false,policy);
        ck(!m.ok || m.rate==wanted,"manual exact clock invariant");
        if(policy!=RateFamilyPolicy::Compatible) ck(!a.ok || !a.crossFamily,"automatic default/strict never crosses");
    }
    std::printf("rate_family_policy: %d checks, %d failures\n",total,fails);
    return fails?1:0;
}
