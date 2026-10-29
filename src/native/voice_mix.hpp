#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
namespace apex {
// Single-producer single-consumer ring of interleaved float frames: the song decoder writes, the voice render thread reads.
class FrameRing {
    std::vector<float> data_;size_t channels_,capacity_;std::atomic<size_t> read_=0,write_=0;
public:
    FrameRing(size_t channels,size_t frames):data_(channels*frames),channels_(channels),capacity_(frames){}
    size_t available()const{return write_.load(std::memory_order_acquire)-read_.load(std::memory_order_relaxed);}
    size_t space()const{return capacity_-(write_.load(std::memory_order_relaxed)-read_.load(std::memory_order_acquire));}
    size_t write(const float* in,size_t frames){
        frames=std::min(frames,space());size_t at=write_.load(std::memory_order_relaxed);
        for(size_t i=0;i<frames;++i)std::memcpy(&data_[(at+i)%capacity_*channels_],in+i*channels_,channels_*sizeof(float));
        write_.store(at+frames,std::memory_order_release);return frames;
    }
    size_t read(float* out,size_t frames){
        frames=std::min(frames,available());size_t at=read_.load(std::memory_order_relaxed);
        for(size_t i=0;i<frames;++i)std::memcpy(out+i*channels_,&data_[(at+i)%capacity_*channels_],channels_*sizeof(float));
        read_.store(at+frames,std::memory_order_release);return frames;
    }
};
// Bridges the microphone clock to the virtual cable clock. Samples pass through unchanged; when the clocks drift apart
// the smoothed fill leaves a band around the level found after start-up, and one frame is dropped or repeated at the
// flattest point of a block. This keeps every other sample bit-exact instead of resampling the whole stream.
// The fill is measured including the frames the microphone has captured since its last packet; otherwise the packet
// phase, which slips slowly between the two clocks, makes the measured fill jump by a whole packet.
class DriftFifo {
    std::vector<float> data_;size_t channels_,prime_,band_,limit_,smoothing_,settle_;
    double average_=0,baseline_=0;size_t settled_=0;bool primed_=false;
    size_t flattest(size_t frames)const{
        size_t best=frames/2;double lowest=1e30;
        for(size_t i=1;i+1<frames;++i){double slope=0;for(size_t c=0;c<channels_;++c)slope+=std::abs(data_[(i+1)*channels_+c]-data_[(i-1)*channels_+c]);if(slope<lowest){lowest=slope;best=i;}}
        return best;
    }
public:
    long long dropped=0,repeated=0,underruns=0,overflows=0;
    // prime: frames buffered before output starts; band: allowed deviation of the smoothed fill; limit: extra latency
    // after which old audio is discarded; smoothing and settle are in frames of output.
    DriftFifo(size_t channels,size_t prime,size_t band,size_t limit,size_t smoothing,size_t settle)
        :channels_(channels),prime_(prime),band_(band),limit_(limit),smoothing_(smoothing),settle_(settle){}
    size_t frames()const{return data_.size()/channels_;}
    double baseline()const{return baseline_;}
    void reset(){data_.clear();primed_=false;settled_=0;}
    void push(const float* in,size_t frames){
        data_.insert(data_.end(),in,in+frames*channels_);
        // A stalled cable must not build up latency.
        if(primed_&&settled_>=settle_&&double(this->frames())>baseline_+double(limit_)){
            size_t drop=this->frames()-size_t(baseline_);data_.erase(data_.begin(),data_.begin()+drop*channels_);average_=baseline_;++overflows;}
    }
    void pull(float* out,size_t frames,double pending=0){
        if(!primed_){
            if(this->frames()<prime_+frames){std::fill(out,out+frames*channels_,0.f);return;}
            primed_=true;settled_=0;average_=double(this->frames())+pending;
        }
        double alpha=std::min(1.0,double(frames)/double(smoothing_));average_+=(double(this->frames())+pending-average_)*alpha;
        if(settled_<settle_){settled_+=frames;baseline_=average_;}
        int adjust=settled_<settle_?0:average_>baseline_+double(band_)?1:average_<baseline_-double(band_)?-1:0;
        size_t take=frames+(adjust>0)-(adjust<0);
        if(this->frames()<take){
            size_t have=std::min(this->frames(),frames);std::memcpy(out,data_.data(),have*channels_*sizeof(float));
            std::fill(out+have*channels_,out+frames*channels_,0.f);data_.erase(data_.begin(),data_.begin()+have*channels_);
            primed_=false;++underruns;return;
        }
        if(adjust==0)std::memcpy(out,data_.data(),frames*channels_*sizeof(float));
        else{
            size_t q=flattest(take),c=channels_;
            if(adjust>0){std::memcpy(out,data_.data(),q*c*sizeof(float));std::memcpy(out+q*c,&data_[(q+1)*c],(take-q-1)*c*sizeof(float));++dropped;average_-=1;}
            else{std::memcpy(out,data_.data(),(q+1)*c*sizeof(float));std::memcpy(out+(q+1)*c,&data_[q*c],(take-q)*c*sizeof(float));++repeated;average_+=1;}
        }
        data_.erase(data_.begin(),data_.begin()+take*channels_);
    }
};
// Linear gain ramp (full scale over rampFrames) so the song starts and stops without a click and volume changes glide.
class SongGain {
    double current_=0,target_=0,step_;
public:
    explicit SongGain(double rampFrames):step_(1/std::max(1.0,rampFrames)){}
    void set(double target){target_=target;}
    void jump(double value){current_=target_=value;}
    bool silent()const{return current_==0&&target_==0;}
    void apply(float* samples,size_t frames,size_t channels){
        for(size_t i=0;i<frames;++i){
            if(current_!=target_)current_=current_<target_?std::min(target_,current_+step_):std::max(target_,current_-step_);
            for(size_t c=0;c<channels;++c)samples[i*channels+c]*=float(current_);
        }
    }
};
// The microphone samples are left untouched; only the song is added.
inline void mixInto(float* out,const float* song,size_t samples){for(size_t i=0;i<samples;++i)out[i]+=song[i];}
// Maps interleaved channels. Mono is copied to every output channel; extra input channels are dropped, except that
// stereo into mono is averaged.
inline void mapChannels(const float* in,size_t inChannels,float* out,size_t outChannels,size_t frames){
    for(size_t i=0;i<frames;++i){const float* f=in+i*inChannels;float* o=out+i*outChannels;
        if(outChannels==1&&inChannels==2)o[0]=(f[0]+f[1])*.5f;
        else for(size_t c=0;c<outChannels;++c)o[c]=inChannels==1?f[0]:c<inChannels?f[c]:0.f;}
}
enum class PcmFormat{Float32,Int16,Int24,Int32};
inline size_t pcmBytes(PcmFormat format){return format==PcmFormat::Int16?2:format==PcmFormat::Int24?3:4;}
// Integer conversions scale by 2^(bits-1) and round, so 16- and 24-bit microphone samples (which Windows delivers as
// exact floats) come back out with their original values.
inline void writePcm(const float* in,size_t samples,uint8_t* out,PcmFormat format){
    for(size_t i=0;i<samples;++i){
        double x=std::isfinite(in[i])?std::clamp(double(in[i]),-1.0,1.0):0.0;
        if(format==PcmFormat::Float32){float v=float(x);std::memcpy(out+i*4,&v,4);continue;}
        int bits=format==PcmFormat::Int16?16:format==PcmFormat::Int24?24:32;double scale=std::ldexp(1.0,bits-1);
        int64_t v=std::clamp<int64_t>(std::llround(x*scale),-int64_t(scale),int64_t(scale)-1);
        if(format==PcmFormat::Int16){int16_t s=int16_t(v);std::memcpy(out+i*2,&s,2);}
        else if(format==PcmFormat::Int24){uint32_t u=uint32_t(int32_t(v));out[i*3]=uint8_t(u);out[i*3+1]=uint8_t(u>>8);out[i*3+2]=uint8_t(u>>16);}
        else{int32_t s=int32_t(v);std::memcpy(out+i*4,&s,4);}
    }
}
inline void readPcm(const uint8_t* in,size_t samples,float* out,PcmFormat format){
    for(size_t i=0;i<samples;++i){
        if(format==PcmFormat::Float32)std::memcpy(out+i,in+i*4,4);
        else if(format==PcmFormat::Int16){int16_t s;std::memcpy(&s,in+i*2,2);out[i]=float(s/32768.0);}
        else if(format==PcmFormat::Int24){int32_t s=int32_t(uint32_t(in[i*3])<<8|uint32_t(in[i*3+1])<<16|uint32_t(in[i*3+2])<<24)>>8;out[i]=float(s/8388608.0);}
        else{int32_t s;std::memcpy(&s,in+i*4,4);out[i]=float(s/2147483648.0);}
    }
}
}
