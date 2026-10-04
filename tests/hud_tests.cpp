#include "../src/native/digits.hpp"
#include "../src/native/hud_tracking.hpp"
#include "../src/native/devlog.hpp"
#include <iostream>
#include <chrono>
#include <thread>
using namespace apex;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void baseline(DamageTracker& tracker,int value=100){tracker.observe(value,0);tracker.observe(value,.1);require(tracker.observe(value,.2).baseline,"baseline not confirmed");}
CpuImage counterRow(const json& config,const std::vector<std::string>& groups){
    std::vector<json> glyphs;int width=8;
    for(const auto& group:groups){glyphs.push_back(nullptr);width+=24;for(char ch:group){auto it=std::find_if(config["templates"].begin(),config["templates"].end(),[&](const auto& g){return g["digit"]==ch-'0';});require(it!=config["templates"].end(),"template missing");glyphs.push_back(*it);width+=int(std::round(it->at("aspect").template get<double>()*32))+3;}width+=20;}
    width-=16;CpuImage out{width,40,std::vector<uint8_t>(size_t(width)*40*4)};int left=4;
    for(const auto& glyph:glyphs){
        if(glyph.is_null()){auto it=std::find_if(config["templates"].begin(),config["templates"].end(),[](const auto& g){return g["digit"]==10&&g["aspect"]==.5625;});require(it!=config["templates"].end(),"icon template missing");auto mask=it->at("mask").get<std::vector<std::string>>();for(int y=0;y<32;++y)for(int x=0;x<18;++x)if(mask[y][x*20/18]=='1'){auto p=out.bgra.data()+(size_t(y+4)*width+left+x)*4;p[0]=p[1]=p[2]=255;p[3]=255;}left+=24;continue;}
        int w=int(std::round(glyph["aspect"].get<double>()*32));auto mask=glyph["mask"].get<std::vector<std::string>>();
        for(int y=0;y<32;++y)for(int x=0;x<w;++x)if(mask[y][x*20/w]=='1'){auto p=out.bgra.data()+(size_t(y+4)*width+left+x)*4;p[0]=p[1]=p[2]=255;p[3]=255;}
        left+=w+3;
        // Space between groups is inserted before the next icon.
        auto next=&glyph-&glyphs[0]+1;if(next<int(glyphs.size())&&glyphs[size_t(next)].is_null())left+=20;
    }
    return out;
}
int main(int argc,char** argv){
    auto root=argc>1?std::filesystem::path(argv[1]):std::filesystem::path(".");
    auto configPath=root/"config"/"hud.zh.v1.json";HudDigits::load(configPath);json config=json::parse(std::ifstream(configPath));
    int passed=0;auto test=[&](const char* name,auto body){try{body();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';throw;}};
    auto temporary=std::filesystem::temp_directory_path()/("apex-hud-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try{
        test("one unreadable damage frame retains the baseline",[]{DamageTracker d;baseline(d);d.observe({},.3);d.observe(180,.4);auto c=d.observe(180,.5);require(c.delta==80&&d.value()==180,"damage lost on unreadable frame");});
        test("rolling intermediate values retain the full increase",[]{DamageTracker d;baseline(d);require(d.observe(140,.3).delta==0,"first rolling frame accepted");auto c=d.observe(190,.4);require(c.delta==90&&c.at==.3&&c.since==.2,"rolling increase lost");});
        test("only a stable drop greater than 100 resets",[]{DamageTracker d;baseline(d,500);for(double t:{.3,.4,.5})require(!d.observe(400,t).reset,"exactly 100 reset");d.observe(200,.6);d.observe(450,.7);require(!d.observe(200,.8).reset,"nonstable lower value reset");require(!d.observe(200,.9).reset,"lower value reset too early");require(d.observe(200,1).reset&&d.value()==200,"stable drop did not reset");});
        test("implausible jumps become a baseline without damage",[]{DamageTracker d;baseline(d);d.observe(4000,.3);d.observe(4000,.4);auto c=d.observe(4000,.5);require(c.baseline&&c.delta==0&&d.value()==4000,"implausible jump counted as damage");});
        test("counter baseline, unreadable frame, repeated step and large jump",[]{CounterTracker c;require(!c.observe(0)&&!c.observe(0),"early counter baseline");require(c.observe(0)->baseline,"counter baseline missing");c.observe(1);c.observe({});auto step=c.observe(1);require(step&&!step->baseline&&step->from==0&&step->to==1,"counter step lost");require(!c.observe(0),"decreasing counter accepted");c.observe(9);c.observe(9);auto jump=c.observe(9);require(jump&&jump->baseline&&c.value()==9,"large jump fabricated results");c.reset();require(!c.value(),"counter reset retained baseline");});
        test("template-synthesized whole row reads kills assists and damage",[&]{auto row=HudDigits::readRow(counterRow(config,{"5","1","1194"}));require(row.kills==5&&row.assists==1&&row.damage==1194&&!row.hidden,"synthetic whole row misread");auto second=HudDigits::readRow(counterRow(config,{"15","4","3919"}));require(second.kills==15&&second.assists==4&&second.damage==3919,"second synthetic row misread");});
        test("template-synthesized damage-only row reports hidden counters",[&]{auto row=HudDigits::readRow(counterRow(config,{"3919"}));require(row.damage==3919&&row.hidden&&!row.kills&&!row.assists,"damage-only row misread");});
        test("own feed red knock icon and elimination are distinct",[]{
            OwnFeedAttribution feed;OcrRead name;name.words={{"Or1nge",.06,.55,.2,.4}};for(int i=0;i<3;++i)feed.observeName(name);feed.observeFeed({},0);
            OcrRead row;row.words={{"Or1nge",.1,.2,.15,.2},{"Enemy",.6,.2,.3,.2}};CpuImage image{100,50,std::vector<uint8_t>(100*50*4)};
            for(int y=12;y<16;++y)for(int x=40;x<44;++x)image.bgra[(size_t(y)*100+x)*4+2]=255;
            feed.observeFeed(row,.3,&image);auto knock=feed.observeFeed(row,.6,&image);require(knock.size()==1&&knock[0].knock,"red knock icon missing");
            std::fill(image.bgra.begin(),image.bgra.end(),0);feed.observeFeed(row,.9,&image);auto elimination=feed.observeFeed(row,1.2,&image);require(elimination.size()==1&&!elimination[0].knock,"elimination merged with knock");require(feed.observeFeed(row,1.5,&image).empty(),"feed duplicate reported");
        });
        test("excluded scene needs three continuous seconds",[]{
            ObservationRules flow({});std::string reason;flow.setBoundaryTrace([&](double,std::string r){reason=r;});Observation o;o.active=true;flow.process(o);o.active=false;o.time=1;o.excludedBy="观战";flow.process(o);o.time=3.9;require(!flow.process(o),"excluded boundary early");o.time=4;require(flow.process(o)&&reason.find("观战")!=std::string::npos,"excluded boundary missing");
        });
        test("ordinary HUD unavailable needs ten seconds",[]{ObservationRules flow({});Observation o;o.active=true;flow.process(o);o.active=false;o.time=1;flow.process(o);o.time=10.9;require(!flow.process(o),"HUD boundary early");o.time=11;require(flow.process(o),"HUD boundary missing");});
        test("reset is an immediate boundary",[]{ObservationRules flow({});Observation o;o.time=1;o.status="伤害计数重置";require(flow.process(o),"reset delayed");});
        test("two to nine second HUD gaps keep opponent history",[]{for(double gap:{2.,5.,9.}){ObservationRules flow({});Observation o;o.active=true;o.time=10;o.events={{10,ResultKind::Knockdown,"EnemyA","",1}};flow.process(o);o.active=false;o.events.clear();o.time=10.1;flow.process(o);o.time=10.1+gap;require(!flow.process(o),"short gap cut");o.active=true;o.time=10.2+gap;o.events={{o.time,ResultKind::Knockdown,"EnemyB","",1}};flow.process(o);flow.rules().tick(50);auto clips=flow.rules().takeReady();require(clips.size()==1&&clips[0].kills==2,"short gap lost opponent history");}});
        test("excluded timer clears between excluded scenes",[]{ObservationRules flow({});Observation o;o.active=true;flow.process(o);o.active=false;o.time=1;o.excludedBy="观战";flow.process(o);o.time=3;o.excludedBy="";flow.process(o);o.time=4;o.excludedBy="观战";require(!flow.process(o),"noncontinuous excluded time accumulated");o.time=6;require(!flow.process(o),"new excluded interval cut early");});
        test("damage uses detection time and splits samples over 500",[]{ObservationRules flow({});Observation o;o.active=true;o.time=20;o.damageAt=19;o.damageSince=18;o.damageDelta=600;flow.process(o);auto stats=flow.rules().burstStats(19);require(stats.sum==600&&stats.peak==600,"large sample was discarded or timestamp changed");});
        test("long missing damage is spread at half-second intervals",[]{ObservationRules flow({});Observation o;o.active=true;o.time=11;o.damageAt=10;o.damageSince=0;o.damageDelta=1000;flow.process(o);auto stats=flow.rules().burstStats(10);require(stats.sum==550&&stats.peak==250,"missing interval fabricated an instantaneous burst");});
        test("kill and assist counters alone never create results",[]{ObservationRules flow({});Observation o;o.active=true;o.time=10;o.killDelta=2;o.killCount=2;flow.process(o);flow.rules().tick(40);require(flow.rules().takeReady().empty()&&!flow.rules().pending(),"counter inference affected clips");});
        test("developer JSON parses after every append and replaces invalid UTF-8",[&]{
            DevRecorder recorder(temporary/"session.json",{{"ocrLanguage","zh-Hans-CN"}},180);
            for(int i=0;i<12;++i){recorder.append({{"type","prompt"},{"t",double(i)},{"text",std::string("名字")+char(0xff)}});auto doc=json::parse(std::ifstream(temporary/"session.json"));require(doc["entries"].size()==size_t(i+1),"append is not complete JSON");}
            ClipPlan clip{2,8,"manual","test",{},false,false,0,"手动保存"};auto slice=recorder.slice(clip,1.5);require(slice["entries"].size()==7,"slice range wrong");
            for(const auto& e:slice["entries"])require(std::abs(e["clipTime"].get<double>()-(e["t"].get<double>()-1.5))<1e-9,"clipTime not relative to keyframe");
            recorder.writeSlice(temporary/"clip.json",slice);require(json::parse(std::ifstream(temporary/"clip.json"))==json::parse(slice.dump(-1,' ',false,json::error_handler_t::replace)),"sidecar differs");
        });
        test("developer observations change-only plus ten second heartbeat",[&]{
            DevRecorder recorder(temporary/"hud.json",{},180);recorder.language("zh-Hans-CN");Observation o;o.time=0;recorder.observe(o);o.time=1;recorder.observe(o);o.time=10;recorder.observe(o);
            auto doc=json::parse(std::ifstream(temporary/"hud.json"));int hud=0;for(const auto& e:doc["entries"])if(e["type"]=="hud")++hud;require(hud==2,"HUD heartbeat or deduplication wrong");
        });
        test("developer retention and concurrent append preserve parseability",[&]{
            DevRecorder recorder(temporary/"concurrent.json",{},5);std::jthread a([&]{for(int i=0;i<100;++i)recorder.append({{"type","rule"},{"t",10.0}});});
            std::jthread b([&]{for(int i=0;i<100;++i)recorder.append({{"type","rule"},{"t",10.0}});});a.join();b.join();recorder.append({{"type","rule"},{"t",20.0}});recorder.append({{"type","manual_save"},{"t",1.0}});
            require(json::parse(std::ifstream(temporary/"concurrent.json"))["entries"].size()==202,"concurrent append lost entries");
            ClipPlan clip{0,20,"manual","test",{},false,false,0,""};require(recorder.slice(clip,0)["entries"].size()==1,"retention did not trim memory");
        });
        test("recorder I/O failure reports once and disables recording",[&]{
            auto blocked=temporary/"blocked";std::ofstream(blocked)<<"file";int errors=0;DevRecorder recorder(blocked/"session.json",{},180,[&](std::string){++errors;});
            recorder.append({{"t",1}});recorder.append({{"t",2}});require(errors==1&&recorder.path().empty(),"recorder failure repeats or remains enabled");
        });
        std::filesystem::remove_all(temporary);std::cout<<passed<<" HUD scenarios passed\n";return 0;
    }catch(...){std::filesystem::remove_all(temporary);return 1;}
}
