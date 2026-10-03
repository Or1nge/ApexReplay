#pragma once
#include "common.hpp"
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}
namespace apex {
// Operates on 48 kHz stereo float samples before microphone gain and AAC encoding.
class MicDenoiser {
    AVFilterGraph* graph_=nullptr;
    AVFilterContext* input_=nullptr;
    AVFilterContext* output_=nullptr;
    int64_t nextInput_=AV_NOPTS_VALUE;
    void initialize(){
        graph_=avfilter_graph_alloc();if(!graph_)throw std::bad_alloc();graph_->nb_threads=1;
        try{
            AVFilterContext* denoise=nullptr;AVFilterContext* format=nullptr;
            ffcheck(avfilter_graph_create_filter(&input_,avfilter_get_by_name("abuffer"),"mic_input",
                "time_base=1/48000:sample_rate=48000:sample_fmt=fltp:channel_layout=stereo",nullptr,graph_),"create microphone filter input");
            ffcheck(avfilter_graph_create_filter(&denoise,avfilter_get_by_name("afftdn"),"mic_denoise",
                "nr=18:nf=-45:tn=1:fo=1.5",nullptr,graph_),"create microphone denoiser");
            ffcheck(avfilter_graph_create_filter(&format,avfilter_get_by_name("aformat"),"mic_format",
                "sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo",nullptr,graph_),"configure denoiser format");
            ffcheck(avfilter_graph_create_filter(&output_,avfilter_get_by_name("abuffersink"),"mic_output",nullptr,nullptr,graph_),"create microphone filter output");
            ffcheck(avfilter_link(input_,0,denoise,0),"link microphone denoiser");
            ffcheck(avfilter_link(denoise,0,format,0),"link microphone format");
            ffcheck(avfilter_link(format,0,output_,0),"link microphone output");
            ffcheck(avfilter_graph_config(graph_,nullptr),"initialize microphone denoiser");
        }catch(...){reset();throw;}
    }
    template<class Sink> void drain(Sink&& sink){
        AVFrame* frame=av_frame_alloc();if(!frame)throw std::bad_alloc();
        try{for(;;){int status=av_buffersink_get_frame(output_,frame);
            if(status==AVERROR(EAGAIN)||status==AVERROR_EOF)break;
            ffcheck(status,"read denoised microphone audio");
            if(frame->format!=AV_SAMPLE_FMT_FLTP||frame->ch_layout.nb_channels!=2||frame->pts==AV_NOPTS_VALUE)throw std::runtime_error("Unexpected microphone denoiser output");
            sink(reinterpret_cast<float*>(frame->extended_data[0]),reinterpret_cast<float*>(frame->extended_data[1]),frame->nb_samples,frame->pts);
            av_frame_unref(frame);
        }}catch(...){av_frame_free(&frame);throw;}av_frame_free(&frame);
    }
public:
    ~MicDenoiser(){reset();}
    void reset(){avfilter_graph_free(&graph_);input_=output_=nullptr;nextInput_=AV_NOPTS_VALUE;}
    template<class Sink> void flush(Sink&& sink){if(!graph_)return;ffcheck(av_buffersrc_add_frame_flags(input_,nullptr,0),"flush microphone denoiser");drain(sink);reset();}
    template<class Sink> void process(float* left,float* right,int count,int64_t pts,bool enabled,Sink&& sink){
        if(count<=0)return;
        if(!enabled){flush(sink);sink(left,right,count,pts);return;}
        // A device clock gap starts a fresh noise history without joining unrelated audio.
        if(graph_&&nextInput_!=AV_NOPTS_VALUE&&std::abs(pts-nextInput_)>960)flush(sink);
        if(!graph_)initialize();
        AVFrame* frame=av_frame_alloc();if(!frame)throw std::bad_alloc();
        try{
            frame->format=AV_SAMPLE_FMT_FLTP;frame->sample_rate=48000;frame->nb_samples=count;frame->pts=pts;av_channel_layout_default(&frame->ch_layout,2);
            ffcheck(av_frame_get_buffer(frame,0),"allocate microphone denoiser input");
            memcpy(frame->extended_data[0],left,size_t(count)*sizeof(float));memcpy(frame->extended_data[1],right,size_t(count)*sizeof(float));
            ffcheck(av_buffersrc_add_frame_flags(input_,frame,AV_BUFFERSRC_FLAG_KEEP_REF),"feed microphone denoiser");nextInput_=pts+count;
            drain(sink);
        }catch(...){av_frame_free(&frame);throw;}av_frame_free(&frame);
    }
};
}
