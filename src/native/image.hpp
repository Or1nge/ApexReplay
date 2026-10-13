#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
namespace apex {
// Platform-neutral BGRA image and crop helpers, shared by the HUD readers and their portable tests.
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
// Windows OCR input. Small crops are enlarged bilinearly, so glyph edges stay smooth instead of stair-stepped. The
// contrast mask keeps white, orange and optionally red or gold text as black ink. Over a bright background (sky,
// snow) the fixed white threshold also keeps the background, so there white text must stand out from its
// surroundings; over a dark background the mask is unchanged.
inline CpuImage ocrInput(const CpuImage& image,bool contrast=false,bool redNames=false,bool goldNames=false){
    int width=image.width,height=image.height,scale=width<400?3:width<900?2:1;
    while(scale>1&&std::max(width,height)*scale>2600)--scale;
    std::vector<float> value(size_t(width)*height*3);
    if(contrast){
        std::vector<uint32_t> sum(size_t(width+1)*(height+1));
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto p=image.bgra.data()+(size_t(y)*width+x)*4;
            sum[size_t(y+1)*(width+1)+x+1]=std::min({p[0],p[1],p[2]})+sum[size_t(y)*(width+1)+x+1]+sum[size_t(y+1)*(width+1)+x]-sum[size_t(y)*(width+1)+x];}
        constexpr int radius=6;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto p=image.bgra.data()+(size_t(y)*width+x)*4;
            int x0=std::max(0,x-radius),x1=std::min(width,x+radius+1),y0=std::max(0,y-radius),y1=std::min(height,y+radius+1);
            double mean=double(sum[size_t(y1)*(width+1)+x1]-sum[size_t(y0)*(width+1)+x1]-sum[size_t(y1)*(width+1)+x0]+sum[size_t(y0)*(width+1)+x0])/((x1-x0)*(y1-y0));
            int lowest=std::min({p[0],p[1],p[2]});bool white=lowest>=155&&(mean<140||lowest>=mean+15);
            bool orange=p[2]>175&&p[1]>65&&p[1]<185&&p[2]>p[1]*1.4&&p[1]>p[0]*1.4;
            bool gold=goldNames&&p[2]>150&&p[1]>90&&p[2]>p[0]*1.5&&p[1]>p[0]*1.3&&p[2]>=p[1]*1.05;
            bool red=redNames&&p[2]>150&&p[2]>p[1]*1.4&&p[2]>p[0]*1.4;
            auto v=white||orange||red||gold?0.f:255.f;auto q=value.data()+(size_t(y)*width+x)*3;q[0]=q[1]=q[2]=v;}
    }else for(size_t i=0;i<size_t(width)*height;++i)for(int c=0;c<3;++c)value[i*3+c]=image.bgra[i*4+c];
    CpuImage out{width*scale,height*scale,{}};out.bgra.resize(size_t(out.width)*out.height*4);
    for(int y=0;y<out.height;++y){double sy=std::clamp((y+.5)/scale-.5,0.,height-1.);int y0=int(sy),y1=std::min(y0+1,height-1);float fy=float(sy-y0);
        for(int x=0;x<out.width;++x){double sx=std::clamp((x+.5)/scale-.5,0.,width-1.);int x0=int(sx),x1=std::min(x0+1,width-1);float fx=float(sx-x0);
            auto at=[&](int px,int py,int c){return value[(size_t(py)*width+px)*3+c];};auto dst=out.bgra.data()+(size_t(y)*out.width+x)*4;
            for(int c=0;c<3;++c)dst[c]=uint8_t(std::lround((at(x0,y0,c)*(1-fx)+at(x1,y0,c)*fx)*(1-fy)+(at(x0,y1,c)*(1-fx)+at(x1,y1,c)*fx)*fy));dst[3]=255;}}
    return out;
}
}
