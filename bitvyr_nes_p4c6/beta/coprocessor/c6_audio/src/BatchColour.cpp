#include "BatchColour.h"
#include <cstring>
namespace nes::c6raw::batch {namespace {
/** @brief Один borrowed TLV, максимум32 элементов без heap. */
struct Item {const std::uint8_t* data{};std::uint16_t bytes{};Command type{};};
/** @brief Сверяет frame lease, исключая packet slot/sequence/opcode и frame-relative slice. */
bool same(const Key& a,const Key& b) noexcept {return a.generation==b.generation&&a.lease==b.lease&&a.session==b.session&&a.frame==b.frame&&a.region==b.region;}
/** @brief Clock необязателен только для native oracle; это не hardware cycle estimate. */
std::uint32_t now(Clock clock) noexcept {return clock?clock():0;}
}
/** @brief Кодирует указанное число LE-байтов без невыровненных word stores. */
void put(std::uint8_t* out,std::uint32_t value,unsigned bytes) noexcept {for(unsigned i=0;i<bytes;++i)out[i]=std::uint8_t(value>>(8U*i));}
/** @brief Читает LE из проверенного wire extent без alias casts. */
std::uint32_t get(const std::uint8_t* in,unsigned bytes) noexcept {std::uint32_t value=0;for(unsigned i=0;i<bytes;++i)value|=std::uint32_t(in[i])<<(8U*i);return value;}
/** @brief Сбрасывает локальный цветной stream только по прежнему разрешённому control пути. */
void Stream::reset() noexcept {rowOpen_=frameOpen_=fault_=haveFrame_=false;nextOrdinal_=0;context_.valid=false;completed_.fill(0);dirty_.fill(0);}
/** @brief Проверяет order/key и исполняет batch colour в bounded caller output. */
Status Stream::process(const Key& key,const std::uint8_t* input,std::size_t bytes,std::uint8_t* output,std::size_t capacity,std::size_t& written,Metrics& metrics,Clock clock) noexcept {
    written=0;metrics={};if(fault_)return Status::Faulted;
    const auto fail=[this](Status status){fault_=true;return status;};
    const auto started=now(clock);
    if(!input||bytes<requestHeaderBytes||!output||key.region>1||input[0]!=1||input[1]>maxRecords||input[2]>maxRows||(input[3]&~3U))return fail(Status::Malformed);
    const auto count=input[1],rows=input[2],flags=input[3];const auto ordinal=get(input+4,4);
    if(capacity<replyHeaderBytes+rows*rowBytes)return fail(Status::Capacity);
    if(flags&beginFrame){if(frameOpen_||rowOpen_||ordinal)return fail(Status::Sequence);
        if(haveFrame_&&key.generation==owner_.generation&&key.lease==owner_.lease&&key.session==owner_.session&&key.region==owner_.region&&key.frame<=owner_.frame)return fail(Status::Sequence);}
    else if(!frameOpen_||!same(key,owner_))return fail(Status::Identity);
    if(!(flags&beginFrame)&&ordinal!=nextOrdinal_)return fail(Status::Sequence);
    std::array<Item,maxRecords> items{};std::size_t at=requestHeaderBytes;unsigned ends=0;
    bool open=(flags&beginFrame)?false:rowOpen_;auto y=y_,prefix=prefix_;auto completed=completed_;if(flags&beginFrame)completed.fill(0);
    for(unsigned i=0;i<count;++i){
        if(at+3>bytes)return fail(Status::Malformed);
        const auto type=Command(input[at]);const auto size=get(input+at+1,2);at+=3;if(size>bytes-at)return fail(Status::Malformed);
        items[i]={input+at,std::uint16_t(size),type};const auto* data=input+at;at+=size;
        if(type==Command::Seed){colour::RowSeed seed{};if(size!=77||!colour::wire::decodeSeed(data+3,74,seed)||seed.pal!=bool(key.region))return fail(Status::Grammar);
            const auto target=get(data,2);if(target>=240||(open&&target!=y)||(!open&&(completed[target>>3U]&(1U<<(target&7U)))))return fail(Status::Grammar);
            if(!open){open=true;y=std::uint16_t(target);prefix=0;}
        }else if(type==Command::Run){colour::Run run{};if(!open||!colour::wire::decodeRun(data,size,run)||run.y!=y||run.x<prefix)return fail(Status::Grammar);
            prefix=std::uint16_t(run.x+run.tiles*8U);
        }else if(type==Command::End){if(size!=3||!open||get(data,2)<prefix||get(data,2)>256||data[2]>7||++ends>maxRows)return fail(Status::Grammar);completed[y>>3U]|=std::uint8_t(1U<<(y&7U));open=false;
        }else return fail(Status::Malformed);
    }
    if(at!=bytes||ends!=rows||((flags&endFrame)&&open))return fail(Status::Grammar);
    metrics.validate=now(clock)-started;
    if(flags&beginFrame){owner_=key;completed_.fill(0);frameOpen_=true;rowOpen_=false;haveFrame_=true;}
    std::memset(output,0,replyHeaderBytes);unsigned produced=0;
    for(unsigned i=0;i<count;++i){const auto& item=items[i];const auto* data=item.data;
        if(item.type==Command::Seed){colour::RowSeed seed{};(void)colour::wire::decodeSeed(data+3,74,seed);const auto t=now(clock);
            if(!colour::index_only::prepare(seed,data[2],context_))return fail(Status::Kernel);
            metrics.prepare+=now(clock)-t;seed_=seed;
            if(!rowOpen_){y_=std::uint16_t(get(data,2));prefix_=0;dirty_.fill(0);result_.indices.fill(0);result_.after={};rowOpen_=true;}
        }else if(item.type==Command::Run){colour::Run run{};(void)colour::wire::decodeRun(data,item.bytes,run);auto t=now(clock);
            if(!colour::index_only::prepare(seed_,run.mask,context_))return fail(Status::Kernel);
            metrics.prepare+=now(clock)-t;t=now(clock);
            if(!colour::index_only::render(context_,run,result_))return fail(Status::Kernel);
            metrics.kernel+=now(clock)-t;
            prefix_=std::uint16_t(run.x+run.tiles*8U);for(unsigned x=run.x;x<prefix_;++x)dirty_[x>>3U]|=std::uint8_t(1U<<(x&7U));metrics.pixels+=run.tiles*8U;
        }else {const auto t=now(clock);auto* row=output+replyHeaderBytes+produced*rowBytes;put(row,y_,2);put(row+2,get(data,2),2);row[4]=data[2];row[5]=key.region;std::memcpy(row+6,dirty_.data(),32);std::memcpy(row+38,result_.indices.data(),256);metrics.pack+=now(clock)-t;++produced;completed_[y_>>3U]|=std::uint8_t(1U<<(y_&7U));rowOpen_=false;}
    }
    nextOrdinal_=ordinal+1;if(flags&endFrame)frameOpen_=false;
    output[0]=1;output[1]=std::uint8_t(produced);output[2]=count;output[3]=std::uint8_t((rowOpen_?1:0)|(frameOpen_?2:0));put(output+4,ordinal,4);put(output+8,nextOrdinal_,4);
    put(output+16,metrics.validate,4);put(output+20,metrics.prepare,4);put(output+24,metrics.kernel,4);put(output+28,metrics.pack,4);put(output+32,metrics.pixels,4);
    written=replyHeaderBytes+produced*rowBytes;return Status::Ok;
}
/** @brief Проверяет reply ordinal/region/extents перед публикацией заимствованных row views. */
bool decodeReply(const std::uint8_t* data,std::size_t bytes,std::uint32_t ordinal,bool pal,std::array<RowView,maxRows>& rows,unsigned& count) noexcept {
    if(!data||bytes<replyHeaderBytes||data[0]!=1||data[1]>maxRows||data[2]>maxRecords||(data[3]&~3U)||get(data+4,4)!=ordinal||get(data+8,4)!=ordinal+1||bytes!=replyHeaderBytes+data[1]*rowBytes)return false;
    std::array<RowView,maxRows> checked{};
    for(unsigned i=0;i<data[1];++i){const auto* p=data+replyHeaderBytes+i*rowBytes;const auto y=get(p,2),prefix=get(p+2,2);if(y>=240||prefix>256||p[4]>7||p[5]>1||bool(p[5])!=pal)return false;
        for(unsigned x=0;x<256;++x)if(p[6+(x>>3U)]&(1U<<(x&7U)))if(x>=prefix||p[38+x]>63)return false;
        for(unsigned j=0;j<i;++j)if(checked[j].y==y)return false;
        checked[i]={std::uint16_t(y),std::uint16_t(prefix),p[4],pal,p+6,p+38};
    }
    rows=checked;count=data[1];return true;
}
}
