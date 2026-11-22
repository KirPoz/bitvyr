/**
 * @file BufferedUart.cpp
 * @brief Ограниченный UART0/VFS proxy: пачки TX, точный CRLF-префикс, завершение без рекурсивного лога.
 */
#include "BufferedUart.h"
#include <ctime>
#include "driver/uart.h"
#include "driver/esp_private/uart_vfs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

extern "C" 
    /**
     * @brief Объявляет оригинальную SDK-точку VFS для GNU linker --wrap; это внешний entrypoint, а не неиспользуемая функция.
     */
    const esp_vfs_fs_ops_t* __real_esp_vfs_uart_get_vfs(void);

namespace nes::app::console {
namespace {
constexpr auto port=UART_NUM_0;
constexpr int rxBytes=2048,txBytes=4096;
constexpr std::size_t emptyTxPayload=4068,rawChunk=256;
constexpr std::int64_t activationTimeoutUs=100000,flushTimeoutUs=1000000;
/** @brief Статическое состояние UART proxy и рекурсивного mutex; таблица VFS неизменяема после установки и живёт до reboot. */
struct Context final {
    const esp_vfs_fs_ops_t* original;
    alignas(esp_vfs_fs_ops_t) unsigned char proxyStorage[sizeof(esp_vfs_fs_ops_t)];
    const esp_vfs_fs_ops_t* proxy;
    StaticSemaphore_t mutexStorage;
    SemaphoreHandle_t mutex;
    bool initialized,attached,attempted,active,pendingLf;
};
Context context{}; // Статический POD: без конструктора, heap и процедуры уничтожения.

/**
 * @brief Разрешает proxy только в работающей задаче RTOS; ISR/ранний старт используют исходный SDK VFS.
 */
bool taskContext() noexcept {
    return !xPortInIsrContext()&&xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED;
}
/** @brief RAII-владение рекурсивным mutex единственного UART TX-производителя. */
struct Guard final {
    bool held;
    /**
     * @brief Захватывает рекурсивный mutex единственного UART-производителя; held подтверждает успешный захват.
     */
    Guard() noexcept:held(xSemaphoreTakeRecursive(context.mutex,portMAX_DELAY)==pdTRUE){}
    /**
     * @brief Возвращает захваченный рекурсивный mutex при любом выходе из записи/flush.
     */
    ~Guard(){if(held)xSemaphoreGiveRecursive(context.mutex);}
};
/**
 * @brief Досылает сохранённый LF после ранее принятого CR, не меняя порядок протокольных байтов.
 */
bool flushPendingLf() noexcept {
    if(!context.pendingLf)return true;
    const char lf='\n';
    if(uart_write_bytes(port,&lf,1)!=1){errno=EIO;return false;}
    context.pendingLf=false;return true;
}
/**
 * @brief Дожидается пустого программного TX ring и аппаратного окончания до абсолютного deadline. Ошибки переводятся в errno, ожидание отдаёт tick RTOS.
 * @param deadline Абсолютный host-срок в микросекундах esp_timer; после него ожидание завершается ошибкой.
 */
bool drain(std::int64_t deadline) noexcept {
    // Для закреплённого SDK пустой ring:4096-8-(8+align4(12))=4068;
    // любая непустая ветвь даёт не более2*2040-20=4060. Элементы, заимствованные ISR,
    // недоступны до возврата. Mutex владельца запрещает новые записи.
    for(;;){
        std::size_t free=0;
        if(uart_get_tx_buffer_free_size(port,&free)!=ESP_OK){errno=EIO;return false;}
        if(free==emptyTxPayload){
            if(context.pendingLf){
                if(esp_timer_get_time()>=deadline){errno=ETIMEDOUT;return false;}
                // Пустой программный ring и единственный производитель гарантируют, что этот
                // байт не будет ждать места в очереди за пределами срока drain.
                if(!flushPendingLf())return false;
                continue;
            }
            break;
        }
        if(esp_timer_get_time()>=deadline){errno=ETIMEDOUT;return false;}
        vTaskDelay(1);
    }
    const auto remaining=deadline-esp_timer_get_time();
    if(remaining<=0){errno=ETIMEDOUT;return false;}
    const auto millis=static_cast<std::uint32_t>((remaining+999)/1000);
    TickType_t ticks=pdMS_TO_TICKS(millis);if(!ticks)ticks=1;
    const auto error=uart_wait_tx_done(port,ticks);
    if(error!=ESP_OK){errno=error==ESP_ERR_TIMEOUT?ETIMEDOUT:EIO;return false;}
    return true;
}
/**
 * @brief Перенаправляет запись UART через ограниченные пачки с CRLF. Возвращает точный принятый исходный префикс, сохраняя частичный LF до следующего write/fsync.
 * @param opaque Контекст исходного SDK VFS, передаваемый без изменения.
 * @param fd Дескриптор UART; чужие дескрипторы направляются исходному VFS.
 * @param bytes Заимствованный вход из size байтов; nullptr допустим только при нулевом размере.
 * @param size Длина исходного write в байтах; CRLF-преобразование не увеличивает возвращаемый счётчик.
 */
ssize_t proxyWrite(void* opaque,int fd,const void* bytes,std::size_t size){
    if(fd!=port||!taskContext())return context.original->write_p(opaque,fd,bytes,size);
    Guard guard;if(!guard.held){errno=EBUSY;return -1;}
    if(!context.active)return context.original->write_p(opaque,fd,bytes,size);
    if(!size)return 0;
    if(!bytes||size>static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())){errno=EINVAL;return -1;}
    if(!flushPendingLf())return -1;
    const auto* source=static_cast<const unsigned char*>(bytes);
    unsigned char encoded[rawChunk*2];std::size_t offset=0;
    while(offset<size){
        const auto count=size-offset<rawChunk?size-offset:rawChunk;
        std::size_t encodedSize=0;
        for(std::size_t i=0;i<count;++i){
            const auto c=source[offset+i];if(c=='\n')encoded[encodedSize++]='\r';encoded[encodedSize++]=c;
        }
        std::size_t sent=0;
        while(sent<encodedSize){
            const auto accepted=uart_write_bytes(port,encoded+sent,encodedSize-sent);
            if(accepted<=0||static_cast<std::size_t>(accepted)>encodedSize-sent){
                // Возвращается точно принятый исходный префикс. Если драйвер остановился
                // между CR и LF, один LF сохраняется в статическом состоянии владельца;
                // следующий write/fsync обязан дослать его раньше любого последующего байта.
                std::size_t raw=0,boundary=0;
                while(raw<count){
                    const std::size_t width=source[offset+raw]=='\n'?2U:1U;
                    if(boundary+width<=sent){boundary+=width;++raw;continue;}
                    if(width==2U&&sent==boundary+1U){context.pendingLf=true;++raw;}
                    break;
                }
                errno=EIO;const auto done=offset+raw;
                return done?static_cast<ssize_t>(done):-1;
            }
            sent+=static_cast<std::size_t>(accepted);
        }
        offset+=count;
    }
    return static_cast<ssize_t>(size);
}
/**
 * @brief Подтверждает окончание накопленного UART-вывода с bounded deadline либо передаёт вызов исходному VFS.
 * @param opaque Контекст исходного SDK VFS, передаваемый без изменения.
 * @param fd Дескриптор UART; чужие дескрипторы направляются исходному VFS.
 */
int proxyFsync(void* opaque,int fd){
    if(fd!=port||!taskContext())return context.original->fsync_p(opaque,fd);
    Guard guard;if(!guard.held){errno=EBUSY;return -1;}
    if(!context.active)return context.original->fsync_p(opaque,fd);
    return drain(esp_timer_get_time()+flushTimeoutUs)?0:-1;
}
}

