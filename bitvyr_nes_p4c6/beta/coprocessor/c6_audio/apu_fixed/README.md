# C6 fixed APU adapter

Здесь сохранены реальные C6 отличия четырёх RootCore APU/renderer файлов и один
FixedFilter.cpp/.h. Их исходные SHA и patch находятся в `../recipe/source-map.json`
и `../patches/fixed-apu.patch`. Это собственная PCM-реконструкция без SDK в APU,
а не второе эмуляторное ядро. RootCore Types/AudioPacket/AudioPacketView и прочие
совпадающие fragments не копируются в этот пакет.

Renderer принадлежит одному C6 worker. Он получает immutable used-record view,
использует записанные DMC ответы и сохраняет прежние sequence/timeline/status
проверки. Переполнение фильтра отравляет результат; частичный PCM не публикуется
как valid. Эти API не разрешают освобождать ROM/wire буферы и не являются join.
Фиксированные таблицы и storage создаются прежним кодом; новых ресурсов перенос
не добавляет. Все consumers этого изменённого Apu.h должны собираться одной
C6-когортой. Смешивать его layout с RootCore Apu.h в одном ELF запрещено.

Проверка переноса: exact исходные байты/токены и projection ownership audit.
Свежая SDK/ABI/SU/HP/PCM/target проверка обязательна отдельно; старый ae1c не
переименовывается в новую сборку и не является доказательством60FPS.
