# Узкий NVS fake

Заголовки повторяют только API, которым пользуется настоящий SettingsStore. Реализации в SettingsStoreTests моделируют ошибки, доступ READONLY/READWRITE и баланс handle. Erase/deinit отсутствуют в интерфейсе, поэтому добавление стирания в адаптер не проходит native-сборку.
