#pragma once
#include "rules.hpp"
#include "hud_attribution.hpp"
#include "json.hpp"
namespace apex {
using json=nlohmann::json;
inline json eventJson(const CombatEvent& e) {
    constexpr const char* kinds[]={"knockdown","elimination","assist","squad_wipe"};
    json value{{"time",e.time},{"kind",kinds[static_cast<int>(e.kind)]},{"target",e.target},{"confidence",e.confidence},{"evidence",e.evidence}};
    if(!e.id.empty())value["id"]=e.id;return value;
}
inline json clipJson(const ClipPlan& c) {
    json events=json::array(); for(const auto& e:c.events)events.push_back(eventJson(e));
    return {{"start",c.start},{"end",c.end},{"kind",c.kind},{"kills",c.kills},{"ruleVersion",c.version},
        {"squadWipe",c.squadWipe},{"truncated",c.truncated},{"reason",c.reason},{"events",events}};
}
inline json optionalJson(const std::optional<int>& value){return value?json(*value):json(nullptr);}
struct Observation {
    double time=0;bool active=false,firing=false,reload=false,hitFeedback=false,countersHidden=false,damageBaseline=false,feedRead=false;
    std::optional<int> ammo,totalDamage,confirmedDamage,kills,assists,killCount,assistCount;
    std::optional<bool> ownFeedKnock;
    std::string weapon,status,prompt,playerName,ownFeedTarget,excludedBy,feedText;
    unsigned magazine=1;int damageDelta=0,killDelta=0,assistDelta=0;double damageAt=0,damageSince=0;
    std::vector<CombatEvent> events;
    std::vector<std::pair<std::string,std::string>> targetAliases;
    std::vector<std::pair<std::string,ResultKind>> resultCorrections;
    std::vector<FeedEntry> feed;
    json toJson()const {
        json eventsJson=json::array();for(const auto& e:events)eventsJson.push_back(eventJson(e));
        json feedJson=json::array();for(const auto& f:feed)feedJson.push_back({{"victim",f.victim},{"knock",f.knock},{"id",f.id}});
        json out{{"time",time},{"active",active},{"firing",firing},{"reload",reload},{"hitFeedback",hitFeedback},
            {"weapon",weapon},{"magazine",magazine},{"damageDelta",damageDelta},{"status",status},{"prompt",prompt},{"events",eventsJson},{"playerName",playerName},{"ownFeedTarget",ownFeedTarget},{"targetAliases",targetAliases},{"resultCorrections",resultCorrections},
            {"countersHidden",countersHidden},{"killDelta",killDelta},{"assistDelta",assistDelta},{"excludedBy",excludedBy},{"ownFeed",feedJson},{"feedRead",feedRead},{"feedText",feedText}};
        out["ammo"]=optionalJson(ammo);out["totalDamage"]=optionalJson(totalDamage);out["confirmedDamage"]=optionalJson(confirmedDamage);
        out["kills"]=optionalJson(kills);out["assists"]=optionalJson(assists);out["killCount"]=optionalJson(killCount);out["assistCount"]=optionalJson(assistCount);
        out["ownFeedKnock"]=ownFeedKnock?json(*ownFeedKnock):json(nullptr);return out;
    }
};
// Healing, the map and grenades hide the ammo HUD for seconds at a time, so only a long gap or a lasting
// spectator / recap / champion screen ends the match; a damage counter reset ends it at once.
class ObservationRules {
    RuleEngine rules_;double inactiveSince_=-1,excludedSince_=-1;bool active_=false;
    std::function<void(double,std::string)> trace_;
public:
    explicit ObservationRules(Rules rules):rules_(std::move(rules)){}
    RuleEngine& rules(){return rules_;}
    void setBoundaryTrace(std::function<void(double,std::string)> trace){trace_=std::move(trace);}
    bool process(const Observation& observation){
        bool cut=false;
        if(observation.status.find("重置")!=std::string::npos){boundary(observation.time,"伤害计数重置");cut=true;}
        else if(observation.active){active_=true;inactiveSince_=excludedSince_=-1;
            for(const auto& alias:observation.targetAliases)rules_.resolveTarget(alias.first,alias.second);
            for(const auto& correction:observation.resultCorrections)rules_.correctResult(correction.first,correction.second);
            if(observation.damageDelta>0){
                double span=observation.damageAt-observation.damageSince;
                int count=std::max((observation.damageDelta+499)/500,span>2?int(std::ceil(span/.5)):1);
                for(int i=0;i<count;++i){int delta=observation.damageDelta/count+(i<observation.damageDelta%count?1:0);
                    double at=span>2?observation.damageSince+span*(i+1)/count:observation.damageAt;
                    rules_.damage({at,delta,observation.magazine,observation.firing});}
            }
            for(const auto& event:observation.events)rules_.result(event);
        }else if(active_){
            if(inactiveSince_<0)inactiveSince_=observation.time;
            if(observation.excludedBy.empty())excludedSince_=-1;else if(excludedSince_<0)excludedSince_=observation.time;
            if(excludedSince_>=0&&observation.time-excludedSince_>=3){boundary(observation.time,"排除画面持续至少 3 秒："+observation.excludedBy);cut=true;}
            else if(observation.time-inactiveSince_>=10){boundary(observation.time,"战斗 HUD 不可用持续至少 10 秒");cut=true;}
        }
        rules_.tick(observation.time);return cut;
    }
    void boundary(double time,std::string reason="采集结束"){rules_.boundary(time);active_=false;inactiveSince_=excludedSince_=-1;if(trace_)trace_(time,std::move(reason));}
};
}