/**
 * @brief Возвращает статический proxy-набор VFS операций; исходные остальные callbacks сохраняются.
 */
const esp_vfs_fs_ops_t* proxyOperations() noexcept {
    // В закреплённом SDK первый вызов — однопоточный console init119 на CPU0, до
    // создания stdio/init120 и задач производителей. Последующие вызовы
    // возвращают неизменяемую таблицу; при активных писателях её не копируют заново.
    if(!context.initialized){
        context.initialized=true;context.original=__real_esp_vfs_uart_get_vfs();
        if(!context.original||!context.original->write_p||!context.original->fsync_p)return context.original;
        context.mutex=xSemaphoreCreateRecursiveMutexStatic(&context.mutexStorage);
        if(!context.mutex)return context.original;
        const auto* original=context.original;
        // Поля SDK const. Полный неизменяемый агрегат конструируется один раз
        // в статической выровненной памяти: без const_cast, перезаписи объекта и heap.
        context.proxy=::new(static_cast<void*>(context.proxyStorage)) esp_vfs_fs_ops_t{
            {proxyWrite},{original->lseek_p},{original->read_p},
            {original->pread_p},{original->pwrite_p},{original->open_p},
            {original->close_p},{original->fstat_p},{original->fcntl_p},
            {original->ioctl_p},{proxyFsync},
#ifdef CONFIG_VFS_SUPPORT_DIR
            original->dir,
#endif
#ifdef CONFIG_VFS_SUPPORT_TERMIOS
            original->termios,
#endif
#if CONFIG_VFS_SUPPORT_SELECT
            original->select,
#endif
        };
        context.attached=true;
    }
    return context.attached?context.proxy:context.original;
}

