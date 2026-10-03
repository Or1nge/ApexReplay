#pragma once
#include "common.hpp"
#include <array>
namespace apex {
class Gpu {
    winrt::com_ptr<ID3D11Device> device_;winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11VideoDevice> video_;winrt::com_ptr<ID3D11VideoContext> videoContext_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;winrt::com_ptr<ID3D11VideoProcessor> processor_;
    winrt::com_ptr<ID3D11Texture2D> bgra_;winrt::com_ptr<ID3D11RenderTargetView> target_;
    winrt::com_ptr<ID3D11Texture2D> analysisBgra_;winrt::com_ptr<ID3D11RenderTargetView> analysisTarget_;
    winrt::com_ptr<ID3D11VertexShader> vertex_;winrt::com_ptr<ID3D11PixelShader> pixel_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;winrt::com_ptr<ID3D11Buffer> constants_;
    std::array<winrt::com_ptr<ID3D11Texture2D>,8> staging_;
    int width_=1920,height_=1080;
    std::string adapter_;
public:
    static constexpr std::array<Region,8> regions{{
        {0.765,0.015,0.230,0.125}, // scoreboard / cumulative damage
        {0.290,0.635,0.440,0.235}, // own result prompts
        {0.760,0.865,0.240,0.135}, // current ammo and weapon
        {0.250,0.015,0.680,0.150}, // spectator / mode tabs
        {0.260,0.330,0.480,0.290}, // victory and death banners
        {0.420,0.275,0.250,0.290}, // visible hit feedback / floating damage
        {0.085,0.887,0.135,0.033}, // own name, never read from game memory
        {0.765,0.170,0.230,0.270}  // public kill feed, attribution only
    }};
    Gpu(std::pair<int,int> size={1920,1080}):width_(size.first),height_(size.second) {
        winrt::com_ptr<IDXGIFactory1> factory;winrt::check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1),factory.put_void()));
        winrt::com_ptr<IDXGIAdapter1> chosen;
        for(UINT i=0;;++i){winrt::com_ptr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,a.put())==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de && !(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){chosen=a;adapter_=utf8(d.Description);break;}}
        if(!chosen)throw std::runtime_error("当前第一版需要支持 HEVC NVENC 的 NVIDIA 显卡");
        D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        winrt::check_hresult(D3D11CreateDevice(chosen.get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels,2,D3D11_SDK_VERSION,device_.put(),nullptr,context_.put()));
        auto multi=context_.as<ID3D11Multithread>();multi->SetMultithreadProtected(TRUE);
        video_=device_.as<ID3D11VideoDevice>();videoContext_=context_.as<ID3D11VideoContext>();
        D3D11_TEXTURE2D_DESC td{};td.Width=width_;td.Height=height_;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        winrt::check_hresult(device_->CreateTexture2D(&td,nullptr,bgra_.put()));winrt::check_hresult(device_->CreateRenderTargetView(bgra_.get(),nullptr,target_.put()));
        constexpr const char* shader=R"(
