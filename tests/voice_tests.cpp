// Virtual microphone mixing: clock-drift bridging, PCM conversion and song gain. Portable C++20.
#include "../src/native/voice_mix.hpp"
#include <cstdio>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
using namespace apex;
namespace {
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
// Microphone frames arrive in 10 ms packets on one clock and the cable asks for 10 ms periods on another. Samples
// carry their frame index so drops and repeats are visible.
struct Bridge{std::vector<float> out;long long underrunsAfterStart=0;double minFill=1e9,maxFill=0;};
Bridge simulate(double ppm,double seconds,size_t channels=2,double phase=.0037){
    DriftFifo fifo(channels,720,240,4800,24000,48000);Bridge result;
    double micPeriod=.01*(1-ppm*1e-6),nextMic=0,nextCable=phase,lastMic=0;size_t produced=0;std::vector<float> block(480*channels);bool started=false;
    while(nextCable<seconds){
        if(nextMic<=nextCable){for(size_t i=0;i<480;++i)for(size_t c=0;c<channels;++c)block[i*channels+c]=float(produced+i+1)+float(c)*.25f;fifo.push(block.data(),480);produced+=480;lastMic=nextMic;nextMic+=micPeriod;}
        else{long long before=fifo.underruns;fifo.pull(block.data(),480,(nextCable-lastMic)*48000/(1-ppm*1e-6));if(started&&fifo.underruns>before)++result.underrunsAfterStart;
            for(size_t i=0;i<480;++i)if(block[i*channels]!=0||started){started=true;result.out.insert(result.out.end(),block.begin()+i*channels,block.begin()+(i+1)*channels);}
            if(started&&nextCable>3){result.minFill=std::min(result.minFill,double(fifo.frames()));result.maxFill=std::max(result.maxFill,double(fifo.frames()));}
            nextCable+=.01;}
    }
    return result;
}
void testSameClockIsBitExact(){
    auto r=simulate(0,120);size_t frames=r.out.size()/2;require(frames>119*48000,"same-clock output too short");
    for(size_t i=0;i<frames;++i)require(r.out[i*2]==float(i+1)&&r.out[i*2+1]==float(i+1)+.25f,"same-clock output changed sample "+std::to_string(i));
    require(r.underrunsAfterStart==0,"same-clock underrun");
}
void testDriftCorrections(double ppm){
    double seconds=300;auto r=simulate(ppm,seconds);size_t frames=r.out.size()/2;long long drops=0,repeats=0;
    for(size_t i=1;i<frames;++i){
        float step=r.out[i*2]-r.out[(i-1)*2];require(step==0||step==1||step==2,"drift output jumped by "+std::to_string(step));
        require(r.out[i*2+1]==r.out[i*2]+.25f,"channels separated by a correction");
        drops+=step==2;repeats+=step==0;
    }
    // The first band's worth of drift is absorbed by the buffer before corrections start.
    double expected=std::abs(ppm)*1e-6*48000*seconds-240;
    long long corrections=ppm>0?drops:repeats;
    require((ppm>0?repeats:drops)==0,"corrections in both directions");
    require(std::abs(corrections-expected)<expected*.03+40,"corrections "+std::to_string(corrections)+" vs expected "+std::to_string(expected));
    require(r.underrunsAfterStart==0,"drift underrun");
    require(r.maxFill-r.minFill<480+2*240+120,"fill wandered "+std::to_string(r.maxFill-r.minFill));
    std::printf("PASS drift %+.0f ppm: %lld corrections in %.0f s, fill %.0f..%.0f frames\n",ppm,corrections,seconds,r.minFill,r.maxFill);
}
void testStallDiscardsLatency(){
    DriftFifo fifo(1,720,240,4800,24000,48000);std::vector<float> block(480,.1f);
    for(int i=0;i<400;++i){fifo.push(block.data(),480);fifo.pull(block.data(),480);}
    for(int i=0;i<30;++i){fifo.push(block.data(),480);require(fifo.frames()<=fifo.baseline()+4800+480,"latency built up");}   // the cable stopped for 300 ms
    require(fifo.overflows>0&&fifo.underruns==0,"stalled cable kept "+std::to_string(fifo.frames())+" frames");
}
void testPcm(){
    std::vector<float> samples;for(int s=-32768;s<32768;++s)samples.push_back(float(s/32768.0));
    for(auto format:{PcmFormat::Int16,PcmFormat::Int24,PcmFormat::Int32,PcmFormat::Float32}){
        std::vector<uint8_t> bytes(samples.size()*pcmBytes(format));writePcm(samples.data(),samples.size(),bytes.data(),format);
        std::vector<float> back(samples.size());readPcm(bytes.data(),samples.size(),back.data(),format);
        for(size_t i=0;i<samples.size();++i)require(back[i]==samples[i],"16-bit value changed through PCM format "+std::to_string(int(format)));
    }
    std::mt19937 random(7);std::vector<float> wide;for(int i=0;i<200000;++i)wide.push_back(float(int32_t(random()%16777216)-8388608)/8388608.f);
    for(auto format:{PcmFormat::Int24,PcmFormat::Int32,PcmFormat::Float32}){
        std::vector<uint8_t> bytes(wide.size()*pcmBytes(format));writePcm(wide.data(),wide.size(),bytes.data(),format);
        std::vector<float> back(wide.size());readPcm(bytes.data(),wide.size(),back.data(),format);
        for(size_t i=0;i<wide.size();++i)require(back[i]==wide[i],"24-bit value changed through PCM format "+std::to_string(int(format)));
    }
    float loud[]={1.5f,-2.f,1.f,-1.f,NAN};int16_t pcm[5];writePcm(loud,5,reinterpret_cast<uint8_t*>(pcm),PcmFormat::Int16);
    require(pcm[0]==32767&&pcm[1]==-32768&&pcm[2]==32767&&pcm[3]==-32768&&pcm[4]==0,"PCM clipping");
}
void testRingAndChannels(){
    FrameRing ring(2,100);std::vector<float> in(2*70),out(2*100);long long next=0,expect=0;
    for(int round=0;round<50;++round){
        for(size_t i=0;i<in.size();i+=2){in[i]=float(next);in[i+1]=-float(next);++next;}
        size_t wrote=ring.write(in.data(),70);next-=70-wrote;size_t got=ring.read(out.data(),45+round%30);
        for(size_t i=0;i<got;++i){require(out[i*2]==float(expect)&&out[i*2+1]==-float(expect),"ring order");++expect;}
    }
    float mono[]={.5f,-.25f},stereo[4];mapChannels(mono,1,stereo,2,2);
    require(stereo[0]==.5f&&stereo[1]==.5f&&stereo[2]==-.25f&&stereo[3]==-.25f,"mono to stereo");
}
void testSongGain(){
    SongGain gain(480);gain.set(.25);std::vector<float> ones(960,1.f);gain.apply(ones.data(),960,1);
    require(ones[0]>0&&ones[0]<.01f&&ones[119]==.25f&&ones[959]==.25f,"song fade-in");
    for(size_t i=1;i<120;++i)require(ones[i]>ones[i-1],"fade-in not rising");
    gain.set(0);std::fill(ones.begin(),ones.end(),1.f);gain.apply(ones.data(),960,1);
    require(ones[0]<.25f&&ones[200]==0&&gain.silent(),"song fade-out");
    float mic[]={.5f,-.5f},song[]={.25f,.25f};mixInto(mic,song,2);require(mic[0]==.75f&&mic[1]==-.25f,"mix");
}
}
int main(){
    try{
        testSameClockIsBitExact();std::puts("PASS same clock: microphone samples pass through bit-exact");
        testDriftCorrections(150);testDriftCorrections(-150);testDriftCorrections(1500);
        testStallDiscardsLatency();std::puts("PASS stalled cable discards built-up latency");
        testPcm();std::puts("PASS PCM formats keep 16/24-bit microphone samples exact and clip safely");
        testRingAndChannels();std::puts("PASS song ring buffer and channel mapping");
        testSongGain();std::puts("PASS song fades and mixing");
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
    return 0;
}
