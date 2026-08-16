#include <stdint.h>

#include "esp_vp.h"

static volatile uint32_t s_ssdp_searches = 0;
static volatile uint32_t s_ssdp_responses = 0;
static volatile uint32_t s_bind_accepts = 0;
static volatile uint32_t s_mqtt_accepts = 0;
static volatile uint32_t s_ftps_accepts = 0;
static volatile uint32_t s_upload_successes = 0;

void esp_vp_diag_record_ssdp_search(void) { s_ssdp_searches++; }
void esp_vp_diag_record_ssdp_response(void) { s_ssdp_responses++; }
void esp_vp_diag_record_bind_accept(void) { s_bind_accepts++; }
void esp_vp_diag_record_mqtt_accept(void) { s_mqtt_accepts++; }
void esp_vp_diag_record_ftps_accept(void) { s_ftps_accepts++; }
void esp_vp_diag_record_upload_success(void) { s_upload_successes++; }

uint32_t esp_vp_diag_ssdp_searches(void) { return s_ssdp_searches; }
uint32_t esp_vp_diag_ssdp_responses(void) { return s_ssdp_responses; }
uint32_t esp_vp_diag_bind_accepts(void) { return s_bind_accepts; }
uint32_t esp_vp_diag_mqtt_accepts(void) { return s_mqtt_accepts; }
uint32_t esp_vp_diag_ftps_accepts(void) { return s_ftps_accepts; }
uint32_t esp_vp_diag_upload_successes(void) { return s_upload_successes; }
