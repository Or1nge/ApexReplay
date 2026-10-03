#pragma once
#include "common.hpp"
#include "gpu.hpp"
#include "digits.hpp"
#include "hud_attribution.hpp"
#include <regex>
#include <xinput.h>
namespace apex {
inline bool has(const std::string& text,const std::string& word){return text.find(word)!=std::string::npos;}
inline std::string compact(std::string text){text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return c<128&&std::isspace(c);}),text.end());return text;}
inline std::optional<int> digits(std::string text) {
    std::string number;for(unsigned char c:text)if(c>='0'&&c<='9')number+=c;else if(c!=' '&&c!='+'&&c!=','&&c!='.')return {};
    if(number.empty() || number.size()>5)return {};try{return std::stoi(number);}catch(...){return {};}
}
class LocalOcr {
    winrt::Windows::Media::Ocr::OcrEngine engine_{nullptr};
public:
    LocalOcr(){
        using winrt::Windows::Media::Ocr::OcrEngine;
        engine_=OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(L"zh-Hans-CN"));
        if(!engine_)engine_=OcrEngine::TryCreateFromUserProfileLanguages();
        if(!engine_)throw std::runtime_error("Windows 本地 OCR 不可用，请在 Windows 语言设置中添加中文基本输入组件");
    }
    std::string language()const{return winrt::to_string(engine_.RecognizerLanguage().LanguageTag());}
    OcrRead read(const CpuImage& image,bool contrast=false) {
        int scale=image.width<900?2:1,w=image.width*scale,h=image.height*scale;
        std::vector<uint8_t> pixels(size_t(w)*h*4);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            const auto* src=image.bgra.data()+(size_t(y/scale)*image.width+x/scale)*4;
            auto* dst=pixels.data()+(size_t(y)*w+x)*4;
            if(contrast){bool white=std::min({src[0],src[1],src[2]})>=155;bool orange=src[2]>175&&src[1]>65&&src[1]<185&&src[2]>src[1]*1.4&&src[1]>src[0]*1.4;uint8_t v=white||orange?0:255;dst[0]=dst[1]=dst[2]=v;}else memcpy(dst,src,3);
            dst[3]=255;
        }
        auto buffer=winrt::Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(pixels);
        auto bitmap=winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(buffer,winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,w,h,winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);
        auto result=engine_.RecognizeAsync(bitmap).get();OcrRead out;out.text=winrt::to_string(result.Text());
        for(auto line:result.Lines()){out.lines.push_back(winrt::to_string(line.Text()));for(auto word:line.Words()){
            auto box=word.BoundingRect();out.words.push_back({winrt::to_string(word.Text()),box.X/w,box.Y/h,box.Width/w,box.Height/h});}}
        out.text=compact(out.text);for(auto& line:out.lines)line=compact(line);
        bitmap.Close();return out;
    }
};
struct Observation {
    double time=0;bool active=false,firing=false,reload=false,hitFeedback=false;
    std::optional<int> ammo,totalDamage;std::string weapon,status,prompt,playerName,ownFeedTarget;
    unsigned magazine=1;int damageDelta=0;std::vector<CombatEvent> events;
    std::vector<std::pair<std::string,std::string>> targetAliases;
    json toJson()const {
        json eventsJson=json::array();for(const auto& e:events)eventsJson.push_back(eventJson(e));
        json out{{"time",time},{"active",active},{"firing",firing},{"reload",reload},{"hitFeedback",hitFeedback},
            {"weapon",weapon},{"magazine",magazine},{"damageDelta",damageDelta},{"status",status},{"prompt",prompt},{"events",eventsJson},{"playerName",playerName},{"ownFeedTarget",ownFeedTarget},{"targetAliases",targetAliases}};
        out["ammo"]=ammo?json(*ammo):json(nullptr);out["totalDamage"]=totalDamage?json(*totalDamage):json(nullptr);return out;
    }
};
class HudDetector {
    LocalOcr ocr_;std::optional<int> ammo_,damage_,pendingDamage_;
    int baselineHits_=0,resetHits_=0;std::optional<int> lowerDamage_;
    double shotAt_=-100,damageAt_=0,lastActive_=0,stateReadAt_=-100;
    unsigned magazine_=1;std::string weapon_,stateText_;
    std::optional<int> risingAmmo_;std::string changingWeapon_;
    OwnFeedAttribution attribution_;double nameReadAt_=-100,feedReadAt_=-100;
    struct UnknownPrompt {double last=-100;unsigned id=0;int hits=0;bool emitted=false,resolved=false;std::string resolvedTarget;int resultDamage=0;};
    std::array<UnknownPrompt,4> unknown_;unsigned promptId_=0;
    std::unordered_map<std::string,std::pair<int,double>> visible_;
    std::unordered_map<std::string,double> emitted_;
    static std::string targetFrom(std::string text,ResultKind kind) {
        for(auto label:{"击倒","淘汰","消灭","击败","KNOCKEDDOWN","ELIMINATED"}){auto at=text.find(label);if(at!=std::string::npos){text=text.substr(at+strlen(label));break;}}
        std::vector<std::string> prefixes={"助攻","协助","击倒","击败","淘汰","消灭","ASSIST","ELIMINATED","KNOCKED DOWN",",","，","：",":"};
        for(const auto& prefix:prefixes){size_t at;while((at=text.find(prefix))!=std::string::npos)text.erase(at,prefix.size());}
        size_t reward=text.find('+');if(reward!=std::string::npos)text.resize(reward);
        text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return c<128&&std::isspace(c);}),text.end());
        return text;
    }
    static std::optional<ResultKind> classify(const std::string& line) {
        if(has(line,"小队全灭")||has(line,"SQUAD ELIMINATED"))return ResultKind::SquadWipe;
        bool assist=has(line,"助攻")||has(line,"协助")||has(line,"ASSIST");
        bool knock=line.starts_with("击倒")||line.starts_with("KNOCKEDDOWN");
        bool elim=line.starts_with("淘汰")||line.starts_with("消灭")||line.starts_with("击败")||line.starts_with("ELIMINATED");
        if(assist){knock=has(line,"击倒")||has(line,"KNOCKEDDOWN");elim=has(line,"淘汰")||has(line,"ELIMINATED");}
        if(assist&&(knock||elim))return ResultKind::Assist;
        if(knock)return ResultKind::Knockdown;if(elim)return ResultKind::Elimination;return {};
    }
    static bool targetReadable(const std::string& text){if(text.empty()||has(text,"造成")||has(text,"伤害")||has(text,"寺")||text.size()<2)return false;
        bool letter=false;int numeric=0;for(unsigned char c:text){if(c>=128||(c>='a'&&c<='z')||(c>='A'&&c<='Z'))letter=true;if(c>='0'&&c<='9')++numeric;}
        return letter&&!(numeric>=2 && text.size()<=5);
    }
    static bool nearTarget(const std::string& a,const std::string& b){auto x=wide(a),y=wide(b);if(std::abs(int(x.size())-int(y.size()))>1)return false;
        std::vector<int> previous(y.size()+1),current(y.size()+1);for(size_t j=0;j<previous.size();++j)previous[j]=int(j);
        for(size_t i=0;i<x.size();++i){current[0]=int(i)+1;for(size_t j=0;j<y.size();++j)current[j+1]=std::min({previous[j+1]+1,current[j]+1,previous[j]+(x[i]!=y[j])});previous.swap(current);}return previous.back()<=1;}
