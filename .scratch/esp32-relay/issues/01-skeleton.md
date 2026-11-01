# Скелет проекта собирается и запускается

**Status:** ready-for-agent
**Blocked by:** None

## What to build

Пустой, но собираемый проект ESP-IDF на Ubuntu. `idf.py build flash monitor`
проходит без ошибок, в мониторе видно стартовое сообщение. Созданы компоненты
с заголовками без реализации — границы будущих срезов зафиксированы до первой
строки кода.

## Acceptance criteria

- [ ] `idf.py set-target esp32 && idf.py build` проходит на чистом Ubuntu
- [ ] Структура: `main/`, `components/{relay,wifi_mgr,http_api,button}`,
      у каждого — `CMakeLists.txt` и `.h` с интерфейсами (реализация — заглушка)
- [ ] `sdkconfig.defaults` содержит `CONFIG_RELAY_API_TOKEN` и
      `CONFIG_RELAY_AP_PASSWORD`
- [ ] GPIO для LED/кнопки/реле заданы в `sdkconfig.defaults` (значения — по
      выбранной плате)
- [ ] При прошивке в `idf.py monitor` видно сообщение о старте с версией
- [ ] README обновлён: точная модель платы и номера GPIO

## Не входит

- Реальная логика Wi-Fi, HTTP, реле, кнопки.