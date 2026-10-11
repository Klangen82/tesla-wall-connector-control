#include "twc_vitals.h"
#include "esphome/components/network/util.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <cmath>
#include <cstring>

namespace esphome::twc_vitals {
static const char *const TAG = "twc_vitals";
struct Body {
  char data[2048] = {};
  size_t length = 0;
  bool overflow = false;
};

void TWCVitals::setup() {
  results_ = xQueueCreate(1, sizeof(Sample));
  if (results_ == nullptr ||
      xTaskCreate(worker_, "twc_vitals", 8192, this, 1, &worker_task_) != pdPASS) {
    if (results_ != nullptr) vQueueDelete(results_);
    results_ = nullptr;
    worker_task_ = nullptr;
    ESP_LOGE(TAG, "Cannot start vitals worker");
    mark_failed();
  }
}

void TWCVitals::update() {
  if (worker_task_ == nullptr) return;
  if (network::is_connected()) {
    xTaskNotifyGive(worker_task_);
  } else {
    Sample result;
    result.error = RequestError::NETWORK;
    result.captured_ms = millis();
    xQueueOverwrite(results_, &result);
  }
}

void TWCVitals::loop() {
  Sample result;
  if (results_ != nullptr && xQueueReceive(results_, &result, 0) == pdTRUE) {
    // Keep request diagnostics separate from the last complete valid snapshot.
    // Failed polls never refresh its timestamp; fresh() enforces the age limit.
    const bool changed = !received_ || last_request_.error != result.error;
    last_request_ = result;
    received_ = true;
    if (result.valid) {
      sample_ = result;
      successful_ = true;
      last_success_ms_ = result.captured_ms;
      if (consecutive_failures_ != 0)
        ESP_LOGI(TAG, "Feedback recovered after %u failed polls (response %u ms)",
                 (unsigned) consecutive_failures_, (unsigned) result.duration_ms);
      consecutive_failures_ = 0;
      status_clear_warning();
    } else {
      failed_ = true;
      last_failure_ = result.error;
      failure_count_++;
      consecutive_failures_++;
      if (changed || consecutive_failures_ == 1 || consecutive_failures_ % 30 == 0)
        ESP_LOGW(TAG, "%s: HTTP %d, transport %s, duration %u ms, consecutive failures %u",
                 request_error_name(result.error), result.http_status,
                 esp_err_to_name(result.transport_error), (unsigned) result.duration_ms,
                 (unsigned) consecutive_failures_);
      status_set_warning();
    }
  }
}

bool TWCVitals::fresh(uint32_t timeout) const {
  return sample_.valid && uint32_t(millis() - sample_.captured_ms) < timeout;
}

void TWCVitals::dump_config() {
  ESP_LOGCONFIG(TAG, "TWC vitals: %s (background HTTP worker)", url_.c_str());
  LOG_UPDATE_INTERVAL(this);
}

esp_err_t TWCVitals::http_event_(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
  auto *body = static_cast<Body *>(event->user_data);
  if (body->length + event->data_len >= sizeof(body->data)) {
    body->overflow = true;
    return ESP_FAIL;
  }
  std::memcpy(body->data + body->length, event->data, event->data_len);
  body->length += event->data_len;
  body->data[body->length] = '\0';
  return ESP_OK;
}

void TWCVitals::worker_(void *arg) {
  auto *self = static_cast<TWCVitals *>(arg);
  // Only immutable configuration and FreeRTOS queues cross the thread boundary.
  // ESPHome entities, loggers and the regulator stay on the main loop.
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Body body;
    Sample result;
    const uint32_t started_ms = millis();
    esp_http_client_config_t config = {};
    config.url = self->url_.c_str();
    config.timeout_ms = self->timeout_ms_;
    config.event_handler = http_event_;
    config.user_data = &body;
    config.disable_auto_redirect = true;
    config.buffer_size = 512;
    auto client = esp_http_client_init(&config);
    if (client != nullptr) {
      const esp_err_t error = esp_http_client_perform(client);
      const int status = esp_http_client_get_status_code(client);
      result.http_status = status;
      result.transport_error = error;
      esp_http_client_cleanup(client);
      if (body.overflow) result.error = RequestError::OVERFLOW;
      else if (error == ESP_ERR_TIMEOUT) result.error = RequestError::TIMEOUT;
      else if (error != ESP_OK) result.error = RequestError::TRANSPORT;
      else if (status != 200) result.error = RequestError::HTTP_STATUS;
      else {
        JsonDocument doc;
        result.error = RequestError::JSON;
        if (!deserializeJson(doc, body.data, body.length)) {
          result.error = RequestError::FIELDS;
          const char *fields[] = {"currentA_a", "currentB_a", "currentC_a", "vehicle_current_a"};
          bool valid = doc["contactor_closed"].is<bool>();
          float values[4] = {};
          for (int i = 0; i < 4; ++i) {
            valid = valid && doc[fields[i]].is<float>();
            values[i] = doc[fields[i]].as<float>();
            valid = valid && std::isfinite(values[i]) && values[i] >= 0 && values[i] <= 200;
          }
          if (valid) {
            for (int i = 0; i < 3; ++i) result.phase[i] = values[i];
            result.vehicle = values[3];
            result.contactor = doc["contactor_closed"].as<bool>();
            result.valid = true;
            result.error = RequestError::NONE;
          }
        }
      }
    }
    result.captured_ms = millis();
    result.duration_ms = uint32_t(result.captured_ms - started_ms);
    xQueueOverwrite(self->results_, &result);
  }
}
}  // namespace esphome::twc_vitals
