# Captive portal: provisioning и toggle реле

**Status:** ready-for-agent
**Blocked by:** 02, 03

## What to build

В AP-режиме клиент при подключении автоматически попадает на страницу с формой
SSID/пароль и кнопкой toggle реле. Provisioning пишет креденшелы в NVS и уводит
в STA; toggle требует ручного ввода Bearer-токена.

## Acceptance criteria

- [ ] В AP-режиме DNS-responder отвечает IP точки на любой запрос
- [ ] Probing-URL `/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`
      редиректят на страницу (Android/iOS/Windows показывают captive-уведомление)
- [ ] Страница отдаётся встроенной строкой (без LittleFS); содержит форму
      SSID/пароль, поле токена и кнопку toggle
- [ ] `POST /api/provision` **без токена** пишет SSID/пароль в NVS и уводит
      в STA; при успехе страница подтверждает переход
- [ ] Toggle на странице работает только с валидным Bearer-токеном;
      без токена → `401`
- [ ] В STA-режиме captive portal недоступен
- [ ] Правка вёрстки требует пересборки (осознанно, ADR-0006)

## Не входит

- Смена токена и имени устройства из UI (ADR-0006 явно исключает)