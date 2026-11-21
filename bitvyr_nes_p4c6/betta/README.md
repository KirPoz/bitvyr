# Bitvyr NES P4+C6

Проект ESP-IDF 6.1 для ESP32-P4. P4 готовит цифровые события APU, C6 рассчитывает PCM 48 кГц и возвращает его по Hosted SDIO. P4 передаёт PCM в I2S. Согласованный C6 worker обслуживает одновременно звук и Bluetooth.

[Проект](docs/01-project.md) · [Архитектура](docs/02-architecture.md) · [Сборка и управление](docs/03-guide.md) · [Результаты](docs/04-results.md) · [Задачи](docs/05-roadmap.md)

Условия использования находятся в [GNU AGPLv3](../../LICENSE), внешние компоненты перечислены в [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
