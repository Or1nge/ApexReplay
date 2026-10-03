#include "../src/native/audio.hpp"
#include "../src/native/hud_attribution.hpp"
#include "../src/native/detector.hpp"
#include <stdexcept>
using namespace apex;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
PacketRef sample(double time,bool key=false,int stream=0){auto packet=av_packet_alloc();ffcheck(av_new_packet(packet,8),"test packet");packet->pts=packet->dts=static_cast<int64_t>(time*1000);packet->flags=key?AV_PKT_FLAG_KEY:0;auto ref=std::make_shared<Packet>(packet,stream,AVRational{1,1000});av_packet_free(&packet);return ref;}
int main(){av_log_set_level(AV_LOG_ERROR);int passed=0;auto test=[&](const char* name,auto body){body();++passed;std::cout<<"PASS "<<name<<'\n';};
    try{
        test("missing damage HUD still permits confirmed multikill results",[]{ObservationRules flow({});Observation observation;
            observation.active=true;observation.ammo=20;observation.time=10;observation.events={{10,ResultKind::Knockdown,"enemy-a","confirmed own prompt",1}};flow.process(observation);
            observation.events.clear();for(double t=10.1;t<15;t+=.1){observation.time=t;require(!flow.process(observation),"unreadable damage counter cut the fight");}
            observation.time=15;observation.events={{15,ResultKind::Knockdown,"enemy-b","confirmed own prompt",1}};flow.process(observation);
            observation.events.clear();observation.time=36;flow.process(observation);auto clips=flow.rules().takeReady();
            require(clips.size()==1&&clips[0].kind=="multikill"&&clips[0].kills==2,"missing damage HUD lost confirmed results");});
        test("microphone denoiser reduces steady noise and retains a voice-band tone",[]{MicDenoiser denoiser;std::array<float,480> left{},right{};unsigned random=1234567;
            double inputNoise=0,outputNoise=0,sine=0,cosine=0;size_t inputCount=0,outputCount=0,toneCount=0;int64_t end=0;
            auto sink=[&](float* l,float* r,int n,int64_t pts){require(pts==end,"denoiser broke sample timestamps");end=pts+n;for(int i=0;i<n;++i){require(std::isfinite(l[i])&&std::isfinite(r[i]),"denoiser produced nonfinite samples");double t=double(pts+i)/48000;
                if(t>2&&t<3){outputNoise+=l[i]*l[i];++outputCount;}if(t>4&&t<5){sine+=l[i]*std::sin((pts+i)*6.283185307179586*1000/48000);cosine+=l[i]*std::cos((pts+i)*6.283185307179586*1000/48000);++toneCount;}}};
            for(int block=0;block<600;++block){for(int i=0;i<480;++i){int64_t sample=block*480+i;random=random*1664525u+1013904223u;float noise=float((double(random)/4294967296.-.5)*.012);double t=double(sample)/48000;
                    if(t>2&&t<3){inputNoise+=noise*noise;++inputCount;}float tone=t>=3&&t<5?float(.12*std::sin(sample*6.283185307179586*1000/48000)):0;left[i]=right[i]=noise+tone;}
                denoiser.process(left.data(),right.data(),480,block*480,true,sink);}
            denoiser.flush(sink);require(end>=6*48000-600&&end<=6*48000+600,"denoiser lost the audio tail");require(outputCount&&toneCount,"denoiser emitted no audio");
            double reduction=10*std::log10((inputNoise/inputCount)/(outputNoise/outputCount));double amplitude=2*std::hypot(sine,cosine)/toneCount;
            std::cout<<"Denoise noise reduction "<<reduction<<" dB, tone amplitude "<<amplitude<<'\n';require(reduction>3,"steady noise was not reduced");require(amplitude>.075,"voice-band signal was suppressed excessively");});
        test("microphone denoiser can switch off without losing the input clock",[]{MicDenoiser denoiser;std::array<float,480> left{},right{};left.fill(.1f);right.fill(.1f);
            int64_t last=-1;auto sink=[&](float*,float*,int,int64_t pts){require(pts>last,"toggle reversed microphone timestamps");last=pts;};
            for(int i=0;i<10;++i)denoiser.process(left.data(),right.data(),480,i*480,true,sink);
            bool bypass=false;denoiser.process(left.data(),right.data(),480,4800,false,[&](float* l,float*,int n,int64_t pts){sink(l,l,n,pts);if(pts==4800){bypass=n==480&&l[0]==.1f;}});
            require(bypass,"disabled denoiser altered microphone samples");
            for(int i=11;i<21;++i)denoiser.process(left.data(),right.data(),480,i*480,true,sink);denoiser.flush(sink);require(last>=5280,"re-enabled denoiser emitted no audio");});
        test("audio meter retains brief peaks and clears silent intervals",[]{AudioPeakMeter meter;
            meter.add(.7f);meter.add(.02f);meter.add(0);require(std::abs(meter.take()-.7f)<1e-6,"brief peak lost before UI refresh");
            require(meter.take()==0,"old peak persisted during silence");meter.add(.5f);meter.reset();require(meter.take()==0,"device reset left a stale peak");});
        test("own feed attribution excludes teammate and stale entries",[]{OwnFeedAttribution feed;OcrRead name;name.words={{"Or1nge",.06,.55,.2,.4}};for(int i=0;i<3;++i)feed.observeName(name);
            auto row=[](std::string actor,std::string target){OcrRead r;r.words={{actor,.15,.1,.13,.07},{target,.55,.1,.35,.07}};return r;};
            feed.observeFeed(row("OrInge","old-target"),0);feed.observeFeed(row("OrInge","old-target"),.3);require(!feed.recentTarget(.3),"old visible entry became new result");
            feed.observeFeed(row("teammate","enemy-a"),.6);feed.observeFeed(row("teammate","enemy-a"),.9);require(!feed.recentTarget(.9),"teammate attributed to own player");
            feed.observeFeed(row("OrInge","enemy-a"),1.2);require(!feed.recentTarget(1.2),"single OCR reading accepted");feed.observeFeed(row("OrInge","enemy-a"),1.5);require(feed.recentTarget(1.5)=="enemya","confirmed own target missing");
            feed.observeFeed(row("OrInge","enemy-b"),1.8);feed.observeFeed(row("OrInge","enemy-b"),2.1);require(feed.recentTarget(2.1)=="enemyb","rapid second own target was suppressed");require(!feed.recentTarget(5),"old feed extended result lifetime");});
        test("thirty minute retention and export references",[]{PacketRing ring;ring.configure(30,60);std::vector<PacketRef> held;double start=0;
            for(int i=0;i<1901;++i){ring.push(sample(i,true));if(i==200)held=ring.snapshot(100,105,start);}
            require(ring.duration()>=1799&&ring.duration()<=1801,"thirty minute retention incorrect");require(!held.empty()&&held.front()->time==100,"export references lost evicted data");
            auto before=ring.bytes();auto a=ring.snapshot(1820.5,1830,start);auto b=ring.snapshot(1825.5,1840,start);require(!a.empty()&&!b.empty()&&ring.bytes()==before,"overlapping snapshot consumed cache");});
        test("candidate pins survive short duration limit",[]{PacketRing ring;ring.configure(1,60);for(int i=0;i<30;++i)ring.push(sample(i,true));ring.pin(20);
            for(int i=30;i<101;++i)ring.push(sample(i,true));double start;auto result=ring.snapshot(20,90,start);require(start==20,"pending candidate lost its history");
            ring.pin({});ring.push(sample(101,true));require(ring.duration()<=61,"released candidate kept growing cache");});
        test("AAC fills multi second device clock gaps",[]{std::vector<PacketRef> packets;AudioEncoder encoder(1,[&](PacketRef p){packets.push_back(std::move(p));});std::array<float,480> audio{};
            encoder.append(audio.data(),audio.data(),480,0);encoder.append(audio.data(),audio.data(),480,96000);encoder.finish();require(packets.size()>90&&packets.back()->time>1.98,"audio timeline collapsed after device interruption");});
        test("speech leveler bounds peaks, keeps timestamps and does not raise steady low noise",[]{SpeechLeveler hot;std::array<float,480> left{},right{};std::vector<float> out;int64_t next=0;
            auto sink=[&](float* l,float*,int n,int64_t pts){require(pts==next,"leveler broke sample timestamps");next+=n;out.insert(out.end(),l,l+n);};
            for(int n=0;n<40;++n){left.fill(2);right.fill(2);hot.process(left.data(),right.data(),480,n*480,12,true,sink);}hot.flush(sink);
            require(out.size()==40*480,"leveler lost samples");for(auto x:out)require(std::abs(x)<=.892,"limiter peak escaped");
            SpeechLeveler quiet;double max=0;for(int n=0;n<2000;++n){left.fill(1e-5f);right.fill(1e-5f);
                quiet.process(left.data(),right.data(),480,n*480,0,true,[&](float* l,float*,int k,int64_t){for(int i=0;i<k;++i)max=std::max(max,double(std::abs(l[i])));});}
            require(max<1.01e-5,"leveler raised steady low noise");require(!quiet.take().learned,"steady noise taught a speaking level");});
        require(Packet::liveBytes==0,"encoded packet references leaked");std::cout<<passed<<" native scenarios passed\n";return 0;
    }catch(...){std::cerr<<errorText()<<'\n';return 1;}
}
