#include "GameLibrary.h"
#include "ZipArchive.h"
#include <cstdio>
namespace nes::storage {
/**
 * @brief Имитирует корректную ZIP-запись Lost.nes и передаёт её visitor; false означает штатную отмену обхода.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
ZipError ZipArchive::scan(const char*,Visitor visitor,void* context) noexcept {
    ZipEntry entry{};entry.uncompressedSize=16400;
    return visitor("Lost.nes",8,entry,context)?ZipError::Ok:ZipError::Cancelled;
}
/**
 * @brief Имитирует потерю носителя при чтении заголовка: возвращает Io и нулевое число байтов, не меняя буфер.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
ZipError ZipArchive::readPrefix(const char*,const ZipEntry&,std::uint8_t*,std::size_t,std::size_t& bytes) noexcept {bytes=0;return ZipError::Io;}
/**
 * @brief Имитирует отказ полного чтения с нулевым результатом, чтобы библиотека не принимала частично загруженную ROM.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
ZipError ZipArchive::read(const char*,const ZipEntry&,std::uint8_t*,std::size_t,std::size_t& bytes) noexcept {bytes=0;return ZipError::Io;}
/**
 * @brief Возвращает стабильный текст fake-ошибки ZIP, используемый проверкой сообщения библиотеки.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
const char* ZipArchive::errorText(ZipError) noexcept {return "archive I/O failed";}
}
/**
 * @brief Проверяет, что ошибка ZIP-prefix обнуляет индекс и делает rescan неуспешным с доступным текстом причины.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int main(int argc,char**argv){
    if(argc!=2)return 2;
    nes::app::GameLibrary library;
    const bool okay=library.open(argv[1]);
    if(okay||library.count()!=0||!library.errorText()[0]){std::fprintf(stderr,"FAIL: prefix I/O must fail rescan, clear index and report an error\n");return 1;}
    std::puts("PASS: ZIP prefix I/O fails rescan and reports card/archive loss.");
}
