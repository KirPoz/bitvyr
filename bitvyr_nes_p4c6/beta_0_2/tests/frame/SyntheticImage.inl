/**
 * @brief Создаёт собственный детерминированный iNES-образ с тестовой программой и CHR-узором.
 * @details mapper кодируется в заголовке и выбирает размеры банков и тестовые
 * регистровые записи; variant меняет палитру, scroll, банковые значения и CHR.
 * Возвращённый std::vector владеет всем образом; входные ROM и файлы не читаются.
 * Программа копируется в PRG-банки вместе с reset/NMI/IRQ vectors для frame fixture.
 * Ошибки выделения std::vector не перехватываются.
 */
static std::vector<unsigned char> image(unsigned mapper,unsigned variant){
    const unsigned prgBanks=mapper==0?2:8,chrBanks=mapper==5?16:4;
    std::vector<unsigned char>r(16U+prgBanks*16384U+chrBanks*8192U);
    r[0]='N';r[1]='E';r[2]='S';r[3]=0x1a;r[4]=static_cast<unsigned char>(prgBanks);r[5]=static_cast<unsigned char>(chrBanks);
    r[6]=static_cast<unsigned char>((mapper&15U)<<4U);r[7]=static_cast<unsigned char>(mapper&0xf0U);
    std::vector<unsigned char>p;p.reserve(1024);p.insert(p.end(),{0x78,0xd8,0xa2,0xff,0x9a});
    /**
     * @brief Дописывает в p пять байтов команд LDA #v и STA a для synthetic программы.
     * @details a усекается до 16 бит, v до 8; захваченный p принадлежит image,
     * реальный MMIO не выполняется. Рост вектора может выбросить bad_alloc.
     */
    const auto write=[&](unsigned a,unsigned v){p.push_back(0xa9);p.push_back(static_cast<unsigned char>(v));p.push_back(0x8d);p.push_back(static_cast<unsigned char>(a));p.push_back(static_cast<unsigned char>(a>>8U));};
    write(0x4017,0x40);write(0x4015,0x0f);write(0x4000,0xbf);write(0x4002,0x70);write(0x4003,0x08);
    if(mapper==5){write(0x5100,3);write(0x5114,0x80);write(0x5115,0x81);write(0x5116,0x82);write(0x5117,0x83);}
    write(0x2006,0x3f);write(0x2006,0);
    for(unsigned i=0;i<32;++i)write(0x2007,(i*3U+variant+1U)&0x3fU);
    write(0x2000,0x80U|((variant&1U)?0x10U:0U));write(0x2001,0x1e);
    const unsigned loop=0xe000U+static_cast<unsigned>(p.size());
    p.insert(p.end(),{0xad,0x02,0x20,0x85,0x20,0xe6,0x21});
    write(0x2005,variant*3U);write(0x2005,0);write(0x2006,0x20);write(0x2006,0x80);
    p.insert(p.end(),{0xa5,0x21,0x8d,0x07,0x20});
    write(0x2006,0x20);write(0x2006,0x80);p.insert(p.end(),{0xad,0x07,0x20,0x85,0x22});
    if(mapper==1){write(0x8000,0x80);for(unsigned i=0;i<5;++i)write(0x8000,(0x1eU>>i)&1U);}
    if(mapper==2||mapper==3||mapper==7)write(0x8000,mapper==7?0x10U:1U);
    if(mapper==4){write(0x8000,2);write(0x8001,variant+1U);write(0xa000,variant&1U);}
    p.push_back(0x4c);p.push_back(static_cast<unsigned char>(loop));p.push_back(static_cast<unsigned char>(loop>>8U));
    for(unsigned bank=0;bank<prgBanks*2U;++bank){auto*b=r.data()+16U+bank*8192U;std::memset(b,0xea,8192);std::memcpy(b,p.data(),p.size());
        b[0x300]=0x40;for(unsigned v:{0x1ffaU,0x1ffcU,0x1ffeU}){b[v]=static_cast<unsigned char>(v==0x1ffcU?0:0);b[v+1]=static_cast<unsigned char>(v==0x1ffcU?0xe0:0xe3);}}
    for(unsigned i=16U+prgBanks*16384U;i<r.size();++i)r[i]=static_cast<unsigned char>((i*37U+(i>>4U)*variant)^0x96U);
    return r;
}
