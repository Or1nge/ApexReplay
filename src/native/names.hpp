#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>
namespace apex {
// OCR-tolerant comparison of opponent names read from the HUD.
inline std::vector<unsigned> nameCharacters(const std::string& text){
    std::vector<unsigned> out;
    for(size_t i=0;i<text.size();){unsigned c=static_cast<unsigned char>(text[i++]);
        if(c<128){c=unsigned(std::tolower(int(c)));if(c=='i'||c=='l')c='1';if(c=='0')c='o';}
        else{int count=(c&0xe0)==0xc0?1:(c&0xf0)==0xe0?2:3;c&=count==1?0x1f:count==2?0x0f:0x07;while(count--&&i<text.size())c=(c<<6)|(static_cast<unsigned char>(text[i++])&0x3f);}
        out.push_back(c);
    }return out;
}
inline int nameDistance(const std::vector<unsigned>& x,const std::vector<unsigned>& y){
    std::vector<int> previous(y.size()+1),current(y.size()+1);for(size_t j=0;j<previous.size();++j)previous[j]=int(j);
    for(size_t i=0;i<x.size();++i){current[0]=int(i)+1;for(size_t j=0;j<y.size();++j)current[j+1]=std::min({previous[j+1]+1,current[j]+1,previous[j]+(x[i]!=y[j])});previous.swap(current);}
    return previous.back();
}
inline bool similarTarget(const std::string& a,const std::string& b){
    auto x=nameCharacters(a),y=nameCharacters(b);if(x==y)return true;
    if(std::min(x.size(),y.size())>=2&&(std::search(x.begin(),x.end(),y.begin(),y.end())!=x.end()||std::search(y.begin(),y.end(),x.begin(),x.end())!=y.end()))return true;
    if(std::min(x.size(),y.size())<4||std::abs(int(x.size())-int(y.size()))>1)return false;
    return nameDistance(x,y)<=1;
}
// A knock and the later elimination of the same opponent are separate prompts read seconds apart,
// so longer names may differ by a few more OCR glyphs (about one in four).
inline bool sameOpponentName(const std::string& a,const std::string& b){
    if(similarTarget(a,b))return true;
    auto x=nameCharacters(a),y=nameCharacters(b);size_t shorter=std::min(x.size(),y.size());
    return shorter>=6&&nameDistance(x,y)<=int(shorter/4);
}
// Placeholder identities for results whose name could not be read.
inline bool unnamedTarget(const std::string& target){return target.empty()||target.starts_with("hud-result-")||target.starts_with("unknown@");}
}
