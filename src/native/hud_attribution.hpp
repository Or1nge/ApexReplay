#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace apex {
struct OcrWord {std::string text;double x=0,y=0,w=0,h=0;};
struct OcrRead {std::string text;std::vector<std::string> lines;std::vector<OcrWord> words;};
class OwnFeedAttribution {
    std::string own_,pendingName_;int nameHits_=0;bool seeded_=false;
    struct Entry{double first=-100,last=-100;int hits=0;};
    std::unordered_map<std::string,Entry> entries_;
    static std::string identifier(std::string text,bool similarGlyphs=false){
        text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return c<128&&!std::isalnum(c)&&c!='_';}),text.end());
        for(auto& ch:text)if(similarGlyphs&&static_cast<unsigned char>(ch)<128){ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));if(ch=='i'||ch=='l')ch='1';if(ch=='0')ch='o';}
        return text;
    }
public:
    void reset(){own_.clear();pendingName_.clear();nameHits_=0;seeded_=false;entries_.clear();}
    const std::string& playerName()const{return own_;}
    void observeName(const OcrRead& read){
        // Leftmost name word below the upgrade icons, within the dedicated own-HUD crop.
        const OcrWord* best=nullptr;
        for(const auto& w:read.words)if(w.x<.16&&w.y>.38&&identifier(w.text).size()>=3&&(!best||w.x<best->x))best=&w;
        if(!best){pendingName_.clear();nameHits_=0;return;}
        auto name=identifier(best->text,true);
        if(name==pendingName_)++nameHits_;else{pendingName_=name;nameHits_=1;}
        if(nameHits_>=3&&own_!=name){own_=name;seeded_=false;entries_.clear();}
    }
    void observeFeed(const OcrRead& read,double time){
        if(own_.empty())return;
        for(const auto& actor:read.words){
            if(identifier(actor.text,true)!=own_)continue;
            std::vector<OcrWord> victim;
            for(const auto& word:read.words){
                double center=word.y+word.h/2,actorCenter=actor.y+actor.h/2;
                if(word.x>actor.x+actor.w+.14&&std::abs(center-actorCenter)<std::max(word.h,actor.h)*.6&&word.h>actor.h*.5&& !identifier(word.text).empty())victim.push_back(word);
            }
            std::sort(victim.begin(),victim.end(),[](const auto& a,const auto& b){return a.x<b.x;});
            std::string name;for(const auto& w:victim)name+=w.text;name=identifier(name);
            if(name.size()<3)continue;
            auto [it,inserted]=entries_.try_emplace(name);
            auto& entry=it->second;if(inserted){entry.first=seeded_?time:-100;entry.hits=0;}
            if(time-entry.last>.9)entry.hits=0;entry.last=time;++entry.hits;
        }
        seeded_=true;
        for(auto it=entries_.begin();it!=entries_.end();)it=time-it->second.last>300?entries_.erase(it):std::next(it);
    }
    std::optional<std::string> recentTarget(double time)const{
        const std::string* name=nullptr;double newest=-100;
        for(const auto& [key,e]:entries_)if(e.hits>=2&&time-e.last<=.9&&time-e.first<=2.5&&e.first>newest){name=&key;newest=e.first;}
        return name?std::optional<std::string>(*name):std::nullopt;
    }
};
}
