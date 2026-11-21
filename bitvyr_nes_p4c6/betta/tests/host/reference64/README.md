# Эталон expansion audio до exact32

Эти четыре test-only файла сохранены из собственных Sunsoft5B/VRC7 моделей перед заменой широкой арифметики. Изменены только имена классов и файлов с добавлением Wide; байты исходника после такой подстановки проверяются с `build/validation/mapper-expansion-20261118/audio32/before`.

Ответственность: независимый от оптимизации 64-битный oracle для `ExpansionAudioExact32Tests.cpp`. Он проверяет прежнюю portable модель, а не аппаратную точность OPLL/YM2149. Исходные границы качества FM, gain и 64-M2 boxcar сохранены. Нельзя переносить exact32 изменения в этот oracle одновременно с продуктовой реализацией.

Зависимости: только C++17 и standard library. Нет heap, SDK, ROM, COM или отдельного владельца потока. Объекты принадлежат одному test-потоку и не заимствуют внешние ресурсы. Несовпадение DAC, горизонтов, event tape или PCM завершает fixture с ненулевым кодом; автоматического исправления и retry нет.

Проверка: target `cartridge_expansion_audio_exact32` линкуется с текущим `NES::Core` и всеми PUBLIC определениями. Fixture исчерпывает 2 147 450 880 пар Q15×amplitude, bounded attack/phase kernels, затем исполняет полные random-MMIO модели и настоящий PCM renderer. До source-before: Sunsoft5BAudio.cpp SHA-256 `73ce907a3def174295c4f0876b7bab0a4b9e50772a7436965291e08aee154bde`, Vrc7Audio.cpp `528268c60646f40dfda1e55e94a245ecbd29d899bae45c00e6d6a465645bd799`.
