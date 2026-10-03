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

namespace apex {
enum class ResultKind { Knockdown, Elimination, Assist, SquadWipe };
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
};
struct DamageSample { double time=0; int damage=0; unsigned magazine=0; bool firing=false; };
struct Burst { double start=0,end=0; int damage=0; };
struct ClipPlan {
    double start=0,end=0;
    std::string kind,version;
    std::vector<CombatEvent> events;
    bool truncated=false, squadWipe=false;
    size_t kills=0;
};
class RuleEngine {
    struct Candidate {
        double firstAction=0,lastResult=0;
        bool longContinuation=false, squadWipe=false,highDamage=false;
        std::set<std::string> opponents;
        std::vector<CombatEvent> events;
    };
    Rules rules_;
    std::deque<DamageSample> damage_;
    std::optional<Candidate> candidate_;
    std::unordered_map<std::string,double> knocked_;
    std::unordered_map<std::string,double> seen_;
    std::optional<double> splitStart_;
    std::vector<ClipPlan> ready_;
    double lastWipe_=-100;
    static std::string identity(const CombatEvent& e) {
        return e.target.empty() ? "unknown@"+std::to_string(e.time) : e.target;
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
    void finish(double available,bool forced=false) {
        if (!candidate_) return;
        const auto& c=*candidate_;
        if (c.opponents.size()>=2 || c.longContinuation || c.highDamage) {
            double start=std::max(0.0,c.firstAction-rules_.longPre);
            if (splitStart_) start=std::max(start,*splitStart_);
            double wanted=c.lastResult+rules_.longPost;
            double end=std::min(wanted,available);
            if (end>start) ready_.push_back({start,end,c.opponents.size()>=2||c.longContinuation?"multikill":"burst",rules_.version,c.events,
                forced && end<wanted,c.squadWipe,c.opponents.size()});
        }
        candidate_.reset(); splitStart_.reset();
    }
public:
    explicit RuleEngine(Rules rules={}):rules_(std::move(rules)) {}
    void updateTimings(double lp,double lo) {
        rules_.longPre=std::clamp(lp,0.0,30.0); rules_.longPost=std::clamp(lo,0.0,20.0);
    }
    void updateThresholds(double seconds,double damage,double fastSeconds,double fastDamage){
        rules_.burstSeconds=std::clamp(seconds,1.0,15.0);rules_.burstDamage=std::clamp(damage,50.0,2000.0);
        rules_.fastBurstSeconds=std::clamp(fastSeconds,.5,rules_.burstSeconds);rules_.fastBurstDamage=std::clamp(fastDamage,0.0,1000.0);
    }
    void damage(DamageSample sample) {
        if (sample.damage<=0 || sample.damage>500 || !std::isfinite(sample.time)) return;
        damage_.push_back(sample);
        while (!damage_.empty() && sample.time-damage_.front().time>120) damage_.pop_front();
    }
    void result(const CombatEvent& event) {
        if (event.confidence<0.8 || !std::isfinite(event.time)) return;
        if (event.kind==ResultKind::SquadWipe) {
            lastWipe_=event.time;
            if (candidate_ && event.time-candidate_->lastResult<=3) candidate_->squadWipe=true;
            return;
        }
        const std::string target=identity(event);
        const std::string token=std::to_string(static_cast<int>(event.kind))+":"+target;
        if (seen_.contains(token) && event.time-seen_[token]<30) return;
        seen_[token]=event.time;
        if (event.kind==ResultKind::Elimination && knocked_.contains(target) && event.time-knocked_[target]<300) {
            if (candidate_) candidate_->events.push_back(event);
            return;
        }
        auto b=burst(event.time);
        if(event.kind==ResultKind::Elimination&&!b){for(const auto& [prior,time]:knocked_)if(prior.starts_with("hud-result-")&&event.time-time<300){if(candidate_)candidate_->events.push_back(event);return;}}
        if (event.kind==ResultKind::Assist && !b) return;
        tick(event.time);
        if (candidate_) {
            double gap=event.time-candidate_->lastResult;
            if (gap>rules_.fastMergeSeconds && !bridge(candidate_->lastResult,event.time)) finish(event.time);
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
        if (!candidate_) candidate_=Candidate{action,event.time,continuingLong,false,false,{},{}};
        candidate_->highDamage|=b.has_value();
        candidate_->lastResult=event.time;
        candidate_->squadWipe|=event.time-lastWipe_<=3;
        candidate_->events.push_back(event);
        if (event.kind!=ResultKind::Assist) candidate_->opponents.insert(target);
        if (event.kind==ResultKind::Knockdown) knocked_[target]=event.time;
    }
    void tick(double now) {
        if (candidate_ && now-candidate_->lastResult>=std::max(rules_.bridgeMergeSeconds,rules_.longPost)) finish(now);
        for (auto it=seen_.begin();it!=seen_.end();) it=now-it->second>300?seen_.erase(it):std::next(it);
        for (auto it=knocked_.begin();it!=knocked_.end();) it=now-it->second>300?knocked_.erase(it):std::next(it);
    }
    void boundary(double available) { finish(available,true); damage_.clear(); knocked_.clear(); seen_.clear();lastWipe_=-100; }
    bool pending() const { return candidate_.has_value(); }
    void resolveTarget(const std::string& oldTarget,const std::string& target){
        if(oldTarget==target||target.empty())return;
        if(candidate_){if(candidate_->opponents.erase(oldTarget))candidate_->opponents.insert(target);for(auto& e:candidate_->events)if(e.target==oldTarget)e.target=target;}
        if(knocked_.contains(oldTarget)){knocked_[target]=knocked_[oldTarget];knocked_.erase(oldTarget);}
        for(int kind=0;kind<3;++kind){auto old=std::to_string(kind)+":"+oldTarget;if(seen_.contains(old)){seen_[std::to_string(kind)+":"+target]=seen_[old];seen_.erase(old);}}
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
