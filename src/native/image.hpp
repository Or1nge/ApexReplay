#pragma once
#include <algorithm>
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
}
