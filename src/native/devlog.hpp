#pragma once
#include "observation.hpp"
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <tuple>
namespace apex {
// Developer-mode timeline of one capture session. The file is rewritten at its fixed tail after every entry, so it
// stays a complete JSON document even if the worker stops abruptly; recent entries stay in memory for clip sidecars.
class DevRecorder {
    mutable std::mutex mutex_;std::filesystem::path file_;std::fstream stream_;json session_,lastHud_;
    std::deque<json> entries_;double retention_=1920,newest_=0,lastHudAt_=-100;
    std::string prompt_,feed_;bool promptSeen_=false,feedSeen_=false,first_=true,enabled_=true;
    std::function<void(std::string)> error_;
    static std::string dump(const json& value){return value.dump(-1,' ',false,json::error_handler_t::replace);}
    void fail(std::unique_lock<std::mutex>& lock)noexcept{
        enabled_=false;stream_.close();lock.unlock();
        try{if(error_)error_("开发者记录写入失败，已停用记录；采集继续");}catch(...){}
    }
    void appendLocked(json entry){
        if(!enabled_)return;
        auto text=dump(entry);stream_.seekp(-4,std::ios::end);if(!first_)stream_<<",\n";stream_<<text<<"\n]}\n";stream_.flush();
        if(!stream_)throw std::runtime_error("开发者记录写入失败");first_=false;
        double time=entry.value("t",0.0);newest_=std::max(newest_,time);
        if(entries_.empty()||entries_.back().value("t",0.0)<=time)entries_.push_back(std::move(entry));
        else{auto position=std::upper_bound(entries_.begin(),entries_.end(),time,[](double t,const json& e){return t<e.value("t",0.0);});entries_.insert(position,std::move(entry));}
        while(!entries_.empty()&&entries_.front().value("t",0.0)<newest_-retention_)entries_.pop_front();
    }
public:
    DevRecorder(std::filesystem::path file,json session,double retention,std::function<void(std::string)> error={})
        :file_(std::move(file)),session_(std::move(session)),retention_(retention),error_(std::move(error)){
        std::unique_lock lock(mutex_);
        try{std::filesystem::create_directories(file_.parent_path());stream_.open(file_,std::ios::binary|std::ios::in|std::ios::out|std::ios::trunc);
            stream_<<"{\"format\":\"apex-replay-dev-v1\",\"session\":"<<dump(session_)<<",\"entries\":[\n]}\n";stream_.flush();if(!stream_)throw std::runtime_error("开发者记录创建失败");}
        catch(...){fail(lock);}
    }
    std::filesystem::path path()const{std::lock_guard lock(mutex_);return enabled_?file_:std::filesystem::path();}
    void language(const std::string& language)noexcept{
        std::unique_lock lock(mutex_);if(!enabled_)return;
        try{if(session_.is_object()&&session_.value("ocrLanguage",json(nullptr))==language)return;session_["ocrLanguage"]=language;appendLocked({{"type","ocr"},{"t",newest_},{"language",language}});}catch(...){fail(lock);}
    }
    void retention(double seconds){std::lock_guard lock(mutex_);retention_=seconds;}
    void append(json entry)noexcept{std::unique_lock lock(mutex_);if(!enabled_)return;try{appendLocked(std::move(entry));}catch(...){fail(lock);}}
    void observe(const Observation& o)noexcept{
        std::unique_lock lock(mutex_);if(!enabled_)return;
        try{
            json hud{{"type","hud"},{"active",o.active},{"status",o.status},{"damage",optionalJson(o.totalDamage)},{"kills",optionalJson(o.kills)},{"assists",optionalJson(o.assists)},
                {"confirmedDamage",optionalJson(o.confirmedDamage)},{"killCount",optionalJson(o.killCount)},{"assistCount",optionalJson(o.assistCount)},{"ammo",optionalJson(o.ammo)},{"countersHidden",o.countersHidden},{"playerName",o.playerName}};
            if(hud!=lastHud_||o.time-lastHudAt_>=10){lastHud_=hud;lastHudAt_=o.time;hud["t"]=o.time;appendLocked(std::move(hud));}
            if(o.damageDelta>0)appendLocked({{"type","damage"},{"t",o.time},{"delta",o.damageDelta},{"total",optionalJson(o.confirmedDamage)},{"at",o.damageAt},{"since",o.damageSince},{"firing",o.firing},{"magazine",o.magazine}});
            if(o.damageBaseline)appendLocked({{"type","damage_baseline"},{"t",o.time},{"total",optionalJson(o.confirmedDamage)}});
            for(auto [type,delta,value]:{std::tuple{"kill_counter",o.killDelta,o.killCount},std::tuple{"assist_counter",o.assistDelta,o.assistCount}})
                if(delta>0&&value)appendLocked({{"type",type},{"t",o.time},{"from",*value-delta},{"to",*value}});
            if(!promptSeen_||prompt_!=o.prompt){promptSeen_=true;prompt_=o.prompt;appendLocked({{"type","prompt"},{"t",o.time},{"text",o.prompt},
                {"damage",optionalJson(o.totalDamage)},{"ownFeedTarget",o.ownFeedTarget},{"ownFeedKnock",o.ownFeedKnock?json(*o.ownFeedKnock):json(nullptr)}});}
            if(o.feedRead&&(!feedSeen_||feed_!=o.feedText)){feedSeen_=true;feed_=o.feedText;appendLocked({{"type","feed"},{"t",o.time},{"text",o.feedText}});}
            for(const auto& f:o.feed)appendLocked({{"type","feed_own"},{"t",f.time},{"victim",f.victim},{"knock",f.knock},{"id",f.id}});
            for(const auto& e:o.events){auto entry=eventJson(e);entry["type"]="result";entry["t"]=e.time;appendLocked(std::move(entry));}
            for(const auto& a:o.targetAliases)appendLocked({{"type","result"},{"t",o.time},{"alias",a.first},{"target",a.second},{"detail","补全目标名称"}});
            for(const auto& c:o.resultCorrections)appendLocked({{"type","result"},{"t",o.time},{"target",c.first},{"kind",resultKindName(c.second)},{"detail","中央提示纠正"}});
        }catch(...){fail(lock);}
    }
    void rule(const RuleNote& n)noexcept{
        try{json entry{{"type","rule"},{"t",n.time},{"what",n.what},{"detail",n.detail},{"target",n.target},{"opponents",n.opponents}};
            if(n.kind)entry["kind"]=resultKindName(*n.kind);
            if(n.burst){const auto& b=*n.burst;entry["burst"]={{"sum",b.sum},{"peak",b.peak},{"qualified",b.qualified},{"start",b.start},{"end",b.end}};}
            if(n.clip)entry["clip"]=clipJson(*n.clip);append(std::move(entry));
        }catch(...){std::unique_lock lock(mutex_);if(enabled_)fail(lock);}
    }
    json slice(const ClipPlan& clip,double videoStart)const{
        std::lock_guard lock(mutex_);if(!enabled_)return nullptr;
        json list=json::array();for(const auto& e:entries_){double t=e.value("t",0.0);if(t>=videoStart&&t<=clip.end){auto entry=e;entry["clipTime"]=t-videoStart;list.push_back(std::move(entry));}}
        return {{"format","apex-replay-dev-v1"},{"session",session_},{"clip",clipJson(clip)},{"videoStart",videoStart},{"entries",list}};
    }
    void writeSlice(const std::filesystem::path& file,const json& slice)noexcept{
        std::unique_lock lock(mutex_);if(!enabled_||slice.is_null())return;
        try{std::ofstream out(file,std::ios::binary);out<<dump(slice)<<'\n';out.flush();if(!out)throw std::runtime_error("片段记录写入失败");}catch(...){fail(lock);}
    }
};
}
