#pragma once
#include "common.hpp"
#include "gpu.hpp"
#include "digits.hpp"
#include "hud_attribution.hpp"
#include "hud_results.hpp"
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
    OcrRead read(const CpuImage& image,bool contrast=false,bool redNames=false) {
        int scale=image.width<900?2:1,w=image.width*scale,h=image.height*scale;
        std::vector<uint8_t> pixels(size_t(w)*h*4);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            const auto* src=image.bgra.data()+(size_t(y/scale)*image.width+x/scale)*4;
            auto* dst=pixels.data()+(size_t(y)*w+x)*4;
            if(contrast){bool white=std::min({src[0],src[1],src[2]})>=155;bool orange=src[2]>175&&src[1]>65&&src[1]<185&&src[2]>src[1]*1.4&&src[1]>src[0]*1.4;
                bool red=redNames&&src[2]>150&&src[2]>src[1]*1.4&&src[2]>src[0]*1.4;uint8_t v=white||orange||red?0:255;dst[0]=dst[1]=dst[2]=v;}else memcpy(dst,src,3);
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
    std::vector<std::pair<std::string,ResultKind>> resultCorrections;
    json toJson()const {
        json eventsJson=json::array();for(const auto& e:events)eventsJson.push_back(eventJson(e));
        json out{{"time",time},{"active",active},{"firing",firing},{"reload",reload},{"hitFeedback",hitFeedback},
            {"weapon",weapon},{"magazine",magazine},{"damageDelta",damageDelta},{"status",status},{"prompt",prompt},{"events",eventsJson},{"playerName",playerName},{"ownFeedTarget",ownFeedTarget},{"targetAliases",targetAliases},{"resultCorrections",resultCorrections}};
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
    ResultPrompts results_;
public:
    std::string language()const{return ocr_.language();}
    void reset(){ammo_.reset();risingAmmo_.reset();damage_.reset();pendingDamage_.reset();lowerDamage_.reset();baselineHits_=resetHits_=0;magazine_=1;weapon_.clear();changingWeapon_.clear();results_.reset();shotAt_=-100;lastActive_=0;stateReadAt_=-100;stateText_.clear();attribution_.reset();nameReadAt_=feedReadAt_=-100;}
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
        // The result row sits above subtitles. Include red opponent names, which the
        // generic white/orange OCR mask previously erased completely.
        auto prompt=ocr_.read(cropCpu(images[1],{.10,.25,.80,.20}),true,true);obs.prompt=prompt.text;
        if(obs.active){
            auto read=results_.process(prompt,time,obs.ownFeedTarget,obs.totalDamage);
            obs.events=std::move(read.events);obs.targetAliases=std::move(read.aliases);obs.resultCorrections=std::move(read.corrections);
        }
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
            for(const auto& correction:observation.resultCorrections)rules_.correctResult(correction.first,correction.second);
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
