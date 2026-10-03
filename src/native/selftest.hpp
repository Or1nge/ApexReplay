#pragma once
#include "engine.hpp"
namespace apex {
inline void testRequire(bool value,const char* text){if(!value)throw std::runtime_error(text);}
inline void playTestTone(double frequency,double seconds){
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),enumerator.put_void()));
    winrt::com_ptr<IMMDevice> device;winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,device.put()));
    winrt::com_ptr<IAudioClient> client;winrt::check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,client.put_void()));
    WAVEFORMATEX format{WAVE_FORMAT_PCM,2,48000,48000*4,4,16,0};
    winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,2000000,0,&format,nullptr));
    winrt::com_ptr<IAudioRenderClient> renderer;winrt::check_hresult(client->GetService(__uuidof(IAudioRenderClient),renderer.put_void()));
    UINT32 size=0;client->GetBufferSize(&size);winrt::check_hresult(client->Start());double begin=qpcSeconds();int64_t phase=0;
    try{while(qpcSeconds()-begin<seconds){UINT32 padding=0;winrt::check_hresult(client->GetCurrentPadding(&padding));UINT32 n=size-padding;
        if(n){BYTE* buffer=nullptr;winrt::check_hresult(renderer->GetBuffer(n,&buffer));auto pcm=reinterpret_cast<int16_t*>(buffer);
            for(UINT32 i=0;i<n;++i,++phase){auto sample=static_cast<int16_t>(std::sin(phase*frequency*6.283185307179586/48000)*655);pcm[i*2]=pcm[i*2+1]=sample;}
            winrt::check_hresult(renderer->ReleaseBuffer(n,0));}std::this_thread::sleep_for(5ms);}}
    catch(...){client->Stop();throw;}client->Stop();
}
inline LRESULT CALLBACK testWindowProc(HWND w,UINT message,WPARAM p,LPARAM l){
    if(message==WM_TIMER){InvalidateRect(w,nullptr,FALSE);return 0;}
    if(message==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);
        auto brush=CreateSolidBrush(RGB(17,24,32));FillRect(dc,&r,brush);DeleteObject(brush);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(60,225,170));
        DrawTextW(dc,L"Apex回放 | Windows capture / HEVC / 3 audio tracks",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        int x=int(std::fmod(qpcSeconds()*150,std::max(1L,r.right-80)));RECT block{x,70,x+80,150};brush=CreateSolidBrush(RGB(40,190,150));FillRect(dc,&block,brush);DeleteObject(brush);
        EndPaint(w,&ps);return 0;}
    return DefWindowProcW(w,message,p,l);
}
inline json captureSelfTest(const std::filesystem::path& directory){
    std::filesystem::create_directories(directory);
    WNDCLASSW wc{};wc.lpfnWndProc=testWindowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ApexPerifCaptureTest";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));RegisterClassW(&wc);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"Apex回放 采集底座测试",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,960,540,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window)throw std::runtime_error("Unable to create capture test window");SetTimer(window,1,16,nullptr);
    json notifications=json::array();std::mutex notificationsMutex;unsigned saved=0;std::atomic<unsigned> failed=0;
    auto notify=[&](json j){std::lock_guard lock(notificationsMutex);if(j.value("type","")=="saved")++saved;if(j.value("type","")=="export_failed")++failed;notifications.push_back(std::move(j));};
    json report;
    try{
        PacketRing ring;Gpu gpu;VideoEncoder video(gpu.device(),[&](PacketRef p){ring.push(std::move(p));});
        auto codecs=std::make_shared<CodecSet>();codecs->parameters[0]=video.parameters();codecs->timebases[0]=video.timebase();
        double epoch=qpcSeconds();std::array<std::unique_ptr<AudioCapture>,3> audio;
        for(int i=0;i<3;++i){audio[i]=std::make_unique<AudioCapture>(i+1,GetCurrentProcessId(),"",epoch,[&](PacketRef p){ring.push(std::move(p));},[]{return 0.;},notify);codecs->parameters[i+1]=audio[i]->parameters();codecs->timebases[i+1]=audio[i]->timebase();audio[i]->start();}
        std::jthread tone([]{winrt::init_apartment(winrt::apartment_type::multi_threaded);try{playTestTone(440,7);}catch(...){std::cerr<<errorText()<<'\n';}});
        WindowCapture capture(window,gpu.device());unsigned captured=0,encoded=0;bool rendered=false;
        {
            ExportQueue exports(notify);bool first=false,second=false,retry=false;double snapshotDuration=0;
            auto queue=[&](double start,double end,const wchar_t* name){ExportJob job;job.clip={start,end,"selftest","test",{},false,false,0};job.codecs=codecs;job.packets=ring.snapshot(start,end,job.actualStart);job.file=directory/name;exports.push(std::move(job));};
            auto steadyBegin=std::chrono::steady_clock::now();
            for(int i=0;i<420;++i){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
                auto frame=capture.take();if(frame){gpu.render(WindowCapture::texture(frame).get(),windowHdr(window));rendered=true;++captured;}
                if(rendered){auto f=video.frame(i);try{gpu.convert(f);video.send(f);}catch(...){av_frame_free(&f);throw;}av_frame_free(&f);++encoded;}
                double now=qpcSeconds()-epoch;
                if(now>3&&!first){snapshotDuration=ring.duration();queue(.5,2.5,L"overlap-1.mp4");testRequire(ring.duration()>=snapshotDuration-.001,"Saving consumed replay cache");first=true;}
                if(now>5&&!second){queue(1.5,4.5,L"overlap-2.mp4");queue(.5,2.5,L"retry-target/retried.mp4");second=true;}
                if(failed.load()>0&&!retry){std::filesystem::create_directories(directory/L"retry-target");exports.retry();retry=true;}
                std::this_thread::sleep_until(steadyBegin+std::chrono::microseconds(int64_t(i+1)*1000000/60));
            }
            video.flush();for(auto& a:audio)a->stop();testRequire(captured>30&&encoded>350,"Window capture or encoder did not produce frames");
            report={{"captureFrames",captured},{"encodedFrames",encoded},{"cacheSeconds",ring.duration()},{"cacheBytes",ring.bytes()},{"snapshotDidNotConsumeCache",ring.duration()>snapshotDuration},{"adapter",gpu.adapter()}};
        }
        tone.join();testRequire(saved==3,"Expected two overlapping exports and a recovered failed export");report["savedFiles"]=saved;report["retryRecovered"]=failed.load()==1;report["notifications"]=notifications;
    }catch(...){DestroyWindow(window);throw;}
    DestroyWindow(window);UnregisterClassW(wc.lpszClassName,wc.hInstance);std::ofstream(directory/L"capture-report.json")<<report.dump(2);return report;
}
inline json sessionSelfTest(const std::filesystem::path& directory,Rules rules,Settings settings={}){
    std::filesystem::create_directories(directory);WNDCLASSW wc{};wc.lpfnWndProc=testWindowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ApexPerifSessionTest";RegisterClassW(&wc);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"Apex回放 实时管线测试",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,960,540,nullptr,nullptr,wc.hInstance,nullptr);if(!window)throw std::runtime_error("Test window failed");SetTimer(window,1,16,nullptr);
    std::mutex mutex;json messages=json::array();std::atomic<bool> fatal=false;std::atomic<unsigned> saved=0;
    auto notify=[&](json j){if(j.value("type","")=="fatal")fatal=true;if(j.value("type","")=="saved")++saved;std::lock_guard lock(mutex);messages.push_back(std::move(j));};
    unsigned frames=0;
    try{{ExportQueue exports(notify);settings.outputDirectory=pathUtf8(std::filesystem::absolute(directory));CaptureSession session({window,GetCurrentProcessId()},rules,settings,exports,notify);
        std::jthread tone([]{winrt::init_apartment(winrt::apartment_type::multi_threaded);try{playTestTone(440,3);}catch(...){std::cerr<<errorText()<<'\n';}});
        double begin=qpcSeconds();bool requested=false,resized=false,noiseOff=false,noiseOn=false;
        while(qpcSeconds()-begin<6){
            MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            if(qpcSeconds()-begin>2&&!noiseOff){settings.micNoiseSuppression=false;session.update(settings);noiseOff=true;}
            if(qpcSeconds()-begin>3&&!resized){SetWindowPos(window,nullptr,0,0,1280,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);resized=true;}
            if(qpcSeconds()-begin>4&&!noiseOn){settings.micNoiseSuppression=true;session.update(settings);noiseOn=true;}
            if(qpcSeconds()-begin>4&&!requested){session.exportForTest(1,3);requested=true;}
            std::this_thread::sleep_for(2ms);
        }
        session.stop();frames=session.framesForTest();testRequire(!fatal,"Realtime capture session failed");if(frames<=330){std::ofstream(directory/L"failure-report.json")<<json{{"frames",frames},{"messages",messages}}.dump(2);throw std::runtime_error("Realtime session fell substantially below 60 fps: "+std::to_string(frames)+" frames / 6 seconds");}}
        testRequire(saved==1,"Realtime session export did not complete");}
    catch(...){DestroyWindow(window);throw;}DestroyWindow(window);UnregisterClassW(wc.lpszClassName,wc.hInstance);
    unsigned meterUpdates=0;double gamePeak=0;for(const auto& message:messages)if(message.value("type","")=="audio_levels"){++meterUpdates;gamePeak=std::max(gamePeak,message.at("levels").at(0).get<double>());}
    testRequire(meterUpdates>=30,"Audio meter refresh too slow");testRequire(gamePeak>.001,"Audio meter did not detect the real test tone");
    for(const auto& entry:std::filesystem::recursive_directory_iterator(directory))if(entry.is_regular_file()&&entry.path().filename().wstring().ends_with(L".mp4.json"))throw std::runtime_error("Video export wrote an unwanted JSON sidecar");
    json report{{"frames",frames},{"seconds",6},{"meterUpdates",meterUpdates},{"gamePeak",gamePeak},{"messages",messages},{"saved",saved.load()},{"noVideoJsonSidecars",true}};std::ofstream(directory/L"session-report.json")<<report.dump(2);return report;
}
}
