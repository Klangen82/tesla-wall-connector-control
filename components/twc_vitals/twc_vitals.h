#pragma once

#include "twc_control.h"
#include "esphome/core/component.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string>
#include <cmath>

namespace esphome::twc_vitals {
enum class RequestError { NONE, NETWORK, CLIENT, TIMEOUT, TRANSPORT, HTTP_STATUS, OVERFLOW, JSON, FIELDS };
inline const char *request_error_name(RequestError error) {
  switch (error) {
    case RequestError::NONE: return "OK";
    case RequestError::NETWORK: return "ESP network disconnected";
    case RequestError::CLIENT: return "HTTP client allocation failed";
    case RequestError::TIMEOUT: return "HTTP timeout";
    case RequestError::TRANSPORT: return "HTTP transport error";
    case RequestError::HTTP_STATUS: return "Unexpected HTTP status";
    case RequestError::OVERFLOW: return "HTTP body exceeds buffer";
    case RequestError::JSON: return "Invalid JSON";
    case RequestError::FIELDS: return "Missing or invalid vitals fields";
  }
  return "Unknown error";
}
struct Sample {
  float phase[3] = {0, 0, 0};
  float vehicle = 0;
  bool contactor = false;
  bool valid = false;
  uint32_t captured_ms = 0;
  uint32_t duration_ms = 0;
  int http_status = 0;
  esp_err_t transport_error = ESP_OK;
  RequestError error = RequestError::CLIENT;
};

class TWCVitals : public PollingComponent {
 public:
  void set_url(const std::string &url) { url_ = url; }
  void set_timeout(uint32_t timeout) { timeout_ms_ = timeout; }
  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  const Sample &sample() const { return sample_; }
  bool fresh(uint32_t timeout) const;
  const char *request_status() const { return received_ ? request_error_name(last_request_.error) : "Awaiting first response"; }
  float response_time_ms() const { return received_ ? float(last_request_.duration_ms) : NAN; }
  int http_status() const { return last_request_.http_status; }
  float last_success_age_s() const { return successful_ ? float(uint32_t(millis() - last_success_ms_)) / 1000.0f : NAN; }
  uint32_t failure_count() const { return failure_count_; }
  uint32_t consecutive_failures() const { return consecutive_failures_; }

 protected:
  static void worker_(void *arg);
  static esp_err_t http_event_(esp_http_client_event_t *event);
  std::string url_;
  uint32_t timeout_ms_ = 2000;
  QueueHandle_t results_ = nullptr;
  TaskHandle_t worker_task_ = nullptr;
  Sample sample_;
  Sample last_request_;
  bool received_ = false;
  bool successful_ = false;
  uint32_t last_success_ms_ = 0;
  uint32_t failure_count_ = 0;
  uint32_t consecutive_failures_ = 0;
};
}  // namespace esphome::twc_vitals
