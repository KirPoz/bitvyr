# LCD, PPA и текст

Display.cpp готовит MIPI-DSI/ST7701, RGB565 и аппаратное масштабирование/поворот; Text.cpp рисует ограниченный ASCII5×7.

Main формирует заявки; callbacks публикуют только разрешённые события и семафоры.

RGB256×240 заимствован PPA до sourceDone; native480×800 — LCD до completion. Три native-буфера допускают безопасное перекрытие.

BoardState, PPA/LCD SDK. Проверка — native presentation state machine, отсутствие меню по краям и аппаратная стабильность.
