# Проверки настроек

[SettingsTests.cpp](SettingsTests.cpp) проверяет реальный codec: значения,
маски, регион, CRC и повреждённые payload. [SettingsStoreTests.cpp](SettingsStoreTests.cpp)
собирает NVS-адаптер с узким fake SDK: save/reload/отказы/баланс ресурсов.

Fake разрешает только нужные API/namespace/key и не стирает пользовательские
данные. Ошибки длины/version/reserved/ranges/commit не становятся успехом.
Fixtures владеют отдельным storage процесса и не обращаются к NVS платы.

Есть общий CTest и самостоятельный [CMake-вход](CMakeLists.txt).
Сгенерированные выходы располагаются в `build/native`, не в исходниках.

[Архитектура](../../docs/02-architecture.md) · [Сборка и использование](../../docs/03-guide.md).
