#include "http_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arpa/inet.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "wifi_mgr.h"

#define AUTH_HEADER_MAX_LEN 256
#define RELAY_BODY_MAX_LEN 128
#define PROVISION_BODY_MAX_LEN 512
#define DNS_BODY_MAX_LEN 512
#define DNS_TTL_SECONDS 60
#define PROVISION_SWITCH_DELAY_MS 500
#define DNS_TASK_STACK 4096
#define DNS_TASK_PRIO 4

static const char *TAG = "http_api";

static httpd_handle_t s_server;
static volatile bool s_running;
static volatile bool s_dns_task_started;

static const char CAPTIVE_PAGE_HTML[] =
    "<!DOCTYPE html>"
    "<html lang='ru'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>ESP32 Relay</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:480px;margin:0 auto;"
    "padding:1rem;background:#f0f2f5;color:#222;}"
    "h1{font-size:1.25rem;margin:0 0 1rem;}"
    ".card{background:#fff;border-radius:12px;padding:1rem;"
    "margin-bottom:1rem;box-shadow:0 2px 6px rgba(0,0,0,.08);}"
    ".state{font-size:3rem;font-weight:800;text-align:center;"
    "padding:.5rem 0;letter-spacing:.1em;}"
    ".state.on{color:#1b8a4c;}"
    ".state.off{color:#c62828;}"
    ".state.unknown{color:#999;}"
    ".row{display:flex;gap:.5rem;margin-top:.75rem;flex-wrap:wrap;}"
    ".row button{flex:1;min-width:120px;}"
    "button{padding:.75rem 1rem;border:0;border-radius:8px;"
    "background:#1976d2;color:#fff;font-size:1rem;font-weight:600;"
    "cursor:pointer;}"
    "button.alt{background:#607d8b;}"
    "button:active{opacity:.8;}"
    "button:disabled{opacity:.5;cursor:default;}"
    "input,select{width:100%;padding:.6rem;margin:.3rem 0;"
    "border:1px solid #cfd8dc;border-radius:8px;font-size:1rem;"
    "box-sizing:border-box;}"
    "label{display:block;margin-top:.5rem;font-size:.85rem;color:#555;}"
    ".info{font-size:.85rem;line-height:1.5;color:#455a64;"
    "white-space:pre-line;font-family:monospace;}"
    ".muted{font-size:.75rem;color:#888;text-align:center;margin-top:1rem;}"
    "</style></head><body>"
    "<h1>ESP32 Wi-Fi Relay</h1>"
    "<div class='card'>"
    "<div class='state unknown' id='state'>?</div>"
    "<div class='row'>"
    "<button id='toggleBtn' onclick='toggleRelay()'>ON / OFF</button>"
    "<button class='alt' onclick='refreshAll()'>Refresh</button>"
    "</div></div>"
    "<div class='card'>"
    "<div class='info' id='info'>loading...</div>"
    "</div>"
    "<div class='card'>"
    "<label for='ssid'>Wi-Fi network</label>"
    "<select id='ssid'>"
    "<option value=''>&mdash; press Scan &mdash;</option>"
    "</select>"
    "<div class='row'>"
    "<button class='alt' id='scanBtn' onclick='scanNetworks()'>Scan</button>"
    "</div>"
    "<label for='password'>Password</label>"
    "<input id='password' type='password' maxlength='64' autocomplete='off'>"
    "<div class='row'>"
    "<button onclick='provision()'>Connect</button>"
    "</div></div>"
    "<div class='muted'>ESP32-Relay &middot; captive portal</div>"
    "<script>"
    "var stateEl=document.getElementById('state');"
    "var infoEl=document.getElementById('info');"
    "async function api(path,method,body){"
    "var opts={method:method||'GET',headers:{}};"
    "if(body){opts.headers['Content-Type']='application/json';"
    "opts.body=JSON.stringify(body);}"
    "var r=await fetch(path,opts);"
    "var text=await r.text();"
    "if(!r.ok)throw new Error(text||('HTTP '+r.status));"
    "return text?JSON.parse(text):null;"
    "}"
    "async function refreshAll(){"
    "try{"
    "var s=await api('/api/status');"
    "var st=(s.relay||'off').toLowerCase();"
    "stateEl.textContent=st.toUpperCase();"
    "stateEl.className='state '+st;"
    "var w=s.wifi||{};"
    "var lines='Mode: '+(w.mode||'?');"
    "lines+='\\nIP: '+(w.ip||'-');"
    "if(w.mode==='sta'&&w.rssi)lines+='\\nRSSI: '+w.rssi+' dBm';"
    "if(w.ssid)lines+='\\nSSID: '+w.ssid;"
    "lines+='\\nUptime: '+(s.uptime_s||0)+' s';"
    "infoEl.textContent=lines;"
    "}catch(e){infoEl.textContent='Error: '+e.message;}"
    "}"
    "async function toggleRelay(){"
    "var next=stateEl.textContent==='ON'?'off':'on';"
    "try{await api('/api/relay','PUT',{state:next});}"
    "catch(e){alert('Toggle failed: '+e.message);}"
    "await refreshAll();"
    "}"
    "async function scanNetworks(){"
    "var btn=document.getElementById('scanBtn');"
    "var sel=document.getElementById('ssid');"
    "btn.disabled=true;btn.textContent='Scanning...';"
    "try{"
    "var list=await api('/api/scan');"
    "sel.innerHTML='';"
    "var def=document.createElement('option');"
    "def.value='';def.textContent='-- select --';"
    "sel.appendChild(def);"
    "for(var i=0;i<list.length;i++){"
    "var o=document.createElement('option');"
    "o.value=list[i].ssid;"
    "o.textContent=list[i].ssid+' ('+list[i].rssi+' dBm)';"
    "sel.appendChild(o);"
    "}"
    "}catch(e){alert('Scan failed: '+e.message);}"
    "btn.disabled=false;btn.textContent='Scan';"
    "}"
    "async function provision(){"
    "var ssid=document.getElementById('ssid').value;"
    "var password=document.getElementById('password').value;"
    "if(!ssid){alert('Select a network');return;}"
    "var data=new URLSearchParams();"
    "data.append('ssid',ssid);"
    "data.append('password',password);"
    "try{"
    "var r=await fetch('/api/provision',{method:'POST',body:data});"
    "var text=await r.text();"
    "if(r.ok)alert('Credentials saved. Switching to STA...');"
    "else alert('Provision failed: '+text);"
    "}catch(e){alert('Provision error: '+e.message);}"
    "}"
    "refreshAll();"
    "setInterval(refreshAll,5000);"
    "</script></body></html>";

