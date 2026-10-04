#pragma once
#include "audio.hpp"
#include "detector.hpp"
#include <array>
namespace apex {
struct ApexWindow{HWND handle=nullptr;DWORD pid=0;};
inline ApexWindow findApex() {
    ApexWindow found{};
    EnumWindows([](HWND window,LPARAM data)->BOOL{
        if(!IsWindowVisible(window)||GetWindow(window,GW_OWNER))return TRUE;
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return TRUE;
        wchar_t path[32768]{};DWORD size=32768;bool valid=QueryFullProcessImageNameW(process,0,path,&size);CloseHandle(process);
        if(!valid)return TRUE;
        auto name=std::filesystem::path(path).filename().wstring();
        if(_wcsicmp(name.c_str(),L"r5apex.exe")&&_wcsicmp(name.c_str(),L"r5apex_dx12.exe"))return TRUE;
        wchar_t title[512]{};GetWindowTextW(window,title,512);
        if(!wcsstr(title,L"Apex")&&!wcsstr(title,L"APEX"))return TRUE;
        RECT rect{};if(!GetClientRect(window,&rect)||rect.right<640||rect.bottom<360)return TRUE;
        auto& result=*reinterpret_cast<ApexWindow*>(data);
        if(!result.handle || GetForegroundWindow()==window)result={window,pid};return TRUE;
    },reinterpret_cast<LPARAM>(&found));return found;
}
inline bool windowHdr(HWND window) {
    HMONITOR monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    winrt::com_ptr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),factory.put_void())))return false;
    for(UINT i=0;;++i){winrt::com_ptr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,a.put())==DXGI_ERROR_NOT_FOUND)break;
        for(UINT j=0;;++j){winrt::com_ptr<IDXGIOutput>o;if(a->EnumOutputs(j,o.put())==DXGI_ERROR_NOT_FOUND)break;DXGI_OUTPUT_DESC d{};o->GetDesc(&d);
            if(d.Monitor==monitor){auto o6=o.try_as<IDXGIOutput6>();if(!o6)return false;DXGI_OUTPUT_DESC1 d1{};o6->GetDesc1(&d1);return d1.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;}}}
    return false;
}
class CaptureSession {
    ApexWindow target_;Rules baseRules_;ExportQueue& exports_;std::function<void(json)> notify_;
    std::atomic<std::shared_ptr<Settings>> settings_;
    std::atomic<std::shared_ptr<DevRecorder>> recorder_;std::atomic<unsigned> devId_=0;bool recorderFailed_=false;
    Gpu gpu_;PacketRing ring_;std::unique_ptr<VideoEncoder> video_;std::array<std::unique_ptr<AudioCapture>,3> audio_;
    std::unique_ptr<WindowCapture> capture_;std::shared_ptr<CodecSet> codecs_;
    double epoch_=0;std::jthread videoThread_,analysisThread_;
    std::mutex analysisMutex_;std::condition_variable_any analysisCv_;
    struct AnalysisFrame{std::array<CpuImage,8> images;double time;bool firing;bool available=true;};
    std::optional<AnalysisFrame> analysisFrame_;
    std::atomic<bool> pending_=false,failed_=false;std::atomic<unsigned> skippedAnalysis_=0,videoFrames_=0,repeatedFrames_=0;
    std::atomic<double> analysisMs_=0;std::atomic<unsigned> exportId_=0;
    std::string detectionStatus_="等待战斗画面";std::mutex statusMutex_;json hud_={{"damage",nullptr},{"kills",nullptr},{"assists",nullptr}};
    void updateRecorder(const Settings& settings){
        auto current=recorder_.load();if(!settings.developerMode){recorder_.store(nullptr);recorderFailed_=false;return;}if(recorderFailed_)return;
        if(current){current->retention(settings.replayMinutes*60+120);current->append({{"type","settings"},{"t",qpcSeconds()-epoch_},{"settings",settingsJson(settings)}});return;}
        SYSTEMTIME now{};GetLocalTime(&now);std::wostringstream name;
        name<<L"Apex_dev_"<<std::setfill(L'0')<<std::setw(4)<<now.wYear<<std::setw(2)<<now.wMonth<<std::setw(2)<<now.wDay<<L"_"
            <<std::setw(2)<<now.wHour<<std::setw(2)<<now.wMinute<<std::setw(2)<<now.wSecond<<L"_"<<std::setw(3)<<now.wMilliseconds<<L"_"<<GetCurrentProcessId()<<L"_"<<devId_++<<L".json";
        // The analysis thread applies the damage criteria and long-clip timings from settings on top of rules.v1.json.
        const auto& r=baseRules_;
        json rules{{"version",r.version},{"burstDamage",settings.burstDamage},{"burstSeconds",settings.burstSeconds},{"fastBurstDamage",settings.fastBurstDamage},{"fastBurstSeconds",settings.fastBurstSeconds},
            {"resultGraceSeconds",r.resultGraceSeconds},{"fastMergeSeconds",r.fastMergeSeconds},{"bridgeMergeSeconds",r.bridgeMergeSeconds},{"bridgeDamage",r.bridgeDamage},{"bridgeDamageSeconds",r.bridgeDamageSeconds},
            {"bridgeQuietSeconds",r.bridgeQuietSeconds},{"maxClipSeconds",r.maxClipSeconds},{"splitOverlapSeconds",r.splitOverlapSeconds},{"longPre",settings.longPre},{"longPost",settings.longPost}};
        auto notify=notify_;
        try{recorder_.store(std::make_shared<DevRecorder>(std::filesystem::path(wide(settings.outputDirectory))/L"开发者记录"/name.str(),
            json{{"settings",settingsJson(settings)},{"rules",rules},{"hudVersion",HudDigits::version},{"startedAt",qpcSeconds()-epoch_}},
            settings.replayMinutes*60+120,[notify](std::string message){notify({{"type","devlog_error"},{"message",message}});}));}
        catch(...){recorderFailed_=true;notify_({{"type","devlog_error"},{"message","开发者记录创建失败，采集继续"}});}
    }
    void publishClip(ClipPlan clip) {
        ExportJob job;job.epoch=epoch_;job.clip=std::move(clip);job.codecs=codecs_;
        if(job.clip.kind!="manual"&&job.clip.end>ring_.latestVideo()+1./60){job.clip.end=ring_.latestVideo()+1./60;job.clip.truncated=true;}
        double segment=ring_.videoSegmentStart();if(job.clip.kind!="manual"&&job.clip.end>segment&&job.clip.start<segment){job.clip.start=segment;job.clip.truncated=true;}
        job.packets=ring_.snapshot(job.clip.start,job.clip.end,job.actualStart,job.clip.kind=="manual");
        if(job.actualStart>job.clip.start+.02)job.clip.truncated=true;
        auto settings=settings_.load();auto directory=std::filesystem::path(wide(settings->outputDirectory));
        SYSTEMTIME now{};GetLocalTime(&now);
        std::wostringstream name;name<<L"Apex_"<<std::setfill(L'0')<<std::setw(4)<<now.wYear<<std::setw(2)<<now.wMonth<<std::setw(2)<<now.wDay<<L"_"
            <<std::setw(2)<<now.wHour<<std::setw(2)<<now.wMinute<<std::setw(2)<<now.wSecond<<L"_"<<std::setw(3)<<now.wMilliseconds
            <<L"_"<<(job.clip.kind=="multikill"?L"长镜头":job.clip.kind=="burst"?L"高伤害":job.clip.kind=="manual"?L"手动":L"测试")<<L"_"<<job.clip.kills<<L"_"<<GetCurrentProcessId()<<L"_"<<exportId_++<<L".mp4";
        job.file=directory/name.str();
        if(auto recorder=recorder_.load()){
            recorder->append({{"type","clip_published"},{"t",qpcSeconds()-epoch_},{"path",pathUtf8(job.file)},{"start",job.clip.start},{"end",job.clip.end},{"videoStart",job.actualStart},{"kind",job.clip.kind},{"opponents",job.clip.kills},{"reason",job.clip.reason}});
            try{job.devSlice=recorder->slice(job.clip,job.actualStart);job.recorder=std::move(recorder);}catch(...){notify_({{"type","devlog_error"},{"message","片段开发者记录切片失败，视频继续保存"}});}
        }
        if(job.clip.kind=="manual")notify_({{"type","manual_save"},{"state","queued"}});exports_.push(std::move(job));
    }
    // Speech leveling delays the audio tracks by about 0.3 s, so a finished clip waits (at most 1.5 s) until all
    // three tracks have reached its end. Only the analysis thread touches the waiting list.
    std::vector<std::pair<ClipPlan,double>> waiting_;
    void drain(RuleEngine& rules,bool final=false){
        double now=qpcSeconds(),audio=ring_.latestAudio();
        for(auto& clip:rules.takeReady())waiting_.push_back({std::move(clip),now});
        std::optional<double> needed=rules.earliestNeeded();
        for(auto it=waiting_.begin();it!=waiting_.end();){
            if(final||it->first.end<=audio||now-it->second>1.5){publishClip(std::move(it->first));it=waiting_.erase(it);}
            else{needed=std::min(needed.value_or(it->first.start),it->first.start);++it;}
        }
        ring_.pin(needed);
    }
    void analyze(std::stop_token stop){
        winrt::init_apartment(winrt::apartment_type::multi_threaded);ObservationRules flow(baseRules_);auto& rules=flow.rules();
        rules.setTrace([this](RuleNote note){if(auto recorder=recorder_.load())recorder->rule(note);});
        flow.setBoundaryTrace([this](double time,std::string reason){if(auto recorder=recorder_.load())recorder->append({{"type","boundary"},{"t",time},{"reason",reason}});});
        try{
            HudDetector detector;auto language=detector.language();notify_({{"type","ocr"},{"language",language},{"ruleVersion",baseRules_.version}});
            while(!stop.stop_requested()){
                AnalysisFrame frame;
                {std::unique_lock lock(analysisMutex_);analysisCv_.wait_for(lock,stop,200ms,[&]{return analysisFrame_.has_value();});if(!analysisFrame_){if(stop.stop_requested())break;lock.unlock();rules.tick(qpcSeconds()-epoch_);drain(rules);pending_=rules.pending();continue;}frame=std::move(*analysisFrame_);analysisFrame_.reset();}
                if(!frame.available){Observation unavailable;unavailable.time=frame.time;unavailable.status="Apex 窗口已最小化";
                    if(auto recorder=recorder_.load())recorder->observe(unavailable);if(flow.process(unavailable))detector.reset();drain(rules);pending_=rules.pending();continue;}
                double begin=qpcSeconds();auto observation=detector.process(frame.images,frame.time,frame.firing);
                auto settings=settings_.load();rules.updateTimings(settings->longPre,settings->longPost);
                rules.updateThresholds(settings->burstSeconds,settings->burstDamage,settings->fastBurstSeconds,settings->fastBurstDamage);
                {std::lock_guard lock(statusMutex_);detectionStatus_=observation.status;hud_={{"damage",optionalJson(observation.confirmedDamage)},{"kills",optionalJson(observation.killCount)},{"assists",optionalJson(observation.assistCount)}};}
                if(auto recorder=recorder_.load()){recorder->language(language);recorder->observe(observation);}
                if(flow.process(observation))detector.reset();for(const auto& event:observation.events)notify_({{"type","event"},{"event",eventJson(event)}});
                for(const auto& correction:observation.resultCorrections)notify_({{"type","result_correction"},{"target",correction.first},{"kind","assist"}});
                drain(rules);pending_=rules.pending();analysisMs_=(qpcSeconds()-begin)*1000;
            }
            flow.boundary(ring_.latest());drain(rules,true);pending_=false;
        }catch(...){failed_=true;notify_({{"type","fatal"},{"message",errorText()}});}
    }
    void record(std::stop_token stop) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try{
            bool rendered=false;double lastNewFrame=qpcSeconds(),lastAnalysis=-100,lastStatus=-100,lastAudio=-100;
            int64_t lastPts=-1;bool hdr=windowHdr(target_.handle);double renderMs=0,convertMs=0,encodeMs=0,readbackMs=0;
            while(!stop.stop_requested()&&!failed_){
                if(capture_->gone()||!IsWindow(target_.handle))break;
                auto frame=capture_->take();
                if(frame){double began=qpcSeconds();auto texture=WindowCapture::texture(frame);gpu_.render(texture.get(),hdr);renderMs=(qpcSeconds()-began)*1000;rendered=true;lastNewFrame=qpcSeconds();}
                double time=qpcSeconds()-epoch_;
                bool minimized=IsIconic(target_.handle);
                if(minimized)rendered=false;
                if(rendered){
                    auto pts=static_cast<int64_t>(std::llround(time*60));
                    if(pts>lastPts){
                        double began=qpcSeconds();auto encoded=video_->frame(pts);
                        try{gpu_.convert(encoded);convertMs=(qpcSeconds()-began)*1000;began=qpcSeconds();video_->send(encoded);encodeMs=(qpcSeconds()-began)*1000;}catch(...){av_frame_free(&encoded);throw;}
                        av_frame_free(&encoded);lastPts=pts;++videoFrames_;if(!frame)++repeatedFrames_;
                    }
                    if(time-lastAnalysis>=.1){
                        double began=qpcSeconds();AnalysisFrame input{gpu_.readRegions(),time,HudDetector::controllerFiring(target_.handle,settings_.load()->controllerFire)};readbackMs=(qpcSeconds()-began)*1000;
                        {std::lock_guard lock(analysisMutex_);if(analysisFrame_)++skippedAnalysis_;analysisFrame_=std::move(input);}analysisCv_.notify_one();lastAnalysis=time;
                    }
                }else if(!minimized&&qpcSeconds()-lastNewFrame>5){throw std::runtime_error("Apex 窗口未提供画面，请使用无边框窗口模式并检查采集权限");}
                if(minimized&&time-lastAnalysis>=1){AnalysisFrame input{{},time,false,false};{std::lock_guard lock(analysisMutex_);analysisFrame_=std::move(input);}analysisCv_.notify_one();lastAnalysis=time;}
                if(time-lastAudio>=.1){
                    json levels=json::array(),health=json::array(),speech=json::array();
                    for(auto& a:audio_){levels.push_back(a->level());health.push_back(a->healthy());
                        auto s=a->speech();speech.push_back({{"active",s.speech},{"learned",s.learned},{"level",s.levelDb},{"gain",s.gainDb}});}
                    notify_({{"type","audio_levels"},{"levels",levels},{"audioHealthy",health},{"speech",speech}});lastAudio=time;
                }
                if(time-lastStatus>=1){
                    std::string detection;json hud;{std::lock_guard lock(statusMutex_);detection=detectionStatus_;hud=hud_;}
                    auto recorder=recorder_.load();auto devPath=recorder?recorder->path():std::filesystem::path();
                    json health=json::array();for(auto& a:audio_)health.push_back(a->healthy());
                    notify_({{"type","status"},{"state",minimized?"paused":pending_?"pending":"buffering"},{"bufferSeconds",ring_.duration()},{"bufferBytes",ring_.bytes()},
                        {"totalBytes",Packet::liveBytes.load()},{"frames",videoFrames_.load()},{"repeatedFrames",repeatedFrames_.load()},
                        {"memoryBudgetBytes",ring_.budget()},{"idleMemoryBytes",ring_.idleBytes()},{"replayMinutes",settings_.load()->replayMinutes},{"memoryPercent",settings_.load()->memoryPercent},
                        {"hud",hud},{"devLogPath",devPath.empty()?"":pathUtf8(devPath)},{"canSaveReplay",ring_.earliestKey()>=0&&std::min(ring_.latestVideo(),ring_.latestAudio())>ring_.earliestKey()&&!exports_.manualBusy()},
                        {"analysisDropped",skippedAnalysis_.load()},{"analysisMs",analysisMs_.load()},{"renderMs",renderMs},{"convertMs",convertMs},{"encodeMs",encodeMs},{"readbackMs",readbackMs},{"detection",detection},
                        {"audioHealthy",health},{"captureBorderHidden",capture_->borderHidden()},{"videoWidth",gpu_.width()},{"videoHeight",gpu_.height()},{"videoCodec",avcodec_get_name(codecs_->parameters[0]->codec_id)},
                        {"videoTargetBitrateMbps",video_->targetBitrateMbps()},{"videoPreset",video_->preset()},{"videoSplitEncodingConfigured",video_->splitEncodingConfigured()},
                        {"micNoiseSuppression",settings_.load()->micNoiseSuppression},{"exporting",exports_.busy()},{"failedExports",exports_.failures()},{"adapter",gpu_.adapter()}});
                    lastStatus=time;
                }
                double delay=(lastPts+1)/60.-(qpcSeconds()-epoch_);
                if(rendered&&delay>0)std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(delay*1000000)));else std::this_thread::sleep_for(minimized?30ms:1ms);
            }
            video_->flush();
        }catch(...){failed_=true;notify_({{"type","fatal"},{"message",errorText()}});}
    }
