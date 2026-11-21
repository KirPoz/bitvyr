/**
 * @file TimingMmc5Lazy.inl
 * @brief Совместимые заглушки прежнего отложенного MMC5 replay.
 * @details Текущий профиль не накапливает этот долг. Методы и поля оставлены,
 * чтобы не менять интерфейс и layout у существующих потребителей.
 */
namespace nes::core {
/** @brief Ничего не материализует: текущий профиль не создаёт отложенный долг. */
void TimingPpu::materializeMmc5Lazy() noexcept {
}
/** @brief Отклоняет добавление без изменения состояния и гостевого времени. */
bool TimingPpu::tryAppendMmc5Lazy(unsigned dots,bool clockFirstM2) noexcept {
    static_cast<void>(dots);static_cast<void>(clockFirstM2);return false;
}
} // namespace nes::core
