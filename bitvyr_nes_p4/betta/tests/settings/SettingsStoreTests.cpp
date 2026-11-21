#include "SettingsStore.h"
#include "nvs.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace nes::app;
/**
 * @brief Одновладельческий fake NVS с отдельными pending/committed байтами.
 * @details Ошибки каждой стадии задаются независимо; счётчики доказывают
 * отсутствие erase, лишних write и незакрытых handle на путях отказа.
 */
struct FakeNvs {
    int initError=ESP_OK,openError=ESP_OK,getError=ESP_OK,setError=ESP_OK,commitError=ESP_OK;
    unsigned init=0,open=0,get=0,set=0,commit=0,close=0;
    bool alive=false; nvs_open_mode_t mode=NVS_READONLY;
    std::vector<std::uint8_t> blob,pending;
} fake;
/**
 * @brief Возвращает фиксированное имя ошибки fake SDK; код ошибки не раскрывает данные NVS.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
const char* esp_err_to_name(int) { return "FAKE_ERROR"; }
/**
 * @brief Считает попытку инициализации и возвращает заданный отказ без стирания fake NVS.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int nvs_flash_init() {++fake.init;return fake.initError;}
/**
 * @brief Проверяет namespace и единственность handle; на успехе заимствует fake-хранилище в запрошенном режиме.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int nvs_open(const char* space,nvs_open_mode_t mode,nvs_handle_t* handle) {
    assert(!std::strcmp(space,"nes_console")&&!fake.alive);++fake.open;fake.mode=mode;
    if(fake.openError)return fake.openError;
    *handle=42;fake.alive=true;return ESP_OK;
}
/**
 * @brief Читает зафиксированный blob через READONLY handle; проверяет ключ и вместимость в байтах, ошибки возвращает до копирования.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int nvs_get_blob(nvs_handle_t handle,const char* key,void* data,std::size_t* size) {
    assert(handle==42&&fake.alive&&fake.mode==NVS_READONLY&&!std::strcmp(key,"cfg_v1"));++fake.get;
    if(fake.getError)return fake.getError;
    if(*size<fake.blob.size()){*size=fake.blob.size();return ESP_ERR_NVS_INVALID_LENGTH;}
    if(!fake.blob.empty())std::memcpy(data,fake.blob.data(),fake.blob.size());
    *size=fake.blob.size();return ESP_OK;
}
/**
 * @brief Записывает16байт в pending через READWRITE handle; commit отдельно определяет видимость сохранённого значения.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int nvs_set_blob(nvs_handle_t handle,const char* key,const void* data,std::size_t size) {
    assert(handle==42&&fake.alive&&fake.mode==NVS_READWRITE&&!std::strcmp(key,"cfg_v1")&&size==16);++fake.set;
    if(fake.setError)return fake.setError;
    const auto* bytes=static_cast<const std::uint8_t*>(data);fake.pending.assign(bytes,bytes+size);return ESP_OK;
}
/**
 * @brief Фиксирует pending только при успешном commit; позволяет доказать обработку отказа без ложного сохранения.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int nvs_commit(nvs_handle_t handle) {assert(handle==42&&fake.alive);++fake.commit;if(fake.commitError)return fake.commitError;fake.blob=fake.pending;return ESP_OK;}
/**
 * @brief Проверяет действующий handle и освобождает его ровно один раз, сохраняя счётчик балансировки.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
void nvs_close(nvs_handle_t handle) {assert(handle==42&&fake.alive);++fake.close;fake.alive=false;}
/**
 * @brief Проверяет полный безопасный набор defaults после отсутствия или повреждения NVS:volume1, mute и NTSC.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
static void quiet(const ConsoleSettings& s){assert(s.volume==1&&s.muted&&!s.swapAB&&s.showFps&&s.region==RegionMode::Ntsc);}
/**
 * @brief Проверяет настоящий SettingsStore на отказах init/open/get/set/commit, неверных blob и повторном чтении; erase запрещён.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int main() {
    SettingsStore store;ConsoleSettings s{};unsigned cases=0;
    fake={};fake.openError=ESP_ERR_NVS_NOT_FOUND;s.volume=100;s.muted=false;assert(store.load(s));quiet(s);assert(!*store.errorText()&&!fake.close);++cases;
    fake={};fake.getError=ESP_ERR_NVS_NOT_FOUND;assert(store.load(s));quiet(s);assert(fake.close==1&&!fake.set&&!fake.commit);++cases;
    for(int error:{ESP_ERR_NVS_NO_FREE_PAGES,ESP_ERR_NVS_NEW_VERSION_FOUND,ESP_FAIL}) {
        fake={};fake.initError=error;s.volume=100;s.muted=false;assert(!store.load(s));quiet(s);assert(*store.errorText()&&!fake.open&&!fake.close);++cases;
        fake={};fake.initError=error;assert(!store.save(ConsoleSettings{}));assert(!fake.open&&!fake.close);++cases;
    }
    fake={};fake.openError=ESP_FAIL;assert(!store.load(s));quiet(s);assert(!fake.close);++cases;
    fake={};fake.openError=ESP_FAIL;assert(!store.save(s)&&!fake.close);++cases;
    fake={};fake.getError=ESP_FAIL;assert(!store.load(s));quiet(s);assert(fake.close==1&&!fake.alive);++cases;
    for(unsigned size:{0U,15U,17U,1000U}){fake={};fake.blob.resize(size,0);assert(!store.load(s));quiet(s);assert(fake.close==1&&!fake.alive);++cases;}
    fake={};EncodedSettings bytes{};assert(encodeSettings(s,bytes));bytes[6]=100;fake.blob.assign(bytes.begin(),bytes.end());assert(!store.load(s));quiet(s);assert(fake.close==1);++cases;
    fake={};fake.setError=ESP_FAIL;assert(!store.save(s)&&fake.close==1&&!fake.commit&&!fake.alive);++cases;
    fake={};fake.commitError=ESP_FAIL;assert(!store.save(s)&&fake.close==1&&fake.commit==1&&!fake.alive);++cases;
    fake={};ConsoleSettings bad{};bad.volume=255;assert(!store.save(bad)&&!fake.init&&!fake.open&&!fake.set);++cases;
    fake={};ConsoleSettings desired{};desired.volume=100;desired.muted=false;desired.swapAB=true;desired.showFps=false;desired.region=RegionMode::Pal;
    assert(store.save(desired)&&fake.set==1&&fake.commit==1&&fake.close==1&&!fake.alive);
    assert(store.load(s)&&s.volume==100&&!s.muted&&s.swapAB&&!s.showFps&&s.region==RegionMode::Pal);
    assert(fake.close==2&&fake.open==2&&!fake.alive&&!*store.errorText());++cases;
    // Изменение defaults не должно заменять корректно сохранённый выбор AUTO.
    desired.region=RegionMode::Auto;
    assert(store.save(desired)&&store.load(s)&&s.region==RegionMode::Auto&&s.effectivePal(true));
    assert(fake.close==4&&fake.open==4&&!fake.alive&&!*store.errorText());++cases;
    std::printf("SETTINGS_STORE_PASS cases=%u exact_namespace_key no_erase balanced_handles\n",cases);
}
