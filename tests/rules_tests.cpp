#include "../src/native/rules.hpp"
#include "../src/native/memory_policy.hpp"
#include <iostream>
#include <stdexcept>
using namespace apex;
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
CombatEvent kd(double t,const char* target) { return {t,ResultKind::Knockdown,target,"central prompt",1}; }
void burst(RuleEngine& r,double t,unsigned mag=1) { r.damage({t,80,mag,true}); r.damage({t+.5,90,mag,true}); }
int main() {
    int passed=0;
    auto test=[&](const char* name,auto body) { body(); ++passed; std::cout<<"PASS "<<name<<'\n'; };
    test("single spectacular knockdown no longer exports a short",[]{ RuleEngine r; burst(r,10); r.result(kd(11,"A")); r.tick(31); require(r.takeReady().empty(),"single burst exported a short"); });
    test("ordinary single discarded",[]{ RuleEngine r; r.damage({10,20,1,true}); r.result(kd(11,"A")); r.tick(31); require(r.takeReady().empty(),"ordinary kill saved"); });
    test("five kills across squads, long chase excluded",[]{ RuleEngine r; for(int i=0;i<5;++i) { burst(r,10+i*3,i+1); auto name=std::to_string(i); r.result(kd(11+i*3,name.c_str())); } r.tick(50); auto c=r.takeReady(); require(c.size()==1 && c[0].kills==5 && c[0].kind=="multikill" && c[0].end==28,"five kill merge / pursuit tail"); });
    test("later isolated burst does not extend the long clip or export a short",[]{ RuleEngine r; burst(r,10); r.result(kd(11,"A")); r.result(kd(14,"B")); r.tick(40); burst(r,70,2); r.result(kd(71,"C")); r.tick(91); auto c=r.takeReady(); require(c.size()==1 && c[0].kind=="multikill" && c[0].kills==2 && c[0].end==19,"late chase joined or exported a short"); });
    test("knockdown then elimination counted once",[]{ RuleEngine r; r.result(kd(10,"A")); r.result({13,ResultKind::Elimination,"A","",1}); r.tick(34); require(r.takeReady().empty(),"elimination promoted same target"); });
    test("sustained damage bridge",[]{ RuleEngine r; r.result(kd(10,"A")); for(double t:{14.,18.,22.,26.})r.damage({t,70,1,true}); r.result(kd(28,"B")); r.tick(48); auto c=r.takeReady(); require(c.size()==1 && c[0].kills==2,"valid bridge rejected"); });
    test("walking and chip damage separate two long fights",[]{ RuleEngine r; r.result(kd(10,"A"));r.result(kd(11,"B")); for(double t:{14.,18.,22.,26.})r.damage({t,5,1,true});r.result(kd(30,"C"));r.result(kd(31,"D")); r.tick(51);auto clips=r.takeReady();require(clips.size()==2&&clips[0].kills==2&&clips[1].kills==2,"chip damage bridged separate fights"); });
    test("reload excludes an unsupported assist from the long fight",[]{ RuleEngine r; r.damage({10,80,1,true}); r.damage({10.5,80,2,true}); r.result({11,ResultKind::Assist,"A","",1});r.result(kd(17,"B"));r.result(kd(18,"C"));r.tick(38);auto clips=r.takeReady();require(clips.size()==1&&clips[0].events.size()==2&&clips[0].start==7,"reload incorrectly admitted an assist"); });
    test("assist alone never exports a clip",[]{ RuleEngine r; r.result({10,ResultKind::Assist,"A","",1}); burst(r,30); r.result({31,ResultKind::Assist,"B","",1}); r.tick(51);require(r.takeReady().empty(),"assist exported a short"); });
    test("repeated prompt / teammate rejection",[]{ RuleEngine r; burst(r,9); for(int i=0;i<6;++i)r.result(kd(10+i*.1,"A")); r.result({12,ResultKind::Knockdown,"team","",.3});r.result(kd(14,"B")); r.tick(34); auto c=r.takeReady(); require(c.size()==1 && c[0].kills==2&&c[0].events.size()==2,"duplicate or teammate counted"); });
    test("match boundary prevents cross match merge",[]{ RuleEngine r; r.result(kd(10,"A")); r.boundary(12); r.result(kd(15,"B")); r.tick(35); require(r.takeReady().empty(),"cross match merge"); });
    test("forced stop clamps unavailable long clip tail",[]{ RuleEngine r; burst(r,9); r.result(kd(10,"A"));r.result(kd(12,"B")); r.boundary(13); auto c=r.takeReady(); require(c.size()==1 && c[0].end==13 && c[0].truncated&&c[0].kind=="multikill","unavailable tail"); });
    test("self allocation does not shrink memory budget",[]{constexpr uint64_t g=1ull<<30;auto before=replayMemoryBudget(32*g,96*g,0,60);auto after=replayMemoryBudget(24*g,96*g,8*g,60);require(before==after,"cache growth caused budget feedback");});
    test("game memory pressure and GUI percentage lower budget",[]{constexpr uint64_t g=1ull<<30;auto normal=replayMemoryBudget(24*g,96*g,8*g,60);auto pressure=replayMemoryBudget(8*g,96*g,8*g,60);auto low=replayMemoryBudget(24*g,96*g,8*g,30);require(pressure<normal && low==normal/2,"memory policy ignored external pressure or user limit");});
    test("ninety second split keeps two seconds of context",[]{RuleEngine r;for(int i=0;i<12;++i){burst(r,10+i*8,i+1);auto target=std::to_string(i);r.result(kd(11+i*8,target.c_str()));}r.tick(130);auto clips=r.takeReady();require(clips.size()==2,"long multikill did not split");for(const auto& clip:clips)require(clip.end-clip.start<=90.001,"clip exceeds ninety seconds");require(std::abs(clips[0].end-clips[1].start-2)<.001,"missing two second context");});
    test("resolved OCR target does not count elimination twice",[]{RuleEngine r;r.result(kd(10,"hud-result-1"));r.resolveTarget("hud-result-1","BBC");r.result({12,ResultKind::Elimination,"BBC","",1});r.tick(32);require(r.takeReady().empty(),"resolved elimination doubled knockdown");});
    test("wipe before one result does not fabricate a long clip",[]{RuleEngine r;burst(r,10);r.result({11,ResultKind::SquadWipe,"","",1});r.result(kd(11.1,"A"));r.tick(32);require(r.takeReady().empty(),"wipe fabricated another kill");});
    test("single-result final segment stays part of the long fight",[]{RuleEngine r;for(int i=0;i<11;++i){burst(r,10+i*8,i+1);auto target=std::to_string(i);r.result(kd(11+i*8,target.c_str()));}r.tick(120);auto clips=r.takeReady();require(clips.size()==2&&clips[1].kills==1&&clips[1].kind=="multikill","split long fight lost its final segment");require(std::abs(clips[0].end-clips[1].start-2)<.001,"split tail lost overlap");});
    test("forced stop discards an isolated burst",[]{RuleEngine r;burst(r,9);r.result(kd(10,"A"));r.boundary(11);require(r.takeReady().empty(),"stopping exported a short burst");});
    std::cout<<passed<<" rule scenarios passed\n";
}
