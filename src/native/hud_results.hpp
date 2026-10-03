#pragma once
#include "rules.hpp"
#include "hud_attribution.hpp"
#include <array>
#include <cctype>

namespace apex {
// A result banner has one identity, even when OCR loses or later recovers its name.
class ResultPrompts {
    struct Banner {
        double first=-100,last=-100;
        unsigned id=0;
        int hits=0,pendingHits=0;
        bool emitted=false,fromFeed=false;
        std::optional<int> resultDamage;
        std::string target,pending;
    };
    std::array<Banner,4> banners_{};
    unsigned nextId_=0;
    bool seeded_=false;
    static constexpr double dropoutSeconds=2.5;
    static bool contains(const std::string& text,const char* word){return text.find(word)!=std::string::npos;}
    static std::vector<unsigned> characters(const std::string& text){
        std::vector<unsigned> out;
        for(size_t i=0;i<text.size();){unsigned c=static_cast<unsigned char>(text[i++]);
            if(c<128){c=unsigned(std::tolower(int(c)));if(c=='i'||c=='l')c='1';if(c=='0')c='o';}
            else{int count=(c&0xe0)==0xc0?1:(c&0xf0)==0xe0?2:3;c&=count==1?0x1f:count==2?0x0f:0x07;while(count--&&i<text.size())c=(c<<6)|(static_cast<unsigned char>(text[i++])&0x3f);}
            out.push_back(c);
        }return out;
    }
    static std::optional<ResultKind> classify(const std::string& text){
        if(contains(text,"小队全灭")||contains(text,"SQUADELIMINATED"))return ResultKind::SquadWipe;
        bool assist=contains(text,"助攻")||contains(text,"协助")||contains(text,"ASSIST");
        bool knock=text.starts_with("击倒")||text.starts_with("KNOCKEDDOWN");
        bool elimination=text.starts_with("淘汰")||text.starts_with("消灭")||text.starts_with("击败")||text.starts_with("ELIMINATED");
        if(assist){knock=contains(text,"击倒")||contains(text,"KNOCKEDDOWN");elimination=contains(text,"淘汰")||contains(text,"ELIMINATED");}
        if(assist&&(knock||elimination))return ResultKind::Assist;
        if(knock)return ResultKind::Knockdown;if(elimination)return ResultKind::Elimination;return {};
    }
    static std::string targetFrom(std::string text){
        for(auto label:{"击倒","淘汰","消灭","击败","KNOCKEDDOWN","ELIMINATED"}){auto at=text.find(label);if(at!=std::string::npos){text=text.substr(at+std::char_traits<char>::length(label));break;}}
        auto reward=text.find('+');if(reward!=std::string::npos)text.resize(reward);
        for(auto punctuation:{"，","：","。",",",":",";","；"}){size_t at;while((at=text.find(punctuation))!=std::string::npos)text.erase(at,std::char_traits<char>::length(punctuation));}
        return text;
    }
    static bool readable(const std::string& text){
        if(characters(text).size()<2)return false;
        for(auto word:{"造成","伤害","护盾","正在","坏蛋","该死","ASSIST"})if(contains(text,word))return false;
        bool letter=false;for(unsigned char c:text)letter|=c>=128||std::isalpha(c);return letter;
    }
    static std::string identity(const Banner& banner){return banner.target.empty()?"hud-result-"+std::to_string(banner.id):banner.target;}
public:
    struct Read {std::vector<CombatEvent> events;std::vector<std::pair<std::string,std::string>> aliases;std::vector<std::pair<std::string,ResultKind>> corrections;};
    void reset(){banners_={};nextId_=0;seeded_=false;}
    static bool similarTarget(const std::string& a,const std::string& b){
        auto x=characters(a),y=characters(b);if(x==y)return true;
        if(std::min(x.size(),y.size())>=2&&(std::search(x.begin(),x.end(),y.begin(),y.end())!=x.end()||std::search(y.begin(),y.end(),x.begin(),x.end())!=y.end()))return true;
        if(std::min(x.size(),y.size())<4||std::abs(int(x.size())-int(y.size()))>1)return false;
        std::vector<int> previous(y.size()+1),current(y.size()+1);for(size_t j=0;j<previous.size();++j)previous[j]=int(j);
        for(size_t i=0;i<x.size();++i){current[0]=int(i)+1;for(size_t j=0;j<y.size();++j)current[j+1]=std::min({previous[j+1]+1,current[j]+1,previous[j]+(x[i]!=y[j])});previous.swap(current);}return previous.back()<=1;
    }
    Read process(const OcrRead& prompt,double time,const std::string& ownFeedTarget={},std::optional<int> totalDamage={}){
        Read read;
        for(const auto& line:prompt.lines){
            auto kind=classify(line);if(!kind)continue;
            auto target=targetFrom(line);bool fromFeed=false;
            if(*kind!=ResultKind::SquadWipe&&!readable(target)){target.clear();if(!ownFeedTarget.empty()){target=ownFeedTarget;fromFeed=true;}}
            auto sameBanner=[&](const Banner& other){return other.emitted&&time-other.last<=dropoutSeconds&&
                ((!other.target.empty()&&!target.empty()&&similarTarget(other.target,target))||
                 ((other.target.empty()||target.empty())&&!(totalDamage&&other.resultDamage&&*totalDamage>*other.resultDamage)));};
            auto& assist=banners_[static_cast<int>(ResultKind::Assist)];
            if(*kind==ResultKind::Knockdown&&sameBanner(assist)){assist.last=time;continue;}
            auto& banner=banners_[static_cast<int>(*kind)];
            if(time-banner.last>dropoutSeconds){banner={};banner.first=time;banner.id=++nextId_;}
            banner.last=time;
            // Seed banners already visible when capture starts; they predate this session.
            if(!seeded_){banner.target=target;banner.fromFeed=fromFeed;banner.emitted=true;banner.resultDamage=totalDamage;continue;}
            if(banner.emitted){
                if(target.empty()||(!banner.target.empty()&&similarTarget(banner.target,target)))continue;
                if(banner.target.empty()||(banner.fromFeed&&!fromFeed&&time-banner.first<=dropoutSeconds)){
                    read.aliases.emplace_back(identity(banner),target);banner.target=target;banner.fromFeed=fromFeed;continue;
                }
                // The kill feed only supplies a missing identity. A new feed row cannot
                // turn a banner that is still visible into another result.
                if(fromFeed)continue;
                // Stable OCR can still read a different fragment of the same red name.
                // A rapid new result needs fresh damage or its own corroborated feed name.
                bool newCombat=totalDamage&&banner.resultDamage&&*totalDamage>*banner.resultDamage;
                bool newOwnTarget=!ownFeedTarget.empty()&&!similarTarget(banner.target,ownFeedTarget)&&similarTarget(target,ownFeedTarget);
                if(!newCombat&&!newOwnTarget){banner.pending.clear();banner.pendingHits=0;continue;}
                if(target==banner.pending)++banner.pendingHits;else{banner.pending=target;banner.pendingHits=1;}
                if(banner.pendingHits<2)continue;
                banner={};banner.first=banner.last=time;banner.id=++nextId_;banner.hits=1;
            }
            if(!target.empty()){
                if(banner.target.empty()||similarTarget(banner.target,target)){banner.target=target;banner.fromFeed=fromFeed;}
                else{banner.target=target;banner.hits=0;banner.fromFeed=fromFeed;}
            }
            if(++banner.hits<2)continue;
            banner.emitted=true;banner.resultDamage=totalDamage;
            auto& knockdown=banners_[static_cast<int>(ResultKind::Knockdown)];
            if(*kind==ResultKind::Assist&&sameBanner(knockdown)){
                auto oldTarget=identity(knockdown);if(banner.target.empty())banner.target=knockdown.target;
                if(oldTarget!=identity(banner))read.aliases.emplace_back(oldTarget,identity(banner));
                read.corrections.emplace_back(identity(banner),ResultKind::Assist);continue;
            }
            read.events.push_back({time,*kind,identity(banner),target.empty()?"连续本人中央结果提示，目标文字不清晰":line+(fromFeed?"；本人击杀信息交叉验证目标":""),target.empty()?.85:.95});
        }
        seeded_=true;return read;
    }
};
}