Texture2D source:register(t0);SamplerState samplerLinear:register(s0);
cbuffer Settings:register(b0){float2 scale;float hdr;float padding;}
struct V{float4 p:SV_POSITION;float2 uv:TEXCOORD;};
V vs(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float3 srgb(float3 x){return lerp(x*12.92,1.055*pow(max(x,0),1.0/2.4)-0.055,step(0.0031308,x));}
float4 ps(V v):SV_TARGET{
 float2 uv=(v.uv-0.5)/scale+0.5;
 if(any(uv<0)||any(uv>1))return float4(0,0,0,1);
 float3 rgb=source.Sample(samplerLinear,uv).rgb;
 if(hdr>0.5){
  rgb=max(rgb,0);
  if(hdr>1.5){rgb*=80.0/203.0;rgb=saturate((rgb*(2.51*rgb+0.03))/(rgb*(2.43*rgb+0.59)+0.14));}
  rgb=srgb(rgb);
 }
 return float4(saturate(rgb),1);
})";
        winrt::com_ptr<ID3DBlob> vs,ps,errors;
        winrt::check_hresult(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,vs.put(),errors.put()));
        errors=nullptr;winrt::check_hresult(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,ps.put(),errors.put()));
        winrt::check_hresult(device_->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex_.put()));
        winrt::check_hresult(device_->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixel_.put()));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        winrt::check_hresult(device_->CreateSamplerState(&sd,sampler_.put()));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        winrt::check_hresult(device_->CreateBuffer(&bd,nullptr,constants_.put()));
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};cd.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;cd.InputFrameRate={60,1};cd.OutputFrameRate={60,1};
        cd.InputWidth=cd.OutputWidth=width_;cd.InputHeight=cd.OutputHeight=height_;cd.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        winrt::check_hresult(video_->CreateVideoProcessorEnumerator(&cd,enumerator_.put()));
        UINT support=0;winrt::check_hresult(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12,&support));
        if(!(support&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))throw std::runtime_error("GPU 不支持 NV12 视频转换");
        winrt::check_hresult(video_->CreateVideoProcessor(enumerator_.get(),0,processor_.put()));
        RECT rect{0,0,width_,height_};videoContext_->VideoProcessorSetStreamSourceRect(processor_.get(),0,TRUE,&rect);
        videoContext_->VideoProcessorSetStreamDestRect(processor_.get(),0,TRUE,&rect);videoContext_->VideoProcessorSetOutputTargetRect(processor_.get(),TRUE,&rect);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{},out{};in.RGB_Range=0;in.YCbCr_Matrix=1;out.YCbCr_Matrix=1;out.Nominal_Range=1;
        videoContext_->VideoProcessorSetStreamColorSpace(processor_.get(),0,&in);videoContext_->VideoProcessorSetOutputColorSpace(processor_.get(),&out);
    }
    ID3D11Device* device()const{return device_.get();}
    int width()const{return width_;}int height()const{return height_;}
    std::string adapter()const{return adapter_;}
    void render(ID3D11Texture2D* texture,bool hdr=false){renderInto(texture,target_.get(),width_,height_,hdr,true);}
    void renderInto(ID3D11Texture2D* texture,ID3D11RenderTargetView* destination,int width,int height,bool hdr,bool preserveAspect) {
        D3D11_TEXTURE2D_DESC desc;texture->GetDesc(&desc);
        winrt::com_ptr<ID3D11ShaderResourceView> source;
        HRESULT result=device_->CreateShaderResourceView(texture,nullptr,source.put());
        winrt::com_ptr<ID3D11Texture2D> copied;
        if(FAILED(result)){auto copyDesc=desc;copyDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;copyDesc.MiscFlags=0;copyDesc.Usage=D3D11_USAGE_DEFAULT;copyDesc.CPUAccessFlags=0;
            winrt::check_hresult(device_->CreateTexture2D(&copyDesc,nullptr,copied.put()));context_->CopyResource(copied.get(),texture);
            winrt::check_hresult(device_->CreateShaderResourceView(copied.get(),nullptr,source.put()));}
        float ratio=float(desc.Width)/desc.Height,targetRatio=float(width)/height;
        float values[]={!preserveAspect||ratio>targetRatio?1.f:ratio/targetRatio,!preserveAspect||ratio<=targetRatio?1.f:targetRatio/ratio,
            desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?(hdr?2.f:1.f):0.f,0.f};
        context_->UpdateSubresource(constants_.get(),0,nullptr,values,0,0);
        auto rtv=destination;context_->OMSetRenderTargets(1,&rtv,nullptr);
        D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context_->RSSetViewports(1,&vp);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.get(),nullptr,0);context_->PSSetShader(pixel_.get(),nullptr,0);
        auto view=source.get();auto sampler=sampler_.get();auto constants=constants_.get();
        context_->PSSetShaderResources(0,1,&view);context_->PSSetSamplers(0,1,&sampler);context_->PSSetConstantBuffers(0,1,&constants);context_->Draw(3,0);
        view=nullptr;context_->PSSetShaderResources(0,1,&view);rtv=nullptr;context_->OMSetRenderTargets(1,&rtv,nullptr);
    }
    void convert(AVFrame* frame) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};input.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
        winrt::com_ptr<ID3D11VideoProcessorInputView> iv;winrt::check_hresult(video_->CreateVideoProcessorInputView(bgra_.get(),enumerator_.get(),&input,iv.put()));
        auto texture=reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);D3D11_TEXTURE2D_DESC td;texture->GetDesc(&td);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output{};
        if(td.ArraySize>1){output.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;output.Texture2DArray.FirstArraySlice=static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));output.Texture2DArray.ArraySize=1;}
        else output.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
        winrt::com_ptr<ID3D11VideoProcessorOutputView> ov;winrt::check_hresult(video_->CreateVideoProcessorOutputView(texture,enumerator_.get(),&output,ov.put()));
        D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=iv.get();
        winrt::check_hresult(videoContext_->VideoProcessorBlt(processor_.get(),ov.get(),0,1,&stream));
    }
    std::array<CpuImage,8> readRegions() {
        std::array<CpuImage,8> result;
        // Downscale only the analysis surface on the GPU; recording keeps its selected resolution.
        auto analysisSource=bgra_.get();
        if(width_!=1920||height_!=1080){
            if(!analysisBgra_){D3D11_TEXTURE2D_DESC d{};d.Width=1920;d.Height=1080;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
                winrt::check_hresult(device_->CreateTexture2D(&d,nullptr,analysisBgra_.put()));winrt::check_hresult(device_->CreateRenderTargetView(analysisBgra_.get(),nullptr,analysisTarget_.put()));}
            renderInto(bgra_.get(),analysisTarget_.get(),1920,1080,false,false);analysisSource=analysisBgra_.get();
        }
        for(size_t i=0;i<regions.size();++i){
            auto r=regions[i];UINT x=UINT(r.x*1920),y=UINT(r.y*1080),w=UINT(r.w*1920),h=UINT(r.h*1080);
            if(!staging_[i]){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                winrt::check_hresult(device_->CreateTexture2D(&d,nullptr,staging_[i].put()));}
            D3D11_BOX box{x,y,0,x+w,y+h,1};context_->CopySubresourceRegion(staging_[i].get(),0,0,0,0,analysisSource,0,&box);
        }
        for(size_t i=0;i<regions.size();++i){
            D3D11_TEXTURE2D_DESC d;staging_[i]->GetDesc(&d);auto& image=result[i];image.width=d.Width;image.height=d.Height;image.bgra.resize(size_t(d.Width)*d.Height*4);
            D3D11_MAPPED_SUBRESOURCE mapped{};winrt::check_hresult(context_->Map(staging_[i].get(),0,D3D11_MAP_READ,0,&mapped));
            for(UINT y=0;y<d.Height;++y)memcpy(image.bgra.data()+size_t(y)*d.Width*4,static_cast<uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,d.Width*4);
            context_->Unmap(staging_[i].get(),0);
        }
        return result;
    }
};
class WindowCapture {
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest_{nullptr};
    winrt::event_token arrived_{},closed_{};
    std::mutex mutex_;std::atomic<bool> gone_=false;bool borderHidden_=false;winrt::Windows::Graphics::SizeInt32 size_{};
public:
    WindowCapture(HWND window,ID3D11Device* device) {
        using namespace winrt::Windows::Graphics::Capture;
        if(!GraphicsCaptureSession::IsSupported())throw std::runtime_error("此 Windows 会话不支持窗口采集");
        auto interop=winrt::get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(window,winrt::guid_of<GraphicsCaptureItem>(),winrt::put_abi(item_)));
        winrt::com_ptr<IDXGIDevice> dxgi;winrt::check_hresult(device->QueryInterface(dxgi.put()));winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
        auto direct=inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();size_=item_.Size();
        pool_=Direct3D11CaptureFramePool::CreateFreeThreaded(direct,winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float,3,size_);
        arrived_=pool_.FrameArrived([this,direct](auto const& pool,auto const&){
            try{
                auto frame=pool.TryGetNextFrame();if(!frame)return;auto size=frame.ContentSize();
                {std::lock_guard lock(mutex_);latest_=frame;}
                if(size.Width!=size_.Width || size.Height!=size_.Height){size_=size;pool.Recreate(direct,winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float,3,size_);}
            }catch(...){gone_=true;}
        });
        closed_=item_.Closed([this](auto const&,auto const&){gone_=true;});
        session_=pool_.CreateCaptureSession(item_);session_.IsCursorCaptureEnabled(false);
        try{
            auto access=winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless).get();
            session_.IsBorderRequired(false);
            borderHidden_=access==winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed&&!session_.IsBorderRequired();
        }catch(...){borderHidden_=false;}
        session_.StartCapture();
    }
    ~WindowCapture(){
        if(pool_)pool_.FrameArrived(arrived_);if(item_)item_.Closed(closed_);
        if(session_)session_.Close();if(pool_)pool_.Close();std::lock_guard lock(mutex_);latest_=nullptr;
    }
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame take(){std::lock_guard lock(mutex_);auto f=latest_;latest_=nullptr;return f;}
    bool gone()const{return gone_;}
    bool borderHidden()const{return borderHidden_;}
    static winrt::com_ptr<ID3D11Texture2D> texture(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame){
        auto access=frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();winrt::com_ptr<ID3D11Texture2D> t;winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),t.put_void()));return t;
    }
};
}
