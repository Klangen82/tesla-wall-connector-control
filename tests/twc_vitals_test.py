"""Exercise the production vitals component with host-side ESP/FreeRTOS stubs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUB = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <cmath>
// macOS math.h defines OVERFLOW; ESP-IDF does not.
#undef OVERFLOW
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_TIMEOUT=1, pdPASS=1, pdTRUE=1;
constexpr unsigned portMAX_DELAY=~0u;
using QueueHandle_t=void*;
using TaskHandle_t=void*;
inline uint32_t test_now=0;
inline void *test_result=nullptr;
inline size_t test_result_size=0;
namespace esphome {
inline uint32_t millis() { return test_now; }
class PollingComponent {
 public:
 virtual void setup() {} virtual void update() {} virtual void loop() {}
 virtual void dump_config() {} void mark_failed() {}
 void status_clear_warning() {} void status_set_warning() {}
};
namespace network { inline bool is_connected() { return true; } }
}
inline void *xQueueCreate(int, size_t) { return reinterpret_cast<void*>(1); }
inline void vQueueDelete(void*) {}
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, void**) { return pdPASS; }
inline void xTaskNotifyGive(void*) {}
inline int xQueueReceive(void*, void *out, int) {
 if (!test_result) return 0;
 __builtin_memcpy(out, test_result, test_result_size); test_result=nullptr; return pdTRUE;
}
inline void xQueueOverwrite(void*, const void*) {}
inline void ulTaskNotifyTake(int, unsigned) {}
inline const char *esp_err_to_name(int) { return "stub"; }
constexpr int HTTP_EVENT_ON_DATA=1;
struct esp_http_client_event_t { int event_id=0; int data_len=0; void *user_data=nullptr; void *data=nullptr; };
struct esp_http_client_config_t {
 const char *url=nullptr; uint32_t timeout_ms=0;
 esp_err_t (*event_handler)(esp_http_client_event_t*)=nullptr;
 void *user_data=nullptr; bool disable_auto_redirect=false; int buffer_size=0;
};
inline void *esp_http_client_init(esp_http_client_config_t*) { return nullptr; }
inline int esp_http_client_perform(void*) { return 0; }
inline int esp_http_client_get_status_code(void*) { return 200; }
inline void esp_http_client_cleanup(void*) {}
struct JsonValue { template<class T> bool is() { return true; } template<class T> T as() { return T{}; } };
struct JsonDocument { JsonValue operator[](const char*) { return {}; } };
inline int deserializeJson(JsonDocument&, const char*, size_t) { return 0; }
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGCONFIG(tag, ...) ((void)(tag))
#define LOG_UPDATE_INTERVAL(...) ((void)0)
'''
TEST = r'''
#include "components/twc_vitals/twc_vitals.h"
#include "components/twc_vitals/twc_control.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace esphome::twc_vitals;
class Vitals : public TWCVitals {
 public:
 void deliver(Sample result) {
   results_=reinterpret_cast<void*>(1);
   test_result=&result; test_result_size=sizeof(result); loop();
 }
};
int main() {
 Vitals v;
 assert(!v.fresh(8000));
 Sample failure; failure.error=RequestError::TRANSPORT; failure.captured_ms=1000;
 test_now=1000; v.deliver(failure);
 assert(!v.fresh(8000)); assert(std::isnan(v.last_success_age_s()));
 assert(std::strcmp(v.last_failure_status(), "HTTP transport error") == 0);
 Sample good; good.valid=true; good.error=RequestError::NONE;
 good.captured_ms=2000; good.http_status=200; good.vehicle=10;
 good.phase[0]=10; good.phase[1]=9; good.phase[2]=8; good.contactor=true;
 test_now=2000; v.deliver(good);
 assert(std::strcmp(v.last_failure_status(), "HTTP transport error") == 0);
 assert(v.fresh(8000)); assert(v.consecutive_failures()==0);
 failure.duration_ms=2400; failure.http_status=-1;
 for (auto error : {RequestError::TRANSPORT, RequestError::TIMEOUT,
                    RequestError::JSON, RequestError::FIELDS, RequestError::NETWORK}) {
   test_now+=1000; failure.captured_ms=test_now; failure.error=error; v.deliver(failure);
   assert(v.fresh(8000)); assert(v.sample().captured_ms==2000);
   assert(v.sample().vehicle==10 && v.sample().phase[2]==8 && v.sample().contactor);
   assert(std::strcmp(v.request_status(), request_error_name(error))==0);
   assert(v.http_status()==-1 && v.response_time_ms()==2400);
 }
 test_now=9999; assert(v.fresh(8000));
 test_now=10000; assert(!v.fresh(8000));
 assert(v.last_success_age_s()==8); assert(v.consecutive_failures()==5);
 twc_control::Controller controller; twc_control::Settings settings;
 twc_control::Input input; input.ready=input.ha_connected=true;
 for (auto &phase : controller.house) phase.update(12, test_now);
 input.feedback_fresh=v.fresh(8000); input.now=test_now;
 const auto paused=controller.tick(settings,input);
 assert(paused.fault==twc_control::Fault::VITALS && paused.target==0);
 good.captured_ms=test_now; good.vehicle=6; v.deliver(good);
 assert(v.fresh(8000) && v.sample().vehicle==6);
 assert(v.consecutive_failures()==0 && v.failure_count()==6 && v.http_status()==200);
 good.captured_ms=0xfffffff0u; test_now=good.captured_ms; v.deliver(good);
 test_now=0x10; assert(v.fresh(8000));
 test_now=uint32_t(good.captured_ms+8000); assert(!v.fresh(8000));
 std::cout << "Vitals cache tests passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix="twc-vitals-test-") as directory:
    tmp = Path(directory)
    (tmp / 'stub.h').write_text(STUB)
    for name in ['esphome/core/component.h', 'esp_http_client.h', 'freertos/FreeRTOS.h',
                 'freertos/queue.h', 'freertos/task.h', 'esphome/components/network/util.h',
                 'esphome/components/json/json_util.h', 'esphome/core/helpers.h', 'esphome/core/log.h']:
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "stub.h"\n')
    source = tmp / 'test.cpp'
    source.write_text(TEST)
    binary = tmp / 'test'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(tmp), '-I', str(ROOT), str(source),
                    str(ROOT / 'components/twc_vitals/twc_vitals.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
