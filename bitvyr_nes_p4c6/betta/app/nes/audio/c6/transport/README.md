# P4 bounded Hosted transport

RawChannel содержит два slot с request1400 и reply1792; release/acquire и
полный immutable key отделяют Main submit/poll от receive/sourceDone callbacks.
P4RawAsyncPort cold-выделяет Channel с align64 и проверяет оба конца HP DRAM,
internal/DMA/8BIT; одного INTERNAL недостаточно. Контекст boot-long не уничтожать
при неизвестном callback/remote владельце. P4RawRx перехватывает только SERIAL1,
передаёт SERIAL0 остальному Hosted и освобождает принятый SDK RX handle ровно раз.

Нужны прежние SDK/FreeRTOS и private Hosted headers плюс `hosted` overlay;
Root wiring добавляет --wrap=serial_ll_rx_handler. Main-only TX не ждёт и не
выделяет память; callback sourceDone не означает remote delivery или WorkerDone.
Host codec/native проверки не заменяют SDK ABI, HP, IRQ/stack и аппаратный drain.
