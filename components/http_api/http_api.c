#include "http_api.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "wifi_mgr.h"

#define AUTH_HEADER_MAX_LEN 256
#define RELAY_BODY_MAX_LEN 128

static const char *TAG = "http_api";

static httpd_handle_t s_server;

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

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *message) {
  char body[128];
  int len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);

  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

static bool has_valid_token(httpd_req_t *req) {
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

  err = httpd_register_uri_handler(s_server, &relay_get_uri);
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &relay_put_uri);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &status_get_uri);
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

  ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
  return ESP_OK;
}

esp_err_t http_api_stop(void) {
  if (s_server == NULL) {
    return ESP_OK;
  }

  esp_err_t err = httpd_stop(s_server);
  s_server = NULL;
  return err;
}
