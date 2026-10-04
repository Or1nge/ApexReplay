#pragma once
#include "common.hpp"
#include "gpu.hpp"
#include "digits.hpp"
#include "hud_attribution.hpp"
#include "hud_results.hpp"
#include "hud_tracking.hpp"
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
class HudDetector {
    LocalOcr ocr_;std::optional<int> ammo_;
    DamageTracker damage_;CounterTracker kills_,assists_;
    double shotAt_=-100,lastActive_=0,stateReadAt_=-100;
    unsigned magazine_=1;std::string weapon_,stateText_;
    std::optional<int> risingAmmo_;std::string changingWeapon_;
    OwnFeedAttribution attribution_;double nameReadAt_=-100,feedReadAt_=-100;
    ResultPrompts results_;
public:
    std::string language()const{return ocr_.language();}
    void reset(){ammo_.reset();risingAmmo_.reset();damage_.reset();kills_.reset();assists_.reset();magazine_=1;weapon_.clear();changingWeapon_.clear();results_.reset();shotAt_=-100;lastActive_=0;stateReadAt_=-100;stateText_.clear();attribution_.reset();nameReadAt_=feedReadAt_=-100;}
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
        // Kills, assists and damage share one right-aligned row; it also stands in for an unreadable damage crop.
        auto counters=HudDigits::readRow(cropCpu(images[0],{.050,.593,.624,.193}));
        if(!obs.totalDamage)obs.totalDamage=counters.damage;
        if(obs.totalDamage&&*obs.totalDamage>20000)obs.totalDamage.reset();
        obs.kills=counters.kills;obs.assists=counters.assists;obs.countersHidden=counters.hidden;
        if(time-stateReadAt_>=1){
            stateText_=ocr_.read(images[3]).text+" "+ocr_.read(images[4]).text;stateReadAt_=time;
        }
        // The Peacekeeper (和平捍卫者) is a weapon name in pickup prompts and the inventory, not the Champion banner.
        auto state=stateText_;for(size_t at;(at=state.find("和平捍卫者"))!=std::string::npos;)state.erase(at,std::char_traits<char>::length("和平捍卫者"));
        for(auto text:{"观战","死亡回放","死亡回顾","捍卫者","你已成为","比赛总结","射击场","SPECTATE","DEATHRECAP","CHAMPION","FIRINGRANGE"})
            if(obs.excludedBy.empty()&&has(state,text))obs.excludedBy=text;
        bool excluded=!obs.excludedBy.empty();
        bool battleLayout=has(stateText_,"剩余小队")||has(score.text,"剩余小队")||has(score.text,"SQUADSLEFT");
        // Damage digits and enemy bars can be hidden independently of the player's HUD.
        // The weapon/ammo HUD and battle layout establish context; result prompts remain usable.
        obs.active=obs.ammo.has_value()&&battleLayout&&!excluded;
        obs.status=excluded?"非本人战斗画面":obs.active?(obs.totalDamage?"HUD 已识别":"HUD 已识别 · 伤害数字不可读"):"等待战斗画面";
        if(obs.active){
            if(time-nameReadAt_>=.5){attribution_.observeName(ocr_.read(images[6]));nameReadAt_=time;}
            if(time-feedReadAt_>=.3){auto feed=ocr_.read(images[7]);obs.feed=attribution_.observeFeed(feed,time,&images[7]);obs.feedRead=true;
                for(const auto& line:feed.lines)obs.feedText+=(obs.feedText.empty()?"":" | ")+line;feedReadAt_=time;}
            obs.playerName=attribution_.playerName();if(auto target=attribution_.recentTarget(time))obs.ownFeedTarget=*target;
        }
        // A monotonic, repeated counter reading is the only source of damage quantities; the last confirmed
        // value survives frames where the rolling number cannot be read. XP / reward numbers in the center
        // prompt never enter this path.
        if(obs.active){
            auto change=damage_.observe(obs.totalDamage,time);
            if(change.reset){reset();obs.status="伤害计数重置，开始新对局";obs.active=false;}
            else{
                obs.damageDelta=change.delta;obs.damageAt=change.at;obs.damageSince=change.since;obs.damageBaseline=change.baseline;
                // The kills and assists box is absent until the first kill or assist of the match.
                auto kills=kills_.observe(counters.hidden?std::optional<int>(0):counters.kills),assists=assists_.observe(counters.hidden?std::optional<int>(0):counters.assists);
                if(kills&&!kills->baseline)obs.killDelta=kills->to-kills->from;
                if(assists&&!assists->baseline)obs.assistDelta=assists->to-assists->from;
                lastActive_=time;
            }
            obs.confirmedDamage=damage_.value();obs.killCount=kills_.value();obs.assistCount=assists_.value();
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
}
