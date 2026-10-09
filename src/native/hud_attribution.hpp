#pragma once
#include "image.hpp"
#include "names.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
namespace apex {
struct OcrWord {std::string text;double x=0,y=0,w=0,h=0;};
struct OcrRead {std::string text;std::vector<std::string> lines;std::vector<OcrWord> words;};
struct FeedEntry {std::string victim;bool knock=false;double time=0;std::string id;};
// Track visible own feed rows; OCR supplies their names, never another event for the same row.
class OwnFeedAttribution {
    using Mask=std::array<unsigned char,128*20>;
    struct Row {std::string victim;bool knock=false;double y=0,h=0;Mask mask{};double aspect=0;};
    struct Track {Row row;unsigned id=0;double first=-100,last=-100;int hits=0;bool historical=false,reported=false;std::string reportedName;int nameHits=1;};
    std::string own_,pendingName_;int nameHits_=0;bool seeded_=false;unsigned nextId_=0;
    Mask ownGlyph_{};double ownAspect_=0;
    std::vector<Track> tracks_;std::vector<std::pair<std::string,std::string>> aliases_;
    static std::string identifier(std::string text,bool player=false){
        text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return c<128&&!std::isalnum(c)&&c!='_';}),text.end());
        if(player){for(size_t at;(at=text.find("氵闰"))!=std::string::npos;)text.replace(at,std::string("氵闰").size(),"润");
            for(auto& ch:text)if(static_cast<unsigned char>(ch)<128){ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));if(ch=='i'||ch=='l')ch='1';if(ch=='0')ch='o';}}
        return text;
    }
    static bool sameLine(const OcrWord& a,const OcrWord& b){return std::abs(a.y+a.h/2-b.y-b.h/2)<std::max(a.h,b.h)*.6;}
    static int redPixels(const CpuImage& image,double x0,double x1,double y0,double y1){
        int red=0;for(int y=std::max(0,int(y0*image.height));y<std::min(image.height,int(std::ceil(y1*image.height)));++y)
            for(int x=std::max(0,int(x0*image.width));x<std::min(image.width,int(std::ceil(x1*image.width)));++x){auto p=image.bgra.data()+(size_t(y)*image.width+x)*4;if(p[2]>150&&p[2]>p[1]*1.6&&p[2]>p[0]*1.6)++red;}return red;
    }
    static bool redWord(const CpuImage& image,const OcrWord& word){return redPixels(image,word.x,word.x+word.w,word.y,word.y+word.h)>std::max(3,int(word.w*image.width*word.h*image.height*.12));}
    static Mask rowMask(const CpuImage& image,const OcrWord& victim,bool gold=false){
        Mask mask{};double left=victim.x,top=victim.y-victim.h*.25,height=victim.h*1.5;if(left>=1||height<=0||victim.w<=0)return mask;
        for(int y=std::max(0,int(top*image.height));y<std::min(image.height,int((top+height)*image.height));++y)
            for(int x=std::max(0,int(left*image.width));x<std::min(image.width,int(std::ceil((left+victim.w)*image.width)));++x){auto p=image.bgra.data()+(size_t(y)*image.width+x)*4;
                bool ink=gold?p[2]>150&&p[1]>90&&p[2]>p[0]*1.5&&p[1]>p[0]*1.3&&p[2]>=p[1]*1.05:std::min({p[0],p[1],p[2]})>=165;if(!ink)continue;
                int mx=std::clamp(int((double(x)/image.width-left)/victim.w*128),0,127),my=std::clamp(int((double(y)/image.height-top)/height*20),0,19);mask[size_t(my)*128+mx]=1;}return mask;
    }
    static bool sameMask(const Mask& a,const Mask& b,double tolerance=.20){
        auto coverage=[&](const Mask& from,const Mask& to){int ink=0,miss=0;for(int y=0;y<20;++y)for(int x=0;x<128;++x)if(from[size_t(y)*128+x]){++ink;bool found=false;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)if(y+dy>=0&&y+dy<20&&x+dx>=0&&x+dx<128)found|=to[size_t(y+dy)*128+x+dx]!=0;if(!found)++miss;}return ink>=20&&miss<=ink*tolerance;};return coverage(a,b)&&coverage(b,a);
    }
