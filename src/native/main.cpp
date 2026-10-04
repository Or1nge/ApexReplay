#include "engine.hpp"
#include "selftest.hpp"
#include <sddl.h>
using namespace apex;
class Pipe {
    HANDLE handle_=INVALID_HANDLE_VALUE,readEvent_=nullptr,writeEvent_=nullptr;std::mutex mutex_;std::string incoming_;
public:
    explicit Pipe(const std::wstring& name){
        auto full=L"\\\\.\\pipe\\"+name;
        if(!WaitNamedPipeW(full.c_str(),10000))throw std::runtime_error("无法连接界面控制管道");
        handle_=CreateFileW(full.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(handle_==INVALID_HANDLE_VALUE)throw std::runtime_error("无法打开本机控制管道");
        readEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);writeEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!readEvent_||!writeEvent_)throw std::runtime_error("无法创建控制管道事件");
    }
    ~Pipe(){if(handle_!=INVALID_HANDLE_VALUE){CancelIoEx(handle_,nullptr);CloseHandle(handle_);}if(readEvent_)CloseHandle(readEvent_);if(writeEvent_)CloseHandle(writeEvent_);}
    void send(json message)noexcept {
        try{std::lock_guard lock(mutex_);auto line=message.dump()+"\n";size_t offset=0;
            while(offset<line.size()){ResetEvent(writeEvent_);OVERLAPPED operation{};operation.hEvent=writeEvent_;DWORD written=0;
                BOOL done=WriteFile(handle_,line.data()+offset,static_cast<DWORD>(line.size()-offset),&written,&operation);
                if(!done&&(GetLastError()!=ERROR_IO_PENDING||!GetOverlappedResult(handle_,&operation,&written,TRUE)))return;
                if(!written)return;offset+=written;}}
        catch(...){}
    }
    std::optional<json> read(){
        for(;;){auto newline=incoming_.find('\n');if(newline!=std::string::npos){auto line=incoming_.substr(0,newline);incoming_.erase(0,newline+1);return json::parse(line);}
            if(incoming_.size()>65536)throw std::runtime_error("Control message too large");
            std::array<char,4096> bytes{};DWORD count=0;ResetEvent(readEvent_);OVERLAPPED operation{};operation.hEvent=readEvent_;
            BOOL done=ReadFile(handle_,bytes.data(),static_cast<DWORD>(bytes.size()),&count,&operation);
            if(!done&&(GetLastError()!=ERROR_IO_PENDING||!GetOverlappedResult(handle_,&operation,&count,TRUE)))return {};
            if(!count)return {};incoming_.append(bytes.data(),count);}
    }
};
int wmain(int argc,wchar_t** argv){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);av_log_set_level(AV_LOG_WARNING);
    try{
        auto executable=std::filesystem::path(argv[0]).parent_path();
        auto config=loadRules(executable/L"config"/L"rules.v1.json");
        HudDigits::load(executable/L"config"/L"hud.zh.v1.json");
        std::wstring mode=argc>1?argv[1]:L"";
        if(mode==L"--inspect-image"&&argc>2){std::cout<<inspectImage(argv[2],argc>3&&std::wstring(argv[3])==L"--gpu").dump(2)<<'\n';return 0;}
        if(mode==L"--test-audio"){playTestTone(argc>2?_wtof(argv[2]):880,argc>3?_wtof(argv[3]):10);return 0;}
        if(mode==L"--selftest"&&argc>2){std::cout<<captureSelfTest(argv[2]).dump(2)<<'\n';return 0;}
        if(mode==L"--session-selftest"&&argc>2){Settings testSettings;if(argc>3){std::ifstream input(argv[3]);json settings;input>>settings;testSettings=Settings::fromJson(settings);}std::cout<<sessionSelfTest(argv[2],config,testSettings).dump(2)<<'\n';return 0;}
        if(mode==L"--diagnose"){
            auto window=findApex();Gpu gpu;LocalOcr ocr;
            std::cout<<json{{"captureSupported",winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()},
                {"adapter",gpu.adapter()},{"hevcNvenc",avcodec_find_encoder_by_name("hevc_nvenc")!=nullptr},{"ocrLanguage",ocr.language()},
                {"microphones",microphones()},{"apexPid",window.pid},{"captureApi","Windows.Graphics.Capture"},
                {"replayMinutes",30},{"memoryPercent",60},{"memoryPolicy","min(duration, dynamic idle-memory allowance)"},{"hudVersion",HudDigits::version},{"gameMemoryAccess",false},{"injection",false}}.dump(2)<<'\n';return 0;
        }
        if(mode==L"--analyze"&&argc>=4){
            double from=argc>4?_wtof(argv[4]):0,duration=argc>5?_wtof(argv[5]):60;
            analyzeFile(argv[2],argv[3],from,duration,config);return 0;
        }
        if(mode!=L"--pipe"||argc<3){std::cout<<"ApexPerif.Worker --pipe NAME | --diagnose | --analyze INPUT OUTPUT.jsonl [START] [SECONDS]\n";return 0;}
        Pipe pipe(argv[2]);auto notify=[&](json j){pipe.send(std::move(j));};ExportQueue exports(notify);
        std::mutex settingsMutex;Settings settings;std::atomic<bool> armed=false,quit=false,stopAnnouncement=false,manualRequest=false;std::atomic<unsigned> revision=0;
        std::jthread monitor([&](std::stop_token stop){
            winrt::init_apartment(winrt::apartment_type::multi_threaded);std::unique_ptr<CaptureSession> session;unsigned applied=0;bool waitingAnnounced=false;
            while(!stop.stop_requested()&&!quit){
                try{
                    if(!armed){if(manualRequest.exchange(false))notify({{"type","manual_save"},{"state","unavailable"}});bool announce=stopAnnouncement.exchange(false);if(session||waitingAnnounced||announce){session.reset();notify({{"type","status"},{"state","stopped"}});}waitingAnnounced=false;std::this_thread::sleep_for(100ms);continue;}
                    auto window=findApex();
                    if(session&&(session->window()!=window.handle || session->failed())){bool failed=session->failed();session.reset();if(failed){armed=false;continue;}}
                    if(!window.handle){if(!waitingAnnounced){notify({{"type","status"},{"state","waiting"}});waitingAnnounced=true;}}
                    else if(!session){Settings copy;{std::lock_guard lock(settingsMutex);copy=settings;}
                        notify({{"type","status"},{"state","starting"}});session=std::make_unique<CaptureSession>(window,config,copy,exports,notify);applied=revision;waitingAnnounced=false;}
                    if(manualRequest.exchange(false)){if(session)session->requestManualSave();else notify({{"type","manual_save"},{"state","unavailable"}});}
                    if(session&&applied!=revision){Settings copy;{std::lock_guard lock(settingsMutex);copy=settings;}session->update(std::move(copy));applied=revision;}
                }catch(...){notify({{"type","fatal"},{"message",errorText()}});armed=false;session.reset();}
                std::this_thread::sleep_for(100ms);
            }
            session.reset();
        });
        notify({{"type","ready"},{"microphones",microphones()},{"replayMinutes",30},{"memoryPercent",60},{"codec","hevc"}});
        while(auto message=pipe.read()){
            auto command=message->value("command",std::string());
            if(command=="start"||command=="configure"){
                try{auto next=Settings::fromJson(message->at("settings"));if(next.outputDirectory.empty())throw std::runtime_error("请先选择输出目录");
                    auto directory=std::filesystem::path(wide(next.outputDirectory));if(!directory.is_absolute())throw std::runtime_error("输出目录必须为绝对路径");
                    {std::lock_guard lock(settingsMutex);settings=std::move(next);}++revision;if(command=="start")armed=true;
                }catch(...){notify({{"type","error"},{"message",errorText()}});}
            }else if(command=="stop"){notify({{"type","status"},{"state","stopping"}});stopAnnouncement=true;armed=false;}
            else if(command=="save_replay")manualRequest=true;
            else if(command=="retry")exports.retry();
            else if(command=="quit"){quit=true;break;}
        }
        quit=true;monitor.request_stop();monitor.join();notify({{"type","exiting"}});return 0;
    }catch(...){std::cerr<<errorText()<<'\n';return 1;}
}
