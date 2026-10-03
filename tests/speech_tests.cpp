// Speech leveler scenarios on synthetic voices, notification sounds and noise. Portable C++20.
#include "../src/native/speech_leveler.hpp"
#include <cstdio>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
using namespace apex;
namespace {
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
// Library distributions differ between standard libraries; these keep the synthetic signals identical everywhere.
struct Random {
    std::mt19937 engine;bool spare=false;double cached=0;
    explicit Random(unsigned seed):engine(seed){}
    double uniform(){return (double(engine())+.5)/4294967296.0;}
    double normal(){if(spare){spare=false;return cached;}double r=std::sqrt(-2*std::log(uniform())),a=2*speech::pi*uniform();cached=r*std::sin(a);spare=true;return r*std::cos(a);}
};
struct Signal{std::vector<float> samples;std::vector<uint8_t> speech,tone;};  // mono, with ground-truth masks
// Mean K-weighted loudness of mask samples within [from,to) seconds.
double loudness(const std::vector<float>& x,const std::vector<uint8_t>& mask,double from,double to){
    Biquad shelf{1.53512485958697,-2.69169618940638,1.19839281085285,-1.69065929318241,.73248077421585},pass{1,-2,1,-1.99004745483398,.99007225036621};
    double e=0;size_t n=0;for(size_t i=0;i<x.size();++i){double k=pass.process(shelf.process(x[i]));if(i>=size_t(from*48000)&&i<size_t(to*48000)&&mask[i]){e+=2*k*k;++n;}}
    return n?-.691+10*std::log10(std::max(e/n,1e-13)):-130;
}
// Formant-filtered glottal pulses (-6 dB/octave source with jitter, shimmer and breath noise), moving
// intonation and formants, syllable envelopes, fricatives and pauses.
class Voice {
    Random random_;double f0_,phase_=0,drift_=0,jitter_=0,shimmer_=1,source_=0;
    struct Resonator{double a1=0,a2=0,g=1,y1=0,y2=0;void set(double f,double bw){double r=std::exp(-speech::pi*bw/48000),t=2*speech::pi*f/48000;a1=2*r*std::cos(t);a2=-r*r;g=1-r;}
        double process(double x){double y=g*x+a1*y1+a2*y2;y2=y1;y1=y;return y;}};
    std::array<Resonator,4> formants_;
public:
    Voice(double f0,unsigned seed):random_(seed),f0_(f0){}
    // Appends running speech whose active parts have the given K-weighted loudness.
    void append(Signal& s,double seconds,double loudnessLufs){
        size_t begin=s.samples.size();const double amplitude=1;
        static const double vowels[][3]={{730,1090,2440},{270,2290,3010},{530,1840,2480},{570,840,2410},{300,870,2240},{660,1720,2410}};
        auto u=[this](Random&){return random_.uniform();};size_t end=s.samples.size()+size_t(seconds*48000);
        while(s.samples.size()<end){
            int words=1+int(u(random_)*4);
            for(int w=0;w<words&&s.samples.size()<end;++w){
                int syllables=1+int(u(random_)*3);
                for(int y=0;y<syllables&&s.samples.size()<end;++y){
                    if(u(random_)<.35){int n=int((.04+.05*u(random_))*48000);double hp=0,prev=0;
                        for(int i=0;i<n;++i){double x=random_.normal()*amplitude*.12;hp=.9*(hp+x-prev);prev=x;s.samples.push_back(float(hp*std::sin(speech::pi*i/n)));s.speech.push_back(0);s.tone.push_back(0);}}
                    // Formants glide from one vowel towards another across the syllable, as in running speech.
                    auto& v=vowels[int(u(random_)*6)];auto& w=vowels[int(u(random_)*6)];double scale=.9+.2*u(random_);
                    int n=int((.12+.16*u(random_))*48000);double accent=.85+.3*u(random_),slope=(u(random_)-.5)*.5;
                    // Each syllable is brought to a common loudness times its accent, so the vowel choice does not
                    // change the speaker's level (the resonators boost low formants more than high ones).
                    size_t syllable=s.samples.size();
                    for(int i=0;i<n;++i){double t=double(i)/n;
                        if(i%48==0){for(int k=0;k<3;++k)formants_[k].set((v[k]+(w[k]-v[k])*t)*scale,60+40*k);formants_[3].set(3500*scale,250);}
                        drift_+=(u(random_)-.5)*.002;drift_=std::clamp(drift_,-.08,.08);
                        double f=f0_*(1+slope*(t-.5)+drift_+jitter_+.03*std::sin(2*speech::pi*5*double(s.samples.size())/48000));
                        phase_+=f/48000;double pulse=0;if(phase_>=1){phase_-=1;pulse=shimmer_;jitter_=(u(random_)-.5)*.02;shimmer_=.85+.3*u(random_);}
                        source_=source_*.974+pulse;double x=source_*.6+random_.normal()*.02*(phase_<.3?1:.3);  // ~200 Hz one-pole: -6 dB/octave
                        double y=x;for(auto& r:formants_)y=r.process(y)*2.2;
                        double env=std::min({1.0,t/.04,(1-t)/.06});s.samples.push_back(float(y*std::max(0.0,env)));s.speech.push_back(1);s.tone.push_back(0);}
                    double power=0;for(size_t i=syllable;i<s.samples.size();++i)power+=double(s.samples[i])*s.samples[i];
                    double gain=amplitude*accent*.1/std::sqrt(std::max(power/double(s.samples.size()-syllable),1e-20));
                    for(size_t i=syllable;i<s.samples.size();++i)s.samples[i]=float(s.samples[i]*gain);
                    int gap=int((.03+.05*u(random_))*48000);s.samples.insert(s.samples.end(),gap,0.f);s.speech.insert(s.speech.end(),gap,0);s.tone.insert(s.tone.end(),gap,0);
                }
                int gap=int((.08+.15*u(random_))*48000);s.samples.insert(s.samples.end(),gap,0.f);s.speech.insert(s.speech.end(),gap,0);s.tone.insert(s.tone.end(),gap,0);
            }
            int pause=int((.3+.5*u(random_))*48000);s.samples.insert(s.samples.end(),pause,0.f);s.speech.insert(s.speech.end(),pause,0);s.tone.insert(s.tone.end(),pause,0);
        }
        s.samples.resize(end);s.speech.resize(end);s.tone.resize(end);
        std::vector<float> part(s.samples.begin()+begin,s.samples.end());std::vector<uint8_t> mask(s.speech.begin()+begin,s.speech.end());
        double scale=std::pow(10,(loudnessLufs-loudness(part,mask,0,1e9))/20);for(size_t i=begin;i<end;++i)s.samples[i]=float(s.samples[i]*scale);
    }
};
void silence(Signal& s,double seconds){size_t n=size_t(seconds*48000);s.samples.insert(s.samples.end(),n,0.f);s.speech.insert(s.speech.end(),n,0);s.tone.insert(s.tone.end(),n,0);}
// Notification-like sounds: decaying sine pings, two-note chimes, square beeps and a sawtooth buzzer.
void notification(Signal& s,int kind,double amplitude){
    struct Note{double f,start,length;};std::vector<Note> notes;std::function<double(double)> wave=[](double p){return std::sin(2*speech::pi*p);};
    if(kind==0)notes={{880,0,.18}};
    else if(kind==1)notes={{660,0,.15},{990,.12,.3}};
    else if(kind==2){notes={{500,0,.2}};wave=[](double p){return std::fmod(p,1.0)<.5?.6:-.6;};}
    else if(kind==3){notes={{220,0,.45}};wave=[](double p){return (std::fmod(p,1.0)-.5)*1.2;};}
    else notes={{1320,0,.1},{1760,.1,.1},{2640,.2,.25}};
    double length=0;for(auto& n:notes)length=std::max(length,n.start+n.length);size_t base=s.samples.size(),count=size_t(length*48000);
    s.samples.resize(base+count,0.f);s.speech.resize(base+count,0);s.tone.resize(base+count,1);
    for(auto& n:notes)for(size_t i=0;i<size_t(n.length*48000);++i){double t=double(i)/48000;double env=std::min(1.0,t/.005)*(kind==2||kind==3?1:std::exp(-t*8));
        s.samples[base+size_t(n.start*48000)+i]+=float(amplitude*env*wave(n.f*t));}
}
void noise(Signal& s,double seconds,double amplitude,unsigned seed){
    Random random(seed);auto white=[&](Random&){return random.normal();};auto u=[&](Random&){return random.uniform();};double b0=0,b1=0,b2=0;size_t n=size_t(seconds*48000);
    for(size_t i=0;i<n;++i){double w=white(random);b0=.99765*b0+w*.0990460;b1=.96300*b1+w*.2965164;b2=.57000*b2+w*1.0526913;double pink=(b0+b1+b2+w*.1848)*.05;
        s.samples.push_back(float(pink*amplitude));s.speech.push_back(0);s.tone.push_back(0);}
    // Keyboard and mouse clicks: short decaying broadband bursts.
    for(size_t at=size_t(48000*.2*u(random));at+600<n;at+=size_t(48000*(.08+.4*u(random))))
        for(int i=0;i<480;++i)s.samples[s.samples.size()-n+at+i]+=float(amplitude*3*white(random)*std::exp(-i/60.0));
}
struct Result{std::vector<float> out;std::vector<uint8_t> speechFrames;size_t frames=0;SpeechLeveler::Status status;};
// Feeds stereo copies in device-sized packets and reassembles the output on its timestamps.
Result run(SpeechLeveler& leveler,const Signal& s,double balance=0,bool enabled=true,int64_t start=0){
    Result result;result.out.assign(s.samples.size(),0.f);std::vector<uint8_t> written(s.samples.size(),0);int64_t expected=start;
    auto sink=[&](float* l,float* r,int n,int64_t pts){require(pts==expected,"leveler broke sample timestamps");expected+=n;
        for(int i=0;i<n;++i){require(std::isfinite(l[i])&&std::isfinite(r[i]),"leveler produced nonfinite samples");require(std::abs(l[i])<=PeakLimiter::ceiling+1e-6,"limiter peak escaped");
            size_t index=size_t(pts-start+i);if(index<result.out.size()){result.out[index]=l[i];written[index]=1;}}};
    std::vector<float> left,right;size_t at=0;int sizes[]={441,480,960,512,1000};int which=0;
    while(at<s.samples.size()){int n=int(std::min<size_t>(sizes[which++%5],s.samples.size()-at));left.assign(s.samples.begin()+at,s.samples.begin()+at+n);right=left;
        leveler.process(left.data(),right.data(),n,start+int64_t(at),balance,enabled,sink);at+=n;
        while(result.speechFrames.size()<at/480)result.speechFrames.push_back(leveler.speaking());}
    leveler.flush(sink);require(expected==start+int64_t(s.samples.size()),"leveler lost samples at flush");
    require(std::all_of(written.begin(),written.end(),[](uint8_t w){return w;}),"leveler output has holes");
    result.status=leveler.take();return result;
}
double gainOn(const Result& r,const Signal& s,const std::vector<uint8_t>& mask,double from,double to){return loudness(r.out,mask,from,to)-loudness(s.samples,mask,from,to);}
// Fraction of 10 ms frames flagged as speech whose centre lies in a masked region (frame flags lag by the lookahead).
double flaggedShare(const Result& r,const std::vector<uint8_t>& mask){
    size_t hit=0,total=0;for(size_t f=0;f<r.speechFrames.size();++f){size_t centre=f*480+240;if(centre<mask.size()&&mask[centre]){++total;hit+=r.speechFrames[f];}}
    return total?double(hit)/total:0;
}
}
int main(){int passed=0;auto test=[&](const char* name,auto body){body();++passed;std::cout<<"PASS "<<name<<'\n';};
    const double target=SpeechLeveler::targetLoudness;
    try{
        test("quiet, normal and loud voices converge on the common speech level",[&]{
            for(double input:{-44.0,-30.0,-10.0}){Signal s;Voice voice(120,7);voice.append(s,14,input);SpeechLeveler leveler;auto r=run(leveler,s);
                double out=loudness(r.out,s.speech,8,14),share=flaggedShare(r,s.speech);
                std::printf("  voice %.1f LUFS -> %.1f LUFS, measured %.1f, gain %+.1f dB, speech share %.2f\n",input,out,r.status.levelDb,r.status.gainDb,share);
                require(r.status.learned,"speaking level was not learned");require(std::abs(out-target)<3,"speech level not balanced to the target");
                require(std::abs(r.status.levelDb-input)<4,"measured speaking level is wrong");require(share>.6,"speech was not detected");}});
        // Synthetic high voices have sparser spectra than recorded ones (a recorded female voice scores 0.9 or more).
        test("low and high voices are detected",[]{for(double f0:{95.0,200.0,260.0}){Signal s;Voice voice(f0,11);voice.append(s,10,-30);SpeechLeveler leveler;auto r=run(leveler,s);
            std::printf("  f0 %.0f Hz speech share %.2f\n",f0,flaggedShare(r,s.speech));require(flaggedShare(r,s.speech)>(f0>220?.5:.6),"voice pitch range not detected");}});
        test("notification sounds are not speech, are not boosted and do not move the speaking level",[]{
            for(int kind=0;kind<5;++kind){Signal only;silence(only,.3);for(int i=0;i<10;++i){notification(only,kind,.2);silence(only,.4);}SpeechLeveler l;auto r=run(l,only);
                double flagged=0;for(auto f:r.speechFrames)flagged+=f;require(flagged/r.speechFrames.size()<.05,"notification kind "+std::to_string(kind)+" detected as speech");
                require(!r.status.learned,"notification kind "+std::to_string(kind)+" taught a speaking level");}
            Signal clean,mixed;Voice a(140,21),b(140,21);
            for(int i=0;i<8;++i){a.append(clean,2.5,-40);b.append(mixed,2.5,-40);silence(clean,.6);notification(mixed,i%5,.3);
                size_t pad=clean.samples.size()-mixed.samples.size();silence(mixed,double(pad)/48000);}
            SpeechLeveler l1,l2;auto r1=run(l1,clean),r2=run(l2,mixed);double share=flaggedShare(r2,mixed.tone),toneGain=gainOn(r2,mixed,mixed.tone,0,1e9);
            std::printf("  level without %.1f / with notifications %.1f LUFS; notification frames flagged %.2f; notification gain %+.1f dB, speech gain %+.1f dB\n",
                r1.status.levelDb,r2.status.levelDb,share,toneGain,r2.status.gainDb);
            require(std::abs(r1.status.levelDb-r2.status.levelDb)<1.5,"notifications moved the speaking level");require(share<.15,"notification sounds classified as speech");
            require(r2.status.gainDb>15&&toneGain<1,"notification sounds were boosted with the speech");});
        test("noise and clicks are neither detected nor amplified",[]{Signal s;noise(s,20,.01,5);SpeechLeveler leveler;auto r=run(leveler,s);
            double flagged=0;for(auto f:r.speechFrames)flagged+=f;std::vector<uint8_t> all(s.samples.size(),1);double gain=gainOn(r,s,all,1,20);
            std::printf("  noise frames flagged %.3f, noise gain %+.2f dB\n",flagged/r.speechFrames.size(),gain);
            require(flagged/r.speechFrames.size()<.03,"noise detected as speech");require(gain<.5,"noise was amplified");});
        test("speech in background noise is still balanced",[&]{Signal s;Voice voice(130,3);voice.append(s,14,-36);Signal n;noise(n,14,.0015,9);
            for(size_t i=0;i<s.samples.size();++i)s.samples[i]+=n.samples[i];
            SpeechLeveler leveler;auto r=run(leveler,s);double out=loudness(r.out,s.speech,8,14);
            std::printf("  noisy voice -> %.1f LUFS, share %.2f\n",out,flaggedShare(r,s.speech));require(std::abs(out-target)<3.5,"noisy speech not balanced");});
        test("gain follows a new speaker and holds through long silence",[&]{Signal s;Voice quiet(110,31),loud(200,32);quiet.append(s,12,-44);silence(s,20);loud.append(s,12,-12);
            SpeechLeveler leveler;auto r=run(leveler,s);
            double first=loudness(r.out,s.speech,6,12),onset=loudness(r.out,s.speech,32,33),later=loudness(r.out,s.speech,36,44);
            std::printf("  quiet speaker %.1f, first second of loud speaker %.1f, loud speaker after 4 s %.1f LUFS\n",first,onset,later);
            require(std::abs(first-target)<3,"quiet speaker not balanced");require(onset<target+12,"much louder new speaker not caught");require(std::abs(later-target)<4,"new speaker not balanced");});
        test("balance offsets the leveled speech and the game track is only limited",[]{Signal s;Voice voice(150,41);voice.append(s,12,-30);
            SpeechLeveler a,b,game;auto ra=run(a,s,0),rb=run(b,s,-6);double d=loudness(rb.out,s.speech,6,12)-loudness(ra.out,s.speech,6,12);
            std::printf("  balance -6 dB -> %+.2f dB\n",d);require(std::abs(d+6)<.7,"balance offset not applied");
            auto rg=run(game,s,0,false);require(std::abs(gainOn(rg,s,s.speech,0,12))<.05,"disabled leveling changed the level");
            Signal hot;for(int i=0;i<48000;++i){hot.samples.push_back(float(1.6*std::sin(i*.05)));hot.speech.push_back(1);hot.tone.push_back(0);}SpeechLeveler limit;run(limit,hot,0,false);});
        test("timestamp gaps restart buffering without losing samples",[]{Signal s;Voice voice(120,51);voice.append(s,3,-30);SpeechLeveler leveler;int64_t expected=1000;size_t total=0;
            auto sink=[&](float*,float*,int n,int64_t pts){require(pts==expected||pts==96000+48000*3,"leveler crossed a timestamp gap");if(pts!=expected)expected=pts;expected+=n;total+=size_t(n);};
            leveler.process(s.samples.data(),s.samples.data(),48000*2,1000,0,true,sink);
            leveler.process(s.samples.data()+96000,s.samples.data()+96000,48000,96000+48000*3,0,true,sink);leveler.flush(sink);
            require(total==size_t(48000*3),"gap lost samples");});
        std::cout<<passed<<" speech scenarios passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
