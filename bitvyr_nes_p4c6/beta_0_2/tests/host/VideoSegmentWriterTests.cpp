/** @file VideoSegmentWriterTests.cpp
 * @brief Дифференциальный формат и fault-границы квалифицированного MMC5 writer. */
#include "ppu/VideoPacket.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace nes::core {
/** @brief Узкий тестовый доступ к двум writer без изменения production layout. */
struct VideoSegmentWriterTestAccess {
    /** @brief Записывает run старым либо новым путём с одинаковыми входами. */
    static bool append(VideoPacket& p,bool optimized,unsigned n,unsigned x,unsigned y,
                       unsigned fine,const std::uint32_t* tokens) noexcept {
        return optimized?p.addMmc5SegmentRun(n,x,y,0x1eU,fine,tokens):
            p.addMmc5InplaceRun(n,x,y,0x1eU,fine,tokens);
    }
};
}
namespace {
using namespace nes::core;
unsigned checks=0;
/** @brief Завершает fixture при первом нарушении с номером проверки. */
void require(bool value) {++checks;if(!value){std::fprintf(stderr,"writer check %u failed\n",checks);std::abort();}}
/** @brief Сравнивает только семантический заголовок и полностью опубликованный payload. */
void equal(const VideoPacket& a,const VideoPacket& b) {
    require(a.state()==b.state());require(a.header().fault==b.header().fault);
    require(a.header().eventCount==b.header().eventCount);
    require(a.legacyEventCount()==b.legacyEventCount());
    require(std::memcmp(a.events(),b.events(),a.header().eventCount*sizeof(VideoEvent))==0);
}
}
/** @brief Проверяет все длины, odd padding, координаты и исходную capacity fault-фазу. */
int main() {
    auto a=std::make_unique<VideoPacket>();auto b=std::make_unique<VideoPacket>();
    std::array<std::uint32_t,32> tokens{};
    for(unsigned i=0;i<tokens.size();++i)tokens[i]=(i*19U)|((i&3U)<<30U);
    for(unsigned tiles=1;tiles<=31U;++tiles)for(unsigned fine=0;fine<8U;++fine){
        a->abandon();b->abandon();require(a->begin(1,0,7,false,nullptr,0));require(b->begin(1,0,7,false,nullptr,0));
        const unsigned x=256U-8U*tiles;
        require(VideoSegmentWriterTestAccess::append(*a,false,tiles,x,239,fine,tokens.data()));
        require(VideoSegmentWriterTestAccess::append(*b,true,tiles,x,239,fine,tokens.data()));
        equal(*a,*b);require(b->legacyEventCount()==tiles*5U);
        require(b->header().eventCount==1U+(tiles+1U)/2U);
    }
    for(unsigned bad=0;bad<6U;++bad){
        a->abandon();b->abandon();require(a->begin(1,0,7,false,nullptr,0));require(b->begin(1,0,7,false,nullptr,0));
        const unsigned n=bad==0?0U:bad==1?32U:3U;
        const unsigned x=bad==2?250U:0U,y=bad==3?240U:0U,fine=bad==4?8U:0U;
        const auto*ptr=bad==5?nullptr:tokens.data();
        require(!VideoSegmentWriterTestAccess::append(*a,false,n,x,y,fine,ptr));
        require(!VideoSegmentWriterTestAccess::append(*b,true,n,x,y,fine,ptr));equal(*a,*b);
    }
    a->abandon();b->abandon();require(a->begin(1,0,7,false,nullptr,0));require(b->begin(1,0,7,false,nullptr,0));
    while(a->legacyEventCount()+15U<=VideoPacket::eventCapacity){
        require(VideoSegmentWriterTestAccess::append(*a,false,3,0,0,0,tokens.data()));
        require(VideoSegmentWriterTestAccess::append(*b,true,3,0,0,0,tokens.data()));
    }
    equal(*a,*b);require(!VideoSegmentWriterTestAccess::append(*a,false,3,0,0,0,tokens.data()));
    require(!VideoSegmentWriterTestAccess::append(*b,true,3,0,0,0,tokens.data()));equal(*a,*b);
    require(b->header().fault==VideoFault::Capacity);
    std::printf("Video segment writer: %u checks\n",checks);return 0;
}
