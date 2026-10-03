#pragma once
#include "common.hpp"
namespace apex {
class HudDigits {
    struct Glyph {int digit;double aspect;std::array<uint8_t,640> mask;};
    static inline std::vector<Glyph> templates_;
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
            std::vector<uint8_t> mask(image.width*image.height);std::vector<int> columns(image.width);
            for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){auto p=image.bgra.data()+(size_t(y)*image.width+x)*4;
                bool white=std::min({p[0],p[1],p[2]})>=threshold;bool colored=red&&p[2]>=220&&p[1]>=90&&p[0]>=70&&p[2]>p[1]*1.4;
                if(white||colored){mask[y*image.width+x]=1;++columns[x];}}
            struct Match{int digit,left,right;double cost;bool icon;};std::vector<Match> matches;
            for(int x=0;x<image.width;){if(columns[x]<2){++x;continue;}int left=x;while(x<image.width&&columns[x]>=2)++x;int right=x;
                int top=image.height,bottom=-1;for(int y=0;y<image.height;++y)for(int c=left;c<right;++c)if(mask[y*image.width+c]){top=std::min(top,y);bottom=std::max(bottom,y);}
                int w=right-left,h=bottom-top+1;if(h<image.height*(maxDigits>2?.25:.40)||w<1)continue;
                std::array<uint8_t,640> normalized{};for(int y=0;y<32;++y)for(int c=0;c<20;++c)normalized[y*20+c]=mask[(top+y*h/32)*image.width+left+c*w/20];
                std::array<double,11> costs;costs.fill(1e9);double aspect=double(w)/h;
                for(const auto& glyph:templates_){int error=0;for(int i=0;i<640;++i)error+=normalized[i]!=glyph.mask[i];double cost=error/640.+std::abs(aspect-glyph.aspect)*.08;costs[glyph.digit]=std::min(costs[glyph.digit],cost);}
                auto winner=std::min_element(costs.begin(),costs.end());int digit=int(winner-costs.begin());double cost=*winner;costs[digit]=1e9;double margin=*std::min_element(costs.begin(),costs.end())-cost;
                if(debug)debug->push_back({{"threshold",threshold},{"digit",digit},{"cost",cost},{"margin",margin},{"left",left},{"right",right},{"height",h},{"top",top}});
                matches.push_back({digit<10&&cost<.25&&margin>.012?digit:-1,left,right,cost,digit==10&&cost<.30});
            }
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
};
}
