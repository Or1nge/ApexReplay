#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "names.hpp"

namespace apex {
enum class ResultKind { Knockdown, Elimination, Assist, SquadWipe };
inline const char* resultKindName(ResultKind kind){constexpr const char* names[]={"knockdown","elimination","assist","squad_wipe"};return names[static_cast<int>(kind)];}
struct Rules {
    std::string version = "apex-zh-v3-results";
    double burstDamage=250, burstSeconds=5, fastBurstDamage=150, fastBurstSeconds=2;
    double resultGraceSeconds=2, fastMergeSeconds=12, bridgeMergeSeconds=20;
    double bridgeDamage=60, bridgeDamageSeconds=3, bridgeQuietSeconds=6;
    double maxClipSeconds=90, splitOverlapSeconds=2;
    double longPre=10, longPost=5;
};
struct CombatEvent {
    double time=0;
    ResultKind kind=ResultKind::Knockdown;
    std::string target, evidence;
    double confidence=1;
    std::string id;
};
struct DamageSample { double time=0; int damage=0; unsigned magazine=0; bool firing=false; };
struct Burst { double start=0,end=0; int damage=0; };
// Best damage windows before a result, kept for diagnostics even when the criteria are not met.
struct BurstStats { int sum=0,peak=0; bool qualified=false; double start=0,end=0; };
struct ClipPlan {
    double start=0,end=0;
    std::string kind,version;
    std::vector<CombatEvent> events;
    bool truncated=false, squadWipe=false;
    size_t kills=0;
    std::string reason;
};
// One rule decision, reported to the developer log.
struct RuleNote {
    double time=0;
    std::string what,detail,target;
    std::optional<ResultKind> kind;
    size_t opponents=0;
    std::optional<BurstStats> burst;
    std::optional<ClipPlan> clip;
};
class RuleEngine {
    struct Candidate {
        double firstAction=0,lastResult=0;
        bool longContinuation=false, squadWipe=false,highDamage=false;
        std::set<std::string> opponents;
        std::vector<CombatEvent> events;
        int bestSum=0,bestPeak=0;
    };
    struct Knock { double time=0; bool finished=false; std::string name; };
    Rules rules_;
    std::deque<DamageSample> damage_;
    std::optional<Candidate> candidate_;
    std::unordered_map<std::string,Knock> knocked_;
    std::unordered_map<std::string,double> seen_;
    std::optional<double> splitStart_;
    std::vector<ClipPlan> ready_;
    double lastWipe_=-100;
    std::function<void(RuleNote)> trace_;
    void note(RuleNote value) const { if (trace_) trace_(std::move(value)); }
    static std::string identity(const CombatEvent& e) {
        return !e.id.empty()?e.id:e.target.empty() ? "unknown@"+std::to_string(e.time) : e.target;
    }
    static std::string victimToken(ResultKind kind,const std::string& target){
        std::string token="victim:"+std::to_string(static_cast<int>(kind));for(auto c:nameCharacters(target))token+=":"+std::to_string(c);return token;
    }
    std::optional<Burst> burst(double time) const {
        for(size_t i=0;i<damage_.size();++i){
            const auto& first=damage_[i];
            if(first.time>time||time-first.time>rules_.burstSeconds+rules_.resultGraceSeconds)continue;
            int sum=0,peak=0;double end=first.time;size_t last=i;
            for(size_t j=i;j<damage_.size();++j){const auto& d=damage_[j];if(d.time-first.time>rules_.burstSeconds||d.time>time)break;sum+=d.damage;end=d.time;last=j;}
            if(sum<rules_.burstDamage||time-end>rules_.resultGraceSeconds)continue;
            for(size_t j=i;j<=last;++j){int fast=0;for(size_t k=j;k<=last&&damage_[k].time-damage_[j].time<=rules_.fastBurstSeconds;++k)fast+=damage_[k].damage;peak=std::max(peak,fast);}
            if(peak>rules_.fastBurstDamage)return Burst{first.time,end,sum};
        }
        return {};
    }
    bool bridge(double from,double to) const {
        if (to-from>rules_.bridgeMergeSeconds) return false;
        double previous=from; bool meaningful=false;
        for (size_t i=0;i<damage_.size();++i) {
            const auto& d=damage_[i];
            if (d.time<=from || d.time>to || !d.firing) continue;
            int sum=0;
            for (size_t j=i;j<damage_.size();++j) {
                if (damage_[j].time-d.time>rules_.bridgeDamageSeconds || damage_[j].time>to) break;
                if (damage_[j].firing) sum+=damage_[j].damage;
            }
            if(sum<rules_.bridgeDamage) continue;
            if (d.time-previous>rules_.bridgeQuietSeconds) return false;
            previous=d.time;
            meaningful=true;
        }
        return meaningful && to-previous<=rules_.bridgeQuietSeconds;
    }
    bool unfinishedFight(double time)const{
        if(!candidate_||candidate_->opponents.size()!=1||time-candidate_->lastResult>rules_.bridgeMergeSeconds)return false;
        for(const auto& key:candidate_->opponents)if(auto it=knocked_.find(key);it!=knocked_.end()&&!it->second.finished)return true;
        return false;
    }
    // An elimination usually finishes an opponent this player knocked earlier. Both prompts are read
    // separately, so the names can differ by OCR or be unreadable on either side.
    std::optional<std::string> knockFinishedBy(const CombatEvent& event,const std::string& target) const {
        auto open=[&](const Knock& knock){return !knock.finished&&event.time>=knock.time&&event.time-knock.time<300;};
        if (auto it=knocked_.find(target); it!=knocked_.end() && open(it->second)) return target;
        std::optional<std::string> unnamed; double newest=-1e9;
        for (const auto& [name,knock]:knocked_) {
            if (!open(knock)) continue;
            auto display=knock.name.empty()?name:knock.name;auto victim=event.target.empty()?target:event.target;
            bool named=!unnamedTarget(display)&&!unnamedTarget(victim);
            if (named && sameOpponentName(display,victim)) return name;
            if (!named && event.time-knock.time<=90 && knock.time>newest) { unnamed=name; newest=knock.time; }
        }
        return unnamed;
    }
    std::string discardReason(const Candidate& c) const {
        return "只有 "+std::to_string(c.opponents.size())+" 名敌人且伤害未达标：5 秒内最高 "+std::to_string(c.bestSum)+"（需 ≥"+std::to_string(int(rules_.burstDamage))+
            "），2 秒内最高 "+std::to_string(c.bestPeak)+"（需 >"+std::to_string(int(rules_.fastBurstDamage))+"）";
    }
    void finish(double available,bool forced=false) {
        if (!candidate_) return;
        const auto& c=*candidate_;
        if (c.opponents.size()>=2 || c.longContinuation || c.highDamage) {
            double start=std::max(0.0,c.firstAction-rules_.longPre);
            if (splitStart_) start=std::max(start,*splitStart_);
            double wanted=c.lastResult+rules_.longPost;
            double end=std::min(wanted,available);
            std::string reason=c.opponents.size()>=2?"连续击倒或淘汰 "+std::to_string(c.opponents.size())+" 名敌人":
                c.longContinuation?"长镜头分段的后续部分":"高伤害：5 秒内 "+std::to_string(c.bestSum)+"，2 秒内 "+std::to_string(c.bestPeak);
            if (end>start) {
                ClipPlan plan{start,end,c.opponents.size()>=2||c.longContinuation?"multikill":"burst",rules_.version,c.events,
                    forced && end<wanted,c.squadWipe,c.opponents.size(),reason};
                note({available,"clip_planned",reason,"",{},c.opponents.size(),{},plan});
                ready_.push_back(std::move(plan));
            } else note({available,"candidate_discarded","可用画面不足："+reason,"",{},c.opponents.size()});
        } else note({available,"candidate_discarded",discardReason(c),"",{},c.opponents.size()});
        candidate_.reset(); splitStart_.reset();
    }
public:
    explicit RuleEngine(Rules rules={}):rules_(std::move(rules)) {}
    void setTrace(std::function<void(RuleNote)> trace){ trace_=std::move(trace); }
    const Rules& rules() const { return rules_; }
    void updateTimings(double lp,double lo) {
        rules_.longPre=std::clamp(lp,0.0,30.0); rules_.longPost=std::clamp(lo,0.0,20.0);
    }
    void updateThresholds(double seconds,double damage,double fastSeconds,double fastDamage){
        rules_.burstSeconds=std::clamp(seconds,1.0,15.0);rules_.burstDamage=std::clamp(damage,50.0,2000.0);
        rules_.fastBurstSeconds=std::clamp(fastSeconds,.5,rules_.burstSeconds);rules_.fastBurstDamage=std::clamp(fastDamage,0.0,1000.0);
    }
    BurstStats burstStats(double time) const {
        BurstStats best;
        for(size_t i=0;i<damage_.size();++i){
            const auto& first=damage_[i];
            if(first.time>time||time-first.time>rules_.burstSeconds+rules_.resultGraceSeconds)continue;
            int sum=0,peak=0;double end=first.time;size_t last=i;
            for(size_t j=i;j<damage_.size();++j){const auto& d=damage_[j];if(d.time-first.time>rules_.burstSeconds||d.time>time)break;sum+=d.damage;end=d.time;last=j;}
            if(time-end>rules_.resultGraceSeconds)continue;
            for(size_t j=i;j<=last;++j){int fast=0;for(size_t k=j;k<=last&&damage_[k].time-damage_[j].time<=rules_.fastBurstSeconds;++k)fast+=damage_[k].damage;peak=std::max(peak,fast);}
            if(sum>best.sum){best.sum=sum;best.start=first.time;best.end=end;}
            best.peak=std::max(best.peak,peak);
        }
        if(auto b=burst(time)){best.qualified=true;best.start=b->start;best.end=b->end;}
        return best;
    }
    void damage(DamageSample sample) {
        if (sample.damage<=0 || sample.damage>500 || !std::isfinite(sample.time)) return;
        damage_.push_back(sample);
        while (!damage_.empty() && sample.time-damage_.front().time>120) damage_.pop_front();
    }
    void result(const CombatEvent& event) {
        if (event.confidence<0.8 || !std::isfinite(event.time)) { note({event.time,"result_ignored","置信度不足",event.target,event.kind}); return; }
        if (event.kind==ResultKind::SquadWipe) {
            lastWipe_=event.time;
            if (candidate_ && event.time-candidate_->lastResult<=3) candidate_->squadWipe=true;
            note({event.time,"squad_wipe","小队全灭提示","",event.kind});
            return;
        }
        const std::string target=identity(event);
        const std::string token=std::to_string(static_cast<int>(event.kind))+":"+target;
        if (seen_.contains(token) && event.time-seen_[token]<30) { note({event.time,"result_duplicate","30 秒内重复的同一结果",target,event.kind}); return; }
        seen_[token]=event.time;
        if(!event.id.empty()&&!unnamedTarget(event.target)&&(event.kind==ResultKind::Knockdown||event.kind==ResultKind::Elimination)){
            auto victim=victimToken(event.kind,event.target);
            if(seen_.contains(victim)&&event.time-seen_[victim]<30){note({event.time,"result_duplicate","同一敌人的结果重新出现，不重复计人",event.target,event.kind,candidate_?candidate_->opponents.size():0});return;}
            seen_[victim]=event.time;
        }
        if(event.kind==ResultKind::Knockdown&&!event.id.empty())for(const auto& [key,knock]:knocked_)
            if(event.time-knock.time<30&&candidate_&&candidate_->opponents.contains(key)&&sameOpponentName(knock.name,event.target)){
                note({event.time,"result_duplicate","同一敌人的重复击倒信息",event.target,event.kind,candidate_->opponents.size()});return;}
        if (event.kind==ResultKind::Elimination) {
            if (auto knock=knockFinishedBy(event,target)) {
                knocked_[*knock].finished=true;
                if (candidate_&&candidate_->opponents.contains(*knock)){candidate_->events.push_back(event);candidate_->lastResult=std::max(candidate_->lastResult,event.time);}
                note({event.time,"elimination_of_knocked","淘汰的是之前击倒的 "+*knock+"，不重复计人",target,event.kind,candidate_?candidate_->opponents.size():0});
                if (unnamedTarget(*knock) && !unnamedTarget(target)) resolveTarget(*knock,target);
                return;
            }
        }
        auto b=burst(event.time);
        auto stats=burstStats(event.time);
        if (event.kind==ResultKind::Assist && !b) {
            note({event.time,"assist_ignored","助攻但伤害未达标，不保存","",event.kind,candidate_?candidate_->opponents.size():0,stats});
            return;
        }
        tick(event.time);
        if (candidate_) {
            double gap=event.time-candidate_->lastResult;
            // Retain the first confirmed knock across healing/reloading within the existing
            // 20-second limit; completed multikill sequences still need a damage bridge.
            if (gap>rules_.fastMergeSeconds && !bridge(candidate_->lastResult,event.time)&&!unfinishedFight(event.time)) finish(event.time);
        }
        double action=b?b->start:event.time;
        if (!b) {
            for (const auto& d:damage_) if (d.firing && d.time>=event.time-3 && d.time<=event.time) { action=d.time; break; }
        }
        bool continuingLong=false;
        if (candidate_) {
            double beginning=splitStart_.value_or(std::max(0.0,candidate_->firstAction-rules_.longPre));
            double post=rules_.longPost;
            if (event.time+post-beginning>rules_.maxClipSeconds) {
                double priorEnd=candidate_->lastResult+post;
                continuingLong=candidate_->opponents.size()>=2 || candidate_->longContinuation;
                finish(event.time);
                splitStart_=std::max(0.0,priorEnd-rules_.splitOverlapSeconds);
            }
        }
        bool fresh=!candidate_;
        if (!candidate_) candidate_=Candidate{action,event.time,continuingLong,false,false,{},{}};
        candidate_->highDamage|=b.has_value();
        candidate_->bestSum=std::max(candidate_->bestSum,stats.sum);
        candidate_->bestPeak=std::max(candidate_->bestPeak,stats.peak);
        candidate_->lastResult=event.time;
        candidate_->squadWipe|=event.time-lastWipe_<=3;
        candidate_->events.push_back(event);
        if (event.kind!=ResultKind::Assist) candidate_->opponents.insert(target);
        if (event.kind==ResultKind::Knockdown) knocked_[target]=Knock{event.time,false,event.target};
        note({event.time,fresh?"candidate_started":"candidate_extended",
            std::string(event.kind==ResultKind::Assist?"助攻":"计为敌人 ")+(event.kind==ResultKind::Assist?"":target)+(b?"；伤害达标":"；伤害未达标"),
            target,event.kind,candidate_->opponents.size(),stats});
    }
    void tick(double now) {
        if (candidate_ && now-candidate_->lastResult>=std::max(rules_.bridgeMergeSeconds,rules_.longPost)) finish(now);
        for (auto it=seen_.begin();it!=seen_.end();) it=now-it->second>300?seen_.erase(it):std::next(it);
        for (auto it=knocked_.begin();it!=knocked_.end();) it=now-it->second.time>300?knocked_.erase(it):std::next(it);
    }
    void boundary(double available) { finish(available,true); damage_.clear(); knocked_.clear(); seen_.clear();lastWipe_=-100; }
    bool pending() const { return candidate_.has_value(); }
    void resolveTarget(const std::string& oldTarget,const std::string& target){
        if(oldTarget==target||target.empty())return;
        if(candidate_){if(candidate_->opponents.erase(oldTarget))candidate_->opponents.insert(target);for(auto& e:candidate_->events)if(e.target==oldTarget)e.target=target;}
        for(auto& [key,knock]:knocked_)if(knock.name==oldTarget)knock.name=target;
        if(knocked_.contains(oldTarget)){knocked_[target]=knocked_[oldTarget];knocked_.erase(oldTarget);}
        for(int kind=0;kind<3;++kind){auto old=std::to_string(kind)+":"+oldTarget;if(seen_.contains(old)){seen_[std::to_string(kind)+":"+target]=seen_[old];seen_.erase(old);}}
        for(auto kind:{ResultKind::Knockdown,ResultKind::Elimination}){auto old=victimToken(kind,oldTarget),resolved=victimToken(kind,target);if(old!=resolved&&seen_.contains(old)){seen_[resolved]=seen_[old];seen_.erase(old);}}
    }
    void correctResult(const std::string& target,ResultKind kind){
        if(kind!=ResultKind::Assist)return;
        if(candidate_){
            for(auto& event:candidate_->events)if(event.target==target&&event.kind==ResultKind::Knockdown)event.kind=kind;
            bool ownResult=false;for(const auto& event:candidate_->events)if(event.target==target&&event.kind!=ResultKind::Assist)ownResult=true;
            if(!ownResult)candidate_->opponents.erase(target);
        }
        knocked_.erase(target);
    }
    std::optional<double> earliestNeeded()const{if(!candidate_)return {};return splitStart_.value_or(std::max(0.,candidate_->firstAction-rules_.longPre-1));}
    std::vector<ClipPlan> takeReady() { auto r=std::move(ready_); ready_.clear(); return r; }
};
}
