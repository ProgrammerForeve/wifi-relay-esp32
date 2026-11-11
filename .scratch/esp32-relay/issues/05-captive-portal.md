# Captive portal: provisioning, toggle и демо-консоль

**Status:** ready-for-agent
**Blocked by:** 02, 03

## What to build
В AP-режиме клиент попадает на страницу с демо-панелью (состояние реле,
режим Wi-Fi, IP, RSSI, uptime), кнопками управления и формой provisioning.
Toggle в AP работает без токена (ADR-0010). CSS для читаемости на проекторе.

## Acceptance criteria
- [ ] DNS-responder, probing-URL (`/generate_204`, `/hotspot-detect.html`,
      `/connecttest.txt`) редиректят на страницу
- [ ] Страница отдаётся встроенной строкой, содержит CSS и панель с индикатором
      состояния (ON/OFF крупно), режимом Wi-Fi, IP, RSSI, uptime
- [ ] Кнопка «Обновить статус» → `GET /api/status`, обновляет панель
- [ ] Кнопка «Обновить состояние реле» → `GET /api/relay`, обновляет индикатор
- [ ] Кнопка toggle → `PUT /api/relay`, без токена в AP
- [ ] Форма provisioning: SSID, пароль; POST /api/provision пишет в NVS,
      устройство уходит в STA
- [ ] В STA-режиме captive-страница недоступна
- [ ] Правка вёрстки требует пересборки (осознанно, ADR-0006)

## Не входит
- WiFi scan и автозаполнение SSID (тикет 07)