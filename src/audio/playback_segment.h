#pragma once
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <cstddef>

namespace rta::audio {
struct PlaybackSegment {
    bool enabled=false,repeat=true;
    double start=0,end=0;
    std::string file;
    size_t queueIndex=std::string::npos;
    double cueStart=-1,cueEnd=-1;
    size_t begin_frame(double rate) const{return (size_t)std::llround(start*rate);}
    size_t end_frame(double rate) const{return (size_t)std::llround(end*rate);}
    bool matches(const std::string& path,double cs,double ce,size_t index=std::string::npos)const{
        return file==path&&cueStart==cs&&cueEnd==ce&&(queueIndex==std::string::npos||index==std::string::npos||queueIndex==index);
    }
};
class PlaybackSegmentState {
    mutable std::mutex mutex_;PlaybackSegment value_;
public:
    PlaybackSegment get()const{std::lock_guard<std::mutex> lock(mutex_);return value_;}
    PlaybackSegment for_track(const std::string& file,double cs,double ce,size_t index=std::string::npos)const{
        auto s=get();if(!s.matches(file,cs,ce,index))s.enabled=false;return s;
    }
    void set(const PlaybackSegment& s){std::lock_guard<std::mutex> lock(mutex_);value_=s;}
};
}