public:
    static constexpr const char* version="apex-results-v4-fight-continuity";
    void reset(){pendingName_.clear();nameHits_=0;seeded_=false;tracks_.clear();aliases_.clear();ownGlyph_={};ownAspect_=0;}
    void rememberPlayer(const std::string& name){own_=identifier(name,true);reset();}
    const std::string& playerName()const{return own_;}
    void observeName(const OcrRead& read,const OcrRead* fallback=nullptr){
        auto candidate=[](const OcrRead& input){
            const OcrWord* first=nullptr;for(const auto& w:input.words)if(w.x<.16&&w.y+w.h/2>.2&&w.y<.85&&!identifier(w.text).empty()&&(!first||w.x<first->x))first=&w;
            if(!first)return std::string();std::vector<OcrWord> words;for(const auto& w:input.words)if(w.x>=first->x&&sameLine(*first,w))words.push_back(w);
            std::sort(words.begin(),words.end(),[](const auto& a,const auto& b){return a.x<b.x;});std::string text;double right=first->x;
            for(const auto& w:words){if(w.x-right>.08)break;text+=w.text;right=w.x+w.w;}return identifier(text,true);
        };
        auto name=candidate(read);if(fallback){auto alternate=candidate(*fallback);if(nameCharacters(alternate).size()>nameCharacters(name).size()||(!own_.empty()&&alternate==own_))name=alternate;}
        if(nameCharacters(name).size()<2||nameCharacters(name).size()>32){pendingName_.clear();nameHits_=0;return;}
        if(name==pendingName_)++nameHits_;else{pendingName_=name;nameHits_=1;}
        if(name==own_||(!own_.empty()&&similarTarget(name,own_)))return;
        if(nameHits_>=(own_.empty()?3:20)){own_=name;seeded_=false;tracks_.clear();aliases_.clear();ownGlyph_={};ownAspect_=0;}
    }
    std::vector<FeedEntry> observeFeed(const OcrRead& read,double time,const CpuImage* image=nullptr,const OcrRead* fallback=nullptr){
        std::vector<FeedEntry> confirmed;if(own_.empty())return confirmed;
        auto parseRows=[&](const OcrRead& input){std::vector<OcrWord> words=input.words;
        std::sort(words.begin(),words.end(),[](const auto& a,const auto& b){return a.x<b.x;});std::vector<Row> rows;
        for(size_t i=0;i<words.size();++i){OcrWord actor=words[i];std::string name=actor.text;
            auto ownActor=[&]{auto normalized=identifier(name,true);if(normalized==own_){if(image){ownGlyph_=rowMask(*image,actor,true);ownAspect_=actor.w/actor.h;}return true;}
                if(!image||ownAspect_<=0)return false;auto candidate=nameCharacters(normalized),known=nameCharacters(own_);
                return candidate.size()>=2&&known.size()>=3&&nameDistance(candidate,known)<=1&&std::abs(actor.w/actor.h-ownAspect_)<=ownAspect_*.2&&sameMask(rowMask(*image,actor,true),ownGlyph_,.08);};
            bool matched=ownActor();
            for(size_t j=i+1;j<words.size()&&!matched;++j){const auto& word=words[j];if(!sameLine(actor,word))continue;if(word.x-actor.x-actor.w>.08)break;
                name+=word.text;actor.w=word.x+word.w-actor.x;matched=ownActor();}
            if(!matched)continue;bool already=false;for(const auto& row:rows)if(std::abs(row.y-actor.y-actor.h/2)<actor.h*.5)already=true;if(already)continue;
            std::string tail;std::vector<const OcrWord*> letters;
            for(const auto& word:words)if(word.x>=actor.x+actor.w&&sameLine(actor,word)){tail+=word.text;for(size_t byte=0;byte<word.text.size();++byte)letters.push_back(&word);}
            double targetLeft=actor.x+actor.w+.14;bool bleeding=false;
            if(auto status=tail.find("失血过多");status!=std::string::npos){bleeding=true;size_t end=status+std::string("失血过多").size();
                if(auto close=tail.find(']',end);close!=std::string::npos)end=close+1;
                if(end&&end<=letters.size())targetLeft=letters[end-1]->x+letters[end-1]->w;}
            std::vector<OcrWord> victim;for(const auto& word:words)if(word.x>targetLeft-.001&&sameLine(actor,word)&&word.h>actor.h*.5&&!identifier(word.text).empty()&&(!image||!redWord(*image,word))){
                if(bleeding&&victim.empty()&&word.w<.02)continue;victim.push_back(word);}
            if(victim.empty())continue;std::string target;for(const auto& word:victim)target+=word.text;target=identifier(target);auto characters=nameCharacters(target);
            if(characters.empty()||(characters.size()==1&&characters.front()<128))continue;
            bool knock=image&&redPixels(*image,actor.x+actor.w,victim.front().x,actor.y-actor.h*.2,actor.y+actor.h*1.2)>=12;
            auto bounds=victim.front();double right=bounds.x+bounds.w,bottom=bounds.y+bounds.h;for(const auto& word:victim){right=std::max(right,word.x+word.w);bottom=std::max(bottom,word.y+word.h);bounds.y=std::min(bounds.y,word.y);}bounds.w=right-bounds.x;bounds.h=bottom-bounds.y;
            rows.push_back({target,knock,actor.y+actor.h/2,actor.h,image?rowMask(*image,bounds):Mask{},bounds.w/bounds.h});
        }
        return rows;};auto rows=parseRows(read);
        // Both OCR variants describe one frame; combine rows before confirmation, never count
        // a second OCR pass as a second temporal observation.
        if(fallback)for(auto& alternate:parseRows(*fallback)){
            auto existing=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return std::abs(row.y-alternate.y)<std::max(row.h,alternate.h)*.7;});
            if(existing==rows.end())rows.push_back(std::move(alternate));
            else if(nameCharacters(existing->victim).size()==1&&nameCharacters(alternate.victim).size()>1)*existing=std::move(alternate);
        }
        std::vector<bool> used(tracks_.size());std::vector<int> assigned(rows.size(),-1);
        // Match names across scrolling first, then unchanged glyphs when OCR reads a different fragment.
        for(int pass=0;pass<2;++pass)for(size_t i=0;i<rows.size();++i)if(assigned[i]<0){int best=-1;double distance=1e9;
            for(size_t j=0;j<tracks_.size();++j)if(!used[j]&&rows[i].knock==tracks_[j].row.knock&&time-tracks_[j].last<=3){
                bool matches=pass==0?sameOpponentName(rows[i].victim,tracks_[j].row.victim):std::abs(rows[i].aspect-tracks_[j].row.aspect)<=std::max(rows[i].aspect,tracks_[j].row.aspect)*.35&&sameMask(rows[i].mask,tracks_[j].row.mask);
                double dy=std::abs(rows[i].y-tracks_[j].row.y);if(matches&&dy<distance){best=int(j);distance=dy;}}
            if(best>=0){assigned[i]=best;used[size_t(best)]=true;}}
        for(size_t i=0;i<used.size();++i)if(!used[i]&&!tracks_[i].reported)tracks_[i].hits=0;
        for(size_t i=0;i<rows.size();++i){if(assigned[i]<0){tracks_.push_back({rows[i],++nextId_,time,time,1,!seeded_,false});continue;}
            auto& track=tracks_[size_t(assigned[i])];if(time-track.last>.9)track.hits=0;track.last=time;++track.hits;
            track.nameHits=track.row.victim==rows[i].victim?track.nameHits+1:1;
            if(track.reported&&track.nameHits>=2&&track.reportedName!=rows[i].victim&&nameCharacters(rows[i].victim).size()>nameCharacters(track.reportedName).size()){
                aliases_.emplace_back(track.reportedName,rows[i].victim);track.reportedName=rows[i].victim;}
            track.row=rows[i];if(track.hits>=2&&!track.reported&&!track.historical){track.reported=true;track.reportedName=track.row.victim;confirmed.push_back({track.row.victim,track.row.knock,time,"feed-result-"+std::to_string(track.id)});}
        }
        seeded_=true;std::erase_if(tracks_,[&](const auto& track){return time-track.last>30;});return confirmed;
    }
    std::vector<std::pair<std::string,std::string>> takeAliases(){auto aliases=std::move(aliases_);aliases_.clear();return aliases;}
    std::optional<FeedEntry> recentResult(double time)const{
        const Track* latest=nullptr;for(const auto& track:tracks_)if(!track.historical&&track.hits>=2&&time-track.last<=.9&&time-track.first<=2.5&&(!latest||track.first>latest->first))latest=&track;
        return latest?std::optional<FeedEntry>({latest->row.victim,latest->row.knock,latest->first,"feed-result-"+std::to_string(latest->id)}):std::nullopt;
    }
    std::optional<std::string> recentTarget(double time)const{auto result=recentResult(time);return result?std::optional<std::string>(result->victim):std::nullopt;}
};
}
