#include "Machine.h"

#include <cstdio>

/**
 * @brief Проверяет безопасный отказ stepFrame без картриджа и load с неверным16-байтовым заголовком.
 * @return 0 при обоих ожидаемых отказах;1/2 различают неверный RunStatus и LoadResult.
 */
int main() {
    nes::core::Machine machine;

    const auto status = machine.stepFrame();
    if (status != nes::core::RunStatus::NoCartridge) return 1;
    const unsigned char malformed[16]{};
    if (machine.load(malformed, sizeof(malformed)) != nes::core::LoadResult::InvalidHeader) return 2;
    std::puts("NES machine rejects missing and malformed cartridges.");
    return 0;
}
