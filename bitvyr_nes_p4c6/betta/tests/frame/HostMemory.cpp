/** @file HostMemory.cpp @brief HOST allocator/caps/time mocks для одного синхронного frame fixture. */
#include "esp_heap_caps.h"
#include <array>
#include <cstdlib>
#include <cstdint>
#include <malloc.h>
namespace {
/**
 * @brief Хранит начало, размер в байтах и mock capabilities одного живого HOST-блока.
 * @details Реестр ограничен 64 записями и используется одним потоком fixture.
 * Структура не освобождает память автоматически; p обнуляется при heap_caps_free.
 * Наличие capabilities отражает только регистрацию и не доказывает свойства ESP RAM.
 */
struct Allocation{void*p{};std::size_t bytes{};unsigned caps{};};
std::array<Allocation,64>allocations{};
/**
 * @brief Возвращает mock capability mask для зарегистрированного блока, содержащего p.
 * @details p заимствуется без разыменования и может указывать внутрь блока.
 * Полуинтервал включает начало и исключает конец; nullptr, неизвестный или уже
 * освобождённый адрес возвращает 0. Реестр и память не изменяются.
 */
unsigned caps(const void*p)noexcept{const auto x=reinterpret_cast<std::uintptr_t>(p);for(const auto&a:allocations){const auto b=reinterpret_cast<std::uintptr_t>(a.p);if(a.p&&x>=b&&x-b<a.bytes)return a.caps;}return 0;}
}
/**
 * @brief Выделяет HOST-блок bytes байтов и регистрирует переданную маску capability.
 * @details alignment задаётся в байтах и обязан удовлетворять _aligned_malloc;
 * значения меньше sizeof(void*) повышаются до этого минимума. Успешный указатель
 * принадлежит вызывающему до heap_caps_free. При отказе _aligned_malloc или
 * заполнении 64 записей возвращает nullptr; незарегистрированный блок освобождается.
 */
void*heap_caps_aligned_alloc(std::size_t alignment,std::size_t bytes,unsigned capability){
    if(alignment<sizeof(void*))alignment=sizeof(void*);
    void*p=_aligned_malloc(bytes,alignment);if(!p)return nullptr;
    for(auto&a:allocations)if(!a.p){a={p,bytes,capability};return p;}
    _aligned_free(p);return nullptr;
}
/**
 * @brief Освобождает точное начало зарегистрированного HOST-блока и очищает его запись.
 * @details nullptr допускается как пустая операция. Неизвестный адрес, адрес
 * внутри блока и повторное освобождение вызывают abort. После успеха указатель
 * недействителен; вызов не моделирует завершение DMA или работу кеша.
 */
void heap_caps_free(void*p){if(!p)return;for(auto&a:allocations)if(a.p==p){a={};_aligned_free(p);return;}std::abort();}
/**
 * @brief Возвращает наличие INTERNAL bit для зарегистрированного блока, содержащего p.
 * @details Принимает начало или внутренний адрес, не читает память по p;
 * для nullptr и неизвестного адреса возвращает false. Аппаратный HP банк не проверяется.
 */
bool esp_ptr_internal(const void*p){return (caps(p)&MALLOC_CAP_INTERNAL)!=0;}
/**
 * @brief Возвращает наличие SPIRAM bit для зарегистрированного блока, содержащего p.
 * @details Принимает заимствованный адрес без разыменования; nullptr и неизвестный
 * адрес дают false. Физическая PSRAM и доступ к ESP памяти не моделируются.
 */
bool esp_ptr_external_ram(const void*p){return (caps(p)&MALLOC_CAP_SPIRAM)!=0;}
/**
 * @brief Возвращает наличие DMA bit для зарегистрированного блока, содержащего p.
 * @details p заимствуется без чтения памяти; nullptr и неизвестный адрес дают false.
 * Проверка отражает только capability при выделении, не работу реального DMA/кеша.
 */
bool esp_ptr_dma_capable(const void*p){return (caps(p)&MALLOC_CAP_DMA)!=0;}
/**
 * @brief Возвращает наличие 8BIT bit для зарегистрированного блока, содержащего p.
 * @details Не разыменовывает p и не продлевает lifetime; nullptr и неизвестный
 * адрес дают false. Владение и своевременный heap_caps_free остаются у вызывающего.
 */
bool esp_ptr_byte_accessible(const void*p){return (caps(p)&MALLOC_CAP_8BIT)!=0;}
/**
 * @brief Возвращает следующий тик детерминированных mock часов вместо реального времени.
 * @details Статический счётчик начинается со 100, первый вызов возвращает 101,
 * каждый следующий увеличивает значение на 1. Возврат совместим с интерфейсом
 * микросекунд SDK, но не отражает walltime, задержки или HOST FPS.
 */
std::int64_t esp_timer_get_time(){static std::int64_t now=100;return ++now;}

/**
 * @brief Выделяет bytes байтов с выравниванием 8 через общий mock реестр.
 * @details capability сохраняется как переданная маска. Успешный результат
 * принадлежит вызывающему до heap_caps_free; nullptr означает отказ HOST allocator
 * или отсутствие свободной записи реестра.
 */
void*heap_caps_malloc(std::size_t bytes,unsigned capability){return heap_caps_aligned_alloc(8,bytes,capability);}

/**
 * @brief Возвращает постоянный mock free budget 1 MiB для диагностик pipeline.
 * @details Маска capabilities игнорируется; число не уменьшается от живых allocations
 * и не является измерением свободной HOST/ESP памяти.
 */
std::size_t heap_caps_get_free_size(unsigned){return 1024U*1024U;}
/**
 * @brief Возвращает постоянный mock largest-block budget 1 MiB.
 * @details Маска capabilities игнорируется; результат не зависит от реестра и
 * фрагментации и не подтверждает возможность реального HP/DMA выделения.
 */
std::size_t heap_caps_get_largest_free_block(unsigned){return 1024U*1024U;}
/**
 * @brief Возвращает постоянный mock minimum-free budget 1 MiB для logging.
 * @details Маска capabilities игнорируется; исторический минимум не учитывается,
 * состояние allocator не меняется и аппаратный watermark не измеряется.
 */
std::size_t heap_caps_get_minimum_free_size(unsigned){return 1024U*1024U;}
