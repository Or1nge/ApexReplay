#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include "json.hpp"
#include "rules.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
namespace apex {
using json=nlohmann::json;
using namespace std::chrono_literals;
inline void ffcheck(int code,const char* operation) {
    if(code<0) { char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code,text,sizeof(text)); throw std::runtime_error(std::string(operation)+": "+text); }
}
inline double qpcSeconds() { LARGE_INTEGER v,f; QueryPerformanceCounter(&v); QueryPerformanceFrequency(&f); return double(v.QuadPart)/f.QuadPart; }
inline std::string utf8(const std::wstring& value) { return winrt::to_string(winrt::hstring(value)); }
inline std::wstring wide(const std::string& value) { return std::wstring(winrt::to_hstring(value)); }
inline std::string pathUtf8(const std::filesystem::path& p) { return utf8(p.wstring()); }
inline std::string errorText() {
    try { throw; } catch(const winrt::hresult_error& e) { return winrt::to_string(e.message())+" (0x"+[] (uint32_t c){std::ostringstream s;s<<std::hex<<c;return s.str();}(e.code())+")"; }
    catch(const std::exception& e) {return e.what();} catch(...) { return "Unknown native error"; }
}
inline json eventJson(const CombatEvent& e) {
    constexpr const char* kinds[]={"knockdown","elimination","assist","squad_wipe"};
    return {{"time",e.time},{"kind",kinds[static_cast<int>(e.kind)]},{"target",e.target},{"confidence",e.confidence},{"evidence",e.evidence}};
}
inline json clipJson(const ClipPlan& c) {
    json events=json::array(); for(const auto& e:c.events)events.push_back(eventJson(e));
    return {{"start",c.start},{"end",c.end},{"kind",c.kind},{"kills",c.kills},{"ruleVersion",c.version},
        {"squadWipe",c.squadWipe},{"truncated",c.truncated},{"events",events}};
}
inline Rules loadRules(const std::filesystem::path& p) {
    Rules r; if(!std::filesystem::exists(p))return r;
    auto j=json::parse(std::ifstream(p));
    r.version=j.value("version",r.version);
#define READ_RULE(field) r.field=j.value(#field,r.field)
    READ_RULE(burstDamage);READ_RULE(burstSeconds);READ_RULE(fastBurstDamage);READ_RULE(fastBurstSeconds);
    READ_RULE(resultGraceSeconds);READ_RULE(fastMergeSeconds);READ_RULE(bridgeMergeSeconds);
    READ_RULE(bridgeDamage);READ_RULE(bridgeDamageSeconds);READ_RULE(bridgeQuietSeconds);
    READ_RULE(maxClipSeconds);READ_RULE(splitOverlapSeconds);READ_RULE(longPre);READ_RULE(longPost);
#undef READ_RULE
    if(r.fastMergeSeconds<0 || r.bridgeMergeSeconds<r.fastMergeSeconds || r.burstSeconds<=0 ||
        r.maxClipSeconds<40 || r.maxClipSeconds>180) throw std::runtime_error("Invalid rule configuration");
    return r;
}
struct Settings {
    std::string outputDirectory,microphoneId,controllerFire="LB";
    double shortPre=5,shortPost=3,longPre=10,longPost=5,balance=0;
    double replayMinutes=30,memoryPercent=60;
    bool micNoiseSuppression=true;
    std::string videoResolution="source",videoCodec="hevc",videoPreset="p6";
    double videoBitrateMbps=60;
    static Settings fromJson(const json& j) {
        Settings s;
        s.outputDirectory=j.value("outputDirectory",std::string());
        s.microphoneId=j.value("microphoneId",std::string());
        s.micNoiseSuppression=j.value("micNoiseSuppression",true);
        s.videoResolution=j.value("videoResolution",std::string("source"));s.videoCodec=j.value("videoCodec",std::string("hevc"));s.videoPreset=j.value("videoPreset",std::string("p6"));
        if(s.videoResolution!="source"&&s.videoResolution!="1080p"&&s.videoResolution!="1440p"&&s.videoResolution!="2160p")throw std::runtime_error("Unsupported video resolution");
        if(s.videoCodec!="hevc"&&s.videoCodec!="h264")throw std::runtime_error("Unsupported video codec");
        if(s.videoPreset!="p4"&&s.videoPreset!="p5"&&s.videoPreset!="p6")throw std::runtime_error("Unsupported video preset");
        s.videoBitrateMbps=j.value("videoBitrateMbps",60.0);if(!std::isfinite(s.videoBitrateMbps))throw std::runtime_error("Nonfinite video bitrate");s.videoBitrateMbps=std::clamp(s.videoBitrateMbps,10.0,200.0);
        s.controllerFire=j.value("controllerFire",std::string("LB"));
        auto number=[&](const char* key,double value,double hi){double x=j.value(key,value); if(!std::isfinite(x))throw std::runtime_error("Nonfinite setting");return std::clamp(x,0.0,hi);};
        s.shortPre=number("shortPre",5,20);s.shortPost=number("shortPost",3,15);s.longPre=number("longPre",10,30);s.longPost=number("longPost",5,20);
        s.balance=std::clamp(j.value("balance",0.0),-12.0,12.0);if(!std::isfinite(s.balance))throw std::runtime_error("Nonfinite balance");
        s.replayMinutes=std::max(1.0,number("replayMinutes",30,120));s.memoryPercent=std::max(10.0,number("memoryPercent",60,90));
        return s;
    }
};
inline std::pair<int,int> videoDimensions(HWND window,const Settings& settings){
    if(settings.videoResolution=="1080p")return {1920,1080};if(settings.videoResolution=="1440p")return {2560,1440};if(settings.videoResolution=="2160p")return {3840,2160};
    RECT rect{};if(!window||!GetClientRect(window,&rect)||rect.right<640||rect.bottom<360)return {1920,1080};
    return {std::max(2,int(rect.right)&~1),std::max(2,int(rect.bottom)&~1)};
}
struct CpuImage { int width=0,height=0;std::vector<uint8_t> bgra; };
struct Region { double x,y,w,h; };
inline CpuImage cropCpu(const CpuImage& image,Region r) {
    int x=int(r.x*image.width),y=int(r.y*image.height);
    CpuImage out{std::max(1,int(r.w*image.width)),std::max(1,int(r.h*image.height)),{}};
    out.width=std::min(out.width,image.width-x);out.height=std::min(out.height,image.height-y);
    out.bgra.resize(size_t(out.width)*out.height*4);
    for(int row=0;row<out.height;++row) memcpy(out.bgra.data()+size_t(row)*out.width*4,image.bgra.data()+(size_t(row+y)*image.width+x)*4,size_t(out.width)*4);
    return out;
}
}
