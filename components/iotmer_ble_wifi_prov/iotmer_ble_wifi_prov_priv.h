#pragma once

#include <stdint.h>

#include "iotmer_ble_wifi_prov.h"

#define IOTMER_BLE_PROV_TAG "iotmer_ble_wifi_prov"

#define IOTMER_BLE_Q_DEPTH 24

typedef enum {
    IOTMER_BLE_Q_GAP = 0,
    IOTMER_BLE_Q_GATT_CTRL,
    IOTMER_BLE_Q_GATT_DATA,
    IOTMER_BLE_Q_TIMER_IDLE,
    IOTMER_BLE_Q_TIMER_SESS,
    IOTMER_BLE_Q_SHUTDOWN,
} iotmer_ble_q_kind_t;

typedef struct {
    iotmer_ble_q_kind_t kind;
    uint16_t            conn_handle;
    uint16_t            len;
    uint8_t             data[244];
    int32_t             gap_status;
    uint8_t             gap_evt;
} iotmer_ble_q_msg_t;
