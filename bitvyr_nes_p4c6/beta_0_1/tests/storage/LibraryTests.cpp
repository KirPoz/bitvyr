#include "GameLibrary.h"
#include "FixtureValues.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#endif

static int allocationsUntilFailure=-1,liveAllocations=0;
/**
 * @brief Подменяет только аллокатор индекса; заданный отказ не освобождает старый блок, размер0 освобождает и обновляет баланс.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
extern "C" void* nesLibraryTestRealloc(void* old,std::size_t size) noexcept {
    if(!size){if(old){--liveAllocations;std::free(old);}return nullptr;}
    if(allocationsUntilFailure==0)return nullptr;
    if(allocationsUntilFailure>0)--allocationsUntilFailure;
    void* result=std::realloc(old,size);if(result&&!old)++liveAllocations;return result;
}
using nes::app::GameLibrary;using nes::app::GameInfo;namespace fs=std::filesystem;
#undef assert
#define assert(condition) do { if (!(condition)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); std::exit(1); } } while(false)
/**
 * @brief Находит fixture по фрагменту пути или ZIP-member; отсутствие ожидаемой записи завершает тест с ошибкой.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
static GameInfo find(GameLibrary& lib,const char* fragment){
    GameInfo info{};for(std::size_t i=0;i<lib.count();++i){assert(lib.game(i,info));if(std::strstr(info.path,fragment)||std::strstr(info.member,fragment))return info;}std::abort();
}
/**
 * @brief Читает synthetic fixture в собственный вектор байтов для проверки изменения файла между индексированием и загрузкой.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
static std::vector<unsigned char> readFile(const char* path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
/**
 * @brief Перезаписывает только временный synthetic fixture переданными байтами и требует успешной записи полного диапазона.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
static void writeFile(const char* path,const std::vector<unsigned char>& bytes){std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));assert(f.good());}
/**
 * @brief Проверяет поколение каталога, сортировку, большое число файлов, длинные пути, изменение ROM и отказ аллокатора без утечек.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int main(int argc,char**argv){
    assert(argc==2);const fs::path base=fs::u8path(argv[1]);const auto raw=(base/"raw").u8string();
    {
        GameLibrary lib;assert(lib.open(raw.c_str()));assert(lib.count()==expectedRaw);assert(lib.skipped()>=4);
        std::vector<std::string> sorted;
        for(std::size_t i=0;i<lib.count();++i){GameInfo a{},b{};assert(lib.game(i,a));assert(lib.game(i,b));assert(!std::strcmp(a.path,b.path));assert(a.entry==i);sorted.emplace_back(a.path);}
        GameInfo invalid{};assert(!lib.game(lib.count(),invalid));assert(!invalid.path[0]);
        std::size_t alphaA=0,alphaB=0,gFirst=0,gLast=0;
        for(std::size_t i=0;i<sorted.size();++i){if(sorted[i].find("a/alpha.nes")!=std::string::npos)alphaA=i;if(sorted[i].find("b/ALPHA.NES")!=std::string::npos)alphaB=i;if(sorted[i].find("g0000.nes")!=std::string::npos)gFirst=i;if(sorted[i].find("g1099.NES")!=std::string::npos)gLast=i;}
        assert(alphaB==alphaA+1&&gFirst<gLast);
        for(unsigned index:{0U,64U,999U,expectedRaw-1}){GameInfo info{};assert(lib.game(index,info));std::vector<unsigned char> bytes(info.size);std::uint32_t crc=0;assert(lib.read(info,bytes.data(),bytes.size(),crc));assert(bytes.size()>=16400&&crc);}
        auto europe=find(lib,"Europe");assert(europe.pal);auto aladdin=find(lib,"Aladdin");assert(aladdin.mapper==160&&!aladdin.pal);
        assert(find(lib,"ExplicitPAL").pal);assert(find(lib,"NES2 NTSC").mapper==0xACB&&!find(lib,"NES2 NTSC").pal);assert(find(lib,"NES2 PAL").pal);
        // POSIX создаёт raw-путь длиннее MAX_PATH; Windows исключает только эту fixture,
        // чтобы штатный Full Clean мог удалить дерево. UTF8/depth/ZIP проверки ниже остаются.
        if constexpr(hasLongRawFixture){
            auto longName=find(lib,"Long.nes");assert(std::strlen(longName.path)>260);
        }
        for(std::size_t i=0;i<lib.count();++i){
            GameInfo info{};assert(lib.game(i,info));
#if defined(_WIN32)
            wchar_t text[112];assert(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,info.name,-1,text,112));
#else
            std::wstring_convert<std::codecvt_utf8<char32_t>,char32_t> converter;
            try{(void)converter.from_bytes(info.name);}catch(const std::range_error&){assert(false);}
#endif
        }
        auto mutableInfo=find(lib,"Mutable.nes");assert(mutableInfo.crc==0);std::vector<unsigned char> bytes(mutableInfo.size);std::uint32_t crc=0;
        assert(!lib.read(mutableInfo,nullptr,bytes.size(),crc));assert(crc==0);
        assert(!lib.read(mutableInfo,bytes.data(),bytes.size()-1,crc));assert(lib.read(mutableInfo,bytes.data(),bytes.size(),crc));assert(crc==expectedCRC);
#if defined(_WIN32)
        const auto exclusive=CreateFileW(fs::u8path(mutableInfo.path).c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);assert(exclusive!=INVALID_HANDLE_VALUE);assert(CloseHandle(exclusive));
#endif
        auto cached=find(lib,"Mutable.nes");assert(cached.crc==crc);
        auto altered=cached;altered.path[0]='?';assert(!lib.read(altered,bytes.data(),bytes.size(),crc));
        altered=cached;std::memset(altered.member,'x',sizeof(altered.member));assert(!lib.read(altered,bytes.data(),bytes.size(),crc));
        const auto path=fs::u8path(cached.path);const auto original=readFile(cached.path);
#if defined(_WIN32)
        WIN32_FILE_ATTRIBUTE_DATA stamp{};assert(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&stamp));
        /**
         * @brief Восстанавливает исходное время записи временного ROM после проверки CRC через Win32 handle.
         * @details Заимствует path/stamp до конца текущего блока; assert требует успешных open, SetFileTime и CloseHandle.
         */
        const auto restoreTime=[&](){const auto h=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);assert(h!=INVALID_HANDLE_VALUE);assert(SetFileTime(h,nullptr,nullptr,&stamp.ftLastWriteTime));assert(CloseHandle(h));};