public:
    CaptureSession(ApexWindow target,Rules rules,Settings settings,ExportQueue& exports,std::function<void(json)> notify)
        :target_(target),baseRules_(std::move(rules)),exports_(exports),notify_(std::move(notify)),settings_(std::make_shared<Settings>(std::move(settings))),gpu_(videoDimensions(target_.handle,*settings_.load())) {
        ring_.configure(settings_.load()->replayMinutes,settings_.load()->memoryPercent);
        video_=std::make_unique<VideoEncoder>(gpu_.device(),[this](PacketRef p){ring_.push(std::move(p));},*settings_.load(),gpu_.width(),gpu_.height());
        epoch_=qpcSeconds();codecs_=std::make_shared<CodecSet>();codecs_->parameters[0]=video_->parameters();codecs_->timebases[0]=video_->timebase();
        for(int i=0;i<3;++i){audio_[i]=std::make_unique<AudioCapture>(i+1,target_.pid,settings_.load()->microphoneId,epoch_,
            [this](PacketRef p){ring_.push(std::move(p));},[this]{return settings_.load()->balance;},[this](json j){if(j.value("type","")=="fatal")failed_=true;notify_(std::move(j));},settings_.load()->micNoiseSuppression);
            codecs_->parameters[i+1]=audio_[i]->parameters();codecs_->timebases[i+1]=audio_[i]->timebase();}
        updateRecorder(*settings_.load());
        capture_=std::make_unique<WindowCapture>(target.handle,gpu_.device());
        for(auto& a:audio_)a->start();analysisThread_=std::jthread([this](auto stop){analyze(stop);});videoThread_=std::jthread([this](auto stop){record(stop);});
    }
    ~CaptureSession(){stop();}
    void stop(){
        videoThread_.request_stop();if(videoThread_.joinable())videoThread_.join();
        for(auto& a:audio_)if(a)a->stop();
        analysisThread_.request_stop();analysisCv_.notify_all();if(analysisThread_.joinable())analysisThread_.join();capture_.reset();
    }
    void update(Settings settings){updateRecorder(settings);ring_.configure(settings.replayMinutes,settings.memoryPercent);if(audio_[1]){audio_[1]->microphone(settings.microphoneId);audio_[1]->noiseSuppression(settings.micNoiseSuppression);}settings_.store(std::make_shared<Settings>(std::move(settings)));}
    void requestManualSave(){
        if(exports_.manualBusy()){notify_({{"type","manual_save"},{"state","busy"}});return;}
        double start=ring_.earliestKey(),end=std::min(ring_.latestVideo(),ring_.latestAudio());
        if(start<0||end<=start){notify_({{"type","manual_save"},{"state","unavailable"}});return;}
        try{if(auto recorder=recorder_.load())recorder->append({{"type","manual_save"},{"t",qpcSeconds()-epoch_},{"state","requested"},{"start",start},{"end",end}});
            ClipPlan clip{start,end,"manual",baseRules_.version,{},false,false,0,"用户手动保存全部缓存"};publishClip(std::move(clip));}
        catch(...){auto message="手动保存失败："+errorText();if(auto recorder=recorder_.load())recorder->append({{"type","manual_save"},{"t",qpcSeconds()-epoch_},{"state","failed"},{"detail",message}});
            notify_({{"type","manual_save"},{"state","failed"},{"message",message}});notify_({{"type","export_failed"},{"message",message},{"retryable",false}});}
    }
    bool failed()const{return failed_;}
    HWND window()const{return target_.handle;}
    void exportForTest(double start,double end){publishClip({start,end,"selftest",baseRules_.version,{},false,false,0});}
    unsigned framesForTest()const{return videoFrames_;}
};
inline CpuImage frameToCpu(AVFrame* frame,SwsContext*& scale) {
    CpuImage out{1920,1080,{}};out.bgra.resize(1920ull*1080*4);
    scale=sws_getCachedContext(scale,frame->width,frame->height,static_cast<AVPixelFormat>(frame->format),1920,1080,AV_PIX_FMT_BGRA,SWS_BILINEAR,nullptr,nullptr,nullptr);
    if(!scale)throw std::runtime_error("Unable to initialize offline video scaling");
    uint8_t* data[]={out.bgra.data()};int stride[]={1920*4};sws_scale(scale,frame->data,frame->linesize,0,frame->height,data,stride);return out;
}
inline json inspectImage(const std::filesystem::path& file,bool gpuCheck=false){
    AVFormatContext* input=nullptr;AVCodecContext* decoder=nullptr;AVFrame* frame=av_frame_alloc();AVPacket* packet=av_packet_alloc();SwsContext* scale=nullptr;json result;
    try{ffcheck(avformat_open_input(&input,pathUtf8(file).c_str(),nullptr,nullptr),"open image");ffcheck(avformat_find_stream_info(input,nullptr),"inspect image");
        int index=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);ffcheck(index,"find image");auto enc=avcodec_find_decoder(input->streams[index]->codecpar->codec_id);
        decoder=avcodec_alloc_context3(enc);ffcheck(avcodec_parameters_to_context(decoder,input->streams[index]->codecpar),"configure image decoder");ffcheck(avcodec_open2(decoder,enc,nullptr),"open image decoder");
        while(av_read_frame(input,packet)>=0){if(packet->stream_index==index){ffcheck(avcodec_send_packet(decoder,packet),"decode image");if(avcodec_receive_frame(decoder,frame)>=0)break;}av_packet_unref(packet);}
        auto image=frameToCpu(frame,scale);LocalOcr ocr;result=json::array();
        for(const auto& region:Gpu::regions){json variants=json::array();for(bool contrast:{false,true}){auto read=ocr.read(cropCpu(image,region),contrast);json words=json::array();for(auto& w:read.words)words.push_back({{"text",w.text},{"x",w.x},{"y",w.y},{"w",w.w},{"h",w.h}});variants.push_back({{"contrast",contrast},{"text",read.text},{"lines",read.lines},{"words",words}});}result.push_back(variants);}
        auto read=ocr.read(cropCpu(cropCpu(image,Gpu::regions[2]),{.54,.16,.24,.31}),true);json words=json::array();for(auto& w:read.words)words.push_back({{"text",w.text},{"x",w.x},{"y",w.y},{"w",w.w},{"h",w.h}});result.push_back({{"ammoCrop",read.text},{"words",words}});
        std::array<CpuImage,8> images;for(size_t i=0;i<images.size();++i)images[i]=cropCpu(image,Gpu::regions[i]);HudDetector detector;detector.process(images,0);auto baseline=detector.process(images,.2);result.push_back({{"observation",baseline.toJson()}});
        if(gpuCheck){json checks=json::array();for(auto size:{std::pair{1920,1080},std::pair{2560,1440},std::pair{3840,2160}}){
            Gpu gpu(size);D3D11_TEXTURE2D_DESC description{};description.Width=image.width;description.Height=image.height;description.MipLevels=description.ArraySize=1;description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;description.SampleDesc.Count=1;description.Usage=D3D11_USAGE_DEFAULT;description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA pixels{image.bgra.data(),UINT(image.width*4),0};winrt::com_ptr<ID3D11Texture2D> texture;winrt::check_hresult(gpu.device()->CreateTexture2D(&description,&pixels,texture.put()));gpu.render(texture.get());auto regions=gpu.readRegions();
            HudDetector normalized;normalized.process(regions,0);auto observed=normalized.process(regions,.2);checks.push_back({{"width",size.first},{"height",size.second},{"passed",observed.active==baseline.active&&observed.ammo==baseline.ammo&&observed.totalDamage==baseline.totalDamage},{"observation",observed.toJson()}});
        }result.push_back({{"gpuHudNormalization",checks}});}
        auto masked=images;auto& scoreboard=masked[0];
        int x=int(scoreboard.width*.520),y=int(scoreboard.height*.637),w=int(scoreboard.width*.142),h=int(scoreboard.height*.126);
        for(int row=y;row<std::min(y+h,scoreboard.height);++row)for(int column=x;column<std::min(x+w,scoreboard.width);++column){auto pixel=scoreboard.bgra.data()+(size_t(row)*scoreboard.width+column)*4;pixel[0]=pixel[1]=pixel[2]=0;pixel[3]=255;}
        std::fill(masked[5].bgra.begin(),masked[5].bgra.end(),0);
        HudDetector occlusion;occlusion.process(images,0);occlusion.process(images,.2);occlusion.process(images,.4);
        auto hidden=occlusion.process(masked,.6),later=occlusion.process(masked,3),restored=occlusion.process(images,3.2);
        result.push_back({{"damageHudOcclusion",{{"passed",hidden.active&&later.active&&!hidden.totalDamage&&!later.totalDamage&&restored.damageDelta==0},
            {"hidden",hidden.toJson()},{"later",later.toJson()},{"restored",restored.toJson()}}}});
        json debug=json::array();auto number=HudDigits::read(cropCpu(images[0],{.520,.637,.142,.126}),5,false,&debug);result.push_back({{"scoreGlyphs",debug},{"counter",number?json(*number):json(nullptr)}});
    }catch(...){sws_freeContext(scale);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&decoder);avformat_close_input(&input);throw;}
    sws_freeContext(scale);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&decoder);avformat_close_input(&input);return result;
}
inline void analyzeFile(const std::filesystem::path& file,const std::filesystem::path& output,double from,double duration,const Rules& config) {
    AVFormatContext* input=nullptr;AVCodecContext* decoder=nullptr;AVFrame* frame=av_frame_alloc();AVPacket* packet=av_packet_alloc();SwsContext* scale=nullptr;
    try{
        ffcheck(avformat_open_input(&input,pathUtf8(file).c_str(),nullptr,nullptr),"open reference recording");ffcheck(avformat_find_stream_info(input,nullptr),"inspect reference recording");
        int stream=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);ffcheck(stream,"find reference video");
        auto source=input->streams[stream];auto codec=avcodec_find_decoder(source->codecpar->codec_id);decoder=avcodec_alloc_context3(codec);
        ffcheck(avcodec_parameters_to_context(decoder,source->codecpar),"configure reference decoder");ffcheck(avcodec_open2(decoder,codec,nullptr),"open reference decoder");
        if(from>0){ffcheck(av_seek_frame(input,stream,static_cast<int64_t>(from/av_q2d(source->time_base)),AVSEEK_FLAG_BACKWARD),"seek reference");avcodec_flush_buffers(decoder);}
        HudDetector detector;ObservationRules flow(config);auto& rules=flow.rules();std::ofstream observations(output);if(!observations)throw std::runtime_error("无法写入离线诊断文件");double next=from,last=from;unsigned count=0;
        auto inspect=[&]{
            double time=frame->best_effort_timestamp*av_q2d(source->time_base);if(time<next || time>from+duration)return;
            auto image=frameToCpu(frame,scale);std::array<CpuImage,8> regions;for(size_t i=0;i<regions.size();++i)regions[i]=cropCpu(image,Gpu::regions[i]);
            auto observation=detector.process(regions,time);observations<<observation.toJson().dump()<<'\n';
            if(flow.process(observation))detector.reset();last=time;next=time+.1;++count;
        };
        while(av_read_frame(input,packet)>=0){
            if(packet->stream_index==stream){ffcheck(avcodec_send_packet(decoder,packet),"decode reference packet");
                while(avcodec_receive_frame(decoder,frame)>=0){inspect();if(frame->best_effort_timestamp*av_q2d(source->time_base)>from+duration)goto complete;}}
            av_packet_unref(packet);
        }
        avcodec_send_packet(decoder,nullptr);while(avcodec_receive_frame(decoder,frame)>=0)inspect();
complete:
        rules.boundary(last);json clips=json::array();for(auto& c:rules.takeReady())clips.push_back(clipJson(c));
        auto clipPath=output;clipPath+=L".clips.json";std::ofstream(clipPath)<<json{{"source",pathUtf8(file)},{"ruleVersion",config.version},{"observations",count},{"clips",clips}}.dump(2);
        std::cout<<json{{"type","analysis_complete"},{"observations",count},{"clips",clips},{"output",pathUtf8(output)}}.dump()<<'\n';
    }catch(...){sws_freeContext(scale);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&decoder);avformat_close_input(&input);throw;}
    sws_freeContext(scale);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&decoder);avformat_close_input(&input);
}
}
