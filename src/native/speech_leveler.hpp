#pragma once
// Speech-driven level balancing for the microphone (track 2) and the other-desktop track (track 3).
// Every 10 ms frame is analysed: voiced speech needs a pitch in the human range, energy above the tracked noise
// floor and a pitch that moves with intonation while the harmonics and formants change. Notification sounds are
// rejected because they hold a fixed pitch, keep an unchanged spectrum of a few strong lines while they ring, or
// have a fundamental above the voice range. Only confirmed speech updates the measured speaking level of that
// track; the gain brings it to a common target, and sounds outside speech are never boosted above unity. A 300 ms
// lookahead lets the boost start with the first syllable and be withdrawn from a sound that turns out to be a
// chime, and a lookahead peak limiter bounds the result. Output keeps the input sample timestamps.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>
namespace apex {
struct Biquad {
    double b0,b1,b2,a1,a2,z1=0,z2=0;
    double process(double x){double y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;
        if(std::abs(z1)<1e-25)z1=0;if(std::abs(z2)<1e-25)z2=0;  // digital silence must not decay into denormals
        return y;}
};
namespace speech {
constexpr int rate=48000,hop=480,analysisHop=160,window=512,fftSize=1024;
constexpr double pi=3.14159265358979323846,binHz=16000.0/fftSize;
inline Biquad lowpass(double frequency,double q,double sampleRate){
    double w=2*pi*frequency/sampleRate,alpha=std::sin(w)/(2*q),c=std::cos(w),a0=1+alpha;
    return {(1-c)/2/a0,(1-c)/a0,(1-c)/2/a0,-2*c/a0,(1-alpha)/a0};
}
// Radix-2 FFT with precomputed bit reversal and twiddles.
class Fft {
    std::vector<size_t> reverse_;std::vector<std::complex<double>> twiddle_;
public:
    explicit Fft(size_t n):reverse_(n),twiddle_(n/2){
        for(size_t i=1,j=0;i<n;++i){size_t bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;reverse_[i]=j;}
        for(size_t k=0;k<n/2;++k)twiddle_[k]=std::polar(1.0,-2*pi*double(k)/double(n));
    }
    void operator()(std::vector<std::complex<double>>& a,bool inverse)const{
        size_t n=a.size();for(size_t i=1;i<n;++i)if(i<reverse_[i])std::swap(a[i],a[reverse_[i]]);
        for(size_t length=2;length<=n;length<<=1){size_t stride=n/length;
            for(size_t i=0;i<n;i+=length)for(size_t j=0;j<length/2;++j){
                auto w=twiddle_[j*stride];if(inverse)w=std::conj(w);auto u=a[i+j],v=a[i+j+length/2]*w;a[i+j]=u+v;a[i+j+length/2]=u-v;}}
        if(inverse)for(auto& x:a)x/=double(n);
    }
};
inline double decibels(double power){return 10*std::log10(std::max(power,1e-13));}
}
// Analyses 10 ms hops of 16 kHz mono audio over a 32 ms Hann window.
class VoiceDetector {
public:
    struct Frame{double voiceDb=-130,snrDb=0,periodicity=0,pitchHz=0,peakiness=0,voiceRatio=0;int lines=0;double similarity=0,chimeScore=0;bool voiced=false,tonal=false,strongTone=false,candidate=false,flatPitch=false,steady=false,chime=false;};
private:
    std::array<double,speech::window> buffer_{};int filled_=0;double floorDb_=-110;bool floorReady_=false;
    std::vector<double> hann_,windowAc_,power_;std::vector<std::complex<double>> work_;std::deque<double> pitchRun_;std::deque<std::vector<double>> shapes_;std::deque<bool> steadyRun_;int unvoiced_=0;double sumW2_=0,chimeScore_=0;speech::Fft fft_{speech::fftSize};
    static int bin(double hz){return static_cast<int>(std::lround(hz/speech::binHz));}
public:
    VoiceDetector():hann_(speech::window),windowAc_(speech::window),power_(speech::fftSize/2+1),work_(speech::fftSize){
        for(int i=0;i<speech::window;++i){hann_[i]=.5-.5*std::cos(2*speech::pi*(i+.5)/speech::window);sumW2_+=hann_[i]*hann_[i];}
        for(int lag=0;lag<speech::window;++lag){double s=0;for(int i=0;i+lag<speech::window;++i)s+=hann_[i]*hann_[i+lag];windowAc_[lag]=s/sumW2_;}
    }
    double floorDb()const{return floorDb_;}
    int pitchRun()const{return static_cast<int>(std::max(pitchRun_.size(),steadyRun_.size()));}
    Frame analyze(const double* input){
        std::move(buffer_.begin()+speech::analysisHop,buffer_.end(),buffer_.begin());
        std::copy(input,input+speech::analysisHop,buffer_.end()-speech::analysisHop);
        filled_=std::min(speech::window,filled_+speech::analysisHop);
        Frame f;if(filled_<speech::window)return f;
        double mean=0;for(double x:buffer_)mean+=x;mean/=speech::window;
        for(int i=0;i<speech::fftSize;++i)work_[i]=i<speech::window?(buffer_[i]-mean)*hann_[i]:0.0;
        fft_(work_,false);
        for(int k=0;k<=speech::fftSize/2;++k)power_[k]=std::norm(work_[k]);
        double scale=2.0/(speech::fftSize*sumW2_),voice=0,total=0,band=0;
        const int voiceLo=bin(200),voiceHi=bin(4000),totalLo=bin(100),totalHi=speech::fftSize/2-1,toneLo=bin(200),toneHi=bin(6000);
        for(int k=totalLo;k<=totalHi;++k){total+=power_[k];if(k>=voiceLo&&k<=voiceHi)voice+=power_[k];if(k>=toneLo&&k<=toneHi)band+=power_[k];}
        f.voiceDb=speech::decibels(voice*scale);f.voiceRatio=total>0?voice/total:0;
        // Share of the band held by its three strongest spectral peaks: ~1 for tones and chimes,
        // clearly lower for speech, whose energy spreads over many harmonics and formants.
        if(band>0){
            std::vector<int> peaks;for(int k=toneLo+1;k<toneHi;++k)if(power_[k]>=power_[k-1]&&power_[k]>power_[k+1])peaks.push_back(k);
            std::sort(peaks.begin(),peaks.end(),[&](int a,int b){return power_[a]>power_[b];});
            std::vector<int> chosen;for(int k:peaks){if(chosen.size()==3)break;
                if(std::all_of(chosen.begin(),chosen.end(),[&](int c){return std::abs(c-k)>4;}))chosen.push_back(k);}
            double held=0;for(int c:chosen)for(int k=std::max(toneLo,c-2);k<=std::min(toneHi,c+2);++k)held+=power_[k];
            f.peakiness=std::min(1.0,held/band);
            // Separate spectral lines within 25 dB of the strongest: a voice shows many harmonics, a ping or chime a few.
            std::vector<int> lines;if(!peaks.empty())for(int k:peaks){if(power_[k]<power_[peaks.front()]*.00316)break;
                if(std::all_of(lines.begin(),lines.end(),[&](int c){return std::abs(c-k)>4;}))lines.push_back(k);}
            f.lines=static_cast<int>(lines.size());
        }
        // Spectral similarity with 40 ms earlier: a ringing chime keeps its lines, a voice moves its harmonics and formants.
        {std::vector<double> shape;shape.reserve(voiceHi-voiceLo+1);double norm=0;for(int k=voiceLo;k<=voiceHi;++k){double m=std::sqrt(power_[k]);shape.push_back(m);norm+=m*m;}
            norm=std::sqrt(norm);if(norm>0)for(double& m:shape)m/=norm;
            if(shapes_.size()>=4&&norm>0){double dot=0;for(size_t i=0;i<shape.size();++i)dot+=shape[i]*shapes_.front()[i];f.similarity=dot;}
            shapes_.push_back(std::move(shape));if(shapes_.size()>4)shapes_.pop_front();}
        // Normalised autocorrelation of the 60-3500 Hz band (Boersma window correction).
        const int acLo=bin(60),acHi=bin(3500);
        for(int k=0;k<=speech::fftSize/2;++k){double p=k>=acLo&&k<=acHi?power_[k]:0;work_[k]=p;if(k>0&&k<speech::fftSize/2)work_[speech::fftSize-k]=p;}
        fft_(work_,true);
        double r0=work_[0].real();
        if(r0>0){
            auto r=[&](int lag){return work_[lag].real()/r0/std::max(windowAc_[lag],1e-3);};
            constexpr int lagLo=16000/400,lagHi=16000/75,shortLo=16000/2000;
            double best=0,shortBest=0;std::vector<std::pair<int,double>> maxima;
            for(int lag=lagLo;lag<=lagHi;++lag){double v=r(lag);if(v>r(lag-1)&&v>=r(lag+1)){maxima.push_back({lag,v});best=std::max(best,v);}}
            for(int lag=shortLo;lag<lagLo;++lag){double v=r(lag);if(v>r(lag-1)&&v>=r(lag+1))shortBest=std::max(shortBest,v);}
            // The first strong peak avoids octave jumps between frames of the same sound.
            for(auto [lag,v]:maxima)if(v>=.9*best){
                double a=r(lag-1),b=v,c=r(lag+1),d=a-2*b+c,offset=std::abs(d)>1e-12?std::clamp(.5*(a-c)/d,-.5,.5):0;
                f.pitchHz=16000/(lag+offset);f.periodicity=v;break;}
            bool highFundamental=shortBest>.85&&shortBest>=best-.03;
            f.voiced=f.periodicity>.5&&!highFundamental;
            f.strongTone=highFundamental;
        }
        if(f.voiced){pitchRun_.push_back(f.pitchHz);if(pitchRun_.size()>12)pitchRun_.pop_front();}else pitchRun_.clear();
        if(pitchRun_.size()>=8){auto [lo,hi]=std::minmax_element(pitchRun_.begin(),pitchRun_.end());f.flatPitch=*hi/ *lo-1<.003;}
        // Most recent voiced frames nearly unchanged after 40 ms: a ringing chime or held note, not a voice.
        unvoiced_=f.voiced?0:unvoiced_+1;if(unvoiced_>20)steadyRun_.clear();
        if(f.voiced){steadyRun_.push_back(f.similarity>.85);if(steadyRun_.size()>10)steadyRun_.pop_front();}
        f.steady=steadyRun_.size()>=6&&std::count(steadyRun_.begin(),steadyRun_.end(),true)*10>=int(steadyRun_.size())*7&&f.voiced;
        // Chime score: voiced frames that are both sparse (a few strong lines) and unchanged count fully. Speech
        // averages about 0.2, ringing notification sounds about 0.8.
        if(unvoiced_>20)chimeScore_=0;
        if(f.voiced){double t=(f.peakiness>.9&&f.lines<=6?.5:0)+(f.similarity>.85?.5:0);chimeScore_+=(t-chimeScore_)/6;}
        f.chimeScore=chimeScore_;f.chime=f.voiced&&chimeScore_>.6;
        f.strongTone=f.strongTone||(f.lines<=1&&f.peakiness>.9)||f.chime;
        f.tonal=f.strongTone||f.flatPitch||f.steady;
        if(!floorReady_){floorDb_=f.voiceDb;floorReady_=true;}
        f.snrDb=f.voiceDb-floorDb_;
        // Steady or flat-pitched frames stay candidates here; the leveler judges them against the pitch run.
        f.candidate=f.voiced&&!f.strongTone&&f.snrDb>=6&&f.voiceRatio>.15&&f.voiceDb>-66;
        // The floor follows quiet frames down quickly and rises slowly only outside speech.
        if(f.voiceDb<floorDb_)floorDb_+=(f.voiceDb-floorDb_)*.2;else if(!f.candidate)floorDb_+=std::min(f.voiceDb-floorDb_,.015);
        floorDb_=std::max(floorDb_,-110.0);
        return f;
    }
};
// Lookahead peak limiter: the gain applied to each sample never exceeds the gain that sample requires.
class PeakLimiter {
public:
    static constexpr int look=72;static constexpr double ceiling=.891250938;
private:
    std::array<float,look> left_{},right_{};std::array<double,look> envelope_{};std::deque<std::pair<int64_t,double>> minimum_;
    int64_t count_=0;double sum_=look,release_=1;
    static constexpr double releaseRate=1-0.999739591;  // about 80 ms at 48 kHz
public:
    PeakLimiter(){envelope_.fill(1);}
    template<class Out> void push(float l,float r,Out&& out){
        double need=std::min(1.0,ceiling/std::max({std::abs(double(l)),std::abs(double(r)),1e-9}));
        while(!minimum_.empty()&&minimum_.back().second>=need)minimum_.pop_back();
        minimum_.push_back({count_,need});while(minimum_.front().first<=count_-look)minimum_.pop_front();
        release_=std::min(minimum_.front().second,release_+(1-release_)*releaseRate);
        int slot=static_cast<int>(count_%look);sum_+=release_-envelope_[slot];envelope_[slot]=release_;
        if(count_>=look-1){int oldest=static_cast<int>((count_+1)%look);double gain=std::min(1.0,sum_/look);
            out(static_cast<float>(std::clamp(left_[oldest]*gain,-ceiling,ceiling)),static_cast<float>(std::clamp(right_[oldest]*gain,-ceiling,ceiling)));}
        left_[slot]=l;right_[slot]=r;++count_;
    }
    template<class Out> void flush(Out&& out){
        int64_t pending=std::min<int64_t>(count_,look-1);
        for(int i=0;i<look-1&&pending>0;++i)push(0,0,[&](float l,float r){if(pending>0){out(l,r);--pending;}});
        reset();
    }
    void reset(){count_=0;sum_=look;release_=1;envelope_.fill(1);minimum_.clear();}
};
class SpeechLeveler {
public:
    static constexpr int lookahead=30;                  // frames of 10 ms
    static constexpr double targetLoudness=-20,minimumGain=-15,maximumGain=24;
    struct Status{bool speech=false,learned=false;double levelDb=0,gainDb=0;};
private:
    // An analysed 10 ms frame waiting in the lookahead. Its classification may still change: confirmed speech
    // reaches back to its first syllable, and a sound that turns out to be a chime loses its speech mark and boost.
    // The speaking level learns from frames as they leave the lookahead, with their final classification.
    struct Pending{double desired,loudness;bool speech,active;};
    VoiceDetector detector_;
    std::array<Biquad,2> shelf_{Biquad{1.53512485958697,-2.69169618940638,1.19839281085285,-1.69065929318241,.73248077421585},
        Biquad{1.53512485958697,-2.69169618940638,1.19839281085285,-1.69065929318241,.73248077421585}};
    std::array<Biquad,2> pass_{Biquad{1,-2,1,-1.99004745483398,.99007225036621},Biquad{1,-2,1,-1.99004745483398,.99007225036621}};
    std::array<Biquad,2> decimate_{speech::lowpass(7000,.5411961,speech::rate),speech::lowpass(7000,1.3065630,speech::rate)};
    std::vector<float> inLeft_,inRight_,outLeft_,outRight_;size_t analyzed_=0;std::deque<Pending> pending_;
    static constexpr int64_t none=std::numeric_limits<int64_t>::min();
    int64_t nextInput_=none,outPts_=0;bool open_=false;double appliedDb_=0,lastTarget_=0;
    PeakLimiter limiter_;
    bool speech_=false;int sinceVoice_=1000;std::deque<bool> recent_;
    double runPitch_=0,runLow_=0,runHigh_=0;int tonalRun_=0,runMoving_=0,sound_=0,soundGap_=0;
    // Measured speaking level (K-weighted loudness of speech frames) and the gain derived from it.
    double longDb_=0,midDb_=0,spread_=2,shift_=0,frames_=0,speechGain_=0;int shifted_=0;
    std::atomic<bool> speechSeen_=false,speechNow_=false,learned_=false;std::atomic<double> levelDb_=0,gainDb_=0;
    static double loudness(double energy){return -.691+speech::decibels(energy);}
    // The speaking level follows the dB mean of speech frames, which a few stray loud frames barely move. A slowly
    // learned offset (the mean frame energy relative to that centre, over about 1000 frames) maps it onto the
    // energy-mean loudness of the voice; being relative, it does not change when the voice gets louder.
    double centre()const{return longDb_+.75*std::clamp(midDb_-longDb_,-12.0,12.0);}
    double level()const{return centre()+std::clamp(10*std::log10(spread_),0.0,10.0);}
    Pending analyze(const float* left,const float* right,double balance,bool enabled){
        if(!enabled){speechNow_=false;return {balance,-130,false,false};}
        double energy=0;std::array<double,speech::analysisHop> mono{};
        for(int i=0;i<speech::hop;++i){
            double l=pass_[0].process(shelf_[0].process(left[i])),r=pass_[1].process(shelf_[1].process(right[i]));energy+=l*l+r*r;
            double m=decimate_[1].process(decimate_[0].process((double(left[i])+right[i])*.5));if(i%3==0)mono[i/3]=m;
        }
        double frameLoudness=loudness(energy/speech::hop);
        auto f=detector_.analyze(mono.data());
        // A run is a stretch of voiced frames without note-like pitch jumps. Natural intonation moves its
        // pitch; a notification holds each note within estimation noise (well under 0.3%).
        bool continues=f.candidate&&runPitch_>0&&std::abs(f.pitchHz/runPitch_-1)<.08;
        if(continues){runLow_=std::min(runLow_,f.pitchHz);runHigh_=std::max(runHigh_,f.pitchHz);}else{runLow_=runHigh_=f.candidate?f.pitchHz:0;runMoving_=0;}
        runPitch_=f.candidate?f.pitchHz:0;if(f.candidate&&f.similarity<.75)++runMoving_;
        // Length of the current continuous sound (voiced, gaps up to 30 ms), for undoing a boost.
        soundGap_=f.voiced?0:soundGap_+1;sound_=soundGap_>3?0:sound_+1;
        // A held vowel at the end of a real pitch glide (the harmonics moved, so the spectrum changed) is still
        // speech; a steady sound without one is a chime or tone.
        bool glided=runLow_>0&&runHigh_/runLow_-1>.03&&runMoving_>=5,tonal=f.strongTone||((f.flatPitch||f.steady)&&!glided);
        if(tonal){runLow_=runHigh_=runPitch_;runMoving_=0;}
        bool voice=f.candidate&&!tonal,intonation=runLow_>0&&runHigh_/runLow_-1>.006,onset=false;
        recent_.push_back(voice);if(recent_.size()>lookahead)recent_.pop_front();
        sinceVoice_=voice?0:sinceVoice_+1;tonalRun_=tonal?tonalRun_+1:0;
        if(speech_){
            // A brief steady vowel stays speech; a pure tone or a sustained ringing sound ends it, and the frames
            // of that sound still in the lookahead lose the speech mark and the boost.
            if((f.strongTone&&tonalRun_>=3)||tonalRun_>=8){speech_=false;
                size_t back=size_t(std::max(tonalRun_+8,sound_))+2;
                for(size_t b=1;b<=back&&b<=pending_.size();++b){auto& p=pending_[pending_.size()-b];p.speech=false;p.desired=std::min(p.desired,std::min(speechGain_,0.0)+balance);}}
            else if(sinceVoice_>30||(sinceVoice_>10&&f.snrDb<3))speech_=false;
        }
        else if(intonation&&std::count(recent_.begin(),recent_.end(),true)>=5)speech_=onset=true;
        if(onset){
            // Confirmed speech: frames still in the lookahead from 20 ms before its first voiced frame join it.
            size_t first=std::find(recent_.begin(),recent_.end(),true)-recent_.begin(),back=recent_.size()-first+1;
            for(size_t b=1;b<=back&&b<=pending_.size();++b){auto& p=pending_[pending_.size()-b];p.speech=true;p.desired=std::max(p.desired,speechGain_+balance);}
        }
        bool learned=frames_>=50;
        speechNow_=speech_;if(speech_)speechSeen_=true;learned_=learned;levelDb_=learned?level():0;gainDb_=speechGain_;
        // Speech receives the measured gain, but no 10 ms frame of it may end up more than 9 dB above the target (a
        // shout, or a much louder voice before its level is measured). Anything else may be reduced, never boosted.
        double gain=speech_?std::min(speechGain_,targetLoudness+9-frameLoudness):std::min(speechGain_,0.0);
        return {std::max(gain,minimumGain)+balance,frameLoudness,speech_&&!f.strongTone,f.snrDb>8};
    }
    void learn(double frameLoudness){
        double c=centre(),raw=frameLoudness;
        // Smoothed distance of incoming frames from the current level: a lasting difference is a new speaker or
        // microphone position and is followed freely and fast; a stray frame far from the level (a cough, a
        // chime frame that slipped through) is clamped instead.
        shift_+=(std::clamp(raw-c,-30.0,30.0)-shift_)/15;shifted_=std::abs(shift_)>6?shifted_+1:0;bool changing=frames_>=50&&shifted_>=15;
        if(frames_>=50&&!changing)frameLoudness=std::clamp(frameLoudness,c-20,c+10);
        frames_+=1;
        // Long-term level over about 300 speech frames (several seconds of talking), medium-term over 40.
        double longRate=std::max(1/frames_,changing?1.0/30:1.0/300),midRate=std::max(1/frames_,1.0/40);
        longDb_+=(frameLoudness-longDb_)*longRate;midDb_+=(frameLoudness-midDb_)*midRate;
        if(frames_>50&&!changing)spread_+=(std::pow(10,std::clamp(raw-c,-20.0,15.0)/10)-spread_)*std::max(1/(frames_-50),1.0/1000);
        if(frames_<50)return;
        // At most 0.2 dB per learned 10 ms frame (0.5 dB while the first seconds of speech are measured); a voice far
        // above the target (a new, louder speaker) is pulled down at 0.5 dB per frame, and capped at once when a
        // frame would land 12 dB above it.
        double wanted=std::clamp(targetLoudness-level(),minimumGain,maximumGain),up=frames_<350?.5:.2;
        double down=frameLoudness+speechGain_>targetLoudness+8?.5:up;speechGain_+=std::clamp(wanted-speechGain_,-down,up);
        speechGain_=std::max(minimumGain,std::min(speechGain_,targetLoudness+12-raw));
    }
    template<class Sink> void emit(size_t count,double target,Sink&& sink){
        static const double rise=1-std::exp(-1/(.01*speech::rate)),fall=1-std::exp(-1/(.015*speech::rate));
        // The dB-domain smoothing is evaluated every 16 samples and interpolated linearly in between.
        constexpr size_t step=16;double gain=std::pow(10,appliedDb_/20);
        for(size_t i=0;i<count;i+=step){
            size_t n=std::min(step,count-i);double rate=target>appliedDb_?rise:fall;appliedDb_+=(target-appliedDb_)*(1-std::pow(1-rate,double(n)));
            double next=std::pow(10,appliedDb_/20),delta=(next-gain)/double(n);
            for(size_t k=i;k<i+n;++k){gain+=delta;limiter_.push(static_cast<float>(inLeft_[k]*gain),static_cast<float>(inRight_[k]*gain),[&](float l,float r){outLeft_.push_back(l);outRight_.push_back(r);});}
            gain=next;
        }
        inLeft_.erase(inLeft_.begin(),inLeft_.begin()+count);inRight_.erase(inRight_.begin(),inRight_.begin()+count);
        send(sink);
    }
    template<class Sink> void send(Sink&& sink){
        if(outLeft_.empty())return;int n=static_cast<int>(outLeft_.size());sink(outLeft_.data(),outRight_.data(),n,outPts_);outPts_+=n;outLeft_.clear();outRight_.clear();
    }
    template<class Sink> void emitFrame(Sink&& sink){
        // Reductions start 40 ms early so a sound right after speech is not caught by the boost.
        lastTarget_=pending_.front().desired;for(size_t i=1;i<std::min<size_t>(pending_.size(),5);++i)lastTarget_=std::min(lastTarget_,pending_[i].desired);
        // Speech frames above the background (and within 15 dB of the known level) teach the speaking level.
        auto& p=pending_.front();if(p.speech&&p.active&&(frames_<50||p.loudness>centre()-15))learn(p.loudness);
        emit(speech::hop,lastTarget_,sink);pending_.pop_front();analyzed_-=speech::hop;
    }
public:
    // Input and output are 48 kHz stereo with sample timestamps; the output trails the input by about 310 ms
    // (lookahead and limiter) but carries the original timestamps.
    template<class Sink> void process(const float* left,const float* right,int count,int64_t pts,double balance,bool enabled,Sink&& sink){
        if(count<=0)return;
        if(open_&&std::abs(pts-nextInput_)>240)flush(sink);
        if(!open_){open_=true;outPts_=pts;nextInput_=pts;}
        nextInput_+=count;inLeft_.insert(inLeft_.end(),left,left+count);inRight_.insert(inRight_.end(),right,right+count);
        while(inLeft_.size()-analyzed_>=size_t(speech::hop)){pending_.push_back(analyze(inLeft_.data()+analyzed_,inRight_.data()+analyzed_,balance,enabled));analyzed_+=speech::hop;}
        while(pending_.size()>size_t(lookahead))emitFrame(sink);
    }
    template<class Sink> void flush(Sink&& sink){
        if(!open_)return;
        while(!pending_.empty())emitFrame(sink);
        if(!inLeft_.empty())emit(inLeft_.size(),lastTarget_,sink);
        limiter_.flush([&](float l,float r){outLeft_.push_back(l);outRight_.push_back(r);});send(sink);
        reset();
    }
    // Drops buffered audio after a device failure; the learned speaking level is kept.
    void reset(){inLeft_.clear();inRight_.clear();outLeft_.clear();outRight_.clear();pending_.clear();analyzed_=0;limiter_.reset();open_=false;nextInput_=none;}
    // A different device or speaker set: measure the speaking level again from scratch.
    void restartLearning(){longDb_=midDb_=shift_=frames_=speechGain_=0;spread_=2;shifted_=0;speech_=false;sinceVoice_=1000;recent_.clear();runPitch_=runLow_=runHigh_=0;tonalRun_=runMoving_=0;learned_=false;}
    bool speaking()const{return speechNow_;}
    Status take(){return {speechSeen_.exchange(false),learned_.load(),levelDb_.load(),gainDb_.load()};}
};
}
