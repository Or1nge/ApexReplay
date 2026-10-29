#pragma once
#include "audio.hpp"
#include "voice_mix.hpp"
#include <avrt.h>
#include <cwctype>
namespace apex {
// Virtual microphone: the real microphone is copied unchanged into a virtual cable (VB-Cable "CABLE Input"), and a song
// is added on request or when an own result appears. Voice apps then use "CABLE Output" as their microphone.
struct VoiceSettings {
    bool enabled=false;std::string microphoneId,outputId,songPath,trigger="squad_wipe";double songGainDb=-12;
    static VoiceSettings fromJson(const json& j){
        VoiceSettings s;s.enabled=j.value("enabled",false);s.microphoneId=j.value("microphoneId",std::string());s.outputId=j.value("outputId",std::string());
        s.songPath=j.value("songPath",std::string());s.trigger=j.value("trigger",std::string("squad_wipe"));
        if(s.trigger!="off"&&s.trigger!="squad_wipe"&&s.trigger!="elimination"&&s.trigger!="result")throw std::runtime_error("Unsupported song trigger");
        s.songGainDb=j.value("songGainDb",-12.0);if(!std::isfinite(s.songGainDb))throw std::runtime_error("Nonfinite song volume");s.songGainDb=std::clamp(s.songGainDb,-40.0,0.0);
        return s;
    }
    bool triggers(const std::string& kind)const{
        if(trigger=="result")return kind=="knockdown"||kind=="elimination"||kind=="squad_wipe";
        if(trigger=="elimination")return kind=="elimination"||kind=="squad_wipe";
        return trigger=="squad_wipe"&&kind=="squad_wipe";
    }
};
// Local copies of PKEY_AudioEngine_DeviceFormat and PKEY_DeviceInterface_FriendlyName.
constexpr PROPERTYKEY deviceFormatKey{{0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}},0};
constexpr PROPERTYKEY interfaceNameKey{{0x026e516e,0xb814,0x414b,{0x83,0xcd,0x85,0x6d,0x6f,0xef,0x48,0x22}},2};
inline std::wstring deviceText(IMMDevice* device,const PROPERTYKEY& key){
    winrt::com_ptr<IPropertyStore> props;if(FAILED(device->OpenPropertyStore(STGM_READ,props.put())))return {};
    PROPVARIANT value{};std::wstring text;if(SUCCEEDED(props->GetValue(key,&value))&&value.vt==VT_LPWSTR)text=value.pwszVal;PropVariantClear(&value);return text;
}
inline json speakers() {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),enumerator.put_void()));
    winrt::com_ptr<IMMDeviceCollection> collection;winrt::check_hresult(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,collection.put()));
    UINT count=0;collection->GetCount(&count);json result=json::array();
    for(UINT i=0;i<count;++i){winrt::com_ptr<IMMDevice> device;collection->Item(i,device.put());LPWSTR id=nullptr;device->GetId(&id);auto name=deviceText(device.get(),PKEY_Device_FriendlyName);
        result.push_back({{"id",utf8(id?id:L"")},{"name",utf8(name.empty()?L"扬声器":name)}});CoTaskMemFree(id);}
    return result;
}
struct PcmLayout{UINT32 rate=48000;WORD channels=2;PcmFormat format=PcmFormat::Float32;WORD validBits=32;};
inline std::optional<PcmLayout> pcmLayout(const WAVEFORMATEX* f){
    bool floating=f->wFormatTag==WAVE_FORMAT_IEEE_FLOAT,integer=f->wFormatTag==WAVE_FORMAT_PCM;WORD valid=f->wBitsPerSample;
    if(f->wFormatTag==WAVE_FORMAT_EXTENSIBLE){auto x=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);floating=x->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;integer=x->SubFormat==KSDATAFORMAT_SUBTYPE_PCM;if(x->Samples.wValidBitsPerSample)valid=x->Samples.wValidBitsPerSample;}
    PcmLayout layout{f->nSamplesPerSec,f->nChannels,PcmFormat::Float32,valid};
    if(floating&&f->wBitsPerSample==32)return layout;
    if(!integer)return {};
    if(f->wBitsPerSample==16)layout.format=PcmFormat::Int16;else if(f->wBitsPerSample==24)layout.format=PcmFormat::Int24;else if(f->wBitsPerSample==32)layout.format=PcmFormat::Int32;else return {};
    return layout;
}
inline WAVEFORMATEXTENSIBLE waveFormat(const PcmLayout& layout){
    WAVEFORMATEXTENSIBLE x{};WORD bits=WORD(pcmBytes(layout.format)*8);
    x.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;x.Format.nChannels=layout.channels;x.Format.nSamplesPerSec=layout.rate;x.Format.wBitsPerSample=bits;
    x.Format.nBlockAlign=WORD(layout.channels*bits/8);x.Format.nAvgBytesPerSec=layout.rate*x.Format.nBlockAlign;x.Format.cbSize=sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
    x.Samples.wValidBitsPerSample=layout.validBits;x.dwChannelMask=layout.channels==1?SPEAKER_FRONT_CENTER:layout.channels==2?KSAUDIO_SPEAKER_STEREO:0;
    x.SubFormat=layout.format==PcmFormat::Float32?KSDATAFORMAT_SUBTYPE_IEEE_FLOAT:KSDATAFORMAT_SUBTYPE_PCM;return x;
}
// Decodes a song file into the cable's rate and channel count on its own thread, about two seconds ahead.
class SongDecoder {
    FrameRing ring_;std::atomic<bool> finished_=false;std::mutex errorMutex_;std::string error_;std::jthread thread_;
    void run(std::stop_token stop,const std::string& path,int rate,int channels){
        AVFormatContext* input=nullptr;AVCodecContext* decoder=nullptr;SwrContext* convert=nullptr;AVFrame* frame=av_frame_alloc();AVPacket* packet=av_packet_alloc();
        AVChannelLayout in{},out{};std::vector<float> block;
        auto deliver=[&](const uint8_t** data,int count){
            int capacity=swr_get_out_samples(convert,count);if(capacity<=0)return;block.resize(size_t(capacity)*channels);uint8_t* output[]={reinterpret_cast<uint8_t*>(block.data())};
            int got=swr_convert(convert,output,capacity,data,count);ffcheck(got,"convert song audio");
            size_t written=0;while(written<size_t(got)&&!stop.stop_requested()){written+=ring_.write(block.data()+written*channels,size_t(got)-written);if(written<size_t(got))std::this_thread::sleep_for(20ms);}
        };
        try{
            ffcheck(avformat_open_input(&input,path.c_str(),nullptr,nullptr),"open song");ffcheck(avformat_find_stream_info(input,nullptr),"inspect song");
            int index=av_find_best_stream(input,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);ffcheck(index,"find song audio");
            auto codec=avcodec_find_decoder(input->streams[index]->codecpar->codec_id);if(!codec)throw std::runtime_error("不支持这种歌曲格式");
            decoder=avcodec_alloc_context3(codec);ffcheck(avcodec_parameters_to_context(decoder,input->streams[index]->codecpar),"configure song decoder");ffcheck(avcodec_open2(decoder,codec,nullptr),"open song decoder");
            if(decoder->ch_layout.order==AV_CHANNEL_ORDER_UNSPEC)av_channel_layout_default(&in,decoder->ch_layout.nb_channels);else ffcheck(av_channel_layout_copy(&in,&decoder->ch_layout),"copy song channels");
            av_channel_layout_default(&out,channels);
            ffcheck(swr_alloc_set_opts2(&convert,&out,AV_SAMPLE_FMT_FLT,rate,&in,decoder->sample_fmt,decoder->sample_rate,0,nullptr),"configure song conversion");ffcheck(swr_init(convert),"initialize song conversion");
            auto receive=[&]{while(!stop.stop_requested()){int got=avcodec_receive_frame(decoder,frame);if(got==AVERROR(EAGAIN)||got==AVERROR_EOF)return;ffcheck(got,"decode song");
                deliver(const_cast<const uint8_t**>(frame->extended_data),frame->nb_samples);av_frame_unref(frame);}};
            while(!stop.stop_requested()&&av_read_frame(input,packet)>=0){
                // A damaged packet only skips a few milliseconds of the song.
                if(packet->stream_index==index){int sent=avcodec_send_packet(decoder,packet);if(sent<0&&sent!=AVERROR_INVALIDDATA)ffcheck(sent,"decode song");}
                av_packet_unref(packet);receive();
            }
            if(!stop.stop_requested()){avcodec_send_packet(decoder,nullptr);receive();deliver(nullptr,0);}
        }catch(...){std::lock_guard lock(errorMutex_);error_=errorText();}
        av_channel_layout_uninit(&in);av_channel_layout_uninit(&out);swr_free(&convert);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&decoder);avformat_close_input(&input);
        finished_=true;
    }