typedef struct {
  char ssid[33];
  char password[65];
} provision_job_t;

static const char *skip_json_ws(const char *cursor, const char *end) {
  while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                          *cursor == '\r' || *cursor == '\n')) {
    cursor++;
  }
  return cursor;
}

static bool parse_json_string(const char **cursor, const char *end, char *out,
                              size_t out_size) {
  const char *p = *cursor;

  if (p >= end || *p != '"') {
    return false;
  }
  p++;

  size_t len = 0;
  while (p < end && *p != '"') {
    unsigned char c = (unsigned char)*p;
    // Escape-последовательности не нужны для контракта API.
    if (c < 0x20 || c == '\\') {
      return false;
    }
    if (len + 1 >= out_size) {
      return false;
    }
    out[len++] = (char)c;
    p++;
  }

  if (p >= end || *p != '"') {
    return false;
  }
  out[len] = '\0';
  *cursor = p + 1;
  return true;
}

bool http_api_parse_relay_state(const char *body, size_t len,
                                relay_state_t *state) {
  if (body == NULL || state == NULL || len == 0) {
    return false;
  }

  const char *p = body;
  const char *end = body + len;
  char key[16];
  char value[8];

  p = skip_json_ws(p, end);
  if (p >= end || *p != '{') {
    return false;
  }
  p++;

  p = skip_json_ws(p, end);
  if (!parse_json_string(&p, end, key, sizeof(key))) {
    return false;
  }

  p = skip_json_ws(p, end);
  if (p >= end || *p != ':') {
    return false;
  }
  p++;

  p = skip_json_ws(p, end);
  if (!parse_json_string(&p, end, value, sizeof(value))) {
    return false;
  }

  p = skip_json_ws(p, end);
  if (p >= end || *p != '}') {
    return false;
  }
  p++;

  p = skip_json_ws(p, end);
  if (p != end || strcmp(key, "state") != 0) {
    return false;
  }

  if (strcmp(value, "on") == 0) {
    *state = RELAY_STATE_ON;
    return true;
  }
  if (strcmp(value, "off") == 0) {
    *state = RELAY_STATE_OFF;
    return true;
  }
  return false;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static bool url_decode(const char *src, size_t src_len, char *dst,
                       size_t dst_size) {
  size_t out = 0;

  for (size_t i = 0; i < src_len; i++) {
    char c = src[i];
    if (c == '+') {
      c = ' ';
    } else if (c == '%') {
      if (i + 2 >= src_len) {
        return false;
      }
      int hi = hex_value(src[++i]);
      int lo = hex_value(src[++i]);
      if (hi < 0 || lo < 0) {
        return false;
      }
      c = (char)((hi << 4) | lo);
    }

    // NUL внутри значения несовместим с C-строками.
    if (c == '\0' || out + 1 >= dst_size) {
      return false;
    }
    dst[out++] = c;
  }

  dst[out] = '\0';
  return true;
}

static bool form_get(const char *body, size_t len, const char *key, char *out,
                     size_t out_size) {
  size_t key_len = strlen(key);
  size_t pos = 0;

  while (pos <= len) {
    size_t end = pos;
    while (end < len && body[end] != '&') {
      end++;
    }

    size_t eq = pos;
    while (eq < end && body[eq] != '=') {
      eq++;
    }

    if (eq < end && eq - pos == key_len &&
        memcmp(body + pos, key, key_len) == 0) {
      return url_decode(body + eq + 1, end - (eq + 1), out, out_size);
    }

    if (end == len) {
      break;
    }
    pos = end + 1;
  }

  return false;
}

bool http_api_parse_provision_form(const char *body, size_t len, char *ssid,
                                   size_t ssid_size, char *password,
                                   size_t password_size) {
  if (body == NULL || ssid == NULL || password == NULL || len == 0) {
    return false;
  }

  if (!form_get(body, len, "ssid", ssid, ssid_size) ||
      !form_get(body, len, "password", password, password_size)) {
    return false;
  }

  return ssid[0] != '\0';
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *message) {
  char body[128];
  int len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);

  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

static bool has_valid_token(httpd_req_t *req) {
  // ADR-0010: в AP-режиме управляющие эндпоинты доступны без токена —
  // защита обеспечивается WPA2-паролем точки доступа. В STA токен обязателен.
  if (wifi_mgr_get_mode() == WIFI_MGR_MODE_AP) {
    return true;
  }

  size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (header_len == 0 || header_len >= AUTH_HEADER_MAX_LEN) {
    return false;
  }

  char header[AUTH_HEADER_MAX_LEN];
  if (httpd_req_get_hdr_value_str(req, "Authorization", header,
                                  sizeof(header)) != ESP_OK) {
    return false;
  }

  static const char prefix[] = "Bearer ";
  if (strncmp(header, prefix, sizeof(prefix) - 1) != 0) {
    return false;
  }

  return strcmp(header + (sizeof(prefix) - 1), CONFIG_RELAY_API_TOKEN) == 0;
}

static bool is_ap_mode(void) { return wifi_mgr_get_mode() == WIFI_MGR_MODE_AP; }

static esp_err_t send_relay_state(httpd_req_t *req) {
  char body[32];
  const char *state = relay_get_state() == RELAY_STATE_ON ? "on" : "off";
  int len = snprintf(body, sizeof(body), "{\"state\":\"%s\"}", state);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

static esp_err_t relay_get_handler(httpd_req_t *req) {
  if (!has_valid_token(req)) {
    return send_error(req, "401 Unauthorized", "unauthorized");
  }
  return send_relay_state(req);
}

static int receive_body(httpd_req_t *req, char *body, size_t body_size) {
  size_t received = 0;

  while (received < req->content_len) {
    int ret = httpd_req_recv(req, body + received, req->content_len - received);
    if (ret <= 0) {
      return -1;
    }
    received += (size_t)ret;
  }

  if (received >= body_size) {
    return -1;
  }
  body[received] = '\0';
  return (int)received;
}

static esp_err_t relay_put_handler(httpd_req_t *req) {
  if (!has_valid_token(req)) {
    return send_error(req, "401 Unauthorized", "unauthorized");
  }

  if (req->content_len == 0 || req->content_len >= RELAY_BODY_MAX_LEN) {
    return send_error(req, "400 Bad Request", "invalid body");
  }

  char body[RELAY_BODY_MAX_LEN];
  int len = receive_body(req, body, sizeof(body));
  if (len < 0) {
    return send_error(req, "400 Bad Request", "invalid body");
  }

  relay_state_t state;
  if (!http_api_parse_relay_state(body, (size_t)len, &state)) {
    return send_error(req, "400 Bad Request", "invalid body");
  }

  esp_err_t err = relay_set_state(state);
  if (err != ESP_OK) {
    return send_error(req, "500 Internal Server Error", "relay error");
  }

  return send_relay_state(req);
}

static esp_err_t status_get_handler(httpd_req_t *req) {
  wifi_mgr_info_t info;
  esp_err_t err = wifi_mgr_get_info(&info);
  if (err != ESP_OK) {
    return send_error(req, "500 Internal Server Error",
                      "wifi info unavailable");
  }

  const char *mode = "off";
  if (info.mode == WIFI_MGR_MODE_STA) {
    mode = "sta";
  } else if (info.mode == WIFI_MGR_MODE_AP) {
    mode = "ap";
  }

  char ssid_json[48] = "";
  if (info.mode == WIFI_MGR_MODE_AP) {
    snprintf(ssid_json, sizeof(ssid_json), ",\"ssid\":\"%s\"", info.ssid);
  }

  const char *relay_state = relay_get_state() == RELAY_STATE_ON ? "on" : "off";
  char body[256];
  int len = snprintf(body, sizeof(body),
                     "{\"wifi\":{\"mode\":\"%s\",\"ip\":\"%s\",\"rssi\":%d%s},"
                     "\"relay\":\"%s\",\"uptime_s\":%lu}",
                     mode, info.ip, info.rssi, ssid_json, relay_state,
                     (unsigned long)wifi_mgr_get_uptime_s());

  if (len < 0 || len >= (int)sizeof(body)) {
    return send_error(req, "500 Internal Server Error", "response too large");
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

static esp_err_t captive_page_get_handler(httpd_req_t *req) {
  if (!is_ap_mode()) {
    return send_error(req, "404 Not Found", "not found");
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, CAPTIVE_PAGE_HTML, sizeof(CAPTIVE_PAGE_HTML) - 1);
}

static esp_err_t captive_redirect_handler(httpd_req_t *req) {
  if (!is_ap_mode()) {
    return send_error(req, "404 Not Found", "not found");
  }

  wifi_mgr_info_t info;
  if (wifi_mgr_get_info(&info) != ESP_OK || info.ip[0] == '\0') {
    return send_error(req, "500 Internal Server Error",
                      "wifi info unavailable");
  }

  char location[32];
  int len = snprintf(location, sizeof(location), "http://%s/", info.ip);
  if (len < 0 || len >= (int)sizeof(location)) {
    return send_error(req, "500 Internal Server Error", "response too large");
  }

  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  return httpd_resp_send(req, "", 0);
}

static void provision_task(void *arg) {
  provision_job_t *job = (provision_job_t *)arg;

  vTaskDelay(pdMS_TO_TICKS(PROVISION_SWITCH_DELAY_MS));

  esp_err_t err = wifi_mgr_provision(job->ssid, job->password);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Provisioning failed: %s", esp_err_to_name(err));
  }

  memset(job->password, 0, sizeof(job->password));
  free(job);
  vTaskDelete(NULL);
}

static esp_err_t provision_post_handler(httpd_req_t *req) {
  if (!is_ap_mode()) {
    return send_error(req, "404 Not Found", "not found");
  }

  if (req->content_len == 0 || req->content_len >= PROVISION_BODY_MAX_LEN) {
    return send_error(req, "400 Bad Request", "invalid body");
  }

  char body[PROVISION_BODY_MAX_LEN];
  int len = receive_body(req, body, sizeof(body));
  if (len < 0) {
    return send_error(req, "400 Bad Request", "invalid body");
  }

  provision_job_t *job = calloc(1, sizeof(*job));
  if (job == NULL) {
    return send_error(req, "500 Internal Server Error", "out of memory");
  }

  if (!http_api_parse_provision_form(body, (size_t)len, job->ssid,
                                     sizeof(job->ssid), job->password,
                                     sizeof(job->password))) {
    memset(job->password, 0, sizeof(job->password));
    free(job);
    return send_error(req, "400 Bad Request", "invalid body");
  }

  if (xTaskCreate(provision_task, "provision", 4096, job, 5, NULL) != pdPASS) {
    memset(job->password, 0, sizeof(job->password));
    free(job);
    return send_error(req, "500 Internal Server Error", "out of memory");
  }

  static const char response[] = "{\"status\":\"provisioning\"}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, response, sizeof(response) - 1);
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
  if (!is_ap_mode()) {
    return send_error(req, "404 Not Found", "not found");
  }

  char *buf = malloc(4096);
  if (buf == NULL) {
    return send_error(req, "500 Internal Server Error", "out of memory");
  }

  esp_err_t err = wifi_mgr_scan_json(buf, 4096);
  if (err != ESP_OK) {
    free(buf);
    if (err == ESP_ERR_TIMEOUT) {
      return send_error(req, "500 Internal Server Error", "scan timeout");
    }
    return send_error(req, "500 Internal Server Error", "scan failed");
  }

  httpd_resp_set_type(req, "application/json");
  esp_err_t ret = httpd_resp_sendstr(req, buf);
  free(buf);
  return ret;
}

static int build_dns_response(const uint8_t *query, int query_len,
                              uint8_t *response, size_t response_size,
                              uint32_t ip) {
  if (query_len < 12 || (query[2] & 0x80) != 0) {
    return 0;
  }

  uint16_t qdcount = (uint16_t)((query[4] << 8) | query[5]);
  if (qdcount < 1) {
    return 0;
  }

  size_t pos = 12;
  while (pos < (size_t)query_len && query[pos] != 0) {
    uint8_t label_len = query[pos];
    if (label_len > 63 || (label_len & 0xC0) != 0) {
      return 0;
    }
    pos += 1 + label_len;
  }
  if (pos >= (size_t)query_len) {
    return 0;
  }
  pos++;
  if (pos + 4 > (size_t)query_len) {
    return 0;
  }

  uint16_t qtype = (uint16_t)((query[pos] << 8) | query[pos + 1]);
  uint16_t qclass = (uint16_t)((query[pos + 2] << 8) | query[pos + 3]);
  bool answer_a = (qtype == 1 && qclass == 1);

  size_t question_len = pos + 4 - 12;
  size_t answer_len = answer_a ? 16 : 0;
  size_t needed = 12 + question_len + answer_len;
  if (needed > response_size) {
    return 0;
  }

  memcpy(response, query, 12);
  response[2] = 0x81;
  response[3] = 0x80;
  response[4] = 0x00;
  response[5] = 0x01;
  response[6] = 0x00;
  response[7] = answer_a ? 0x01 : 0x00;
  response[8] = 0x00;
  response[9] = 0x00;
  response[10] = 0x00;
  response[11] = 0x00;

  memcpy(response + 12, query + 12, question_len);

  size_t out = 12 + question_len;
  if (answer_a) {
    response[out++] = 0xC0;
    response[out++] = 0x0C;
    response[out++] = 0x00;
    response[out++] = 0x01;
    response[out++] = 0x00;
    response[out++] = 0x01;
    response[out++] = 0x00;
    response[out++] = 0x00;
    response[out++] = 0x00;
    response[out++] = DNS_TTL_SECONDS;
    response[out++] = 0x00;
    response[out++] = 0x04;
    memcpy(response + out, &ip, 4);
    out += 4;
  }

  return (int)out;
}

static void dns_responder_task(void *arg) {
  (void)arg;

  int sock = -1;
  bool bind_warned = false;
  uint8_t rx[DNS_BODY_MAX_LEN];
  uint8_t tx[DNS_BODY_MAX_LEN];

  while (s_running) {
    if (!is_ap_mode()) {
      if (sock >= 0) {
        close(sock);
        sock = -1;
        ESP_LOGI(TAG, "DNS responder stopped");
      }
      bind_warned = false;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    wifi_mgr_info_t info;
    if (wifi_mgr_get_info(&info) != ESP_OK || info.mode != WIFI_MGR_MODE_AP) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    uint32_t ap_ip = inet_addr(info.ip);
    if (ap_ip == 0 || ap_ip == INADDR_NONE) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (sock < 0) {
      sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (sock < 0) {
        if (!bind_warned) {
          ESP_LOGW(TAG, "Failed to create DNS socket");
          bind_warned = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(53);
      addr.sin_addr.s_addr = ap_ip;

      if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (!bind_warned) {
          ESP_LOGW(TAG, "Failed to bind DNS socket on %s", info.ip);
          bind_warned = true;
        }
        close(sock);
        sock = -1;
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      struct timeval timeout = {
          .tv_sec = 1,
          .tv_usec = 0,
      };
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      bind_warned = false;
      ESP_LOGI(TAG, "DNS responder started on %s", info.ip);
    }

    struct sockaddr_in source;
    socklen_t source_len = sizeof(source);
    int received = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&source,
                            &source_len);
    if (received <= 0) {
      continue;
    }

    int response_len = build_dns_response(rx, received, tx, sizeof(tx), ap_ip);
    if (response_len > 0) {
      sendto(sock, tx, response_len, 0, (struct sockaddr *)&source, source_len);
    }
  }

  if (sock >= 0) {
    close(sock);
  }
  s_dns_task_started = false;
  vTaskDelete(NULL);
}

static esp_err_t http_error_handler(httpd_req_t *req, httpd_err_code_t error) {
  switch (error) {
  case HTTPD_404_NOT_FOUND:
    return send_error(req, "404 Not Found", "not found");
  case HTTPD_405_METHOD_NOT_ALLOWED:
    return send_error(req, "405 Method Not Allowed", "method not allowed");
  default:
    return send_error(req, "500 Internal Server Error", "internal error");
  }
}

esp_err_t http_api_start(void) {
  if (s_server != NULL) {
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 12;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
    s_server = NULL;
    return err;
  }

  const httpd_uri_t relay_get_uri = {
      .uri = "/api/relay",
      .method = HTTP_GET,
      .handler = relay_get_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t relay_put_uri = {
      .uri = "/api/relay",
      .method = HTTP_PUT,
      .handler = relay_put_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t status_get_uri = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = status_get_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t provision_post_uri = {
      .uri = "/api/provision",
      .method = HTTP_POST,
      .handler = provision_post_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t scan_get_uri = {
      .uri = "/api/scan",
      .method = HTTP_GET,
      .handler = scan_get_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t captive_page_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = captive_page_get_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t generate_204_uri = {
      .uri = "/generate_204",
      .method = HTTP_GET,
      .handler = captive_redirect_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t hotspot_detect_uri = {
      .uri = "/hotspot-detect.html",
      .method = HTTP_GET,
      .handler = captive_redirect_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t connecttest_uri = {
      .uri = "/connecttest.txt",
      .method = HTTP_GET,
      .handler = captive_redirect_handler,
      .user_ctx = NULL,
  };

  err = httpd_register_uri_handler(s_server, &relay_get_uri);
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &relay_put_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &status_get_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &provision_post_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &scan_get_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &captive_page_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &generate_204_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &hotspot_detect_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &connecttest_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND,
                                     http_error_handler);
  }
  if (err == ESP_OK) {
    err = httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED,
                                     http_error_handler);
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register HTTP handlers: %s", esp_err_to_name(err));
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  s_running = true;
  if (!s_dns_task_started) {
    s_dns_task_started = true;
    if (xTaskCreate(dns_responder_task, "dns_responder", DNS_TASK_STACK, NULL,
                    DNS_TASK_PRIO, NULL) != pdPASS) {
      s_dns_task_started = false;
      s_running = false;
      ESP_LOGE(TAG, "Failed to start DNS responder");
      httpd_stop(s_server);
      s_server = NULL;
      return ESP_ERR_NO_MEM;
    }
  }

  ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
  return ESP_OK;
}

esp_err_t http_api_stop(void) {
  s_running = false;

  if (s_server == NULL) {
    return ESP_OK;
  }

  esp_err_t err = httpd_stop(s_server);
  s_server = NULL;
  return err;
}
