/**
 * iotmer_tls.c — shared TLS trust for MQTT + HTTPS (provision / OTA / device-auth).
 *
 * Default: Mozilla CA bundle (esp_crt_bundle_attach) — unchanged for existing apps.
 * Optional: application pins one or more root CA PEMs via iotmer_tls_set_ca_cert_pem()
 * or iotmer_config_t.ca_cert_pem (AWS IoT-style small trust store).
 */

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "iotmer_client.h"
#include "iotmer_internal.h"

#define TAG "iotmer_tls"

static const char *s_ca_pem;

void iotmer_tls_set_ca_cert_pem(const char *pem)
{
    s_ca_pem = (pem && pem[0] != '\0') ? pem : NULL;
    if (s_ca_pem) {
        ESP_LOGI(TAG, "Pinned CA trust store active (esp_crt_bundle disabled for IoTMER TLS)");
    } else {
        ESP_LOGI(TAG, "Using esp_crt_bundle for IoTMER TLS");
    }
}

const char *iotmer_tls_get_ca_cert_pem(void)
{
    return s_ca_pem;
}

bool iotmer_tls_using_pinned_ca(void)
{
    return s_ca_pem != NULL;
}

void iotmer_tls_apply_http_client_config(esp_http_client_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    if (s_ca_pem) {
        cfg->cert_pem = s_ca_pem;
        cfg->crt_bundle_attach = NULL;
    } else {
        cfg->crt_bundle_attach = esp_crt_bundle_attach;
    }
}

void iotmer_tls_apply_mqtt_verification(esp_mqtt_client_config_t *mcfg)
{
    if (!mcfg) {
        return;
    }
    if (s_ca_pem) {
        mcfg->broker.verification.certificate = s_ca_pem;
        mcfg->broker.verification.crt_bundle_attach = NULL;
    } else {
        mcfg->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
}
