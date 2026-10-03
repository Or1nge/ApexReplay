#pragma once
#include "common.hpp"
#include "memory_policy.hpp"
namespace apex {
struct Packet {
    AVPacket* data=nullptr; AVRational timebase{}; int stream=0; double time=0;
    static inline std::atomic<uint64_t> liveBytes=0;
    Packet(const AVPacket* p,int s,AVRational tb):data(av_packet_clone(p)),timebase(tb),stream(s) {
        if(!data)throw std::bad_alloc();time=(data->pts==AV_NOPTS_VALUE?data->dts:data->pts)*av_q2d(tb);liveBytes+=data->size;
    }
    ~Packet(){liveBytes-=data->size;av_packet_free(&data);}
};
using PacketRef=std::shared_ptr<Packet>;
class PacketRing {
    mutable std::mutex mutex_;std::deque<PacketRef> packets_;uint64_t bytes_=0;
    double latest_=0,latestVideo_=0,videoSegmentStart_=0,maxSeconds_=1800,pinStart_=-1,lastBudgetRefresh_=-100;bool hasKey_=false;
    double memoryPercent_=60;uint64_t budget_=8ull<<30,idleBytes_=0;
    std::deque<double> keys_;
    void dropBefore(double time) {
        while(!packets_.empty()&&packets_.front()->time<time){bytes_-=packets_.front()->data->size;packets_.pop_front();}
        while(keys_.size()>1&&keys_[1]<=time)keys_.pop_front();
    }
    void refreshBudget(){MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);if(!GlobalMemoryStatusEx(&memory))throw std::runtime_error("无法读取系统可用内存");
        idleBytes_=std::min(memory.ullTotalPhys,memory.ullAvailPhys+Packet::liveBytes.load());budget_=replayMemoryBudget(memory.ullAvailPhys,memory.ullTotalPhys,Packet::liveBytes,memoryPercent_);lastBudgetRefresh_=qpcSeconds();}
public:
    PacketRing(){refreshBudget();}
    void configure(double minutes,double percent){std::lock_guard lock(mutex_);maxSeconds_=std::clamp(minutes,1.0,120.0)*60;memoryPercent_=std::clamp(percent,10.0,90.0);refreshBudget();}
    void pin(std::optional<double> start){std::lock_guard lock(mutex_);pinStart_=start.value_or(-1);}
    void push(PacketRef p) {
        std::lock_guard lock(mutex_);
        if(qpcSeconds()-lastBudgetRefresh_>=2)refreshBudget();
        if(p->stream==0 && (p->data->flags&AV_PKT_FLAG_KEY)){hasKey_=true;keys_.push_back(p->time);}
        if(!hasKey_)return;
        latest_=std::max(latest_,p->time);bytes_+=p->data->size;packets_.push_back(std::move(p));
        if(packets_.back()->stream==0){if(packets_.back()->time-latestVideo_>.5)videoSegmentStart_=packets_.back()->time;latestVideo_=std::max(latestVideo_,packets_.back()->time);}
        double target=latest_-maxSeconds_;if(pinStart_>=0)target=std::min(target,pinStart_);
        while(keys_.size()>1&&keys_[1]<=target)dropBefore(keys_[1]);
        if(Packet::liveBytes>budget_){while(keys_.size()>1&&Packet::liveBytes>budget_*.9){
            double next=keys_[1];if(pinStart_>=0&&next>pinStart_)break;dropBefore(next);}}
        if(Packet::liveBytes>budget_)throw std::runtime_error("缓存及待导出素材达到当前内存上限，已暂停采集。请重试失败的保存或降低缓存设置。");
    }
    std::vector<PacketRef> snapshot(double start,double end,double& actualStart)const {
        std::lock_guard lock(mutex_);actualStart=-1;
        double oldest=keys_.empty()?-1:keys_.front();for(double key:keys_)if(key<=start)actualStart=key;else break;
        if(actualStart<0)actualStart=oldest;
        if(actualStart<0 || actualStart>=end)throw std::runtime_error("回放缓存中尚无可解码片段");
        std::vector<PacketRef> result;
        for(const auto& p:packets_)if(p->time+1e-6>=actualStart && p->time<=end)result.push_back(p);
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){
            double ta=a->data->dts*av_q2d(a->timebase),tb=b->data->dts*av_q2d(b->timebase);
            return ta==tb?a->stream<b->stream:ta<tb;
        });
        return result;
    }
    uint64_t bytes()const{std::lock_guard lock(mutex_);return bytes_;}
    double duration()const{std::lock_guard lock(mutex_);return packets_.empty()?0:latest_-packets_.front()->time;}
    double latest()const{std::lock_guard lock(mutex_);return latest_;}
    double latestVideo()const{std::lock_guard lock(mutex_);return latestVideo_;}
    double videoSegmentStart()const{std::lock_guard lock(mutex_);return videoSegmentStart_;}
    uint64_t budget()const{std::lock_guard lock(mutex_);return budget_;}
    uint64_t idleBytes()const{std::lock_guard lock(mutex_);return idleBytes_;}
};
class Encoder {
protected:
    AVCodecContext* codec_=nullptr;int stream_;std::function<void(PacketRef)> sink_;
    void receive() {
        AVPacket* p=av_packet_alloc();
        if(!p)throw std::bad_alloc();
        try {for(;;){int ret=avcodec_receive_packet(codec_,p);if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF)break;ffcheck(ret,"receive encoded packet");sink_(std::make_shared<Packet>(p,stream_,codec_->time_base));av_packet_unref(p);}}
        catch(...){av_packet_free(&p);throw;}av_packet_free(&p);
    }
