# ESP32 Wi-Fi Relay

Учебный проект: Wi-Fi-реле на ESP32. Разработка на Ubuntu, ESP-IDF.
Архитектура зафиксирована в ADR-0001…0009, термины — в `GLOSSARY.md`.
Реализованы реле, Wi-Fi STA с fallback в AP, captive portal, HTTP REST API
и физическая кнопка. Проверка — ручной smoke-тест по ADR-0009.

## Что входит в проект

- Управление одним реле по HTTP REST (Bearer-токен, без TLS).
- Подключение к домашней сети (STA) с fallback в собственную точку доступа (AP).
- Первичная настройка Wi-Fi через captive portal: форма SSID/пароль + кнопка
  toggle реле на той же странице.
- Физическая кнопка: короткое нажатие — toggle, 5 с — сброс Wi-Fi-креденшелов
  и переход в AP.
- Диагностический эндпоинт `/api/status` (read-only, без токена).
- Ручной smoke-тест как Definition of Done.

## Что НЕ входит в проект

Осознанные ограничения, а не «не успели»:

- MQTT и интеграция с Home Assistant (ADR-0001).
- TLS и защита канала от прослушивания (ADR-0002).
- OTA-обновление прошивки (ADR-0003).
- Персистентность состояния реле между перезагрузками (ADR-0008).
- Автотесты, включая host-based Unity (ADR-0009).
- Версионирование API (`/api/v1/`) (ADR-0001).
- Несколько каналов реле (ADR-0005).

## Требования

**Аппаратное обеспечение**
- ESP32 DevKitC V4 (ESP32-WROOM-32, 4 MB flash).
- Модуль реле с оптопарой, управление активным низким уровнем.
- Питание от блока питания (постоянный свет LED в режиме «STA подключён»
  допустим).

**Распиновка учебного стенда**

| Назначение | GPIO | Примечание |
|------------|------|------------|
| Реле       | 26   | Активный низкий уровень; при старте OFF (ADR-0008) |
| Кнопка     | 27   | Замыкает на GND, внутренняя подтяжка вверх |
| LED статуса| 2    | Встроенный LED DevKitC V4 |

Номера GPIO заданы в `sdkconfig.defaults` и меняются через `idf.py menuconfig`
(`CONFIG_RELAY_GPIO`, `CONFIG_BUTTON_GPIO`, `CONFIG_LED_GPIO`).

**Программное обеспечение**
- Ubuntu 22.04 / 24.04.
- ESP-IDF v5.x, установленный по официальной инструкции.

## API

Все управляющие эндпоинты требуют заголовок `Authorization: Bearer <token>`.
Токен задаётся через Kconfig (`CONFIG_RELAY_API_TOKEN`) и меняется пересборкой.

| Метод | Путь                   | Токен        | Назначение                                        |
|-------|------------------------|--------------|---------------------------------------------------|
| GET   | `/api/relay`           | обязателен   | Текущее логическое состояние реле                 |
| PUT   | `/api/relay`           | обязателен   | Установка состояния, тело `{"state":"on"\|"off"}` |
| GET   | `/api/status`          | не требуется | Диагностика: режим Wi-Fi, IP, RSSI, uptime        |
| POST  | `/api/provision`       | не требуется | Запись SSID/пароля Wi-Fi в NVS, только AP         |
| GET   | `/`                    | не требуется | Captive-страница, только AP                       |
| GET   | `/generate_204`        | не требуется | Captive-probing Android, только AP                |
| GET   | `/hotspot-detect.html` | не требуется | Captive-probing iOS, только AP                    |
| GET   | `/connecttest.txt`     | не требуется | Captive-probing Windows, только AP                |

Коды ответов: `200`, `400` (невалидное тело), `401` (нет/неверный токен),
`404`, `405`, `500`. Тело ошибки — JSON `{"error":"..."}`.

В AP-режиме DNS-responder отвечает на A-запросы адресом точки, а probing-URL
редиректят на встроенную страницу. На странице есть форма SSID/пароль и toggle
реле; для toggle нужно вручную ввести Bearer-токен. `POST /api/provision`
принимает `application/x-www-form-urlencoded` с полями `ssid` и `password`
и не требует токена (ADR-0002, ADR-0006). В STA-режиме captive-эндпоинты
недоступны.

Примеры:

```bash
curl -H "Authorization: Bearer $TOKEN" \
     -X PUT -d '{"state":"on"}' \
     http://<ip-устройства>/api/relay

# Только в AP-режиме, без токена:
curl -X POST -d 'ssid=<ssid>&password=<password>' \
     http://192.168.4.1/api/provision
```

## Структура проекта

```
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/           # точка входа, инициализация компонентов
├── components/
│   ├── relay/      # логика реле, мьютекс состояния
│   ├── wifi_mgr/   # STA + AP fallback, backoff, provisioning
│   ├── http_api/   # esp_http_server, обработчики, captive portal
│   └── button/     # антидребезг, короткое/длинное нажатие
├── docs/
│   └── adr/        # ADR-0001 … ADR-0009
└── GLOSSARY.md
```

Компоненты содержат публичные заголовки в `include/` и реализацию.
Границы компонентов зафиксированы в ADR: реле — в `relay`, Wi-Fi и NVS —
в `wifi_mgr`, HTTP и captive portal — в `http_api`.

## Сборка и прошивка

```bash
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py menuconfig      # CONFIG_RELAY_API_TOKEN, CONFIG_RELAY_AP_PASSWORD
idf.py build flash monitor
```

Или одной командой (сборка, прошивка и монитор):

```bash
./flash.sh                  # порт определяется автоматически
./flash.sh /dev/ttyUSB0     # явный порт
```

Прошивка идёт на 460800 бод, монитор — на 115200 (консоль ESP32 по умолчанию).
При необходимости скорости задаются через `FLASH_BAUD` и `MONITOR_BAUD`.

## Документация

- [`HARDWARE.md`](HARDWARE.md) — распиновка, подключение реле/кнопки, питание
  и меры безопасности.
- [`docs/adr/`](docs/adr/) — Architecture Decision Records, 0001…0009.
- [`GLOSSARY.md`](GLOSSARY.md) — термины проекта; при изменении решения
  сначала правится ADR, потом глоссарий.
- Smoke-тест — чек-лист в [ADR-0009](docs/adr/0009-manual-smoke-test-only.md).

## Статус

- [x] Требования уточнены, архитектура зафиксирована в ADR-0001…0009.
- [x] Глоссарий согласован с ADR.
- [x] Скаффолдинг проекта (CMake, интерфейсы компонентов, `sdkconfig.defaults`).
- [x] Реализация компонентов: реле, Wi-Fi, HTTP API, captive portal, кнопка.
- [ ] Прохождение smoke-теста на железе и отчёт (ADR-0009).
