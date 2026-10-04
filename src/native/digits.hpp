#pragma once
#include "image.hpp"
#include "json.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
namespace apex {
using json=nlohmann::json;
// Top-right counter row of the current HUD: [skull] kills [handshake] assists [burst] damage. The kills and
// assists box only appears after the first kill or assist; until then the row shows damage alone (hidden).
struct HudCounters { std::optional<int> kills,assists,damage; bool hidden=false; };
class HudDigits {
    struct Glyph {int digit;double aspect;std::array<uint8_t,640> mask;};
    struct Match{int digit,left,right;double cost;bool icon;int top,height;};
    static inline std::vector<Glyph> templates_;
    static std::vector<uint8_t> binarize(const CpuImage& image,int threshold,bool red,std::vector<int>& columns){
        std::vector<uint8_t> mask(size_t(image.width)*image.height);columns.assign(image.width,0);
        for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){auto p=image.bgra.data()+(size_t(y)*image.width+x)*4;
            bool white=std::min({p[0],p[1],p[2]})>=threshold;bool colored=red&&p[2]>=220&&p[1]>=90&&p[0]>=70&&p[2]>p[1]*1.4;
            if(white||colored){mask[size_t(y)*image.width+x]=1;++columns[x];}}
        return mask;
    }
    // Binary glyph match of one column run; digit -1 when no digit is clear enough.
    static std::optional<Match> match(const std::vector<uint8_t>& mask,int width,int height,int left,int right,double minHeight,int threshold,json* debug){
        int top=height,bottom=-1;for(int y=0;y<height;++y)for(int c=left;c<right;++c)if(mask[size_t(y)*width+c]){top=std::min(top,y);bottom=std::max(bottom,y);}
        int w=right-left,h=bottom-top+1;if(h<minHeight||w<1)return {};
        std::array<uint8_t,640> normalized{};for(int y=0;y<32;++y)for(int c=0;c<20;++c)normalized[y*20+c]=mask[size_t(top+y*h/32)*width+left+c*w/20];
        std::array<double,11> costs;costs.fill(1e9);double aspect=double(w)/h;
        for(const auto& glyph:templates_){int error=0;for(int i=0;i<640;++i)error+=normalized[i]!=glyph.mask[i];double cost=error/640.+std::abs(aspect-glyph.aspect)*.08;costs[glyph.digit]=std::min(costs[glyph.digit],cost);}
        auto winner=std::min_element(costs.begin(),costs.end());int digit=int(winner-costs.begin());double cost=*winner;costs[digit]=1e9;double margin=*std::min_element(costs.begin(),costs.end())-cost;
        if(debug)debug->push_back({{"threshold",threshold},{"digit",digit},{"cost",cost},{"margin",margin},{"left",left},{"right",right},{"height",h},{"top",top}});
        return Match{digit<10&&cost<.25&&margin>.012?digit:-1,left,right,cost,digit==10&&cost<.30,top,h};
    }
    static std::vector<Match> blobs(const std::vector<uint8_t>& mask,const std::vector<int>& columns,int width,int height,double minHeight,int threshold,json* debug){
        std::vector<Match> matches;
        for(int x=0;x<width;){if(columns[x]<2){++x;continue;}int left=x;while(x<width&&columns[x]>=2)++x;
            if(auto m=match(mask,width,height,left,x,minHeight,threshold,debug))matches.push_back(*m);}
        return matches;
    }
public:
    static inline std::string version="unavailable";
    static void load(const std::filesystem::path& file){
        std::ifstream input(file);if(!input)throw std::runtime_error("缺少中文 HUD 数字模板");json config;input>>config;
        version=config.at("version").get<std::string>();templates_.clear();std::set<int> digits;
        for(auto& item:config.at("templates")){Glyph glyph{};glyph.digit=item.at("digit");glyph.aspect=item.at("aspect");auto rows=item.at("mask").get<std::vector<std::string>>();
            if(rows.size()!=32 || glyph.digit<0 || glyph.digit>10)throw std::runtime_error("HUD 数字模板损坏");
            for(int y=0;y<32;++y){if(rows[y].size()!=20)throw std::runtime_error("HUD 数字模板尺寸错误");for(int x=0;x<20;++x)glyph.mask[y*20+x]=rows[y][x]=='1';}
            if(glyph.digit<10)digits.insert(glyph.digit);templates_.push_back(glyph);}
        if(digits.size()!=10)throw std::runtime_error("HUD 数字模板未覆盖十个数字");
    }
    static std::optional<int> read(const CpuImage& image,int maxDigits=5,bool red=false,json* debug=nullptr){
        std::optional<int> answer;double bestCost=1e9;std::unordered_map<int,std::pair<int,double>> votes;
        for(int threshold:{180,160,205}){
            std::vector<int> columns;auto mask=binarize(image,threshold,red,columns);
            auto matches=blobs(mask,columns,image.width,image.height,image.height*(maxDigits>2?.25:.40),threshold,debug);
            // The damage counter is right aligned; a preceding HUD icon cannot become a digit.
            int value=0,multiplier=1,count=0;double cost=0;int priorRight=-1;
            for(auto it=matches.rbegin();it!=matches.rend();++it){
                if(it->digit<0){if(!it->icon&&count && it->right-it->left<image.height*.75 && priorRight-it->right<image.height*.55)count=0;break;}
                if(priorRight>=0&&priorRight-it->right>image.height*.55)break;
                value+=it->digit*multiplier;multiplier*=10;cost+=it->cost;priorRight=it->left;++count;if(count>=maxDigits)break;
            }
            if(count){auto& vote=votes[value];++vote.first;vote.second+=cost/count;if(cost/count<bestCost){answer=value;bestCost=cost/count;}}
        }
        if(maxDigits>2){answer.reset();int bestVotes=1;bestCost=1e9;for(const auto& [value,vote]:votes)if(vote.first>=2 && (vote.first>bestVotes || vote.second/vote.first<bestCost)){answer=value;bestVotes=vote.first;bestCost=vote.second/vote.first;}}
        return answer;
    }
    // Whole counter row, read right to left: the last three digit groups are kills, assists and damage.
    // Five binarization thresholds vote; a value needs two agreeing thresholds and a clear majority.
    static HudCounters readRow(const CpuImage& image,json* debug=nullptr){
        std::map<int,int> kills,assists,damage;int hidden=0;
        for(int threshold:{160,180,205,220,235}){
            std::vector<int> columns;auto mask=binarize(image,threshold,false,columns);
            auto matches=blobs(mask,columns,image.width,image.height,image.height*.45,threshold,nullptr);
            // Each number follows its icon closely; a group without that icon lost its leading digits.
            struct Group{int left,right,value,digits;bool anchored;};std::vector<Group> groups;bool broken=false;
            for(size_t i=0;i<matches.size();++i){
                const auto& m=matches[i];
                if(m.digit<0){
                    // A blob of digit size right next to a digit is a misread digit, which would shift the groups.
                    auto touches=[&](size_t j){if(j>=matches.size()||matches[j].digit<0)return false;const auto& d=matches[j];
                        return std::max(d.left-m.right,m.left-d.right)<=d.height*.35&&std::abs(m.height-d.height)<=d.height*.25&&m.right-m.left<=d.height*.85;};
                    if(!m.icon&&(touches(i+1)||(i>0&&touches(i-1))))broken=true;
                    continue;
                }
                bool joined=i>0&&matches[i-1].digit>=0&&!groups.empty()&&m.left-groups.back().right<=std::max(3.,m.height*.55);
                if(joined&&groups.back().digits<5){groups.back().value=groups.back().value*10+m.digit;groups.back().right=m.right;++groups.back().digits;}
                else groups.push_back({m.left,m.right,m.digit,1,i>0&&matches[i-1].digit<0&&m.left-matches[i-1].right<=m.height*1.2});
            }
            if(debug){json list=json::array();for(const auto& g:groups)list.push_back({{"value",g.value},{"left",g.left},{"right",g.right},{"anchored",g.anchored}});debug->push_back({{"threshold",threshold},{"broken",broken},{"groups",list}});}
            if(broken||groups.empty())continue;
            // Damage is right aligned in the row; the kills and assists box sits to its left.
            const auto& last=groups.back();if(!last.anchored||last.right<image.width*.88)continue;
            ++damage[last.value];
            size_t n=groups.size();
            if(n>=3&&groups[n-2].anchored&&groups[n-3].anchored&&groups[n-2].digits<=2&&groups[n-3].digits<=2){++assists[groups[n-2].value];++kills[groups[n-3].value];}
            else if(n==1)++hidden;
        }
        auto pick=[](const std::map<int,int>& votes){std::optional<int> best;int top=0,second=0;
            for(const auto& [value,count]:votes){if(count>top){second=top;top=count;best=value;}else if(count>second)second=count;}
            return top>=2&&top>second?best:std::nullopt;};
        HudCounters out{pick(kills),pick(assists),pick(damage),false};
        out.hidden=!out.kills&&!out.assists&&hidden>=2;
        return out;
    }
};
}