public:
    Encoder(int stream,std::function<void(PacketRef)> sink):stream_(stream),sink_(std::move(sink)){}
    virtual ~Encoder(){avcodec_free_context(&codec_);}
    void send(AVFrame* frame){ffcheck(avcodec_send_frame(codec_,frame),"send encoding frame");receive();}
    void flush(){if(codec_){int r=avcodec_send_frame(codec_,nullptr);if(r>=0)receive();}}
    AVCodecParameters* parameters()const {
        auto p=avcodec_parameters_alloc();ffcheck(avcodec_parameters_from_context(p,codec_),"copy codec parameters");return p;
    }
    AVRational timebase()const{return codec_->time_base;}
};
class VideoEncoder:public Encoder {
    AVBufferRef* device_=nullptr;AVBufferRef* frames_=nullptr;
    int64_t lastKeySecond_=-1;
    std::string preset_;bool splitEncoding_=false;
public:
    VideoEncoder(ID3D11Device* device,std::function<void(PacketRef)> sink,const Settings& settings={},int width=1920,int height=1080):Encoder(0,std::move(sink)) {
        preset_=settings.videoPreset;splitEncoding_=settings.videoCodec=="hevc"&&(width>=3840||height>=2160);
        device_=av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);if(!device_)throw std::bad_alloc();
        auto hw=static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device_->data)->hwctx);
        hw->device=device;device->AddRef();ffcheck(av_hwdevice_ctx_init(device_),"initialize D3D11 encoding device");
        frames_=av_hwframe_ctx_alloc(device_);if(!frames_)throw std::bad_alloc();
        auto f=reinterpret_cast<AVHWFramesContext*>(frames_->data);
        f->format=AV_PIX_FMT_D3D11;f->sw_format=AV_PIX_FMT_NV12;f->width=width;f->height=height;f->initial_pool_size=0;
        static_cast<AVD3D11VAFramesContext*>(f->hwctx)->BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ffcheck(av_hwframe_ctx_init(frames_),"initialize GPU frame pool");
        auto enc=avcodec_find_encoder_by_name(settings.videoCodec=="h264"?"h264_nvenc":"hevc_nvenc");if(!enc)throw std::runtime_error("FFmpeg 不包含所选 NVENC 编码器");
        codec_=avcodec_alloc_context3(enc);
        codec_->width=width;codec_->height=height;codec_->time_base={1,60};codec_->framerate={60,1};
        codec_->pix_fmt=AV_PIX_FMT_D3D11;codec_->hw_frames_ctx=av_buffer_ref(frames_);
        codec_->bit_rate=int64_t(settings.videoBitrateMbps*1000000);codec_->rc_max_rate=codec_->bit_rate*3/2;codec_->rc_buffer_size=codec_->bit_rate*2;
        codec_->gop_size=60;codec_->max_b_frames=0;codec_->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
        codec_->color_primaries=AVCOL_PRI_BT709;codec_->color_trc=AVCOL_TRC_BT709;codec_->colorspace=AVCOL_SPC_BT709;codec_->color_range=AVCOL_RANGE_MPEG;
        AVDictionary* opts=nullptr;for(auto [key,val]:{std::pair{"tune","hq"},{"rc","vbr"},{"spatial-aq","1"},{"temporal-aq","1"},{"forced-idr","1"},{"rc-lookahead","0"},{"zerolatency","1"}})av_dict_set(&opts,key,val,0);
        av_dict_set(&opts,"preset",settings.videoPreset.c_str(),0);av_dict_set(&opts,"multipass","disabled",0);
        // HQ presets do not automatically share one stream across available NVENC engines.
        if(splitEncoding_)av_dict_set(&opts,"split_encode_mode","forced",0);
        int ret=avcodec_open2(codec_,enc,&opts);av_dict_free(&opts);ffcheck(ret,"open selected NVENC video encoder");
    }
    ~VideoEncoder(){av_buffer_unref(&frames_);av_buffer_unref(&device_);}
    double targetBitrateMbps()const{return codec_->bit_rate/1000000.;}
    const std::string& preset()const{return preset_;}
    bool splitEncodingConfigured()const{return splitEncoding_;}
    AVFrame* frame(int64_t pts) {
        AVFrame* f=av_frame_alloc();if(!f)throw std::bad_alloc();
        int r=av_hwframe_get_buffer(frames_,f,0);if(r<0){av_frame_free(&f);ffcheck(r,"get GPU encoding frame");}
        f->pts=pts;if(pts/60>lastKeySecond_){f->pict_type=AV_PICTURE_TYPE_I;lastKeySecond_=pts/60;}return f;
    }
};
class AudioEncoder:public Encoder {
    AVAudioFifo* fifo_=nullptr;int64_t nextPts_=0;bool started_=false;
    void drainFifo(){
        while(av_audio_fifo_size(fifo_)>=codec_->frame_size){AVFrame* f=av_frame_alloc();if(!f)throw std::bad_alloc();
            try{f->format=codec_->sample_fmt;f->sample_rate=48000;f->nb_samples=codec_->frame_size;av_channel_layout_copy(&f->ch_layout,&codec_->ch_layout);
                ffcheck(av_frame_get_buffer(f,0),"allocate audio frame");av_audio_fifo_read(fifo_,reinterpret_cast<void**>(f->data),f->nb_samples);f->pts=nextPts_;nextPts_+=f->nb_samples;send(f);}
            catch(...){av_frame_free(&f);throw;}av_frame_free(&f);}
    }
public:
    AudioEncoder(int stream,std::function<void(PacketRef)> sink):Encoder(stream,std::move(sink)) {
        auto enc=avcodec_find_encoder(AV_CODEC_ID_AAC);codec_=avcodec_alloc_context3(enc);
        codec_->sample_rate=48000;av_channel_layout_default(&codec_->ch_layout,2);
        codec_->sample_fmt=AV_SAMPLE_FMT_FLTP;codec_->time_base={1,48000};codec_->bit_rate=256000;codec_->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
        ffcheck(avcodec_open2(codec_,enc,nullptr),"open AAC encoder");fifo_=av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,2,4096);if(!fifo_)throw std::bad_alloc();
    }
    ~AudioEncoder(){av_audio_fifo_free(fifo_);}
    void append(float* left,float* right,int count,int64_t firstPts) {
        if(count<=0)return;
        if(!started_){nextPts_=std::max<int64_t>(0,firstPts);started_=true;}
        int64_t expected=nextPts_+av_audio_fifo_size(fifo_);
        int64_t difference=firstPts-expected;
        if(difference>240) {
            if(difference>48000*60)throw std::runtime_error("音频设备时钟跳跃超过一分钟，请重新启动采集");
            std::array<float,2048> silence{};void* p[]={silence.data(),silence.data()};
            while(difference>0){int n=static_cast<int>(std::min<int64_t>(difference,silence.size()));ffcheck(av_audio_fifo_write(fifo_,p,n),"fill audio clock gap");difference-=n;drainFifo();}
        } else if(difference<-240) {
            int skip=static_cast<int>(std::min<int64_t>(count,-difference));left+=skip;right+=skip;count-=skip;
        }
        void* data[]={left,right};if(count>0)ffcheck(av_audio_fifo_write(fifo_,data,count),"append audio FIFO");
        drainFifo();
    }
    void finish() {
        if(av_audio_fifo_size(fifo_)>0) {int n=codec_->frame_size-av_audio_fifo_size(fifo_);std::vector<float> z(n);append(z.data(),z.data(),n,nextPts_+av_audio_fifo_size(fifo_));}
        flush();
    }
};
struct CodecSet {
    std::array<AVCodecParameters*,4> parameters{};std::array<AVRational,4> timebases{};
    ~CodecSet(){for(auto& p:parameters)avcodec_parameters_free(&p);}
};
struct ExportJob {ClipPlan clip;double actualStart=0;std::vector<PacketRef> packets;std::shared_ptr<CodecSet> codecs;std::filesystem::path file;};
class ExportQueue {
    std::mutex mutex_;std::condition_variable_any cv_;std::deque<ExportJob> queued_,failed_;
    std::jthread thread_;std::function<void(json)> notify_;std::atomic<bool> busy_=false;
    static void write(const ExportJob& job) {
        auto partial=job.file;partial+=L".partial.mp4";
        AVFormatContext* out=nullptr;ffcheck(avformat_alloc_output_context2(&out,nullptr,"mp4",pathUtf8(partial).c_str()),"create MP4 muxer");
        try {
            constexpr const char* names[]={"Video","Apex","Microphone","Other desktop"};
            for(int i=0;i<4;++i){
                auto s=avformat_new_stream(out,nullptr);if(!s)throw std::bad_alloc();
                ffcheck(avcodec_parameters_copy(s->codecpar,job.codecs->parameters[i]),"copy output stream");
                s->time_base=job.codecs->timebases[i];s->codecpar->codec_tag=i==0?(s->codecpar->codec_id==AV_CODEC_ID_HEVC?MKTAG('h','v','c','1'):MKTAG('a','v','c','1')):0;
                av_dict_set(&s->metadata,"title",names[i],0);av_dict_set(&s->metadata,"handler_name",names[i],0);
                s->disposition=i==1?AV_DISPOSITION_DEFAULT:0;
            }
            ffcheck(avio_open(&out->pb,pathUtf8(partial).c_str(),AVIO_FLAG_WRITE),"open output directory");
            AVDictionary* opts=nullptr;av_dict_set(&opts,"movflags","+faststart",0);int h=avformat_write_header(out,&opts);av_dict_free(&opts);ffcheck(h,"write MP4 header");
            std::array<bool,4> present{};
            for(const auto& packet:job.packets){
                AVPacket* p=av_packet_clone(packet->data);if(!p)throw std::bad_alloc();
                int i=packet->stream;p->stream_index=i;
                av_packet_rescale_ts(p,packet->timebase,out->streams[i]->time_base);
                auto offset=av_rescale_q(static_cast<int64_t>(std::llround(job.actualStart*1000000)),AVRational{1,1000000},out->streams[i]->time_base);
                p->pts-=offset;p->dts-=offset;
                if(p->pts>=0 && p->dts>=0){int ret=av_interleaved_write_frame(out,p);av_packet_free(&p);ffcheck(ret,"write clip packet");present[i]=true;}else av_packet_free(&p);
            }
            for(bool yes:present)if(!yes)throw std::runtime_error("片段缺少视频或音轨，暂不发布为正式文件");
            ffcheck(av_write_trailer(out),"finalize MP4");ffcheck(avio_closep(&out->pb),"close MP4");avformat_free_context(out);out=nullptr;
            AVFormatContext* check=nullptr;ffcheck(avformat_open_input(&check,pathUtf8(partial).c_str(),nullptr,nullptr),"validate MP4");
            int ret=avformat_find_stream_info(check,nullptr);bool valid=ret>=0 && check->nb_streams==4 && check->streams[0]->codecpar->codec_id==job.codecs->parameters[0]->codec_id&&check->streams[0]->codecpar->width==job.codecs->parameters[0]->width&&check->streams[0]->codecpar->height==job.codecs->parameters[0]->height;
            avformat_close_input(&check);if(!valid)throw std::runtime_error("导出的 MP4 未通过完整性检查");
            std::filesystem::rename(partial,job.file);
        }catch(...){if(out){if(out->pb)avio_closep(&out->pb);avformat_free_context(out);}throw;}
    }
public:
    explicit ExportQueue(std::function<void(json)> notify):notify_(std::move(notify)){
        thread_=std::jthread([this](std::stop_token stop){
            for(;;){
                ExportJob job;
                {std::unique_lock lock(mutex_);cv_.wait(lock,stop,[&]{return !queued_.empty();});if(queued_.empty()){if(stop.stop_requested())break;continue;}job=std::move(queued_.front());queued_.pop_front();}
                busy_=true;
                try{write(job);notify_({{"type","saved"},{"path",pathUtf8(job.file)},{"clip",clipJson(job.clip)}});}
                catch(...){auto error=errorText();{std::lock_guard lock(mutex_);failed_.push_back(std::move(job));}notify_({{"type","export_failed"},{"message",error}});}
                busy_=false;
            }
        });
    }
    ~ExportQueue(){thread_.request_stop();cv_.notify_all();if(thread_.joinable())thread_.join();}
    void push(ExportJob job){{std::lock_guard lock(mutex_);queued_.push_back(std::move(job));}cv_.notify_one();}
    void retry(){{std::lock_guard lock(mutex_);while(!failed_.empty()){queued_.push_back(std::move(failed_.front()));failed_.pop_front();}}cv_.notify_one();}
    size_t failures(){std::lock_guard lock(mutex_);return failed_.size();}
    bool busy()const{return busy_;}
};
}
