#pragma once
#include "media.hpp"
#include "mic_denoiser.hpp"
#include "speech_leveler.hpp"
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl.h>
#include <ks.h>
#include <ksmedia.h>
#include <array>
namespace apex {
class ActivationHandler:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IActivateAudioInterfaceCompletionHandler,Microsoft::WRL::FtmBase> {
public:
    HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);HRESULT result=E_PENDING;winrt::com_ptr<IAudioClient> client;
    ~ActivationHandler(){CloseHandle(done);}
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation)override {
        winrt::com_ptr<IUnknown> unknown;HRESULT activate=E_FAIL;result=operation->GetActivateResult(&activate,unknown.put());
        if(SUCCEEDED(result))result=activate;if(SUCCEEDED(result))result=unknown->QueryInterface(client.put());
        SetEvent(done);return S_OK;
    }
};
inline winrt::com_ptr<IAudioClient> activateLoopback(DWORD pid,bool include) {
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};parameters.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId=pid;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode=include?PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE:PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT value{};value.vt=VT_BLOB;value.blob.cbSize=sizeof(parameters);value.blob.pBlobData=reinterpret_cast<BYTE*>(&parameters);
    auto handler=Microsoft::WRL::Make<ActivationHandler>();winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
    winrt::check_hresult(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,__uuidof(IAudioClient),&value,handler.Get(),operation.put()));
    if(WaitForSingleObject(handler->done,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Windows 进程音频接口初始化超时");
    winrt::check_hresult(handler->result);return handler->client;
}
inline json microphones() {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),enumerator.put_void()));
    winrt::com_ptr<IMMDeviceCollection> collection;winrt::check_hresult(enumerator->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,collection.put()));
    UINT count=0;collection->GetCount(&count);json result=json::array();
    for(UINT i=0;i<count;++i){winrt::com_ptr<IMMDevice> device;collection->Item(i,device.put());LPWSTR id=nullptr;device->GetId(&id);
        winrt::com_ptr<IPropertyStore> props;device->OpenPropertyStore(STGM_READ,props.put());PROPVARIANT name{};props->GetValue(PKEY_Device_FriendlyName,&name);
        result.push_back({{"id",utf8(id?id:L"")},{"name",utf8(name.vt==VT_LPWSTR?name.pwszVal:L"麦克风")}});
        PropVariantClear(&name);CoTaskMemFree(id);}
    return result;
}
class AudioPeakMeter {
    std::atomic<float> peak_=0;
public:
    void add(float peak){float previous=peak_.load();while(peak>previous&&!peak_.compare_exchange_weak(previous,peak)){} }
    float take(){return peak_.exchange(0);}
    void reset(){peak_=0;}
};
class AudioCapture {
    int stream_;DWORD pid_;std::string mic_;double epoch_;
    std::atomic<std::shared_ptr<std::string>> requestedMic_;
    AudioEncoder encoder_;SpeechLeveler leveler_;MicDenoiser denoiser_;std::atomic<bool> noiseSuppression_=true;
    std::function<double()> balance_;std::function<void(json)> notify_;
    std::jthread thread_;AudioPeakMeter meter_;std::atomic<bool> healthy_=false;
    winrt::com_ptr<IAudioClient> client_;winrt::com_ptr<IAudioCaptureClient> capture_;
    SwrContext* resampler_=nullptr;WAVEFORMATEX* format_=nullptr;
    int64_t expectedPts_=0;
    void cleanup() {
        if(client_)client_->Stop();capture_=nullptr;client_=nullptr;
        swr_free(&resampler_);if(format_)CoTaskMemFree(format_);format_=nullptr;healthy_=false;meter_.reset();
        denoiser_.reset();leveler_.reset();
    }
    void initialize() {
        cleanup();
        if(stream_!=2){
            client_=activateLoopback(pid_,stream_==1);
            format_=static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));*format_={WAVE_FORMAT_IEEE_FLOAT,2,48000,48000*8,8,32,0};
        }else{
            winrt::com_ptr<IMMDeviceEnumerator> enumerator;winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),enumerator.put_void()));
            winrt::com_ptr<IMMDevice> device;
            if(mic_.empty()){HRESULT r=enumerator->GetDefaultAudioEndpoint(eCapture,eCommunications,device.put());if(FAILED(r))winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eCapture,eConsole,device.put()));}
            else winrt::check_hresult(enumerator->GetDevice(wide(mic_).c_str(),device.put()));
            winrt::check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,client_.put_void()));
            winrt::check_hresult(client_->GetMixFormat(&format_));
        }
        DWORD flags=stream_==2?0:AUDCLNT_STREAMFLAGS_LOOPBACK;
        winrt::check_hresult(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,2000000,0,format_,nullptr));
        winrt::check_hresult(client_->GetService(__uuidof(IAudioCaptureClient),capture_.put_void()));
        bool floating=format_->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
        if(format_->wFormatTag==WAVE_FORMAT_EXTENSIBLE)floating=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format_)->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        AVSampleFormat input=floating?AV_SAMPLE_FMT_FLT:(format_->wBitsPerSample==16?AV_SAMPLE_FMT_S16:AV_SAMPLE_FMT_S32);
        if((floating && format_->wBitsPerSample!=32) || (!floating && format_->wBitsPerSample!=16 && format_->wBitsPerSample!=32))throw std::runtime_error("麦克风采样格式不受支持");
        AVChannelLayout in{},out{};av_channel_layout_default(&in,format_->nChannels);av_channel_layout_default(&out,2);
        ffcheck(swr_alloc_set_opts2(&resampler_,&out,AV_SAMPLE_FMT_FLTP,48000,&in,input,format_->nSamplesPerSec,0,nullptr),"configure audio resampling");
        av_channel_layout_uninit(&in);av_channel_layout_uninit(&out);ffcheck(swr_init(resampler_),"initialize audio resampling");
        winrt::check_hresult(client_->Start());healthy_=true;
    }
    void silenceUntil(double time) {
        int64_t target=std::max<int64_t>(0,static_cast<int64_t>(std::llround(time*48000)));
        while(expectedPts_+480<=target){std::array<float,480> zero{};encoder_.append(zero.data(),zero.data(),480,expectedPts_);expectedPts_+=480;}
    }
    void write(float* left,float* right,int count,int64_t pts){
        float peak=0;for(int i=0;i<count;++i)peak=std::max({peak,std::abs(left[i]),std::abs(right[i])});meter_.add(peak);
        encoder_.append(left,right,count,pts);expectedPts_=std::max(expectedPts_,pts+count);
    }
    // The microphone and the other-desktop track are balanced on their measured speaking level; the game track
    // is only peak limited. The leveler keeps the timestamps but holds about 0.3 s of audio.
    void encode(float* left,float* right,int count,int64_t pts){
        double bal=stream_==2?balance_()/2:stream_==3?-balance_()/2:0;
        leveler_.process(left,right,count,pts,bal,stream_!=1,[this](float* l,float* r,int n,int64_t t){write(l,r,n,t);});
    }
    void drainProcessing(){
        if(stream_==2)denoiser_.flush([this](auto l,auto r,int n,int64_t t){encode(l,r,n,t);});
        leveler_.flush([this](float* l,float* r,int n,int64_t t){write(l,r,n,t);});
    }
    void run(std::stop_token stop) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        double retryAt=0,lastData=qpcSeconds(),lastWarning=0;
        while(!stop.stop_requested()){
            if(stream_==2){auto requested=requestedMic_.load();if(requested && *requested!=mic_){
                try{drainProcessing();}catch(...){}mic_=*requested;cleanup();leveler_.restartLearning();retryAt=0;}}
            if(!client_){
                if(qpcSeconds()>=retryAt){
                    try{initialize();notify_({{"type","audio_status"},{"track",stream_},{"healthy",true}});lastData=qpcSeconds();}
                    catch(...){std::string error=errorText();cleanup();retryAt=qpcSeconds()+2;
                        if(qpcSeconds()-lastWarning>10){notify_({{"type","audio_status"},{"track",stream_},{"healthy",false},{"message",error}});lastWarning=qpcSeconds();}}
                }
                try{silenceUntil(qpcSeconds()-epoch_);}catch(...){notify_({{"type","fatal"},{"message",errorText()}});break;}
                std::this_thread::sleep_for(10ms);continue;
            }
            try{
                UINT32 available=0;winrt::check_hresult(capture_->GetNextPacketSize(&available));
                while(available){
                    BYTE* input=nullptr;UINT32 frames=0;DWORD flags=0;UINT64 position=0,qpc=0;
                    winrt::check_hresult(capture_->GetBuffer(&input,&frames,&flags,&position,&qpc));
                    double packetTime=double(qpc)/10000000-epoch_;
                    if((flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)||qpc==0)packetTime=qpcSeconds()-epoch_-double(frames)/format_->nSamplesPerSec;
                    int n=static_cast<int>(av_rescale_rnd(swr_get_delay(resampler_,format_->nSamplesPerSec)+frames,48000,format_->nSamplesPerSec,AV_ROUND_UP));
                    std::vector<float> left(n),right(n);uint8_t* output[]={reinterpret_cast<uint8_t*>(left.data()),reinterpret_cast<uint8_t*>(right.data())};
                    std::vector<uint8_t> zero;
                    if(flags&AUDCLNT_BUFFERFLAGS_SILENT){zero.resize(size_t(frames)*format_->nBlockAlign);input=zero.data();}
                    const uint8_t* buffers[]={input};
                    int converted=swr_convert(resampler_,output,n,buffers,frames);
                    HRESULT release=capture_->ReleaseBuffer(frames);ffcheck(converted,"resample captured audio");winrt::check_hresult(release);
                    int64_t pts=std::max<int64_t>(0,static_cast<int64_t>(std::llround(packetTime*48000)));
                    if(stream_==2)denoiser_.process(left.data(),right.data(),converted,pts,noiseSuppression_.load(),[this](auto l,auto r,int n,int64_t t){encode(l,r,n,t);});
                    else encode(left.data(),right.data(),converted,pts);
                    lastData=qpcSeconds();
                    winrt::check_hresult(capture_->GetNextPacketSize(&available));
                }
                // Process loopback may omit silent packets; generate silence on the same clock.
                if(qpcSeconds()-lastData>.1){drainProcessing();silenceUntil(qpcSeconds()-epoch_-.05);}
            }catch(...){std::string error=errorText();try{drainProcessing();}catch(...){}cleanup();retryAt=qpcSeconds()+2;notify_({{"type","audio_status"},{"track",stream_},{"healthy",false},{"message",error}});}
            std::this_thread::sleep_for(5ms);
        }
        try{drainProcessing();encoder_.finish();}catch(...){notify_({{"type","warning"},{"message",errorText()}});}cleanup();
    }
public:
    AudioCapture(int stream,DWORD pid,std::string mic,double epoch,std::function<void(PacketRef)> sink,
        std::function<double()> balance,std::function<void(json)> notify,bool noiseSuppression=true)
        :stream_(stream),pid_(pid),mic_(std::move(mic)),epoch_(epoch),encoder_(stream,std::move(sink)),noiseSuppression_(noiseSuppression),balance_(std::move(balance)),notify_(std::move(notify)){}
    ~AudioCapture(){stop();}
    void start(){thread_=std::jthread([this](std::stop_token stop){run(stop);});}
    void stop(){thread_.request_stop();if(thread_.joinable())thread_.join();}
    void microphone(std::string id){if(stream_==2)requestedMic_.store(std::make_shared<std::string>(std::move(id)));}
    void noiseSuppression(bool enabled){if(stream_==2)noiseSuppression_=enabled;}
    float level(){return meter_.take();}bool healthy()const{return healthy_;}
    SpeechLeveler::Status speech(){return leveler_.take();}
    AVCodecParameters* parameters()const{return encoder_.parameters();}
    AVRational timebase()const{return encoder_.timebase();}
};
}