#else
        const auto stamp=fs::last_write_time(path);
        /**
         * @brief Восстанавливает исходное время записи временного ROM после проверки CRC через std::filesystem.
         * @details Заимствует path/stamp до конца текущего блока; ошибка filesystem прерывает native-тест исключением.
         */
        const auto restoreTime=[&](){fs::last_write_time(path,stamp);};
#endif
        auto corrupt=original;corrupt[100]^=1;writeFile(cached.path,corrupt);restoreTime();
        assert(!lib.read(cached,bytes.data(),bytes.size(),crc));assert(std::strstr(lib.errorText(),"CRC"));writeFile(cached.path,original);restoreTime();
        auto removed=find(lib,"Removed.nes");const auto saved=readFile(removed.path);fs::remove(fs::u8path(removed.path));assert(!lib.read(removed,bytes.data(),bytes.size(),crc));writeFile(removed.path,saved);
        assert(lib.open(raw.c_str()));assert(lib.count()==expectedRaw);assert(!lib.read(cached,bytes.data(),bytes.size(),crc));
        for(std::size_t i=0;i<sorted.size();++i){GameInfo info{};assert(lib.game(i,info));assert(sorted[i]==info.path);}
        assert(!lib.open((base/"missing").u8string().c_str()));assert(lib.count()==0&&lib.errorText()[0]);
        assert(lib.open((base/"empty").u8string().c_str()));assert(lib.count()==0&&!lib.errorText()[0]);
        assert(!lib.open(nullptr));assert(lib.count()==0);
        assert(!lib.open(std::string(1100,'x').c_str()));assert(lib.count()==0);
    }
    assert(liveAllocations==0);
    for(int budget=0;budget<10;++budget){GameLibrary lib;allocationsUntilFailure=budget;assert(!lib.open(raw.c_str()));assert(lib.count()==0&&lib.errorText()[0]);allocationsUntilFailure=-1;}
    assert(liveAllocations==0);
    {
        GameLibrary lib;assert(lib.open((base/"archives").u8string().c_str()));assert(lib.count()==1108);
        for(std::size_t index:{std::size_t(0),std::size_t(64),std::size_t(1000),lib.count()-1}){GameInfo info{};assert(lib.game(index,info));assert(info.archived&&info.member[0]&&info.crc==expectedCRC);std::vector<unsigned char> bytes(info.size);std::uint32_t crc=0;assert(lib.read(info,bytes.data(),bytes.size(),crc)&&crc==expectedCRC);}
        auto compressed=find(lib,"Compressed");assert(compressed.pal);auto stored=find(lib,"Stored.NES");assert(!stored.pal);
        assert(lib.open((base/"corrupt").u8string().c_str()));assert(lib.count()==1);GameInfo info{};assert(lib.game(0,info));std::vector<unsigned char> bytes(info.size);std::uint32_t crc=0;assert(!lib.read(info,bytes.data(),bytes.size(),crc)&&crc==0);
        assert(lib.open((base/"badarchive").u8string().c_str()));assert(lib.count()==0&&lib.skipped()>=2);
    }
    assert(liveAllocations==0);
    std::printf("PASS raw=%u ZIP=1108; arbitrary indices, UTF8/long paths, metadata/CRC, stale/rescan/remove/OOM; symlink_fixture=%d long_raw_checked=%d\n",expectedRaw,hasSymlink,int(hasLongRawFixture));
}