public:
    SongDecoder(std::string path,int rate,int channels):ring_(channels,size_t(rate)*2){thread_=std::jthread([this,path=std::move(path),rate,channels](std::stop_token stop){run(stop,path,rate,channels);});}
    size_t read(float* out,size_t frames){return ring_.read(out,frames);}
    // Only the reading thread may ask; the ring is drained before the song counts as finished.
    bool finished()const{return finished_&&ring_.available()==0;}
    std::string error(){std::lock_guard lock(errorMutex_);return error_;}
};
class VoiceRouter {
    std::function<void(json)> notify_;std::atomic<std::shared_ptr<const VoiceSettings>> settings_;
    std::atomic<int> songRequest_=0;   // 1 result trigger, 2 play now, 3 stop
    // Everything below belongs to the router thread.
    winrt::com_ptr<IAudioClient> mic_,out_;winrt::com_ptr<IAudioCaptureClient> capture_;winrt::com_ptr<IAudioRenderClient> render_;
    HANDLE event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);PcmLayout micLayout_,outLayout_;bool exclusive_=false;UINT32 bufferFrames_=0;
    SwrContext* resampler_=nullptr;std::unique_ptr<DriftFifo> fifo_;std::unique_ptr<SongDecoder> song_;SongGain songGain_{4800};bool stopping_=false;
    std::string micName_,outName_,songName_;double lastPacketEnd_=0;std::vector<float> micFloat_,resampled_,mapped_,mix_,songBlock_;
    std::jthread thread_;
    json state(const std::string& name,const std::string& message={}){
        json j{{"type","voice_status"},{"state",name},{"playing",song_!=nullptr&&!stopping_},{"song",songName_}};if(!message.empty())j["message"]=message;
        if(name=="running")j.update({{"microphone",micName_},{"output",outName_},{"exclusive",exclusive_},{"microphoneRate",micLayout_.rate},{"outputRate",outLayout_.rate},
            {"resampled",resampler_!=nullptr},{"latencyMs",fifo_?fifo_->baseline()*1000/outLayout_.rate:0},{"corrections",fifo_?fifo_->dropped+fifo_->repeated:0},{"underruns",fifo_?fifo_->underruns:0}});
        return j;
    }
    void cleanup(){
        if(mic_)mic_->Stop();if(out_)out_->Stop();capture_=nullptr;render_=nullptr;mic_=nullptr;out_=nullptr;
        swr_free(&resampler_);fifo_.reset();song_.reset();stopping_=false;exclusive_=false;lastPacketEnd_=0;
    }
    winrt::com_ptr<IMMDevice> findCable(IMMDeviceEnumerator* enumerator){
        winrt::com_ptr<IMMDeviceCollection> collection;winrt::check_hresult(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,collection.put()));
        UINT count=0;collection->GetCount(&count);winrt::com_ptr<IMMDevice> fallback;
        for(UINT i=0;i<count;++i){winrt::com_ptr<IMMDevice> device;collection->Item(i,device.put());auto name=deviceText(device.get(),PKEY_Device_FriendlyName);
            std::transform(name.begin(),name.end(),name.begin(),::towupper);
            if(name.find(L"CABLE INPUT")!=std::wstring::npos)return device;if(!fallback&&name.find(L"CABLE")!=std::wstring::npos)fallback=device;}
        if(!fallback)throw std::runtime_error("没有找到 VB-Cable 的 CABLE Input，请先安装 VB-Cable，或在“输出到”里选择虚拟线缆");
        return fallback;
    }
    // Exclusive mode keeps the samples away from the Windows mixer (no volume or format change) and out of the
    // process loopback that records the other-apps track. Formats the cable accepts are tried, its own format first.
    bool openExclusive(IMMDevice* device,const PcmLayout& preferred){
        std::vector<UINT32> rates;for(UINT32 rate:{preferred.rate,micLayout_.rate,48000u,44100u})if(std::find(rates.begin(),rates.end(),rate)==rates.end())rates.push_back(rate);
        std::vector<WORD> channels;for(WORD count:{preferred.channels,WORD(2),WORD(1)})if(std::find(channels.begin(),channels.end(),count)==channels.end())channels.push_back(count);
        std::vector<std::pair<PcmFormat,WORD>> formats{{PcmFormat::Float32,32},{PcmFormat::Int32,24},{PcmFormat::Int24,24},{PcmFormat::Int32,32},{PcmFormat::Int16,16}};
        winrt::com_ptr<IAudioClient> probe;winrt::check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,probe.put_void()));
        for(auto rate:rates)for(auto count:channels)for(auto [format,valid]:formats){
            PcmLayout layout{rate,count,format,valid};auto wave=waveFormat(layout);
            if(probe->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&wave.Format,nullptr)!=S_OK)continue;
            winrt::com_ptr<IAudioClient> client;winrt::check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,client.put_void()));
            REFERENCE_TIME period=0,minimum=0;winrt::check_hresult(client->GetDevicePeriod(&period,&minimum));
            HRESULT result=client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,period,period,&wave.Format,nullptr);
            if(result==AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED){UINT32 frames=0;winrt::check_hresult(client->GetBufferSize(&frames));period=REFERENCE_TIME(10000000.0*frames/rate+.5);
                client=nullptr;winrt::check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,client.put_void()));
                result=client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,period,period,&wave.Format,nullptr);}
            if(FAILED(result))return false;
            out_=client;outLayout_=layout;return true;
        }
        return false;
    }
    void initialize(const VoiceSettings& s){
        cleanup();
        winrt::com_ptr<IMMDeviceEnumerator> enumerator;winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),enumerator.put_void()));
        winrt::com_ptr<IMMDevice> output,microphone;
        if(s.outputId.empty())output=findCable(enumerator.get());else winrt::check_hresult(enumerator->GetDevice(wide(s.outputId).c_str(),output.put()));
        if(s.microphoneId.empty()){HRESULT r=enumerator->GetDefaultAudioEndpoint(eCapture,eCommunications,microphone.put());if(FAILED(r))winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eCapture,eConsole,microphone.put()));}
        else winrt::check_hresult(enumerator->GetDevice(wide(s.microphoneId).c_str(),microphone.put()));
        micName_=utf8(deviceText(microphone.get(),PKEY_Device_FriendlyName));outName_=utf8(deviceText(output.get(),PKEY_Device_FriendlyName));
        auto micInterface=deviceText(microphone.get(),interfaceNameKey);
        if(!micInterface.empty()&&micInterface==deviceText(output.get(),interfaceNameKey))throw std::runtime_error("麦克风选成了虚拟线缆本身（"+micName_+"），请在“声音 › 麦克风”里选择实际的麦克风");
        winrt::check_hresult(microphone->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,mic_.put_void()));
        WAVEFORMATEX* mix=nullptr;winrt::check_hresult(mic_->GetMixFormat(&mix));auto micLayout=pcmLayout(mix);
        HRESULT micResult=micLayout?mic_->Initialize(AUDCLNT_SHAREMODE_SHARED,0,2000000,0,mix,nullptr):E_FAIL;CoTaskMemFree(mix);
        if(!micLayout)throw std::runtime_error("麦克风采样格式不受支持");winrt::check_hresult(micResult);micLayout_=*micLayout;
        winrt::check_hresult(mic_->GetService(__uuidof(IAudioCaptureClient),capture_.put_void()));
        PcmLayout preferred=micLayout_;
        {winrt::com_ptr<IPropertyStore> props;PROPVARIANT value{};
            if(SUCCEEDED(output->OpenPropertyStore(STGM_READ,props.put()))&&SUCCEEDED(props->GetValue(deviceFormatKey,&value))&&value.vt==VT_BLOB&&value.blob.cbSize>=sizeof(WAVEFORMATEX))
                if(auto layout=pcmLayout(reinterpret_cast<WAVEFORMATEX*>(value.blob.pBlobData)))preferred=*layout;
            PropVariantClear(&value);}
        exclusive_=openExclusive(output.get(),preferred);
        if(!exclusive_){
            winrt::check_hresult(output->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,out_.put_void()));
            winrt::check_hresult(out_->GetMixFormat(&mix));auto outLayout=pcmLayout(mix);
            HRESULT outResult=outLayout?out_->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,200000,0,mix,nullptr):E_FAIL;CoTaskMemFree(mix);
            if(!outLayout)throw std::runtime_error("虚拟线缆采样格式不受支持");winrt::check_hresult(outResult);outLayout_=*outLayout;
        }
        winrt::check_hresult(out_->SetEventHandle(event_));winrt::check_hresult(out_->GetBufferSize(&bufferFrames_));
        winrt::check_hresult(out_->GetService(__uuidof(IAudioRenderClient),render_.put_void()));
        if(micLayout_.rate!=outLayout_.rate){
            AVChannelLayout layout{};av_channel_layout_default(&layout,micLayout_.channels);
            int r=swr_alloc_set_opts2(&resampler_,&layout,AV_SAMPLE_FMT_FLT,outLayout_.rate,&layout,AV_SAMPLE_FMT_FLT,micLayout_.rate,0,nullptr);av_channel_layout_uninit(&layout);
            ffcheck(r,"configure microphone resampling");ffcheck(swr_init(resampler_),"initialize microphone resampling");
        }
        size_t rate=outLayout_.rate,period=exclusive_?bufferFrames_:rate/100;
        fifo_=std::make_unique<DriftFifo>(outLayout_.channels,period+rate*15/1000,rate*5/1000,rate/10,rate/2,rate);
        BYTE* data=nullptr;winrt::check_hresult(render_->GetBuffer(bufferFrames_,&data));winrt::check_hresult(render_->ReleaseBuffer(bufferFrames_,AUDCLNT_BUFFERFLAGS_SILENT));
        winrt::check_hresult(mic_->Start());winrt::check_hresult(out_->Start());
    }
    void drainMicrophone(){
        UINT32 available=0;winrt::check_hresult(capture_->GetNextPacketSize(&available));
        while(available){
            BYTE* input=nullptr;UINT32 frames=0;DWORD flags=0;UINT64 position=0,qpc=0;winrt::check_hresult(capture_->GetBuffer(&input,&frames,&flags,&position,&qpc));
            size_t samples=size_t(frames)*micLayout_.channels;micFloat_.resize(samples);
            if(flags&AUDCLNT_BUFFERFLAGS_SILENT)std::fill(micFloat_.begin(),micFloat_.end(),0.f);else readPcm(input,samples,micFloat_.data(),micLayout_.format);
            winrt::check_hresult(capture_->ReleaseBuffer(frames));
            lastPacketEnd_=(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)||!qpc?qpcSeconds():double(qpc)/10000000+double(frames)/micLayout_.rate;
            const float* source=micFloat_.data();size_t count=frames;
            if(resampler_){int capacity=swr_get_out_samples(resampler_,int(frames));resampled_.resize(size_t(std::max(capacity,0))*micLayout_.channels);
                uint8_t* output[]={reinterpret_cast<uint8_t*>(resampled_.data())};const uint8_t* inputs[]={reinterpret_cast<const uint8_t*>(micFloat_.data())};
                int got=swr_convert(resampler_,output,capacity,inputs,int(frames));ffcheck(got,"resample microphone");source=resampled_.data();count=size_t(got);}
            mapped_.resize(count*outLayout_.channels);mapChannels(source,micLayout_.channels,mapped_.data(),outLayout_.channels,count);fifo_->push(mapped_.data(),count);
            winrt::check_hresult(capture_->GetNextPacketSize(&available));
        }
    }
    void startSong(const VoiceSettings& s){
        if(s.songPath.empty()){notify_(state("running","请先选择歌曲"));return;}
        song_=std::make_unique<SongDecoder>(s.songPath,int(outLayout_.rate),outLayout_.channels);stopping_=false;songGain_.jump(0);
        songName_=pathUtf8(std::filesystem::path(wide(s.songPath)).filename());notify_(state("running"));
    }
    void mixSong(const VoiceSettings& s,size_t frames){
        if(!song_)return;
        size_t channels=outLayout_.channels;songBlock_.assign(frames*channels,0.f);song_->read(songBlock_.data(),frames);
        songGain_.set(stopping_?0:std::pow(10.0,s.songGainDb/20));songGain_.apply(songBlock_.data(),frames,channels);mixInto(mix_.data(),songBlock_.data(),frames*channels);
        if(song_->finished()||(stopping_&&songGain_.silent())){auto error=song_->error();song_.reset();stopping_=false;notify_(state("running",error.empty()?"":"歌曲无法播放："+error));}
    }
    void step(const VoiceSettings& s){
        if(WaitForSingleObject(event_,500)!=WAIT_OBJECT_0)throw std::runtime_error("虚拟线缆没有响应");
        int request=songRequest_.exchange(0);
        if(request==3&&song_){stopping_=true;notify_(state("running"));}else if(request==2||(request==1&&!song_))startSong(s);
        drainMicrophone();
        UINT32 frames=bufferFrames_;if(!exclusive_){UINT32 padding=0;winrt::check_hresult(out_->GetCurrentPadding(&padding));frames-=padding;}
        if(!frames)return;
        mix_.resize(size_t(frames)*outLayout_.channels);
        double pending=lastPacketEnd_>0?std::clamp((qpcSeconds()-lastPacketEnd_)*outLayout_.rate,0.0,outLayout_.rate*.05):0;
        fifo_->pull(mix_.data(),frames,pending);mixSong(s,frames);
        BYTE* data=nullptr;winrt::check_hresult(render_->GetBuffer(frames,&data));writePcm(mix_.data(),mix_.size(),data,outLayout_.format);winrt::check_hresult(render_->ReleaseBuffer(frames,0));
    }
    void run(std::stop_token stop){
        winrt::init_apartment(winrt::apartment_type::multi_threaded);DWORD task=0;HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);
        std::shared_ptr<const VoiceSettings> applied;double retryAt=0,lastStatus=0,lastWarning=-100;std::string lastError;
        while(!stop.stop_requested()){
            auto s=settings_.load();
            if(s!=applied){
                bool devices=!applied||!s||s->enabled!=applied->enabled||s->microphoneId!=applied->microphoneId||s->outputId!=applied->outputId;applied=s;
                if(devices){bool wasRunning=out_!=nullptr;cleanup();retryAt=0;lastError.clear();if(applied&&!applied->enabled&&wasRunning)notify_(state("off"));}
            }
            if(!applied||!applied->enabled){if(songRequest_.exchange(0)==2)notify_(state("off","虚拟麦克风未开启"));std::this_thread::sleep_for(50ms);continue;}
            if(!out_){
                if(qpcSeconds()>=retryAt){
                    try{initialize(*applied);lastError.clear();notify_(state("running"));lastStatus=qpcSeconds();}
                    catch(...){std::string error=errorText();cleanup();retryAt=qpcSeconds()+2;
                        if(error!=lastError||qpcSeconds()-lastWarning>10){notify_(state("error",error));lastError=error;lastWarning=qpcSeconds();}}
                }
                if(!out_){if(songRequest_.exchange(0)==2)notify_(state("error",lastError.empty()?"虚拟麦克风未运行":lastError));std::this_thread::sleep_for(20ms);continue;}
            }
            try{step(*applied);if(qpcSeconds()-lastStatus>=2){notify_(state("running"));lastStatus=qpcSeconds();}}
            catch(...){std::string error=errorText();cleanup();retryAt=qpcSeconds()+2;notify_(state("error",error));lastError=error;lastWarning=qpcSeconds();}
        }
        cleanup();if(mmcss)AvRevertMmThreadCharacteristics(mmcss);
    }
public:
    explicit VoiceRouter(std::function<void(json)> notify):notify_(std::move(notify)){thread_=std::jthread([this](std::stop_token stop){run(stop);});}
    ~VoiceRouter(){thread_.request_stop();if(thread_.joinable())thread_.join();CloseHandle(event_);}
    void configure(VoiceSettings settings){settings_.store(std::make_shared<const VoiceSettings>(std::move(settings)));}
    void play(){songRequest_=2;}
    void stopSong(){songRequest_=3;}
    // Own results from the HUD; a song that is already playing keeps playing.
    void event(const std::string& kind){auto s=settings_.load();int none=0;if(s&&s->enabled&&s->triggers(kind))songRequest_.compare_exchange_strong(none,1);}
};
}