/**
 * @brief Однократно подключает проверенный UART0/VFS proxy после RTOS. Неподдерживаемый SDK/состояние оставляет исходный путь без небезопасной подмены.
 */
void initializeBufferedUart() noexcept {
    if(!taskContext())return;
    if(!context.attached){ESP_LOGI("nes_uart","NES_UART_TX mode=synchronous reason=proxy_unavailable");return;}
    {
        Guard guard;if(!guard.held||context.attempted)return;
        context.attempted=true;
    }
    // Выделение и настройка драйвера могут печатать диагностику SDK. Они идут вне
    // proxy-mutex, пока все писатели ещё используют исходный синхронный путь.
    if(uart_is_driver_installed(port)){ESP_LOGI("nes_uart","NES_UART_TX mode=preexisting");return;}
    const auto optional=uart_driver_install(port,rxBytes,txBytes,0,nullptr,0);
    if(optional!=ESP_OK){
        if(!uart_is_driver_installed(port))ESP_ERROR_CHECK(uart_driver_install(port,rxBytes,0,0,nullptr,0));
        ESP_LOGI("nes_uart","NES_UART_TX mode=synchronous reason=buffered_install error=%d",int(optional));return;
    }
    bool activated=false;
    {
        Guard guard;
        if(guard.held){
            std::size_t free=0;
            if(uart_get_tx_buffer_free_size(port,&free)==ESP_OK&&free==emptyTxPayload&&
               drain(esp_timer_get_time()+activationTimeoutUs)){
                context.active=true;activated=true;
            }
        }
    }
    // Внутри proxy/drain/захваченного mutex запрещён рекурсивный лог. Все ресурсы
    // живут до перезапуска, включая неактивное необязательное выделение TX.
    ESP_LOGI("nes_uart","NES_UART_TX mode=%s rx=2048 tx=4096",activated?"bulk":"synchronous");
}
}
extern "C" 
    /**
     * @brief Linker entrypoint --wrap: возвращает собственный VFS proxy только после безопасной инициализации, иначе исходный SDK интерфейс.
     */
    const esp_vfs_fs_ops_t* __wrap_esp_vfs_uart_get_vfs(void){return nes::app::console::proxyOperations();}
