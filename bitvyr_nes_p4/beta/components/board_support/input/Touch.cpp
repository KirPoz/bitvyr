#include "../BoardInternal.h"

#include <initializer_list>
#include "esp_log.h"
#include "freertos/task.h"

namespace nes::platform {
namespace {
/**
 * @brief Читает регистр GT911 с двухбайтовым адресом по I2C.
 * @details Адрес передаётся старшим байтом первым; выходной массив принадлежит вызывающему коду, ожидание ограничено 30 мс.
 * @param device Дескриптор зарегистрированного I2C-устройства; владение не передаётся.
 * @param reg Адрес аппаратного регистра выбранного устройства.
 * @param data Буфер данных вызывающего кода достаточного размера.
 * @param size Размер данных/поля; единица задаётся протоколом текущей функции.
 * @return Код ESP-IDF от операции transmit/receive.
 */
esp_err_t readRegister(i2c_master_dev_handle_t device, std::uint16_t reg, void* data, std::size_t size) noexcept {
    const std::uint8_t address[]{static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg)};
    return i2c_master_transmit_receive(device, address, sizeof(address), static_cast<std::uint8_t*>(data), size, 30);
}
}

/**
 * @brief Находит GT911 по одному из штатных I2C-адресов.
 * @details Не переключает RST/INT; до пяти ограниченных попыток с паузой. Неуспешно прочитанное устройство удаляется перед следующей попыткой.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return ESP_OK либо последняя/неустранимая ошибка I2C.
 */
esp_err_t initializeTouch(BoardState& state) noexcept {
    // INT и RST намеренно не меняются. После тёплого запуска может потребоваться
    // ограниченная пауза, прежде чем один из адресов GT911 ответит.
    constexpr unsigned attempts = 5;
    esp_err_t lastError = ESP_ERR_NOT_FOUND;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        for (const std::uint8_t address : {0x5D, 0x14}) {
            auto error = i2c_master_probe(state.i2c, address, 40);
            if (error != ESP_OK) {
                if (error != ESP_ERR_NOT_FOUND && error != ESP_ERR_TIMEOUT) return error;
                if (error != ESP_ERR_NOT_FOUND) lastError = error;
                continue;
            }
            i2c_device_config_t device{};
            device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            device.device_address = address;
            device.scl_speed_hz = 400000;
            error = i2c_master_bus_add_device(state.i2c, &device, &state.touch);
            if (error != ESP_OK) return error;
            char product[5]{};
            error = readRegister(state.touch, 0x8140, product, 4);
            if (error == ESP_OK) {
                ESP_LOGI("nes_touch", "GT911 address=0x%02x product=%.4s attempt=%u", address, product, attempt + 1);
                return ESP_OK;
            }
            // После ошибки чтения удаляем зарегистрированное устройство перед новой попыткой.
            const auto removed = i2c_master_bus_rm_device(state.touch);
            if (removed != ESP_OK) return removed;
            state.touch = nullptr;
            lastError = error;
            if (error == ESP_ERR_INVALID_ARG || error == ESP_ERR_INVALID_STATE) return error;
        }
        if (attempt + 1 < attempts) {
            ESP_LOGW("nes_touch", "GT911 retry attempt=%u/%u last_error=0x%x", attempt + 1, attempts, unsigned(lastError));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    return lastError;
}

/**
 * @brief Получает проверенный снимок контактов в координатах 800×480.
 * @details Синхронный вариант подтверждает статус GT911 и проверяет координаты/дубликаты ID. Фоновый вариант копирует публикацию под коротким lock и отвергает снимок старше 40 мс.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param contacts Выходной ограниченный снимок контактов; при ошибке сбрасывается.
 * @return true для достоверного снимка, в том числе пустого; false при ошибке.
 */
bool readTouchContacts(BoardState& state, TouchContacts& contacts) noexcept {
    contacts={};
    /**
     * @brief Отвергает неудачный опрос и старый снимок, исключая залипшее нажатие.
     * @return false после очистки выходных и сохранённых контактов.
     */
    const auto failed=[&state,&contacts]() noexcept {
        state.lastTouches={};state.touchSampleValid=false;contacts={};return false;
    };
    std::uint8_t status = 0;
    if (readRegister(state.touch, 0x814E, &status, 1) != ESP_OK) return failed();
    if (!(status & 0x80)) {
        if(!state.touchSampleValid)return false;
        contacts=state.lastTouches;
        return true;
    }
    const unsigned count = status & 0x0F;
    bool result = count<=TouchContacts::capacity;
    if (count && result) {
        std::uint8_t records[TouchContacts::capacity*8]{};
        result = readRegister(state.touch,0x814F,records,count*8)==ESP_OK;
        if (result) {
            std::uint16_t seen=0;
            for(unsigned i=0;i<count;++i) {
                const auto* record=records+i*8;
                const unsigned id=record[0]&15;
                const unsigned nativeX=record[1]|(static_cast<unsigned>(record[2])<<8);
                const unsigned nativeY=record[3]|(static_cast<unsigned>(record[4])<<8);
                if(nativeX>=480||nativeY>=800||(seen&(1U<<id))){result=false;break;}
                seen=static_cast<std::uint16_t>(seen|(1U<<id));
                contacts.points[i]={static_cast<std::uint16_t>(nativeY),
                    static_cast<std::uint16_t>(479-nativeX),true,static_cast<std::uint8_t>(id)};
            }
            if(result)contacts.count=static_cast<std::uint8_t>(count);
        }
    }
    const std::uint8_t acknowledge[]{0x81, 0x4E, 0x00};
    result = i2c_master_transmit(state.touch, acknowledge, sizeof(acknowledge), 30) == ESP_OK && result;
    if(!result)return failed();
    state.lastTouches=contacts;state.touchSampleValid=true;
    return true;
}


/**
 * @brief Выбирает первый контакт из полного проверенного снимка.
 * @details Не обещает наличие нажатия при успешном чтении; при отсутствии контактов возвращает обнуленную точку.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param point Выходная точка первого контакта либо обнулённое отсутствие нажатия.
 * @return Успех чтения снимка, а не признак pressed.
 */
bool readTouch(BoardState& state,TouchPoint& point) noexcept {
    TouchContacts contacts;
    point={};
    if(!readTouchContacts(state,contacts))return false;
    if(contacts.count)point=contacts.points[0];
    return true;
}

} // пространство имён nes::platform
