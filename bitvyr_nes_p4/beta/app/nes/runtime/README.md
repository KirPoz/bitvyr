# Видеоконвейер P4

FramePipeline публикует и потребляет два output-слота, проверяет полный ключ и хранит завершённый снимок.

produce — CPU1; consume, принятие кадра, PCM и снимок — CPU0. Передача через release/acquire и явный completion.

ROM/RGB/PCM заимствованы до join и sourceDone. Sticky fault блокирует принятие частичного кадра; destroyJoined отказывает при активном владельце.

Core, heap caps и RTOS-адаптер. Проверка — output/lease native-тесты и аппаратная сверка игровых сцен.