public:
    std::string language()const{return ocr_.language();}
    void reset(){ammo_.reset();risingAmmo_.reset();damage_.reset();pendingDamage_.reset();lowerDamage_.reset();baselineHits_=resetHits_=0;magazine_=1;weapon_.clear();changingWeapon_.clear();visible_.clear();emitted_.clear();unknown_={};shotAt_=-100;lastActive_=0;stateReadAt_=-100;stateText_.clear();attribution_.reset();nameReadAt_=feedReadAt_=-100;}
    Observation process(const std::array<CpuImage,8>& images,double time,bool controllerFire=false) {
        Observation obs;obs.time=time;
        obs.ammo=HudDigits::read(cropCpu(images[2],{.578,.193,.130,.223}),2,true);
        if(obs.ammo&&*obs.ammo>80)obs.ammo.reset();
        auto weaponRead=ocr_.read(cropCpu(images[2],{.05,.80,.53,.20}));
        constexpr const char* weapons[]={"复仇女神","专注","平行","R-99","R99","R-301","R301","VK-47","赫姆洛克","猎兽","哈沃克","电能","转换者",
            "冲锋枪","轻机枪","和平捍卫者","獒犬","莫桑比克","EVA","长弓","三重","哨兵","充能步枪","G7","30-30","小帮手","P2020","RE-45","克雷贝尔",
            "Nemesis","Flatline","Havoc","Devotion","Volt","Alternator","Hemlok","CAR","Wingman","Mastiff","Sentinel","Peacekeeper"};
        for(auto name:weapons)if(has(weaponRead.text,name)){obs.weapon=name;break;}
        if(obs.ammo && ammo_){
            bool weaponChanged=!obs.weapon.empty()&&!weapon_.empty()&&obs.weapon!=weapon_&&changingWeapon_==obs.weapon;
            if(*obs.ammo>*ammo_){if(risingAmmo_&&*risingAmmo_==*obs.ammo){++magazine_;obs.reload=true;ammo_=obs.ammo;risingAmmo_.reset();}else risingAmmo_=obs.ammo;}
            else{risingAmmo_.reset();if(*obs.ammo<*ammo_)shotAt_=time;ammo_=obs.ammo;}
            if(weaponChanged&&!obs.reload){++magazine_;obs.reload=true;}
        }
        if(obs.ammo&&!ammo_)ammo_=obs.ammo;
        if(!obs.weapon.empty()){if(weapon_.empty()||obs.weapon==weapon_||changingWeapon_==obs.weapon)weapon_=obs.weapon;changingWeapon_=obs.weapon;}
        obs.magazine=magazine_;obs.firing=time-shotAt_<=.8 || controllerFire;
        auto score=ocr_.read(images[0],true);
        obs.totalDamage=HudDigits::read(cropCpu(images[0],{.520,.637,.142,.126}));
        if(obs.totalDamage&&*obs.totalDamage>20000)obs.totalDamage.reset();
        if(time-stateReadAt_>=1){
            stateText_=ocr_.read(images[3]).text+" "+ocr_.read(images[4]).text;stateReadAt_=time;
        }
        bool excluded=false;
        for(auto text:{"观战","死亡回放","死亡回顾","捍卫者","你已成为","比赛总结","射击场","SPECTATE","DEATHRECAP","CHAMPION","FIRINGRANGE"})
            excluded|=has(stateText_,text);
        bool battleLayout=has(stateText_,"剩余小队")||has(score.text,"剩余小队")||has(score.text,"SQUADSLEFT");
        // Damage digits and enemy bars can be hidden independently of the player's HUD.
        // The weapon/ammo HUD and battle layout establish context; result prompts remain usable.
        obs.active=obs.ammo.has_value()&&battleLayout&&!excluded;
        obs.status=excluded?"非本人战斗画面":obs.active?(obs.totalDamage?"HUD 已识别":"HUD 已识别 · 伤害数字不可读"):"等待战斗画面";
        if(!obs.totalDamage){damage_.reset();pendingDamage_.reset();lowerDamage_.reset();baselineHits_=resetHits_=0;}
        if(obs.active){
            if(time-nameReadAt_>=.5){attribution_.observeName(ocr_.read(images[6]));nameReadAt_=time;}
            if(time-feedReadAt_>=.3){attribution_.observeFeed(ocr_.read(images[7]),time);feedReadAt_=time;}
            obs.playerName=attribution_.playerName();if(auto target=attribution_.recentTarget(time))obs.ownFeedTarget=*target;
        }
        // A monotonic, repeated scoreboard reading is the only source of damage quantities.
        // XP / reward numbers in the center prompt never enter this path.
        if(obs.active&&obs.totalDamage){
            int current=*obs.totalDamage;
            if(!damage_){if(pendingDamage_&&*pendingDamage_==current)++baselineHits_;else{pendingDamage_=current;baselineHits_=1;}if(baselineHits_>=3){damage_=current;pendingDamage_.reset();}}
            else if(current<*damage_ && *damage_-current>100){if(lowerDamage_&&*lowerDamage_==current)++resetHits_;else{lowerDamage_=current;resetHits_=1;}
                if(resetHits_>=3){reset();obs.status="伤害计数重置，开始新对局";obs.active=false;}}
            else if(current>*damage_ && current-*damage_<=500){
                if(pendingDamage_ && current>=*pendingDamage_ && time-damageAt_<=1){
                    obs.damageDelta=current-*damage_;damage_=current;pendingDamage_.reset();
                }else {pendingDamage_=current;damageAt_=time;}
            }else if(current==*damage_){pendingDamage_.reset();lowerDamage_.reset();resetHits_=0;}
            lastActive_=time;
        }
        size_t colored=0;
        for(size_t i=0;i+3<images[5].bgra.size();i+=4){int b=images[5].bgra[i],g=images[5].bgra[i+1],r=images[5].bgra[i+2];
            if((r>180&&r>g*1.35)||(b>190&&b>r*1.25)||(r>160&&b>160&&g<140))++colored;}
        obs.hitFeedback=colored>20;
        auto prompt=ocr_.read(images[1],true);obs.prompt=prompt.text;
        if(obs.active){
            std::set<std::string> current;
            std::array<bool,4> hasLabel{},hasTarget{};
            for(const auto& line:prompt.lines){
                auto kind=classify(line);if(!kind)continue;
                int index=static_cast<int>(*kind);hasLabel[index]=true;
                auto target=targetFrom(line,*kind);
                bool fromFeed=(*kind==ResultKind::Knockdown||*kind==ResultKind::Elimination)&&!targetReadable(target)&&!obs.ownFeedTarget.empty();
                if(fromFeed)target=obs.ownFeedTarget;
                if(*kind!=ResultKind::SquadWipe&&!targetReadable(target))continue;
                if(*kind!=ResultKind::SquadWipe){auto prefix=std::to_string(index)+":";for(const auto& [token,seen]:visible_)if(token.starts_with(prefix)&&time-seen.second<.9&&nearTarget(token.substr(prefix.size()),target)){target=token.substr(prefix.size());break;}}
                auto& uncertain=unknown_[index];
                if(*kind!=ResultKind::SquadWipe&&uncertain.emitted&&time-uncertain.last<.9){
                    if(!uncertain.resolved){obs.targetAliases.emplace_back("hud-result-"+std::to_string(uncertain.id),target);uncertain.resolved=true;uncertain.resolvedTarget=target;hasTarget[index]=true;uncertain.last=time;continue;}
                    if(nearTarget(uncertain.resolvedTarget,target)){hasTarget[index]=true;uncertain.last=time;continue;}
                    // An OCR spelling change in the same feed row is not a second
                    // opponent. Ambiguous rapid results need new combat evidence.
                    if(fromFeed&&obs.totalDamage.value_or(0)<=uncertain.resultDamage){hasTarget[index]=true;uncertain.last=time;continue;}
                    uncertain={};
                }
                hasTarget[index]=true;
                auto token=std::to_string(static_cast<int>(*kind))+":"+target;current.insert(token);
                auto& v=visible_[token];if(time-v.second>.9)v.first=0;++v.first;v.second=time;
                if(v.first>=2 && (!emitted_.contains(token)||time-emitted_[token]>30)){
                    emitted_[token]=time;obs.events.push_back({time,*kind,target,fromFeed?line+"；本人击杀信息交叉验证目标":line,.95});
                }
            }
            for(int i=0;i<3;++i)if(hasLabel[i]&&!hasTarget[i]){auto& u=unknown_[i];if(time-u.last>.9){u={time,++promptId_,0,false,false};}u.last=time;
                if(++u.hits>=2&&!u.emitted){u.emitted=true;u.resultDamage=obs.totalDamage.value_or(0);obs.events.push_back({time,static_cast<ResultKind>(i),"hud-result-"+std::to_string(u.id),"连续本人中央结果提示，目标文字不清晰",.85});}}
            for(auto it=visible_.begin();it!=visible_.end();)it=time-it->second.second>1?visible_.erase(it):std::next(it);
        }
        for(auto it=emitted_.begin();it!=emitted_.end();)it=time-it->second>300?emitted_.erase(it):std::next(it);
        return obs;
    }
    static bool controllerFiring(HWND window,const std::string& button){
        if(GetForegroundWindow()!=window)return false;
        for(DWORD id=0;id<4;++id){XINPUT_STATE s{};if(XInputGetState(id,&s)!=ERROR_SUCCESS)continue;
            if(button=="LB" && (s.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER))return true;
            if(button=="RB" && (s.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER))return true;
            if(button=="RT" && s.Gamepad.bRightTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD)return true;
            if(button=="LT" && s.Gamepad.bLeftTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD)return true;
        }
        return false;
    }
};
class ObservationRules {
    RuleEngine rules_;double inactiveSince_=-1;bool active_=false;
public:
    explicit ObservationRules(Rules rules):rules_(std::move(rules)){}
    RuleEngine& rules(){return rules_;}
    bool process(const Observation& observation){
        bool cut=false;
        if(observation.active){active_=true;inactiveSince_=-1;
            for(const auto& alias:observation.targetAliases)rules_.resolveTarget(alias.first,alias.second);
            if(observation.damageDelta>0)rules_.damage({observation.time,observation.damageDelta,observation.magazine,observation.firing});
            for(const auto& event:observation.events)rules_.result(event);
        }else if(active_){if(inactiveSince_<0)inactiveSince_=observation.time;
            if(observation.time-inactiveSince_>=2||has(observation.status,"重置")){boundary(observation.time);cut=true;}}
        rules_.tick(observation.time);
        return cut;
    }
    void boundary(double time){rules_.boundary(time);active_=false;inactiveSince_=-1;}
};
}
