/*
 * Quectel.h
 *
 *  Created on: Dec 11, 2024
 *      Author: ERIC MULWA
 */

#ifndef QUECTEL_H
#define QUECTEL_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include <stdio.h>
#include "string.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <bootloader_common.h>
#include "esp_app_desc.h"
#include <sdkconfig.h>
#include "esp_partition.h"

// Definitions
#define MODEM_UART_NUM UART_NUM_1
#define TX_PIN 17
#define RX_PIN 16
#define RESET_PIN 19
#define PWR_PIN 18 
#define GSM_EN 4
#define ESP_BAUDRATE 115200
#define QUECTEL_BAUDRATE 115200
#define TAG_GSM "MODEM"
#define BUF_SIZE 1024
#define APN "safaricomiot"

/*******************************************************************************************************************************
FIRMWARE: METADATA
*******************/ 
#define FIRMWARE_TITLE    "gsm_get_dev"
#define FIRMWARE_VERSION  "v1.0"


static const int RETRIES = 2;
static const int CMD_DELAY_MS = 3000;
//static char APN = "safaricomiot";

// Declare external payload function
extern void error(int count);
extern void error(int count);
extern bool ota_flag;

/**
 * @brief Gsm function prototypes
 */ 
void uart_init();
void initialize_nvs();
void gsm_reset();
void gsm_poweron();
void hardware_poweroff();
void at_poweroff();
bool send_at_command(const char *command, const char *expected_response, int retries, uint32_t timeout_ms);
bool poweron_modem();
bool activate_pdp();
bool deactivate_pdp();
void powerdown_modem();

/**
 * @brief Ota function prototypes
 */ 
uint32_t otagetChipId();
void otagetChipIdString(char *device_token, size_t size);
bool get_thingsboard_fw_info(void);
void perform_secure_ota_update();
void tb_report_ota_status(const char *state, int progress, const char *error);
bool perform_ota();
bool is_ota_required(void);
void running_partition();

#endif // QUECTEL_H
