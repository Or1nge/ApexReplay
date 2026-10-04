#pragma once
#include <algorithm>
#include <optional>
namespace apex {
// Cumulative damage from the top-right counter. The number rolls through intermediate values and is often
// unreadable for a frame or two while it changes, so the last confirmed value survives unreadable frames.
// An increase needs two agreeing reads; it is reported with the time it was first seen and the time of the
// previous confirmed value, so a gap after a long occlusion can be spread instead of becoming one spike.
struct DamageChange { int delta=0; double at=0,since=0; bool baseline=false,reset=false; };
class DamageTracker {
    std::optional<int> value_,pending_,lower_;
    int pendingHits_=0,lowerHits_=0;double seenAt_=0,pendingAt_=0;
public:
    void reset(){value_.reset();pending_.reset();lower_.reset();pendingHits_=lowerHits_=0;seenAt_=pendingAt_=0;}
    std::optional<int> value()const{return value_;}
    DamageChange observe(std::optional<int> reading,double time){
        DamageChange change;
        if(!reading)return change;
        int current=*reading;
        auto stable=[&](int needed){if(pending_&&*pending_==current)++pendingHits_;else{pending_=current;pendingAt_=time;pendingHits_=1;}
            if(pendingHits_<needed)return false;value_=current;seenAt_=time;pending_.reset();pendingHits_=0;return true;};
        if(!value_){change.baseline=stable(3);return change;}
        if(current==*value_){seenAt_=time;pending_.reset();pendingHits_=0;lower_.reset();lowerHits_=0;return change;}
        if(current<*value_){
            // A large drop held for three reads is a new match; small drops are misreads of a changing number.
            if(*value_-current>100){if(lower_&&*lower_==current)++lowerHits_;else{lower_=current;lowerHits_=1;}
                if(lowerHits_>=3){value_=current;seenAt_=time;lower_.reset();lowerHits_=0;pending_.reset();pendingHits_=0;change.reset=true;}}
            else{lower_.reset();lowerHits_=0;}
            return change;
        }
        lower_.reset();lowerHits_=0;
        // Growth beyond what fits in the time since the last confirmed read is a misread or a missed match
        // change; it only becomes a new baseline once stable, without being counted as damage.
        if(current-*value_>500+400*std::max(0.0,time-seenAt_-1)){change.baseline=stable(3);return change;}
        if(pending_&&current>=*pending_&&time-pendingAt_<=1){
            change={current-*value_,pendingAt_,seenAt_,false,false};value_=current;seenAt_=time;pending_.reset();pendingHits_=0;
        }else{pending_=current;pendingAt_=time;pendingHits_=1;}
        return change;
    }
};
// Kill and assist counters only grow during a match. A step of up to three needs two agreeing reads; a larger
// jump (a capture started mid-match, or a misread) needs three and becomes the baseline without a result.
struct CounterChange { int from=0,to=0;bool baseline=false; };
class CounterTracker {
    std::optional<int> value_,pending_;int hits_=0;
public:
    void reset(){value_.reset();pending_.reset();hits_=0;}
    std::optional<int> value()const{return value_;}
    std::optional<CounterChange> observe(std::optional<int> reading){
        if(!reading)return {};
        int current=*reading;
        if(value_&&current==*value_){pending_.reset();hits_=0;return {};}
        if(value_&&current<*value_)return {};
        if(pending_&&*pending_==current)++hits_;else{pending_=current;hits_=1;}
        bool step=value_&&current-*value_<=3;
        if(hits_<(step?2:3))return {};
        CounterChange change{value_.value_or(current),current,!step};
        value_=current;pending_.reset();hits_=0;return change;
    }
};
}
