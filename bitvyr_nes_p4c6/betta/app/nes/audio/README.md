# Аудио приложения

`PcmStreamGuard` — Main-only числовая проверка принятого PCM-потока: полные
поколения/lease/epoch/session, непрерывные кадры и M2, регион/capabilities,
выходной sample count и повторные partial-I2S предложения одного ticket.
Число сэмплов вычисляется по целой фазе `Apu::sampleRate / cpuRate`.
Wire-модель C6 требует общего PUBLIC `NES_CORE_EXPANSION_DAC_PACKET=1`;
native target регистрируется только в этом согласованном варианте Core.

Observer не владеет PCM и не читает его значения. Он не заменяет wireCRC,
проверку правильности синтеза, transport/session guards, actual join/CONSUMED
или передачу владения I2S. Отказ не меняет состояние и не освобождает память.
Повторное связывание нового ROM допускается внешним владельцем после настоящего
завершения старого потока; сброс числового observer это завершение не доказывает.

`c6` содержит перенесённый экспериментальный transport/protocol/runtime и
отдельную qualification policy. Подключение к обычному runtime выполняется
поэтапно; само наличие пакета не объявляет backend или60FPS готовыми.

Проверка: `PcmStreamGuardTests.cpp`; реальные временные ключи и sample counts
берутся из сохранённых CV/JB input-wire captures. Root SDK и аппаратные проверки
относятся к отдельно зафиксированному профилю приложения.
