/*
 * Quectel.c
 *
 *  Created: 11/12/2024
 *	Modified: 14/03/2025
 *    
 *		Author: ERIC MULWA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <errno.h>
#include <sys/param.h>
#include <inttypes.h>
#include "gsm_ota.h"

char tb_fw_info[BUF_SIZE];
char response[BUF_SIZE];

/*******************************************************************************************************************************
GLOBAL: Global Definitions
**************************/     
uint32_t storedChipId = 0;
/*******************************************************************************************************************************/

/*******************************************************************************************************************************
THINGSBOARD: GET DEFINITIONS
***************************/  
#define MAX_RESPONSE_SIZE 2048
char http_response_buffer[MAX_RESPONSE_SIZE];
size_t http_response_length = 0;
// ThingsBoard GET credentials
#define THINGSBOARD_HOST "demo.thingsboard.io"
char device_token[12];
// OTA variables struct
typedef struct {
    char fw_title[128];
    char fw_version[64];
    char fw_checksum[128];
    char fw_checksum_algorithm[32];
} tb_fw_info_t;
tb_fw_info_t tb_info;
// OTA Flags
bool ota_required = false;
bool urc_to_usb = false;
/*******************************************************************************************************************************/

/************************************************************************************************************************************** 
FUNCTIONS: INITIALIZATION
************************/

void initialize_nvs() {
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Function to get chip ID based on the MAC address
uint32_t otagetChipId() {
    if (storedChipId == 0) {
        uint8_t mac[6];
        esp_base_mac_addr_get(mac); 

        for (int i = 0; i < 6; i++) {
            storedChipId |= ((uint32_t)mac[i] << (8 * i));
        }
    }
    return storedChipId;
}
// Function to get the chip ID as a string
void otagetChipIdString(char *device_token, size_t size) {
    uint32_t chipId = otagetChipId();
    snprintf(device_token, size, "%" PRIu32, chipId);  
    printf("Device Serial Number (Chip ID): %s\n", device_token); 
} 

/**
*  @brief UART Configuration
*/
void uart_init() {
    uart_config_t uart_config = {
        .baud_rate = QUECTEL_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_driver_install(MODEM_UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(MODEM_UART_NUM, &uart_config);
    uart_set_pin(MODEM_UART_NUM, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/**
*  @brief Hardware Reset
*/
void gsm_reset() {
	esp_rom_gpio_pad_select_gpio(RESET_PIN);
    gpio_set_direction(RESET_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

/**
*  @brief Hardware Power On
*/
void gsm_poweron() {
	// connect to power
    esp_rom_gpio_pad_select_gpio(GSM_EN);
	gpio_set_direction(GSM_EN, GPIO_MODE_OUTPUT);
	gpio_set_level(GSM_EN, 1);
	vTaskDelay(pdMS_TO_TICKS(1000));
	// power on with PWR 
	esp_rom_gpio_pad_select_gpio(PWR_PIN);
    gpio_set_direction(PWR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500)); // minimum of 500ms
    gpio_set_level(PWR_PIN, 1); 
    vTaskDelay(pdMS_TO_TICKS(500)); 
    gpio_set_level(PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(12000)); // Wait for the module to initialize (minimum of 10s)
}

/**
*  @brief Hardware Power Down
*/
void hardware_poweroff() {
	esp_rom_gpio_pad_select_gpio(PWR_PIN);
    gpio_set_direction(PWR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500)); // minimum of 500ms
    gpio_set_level(PWR_PIN, 1); 
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500)); 
	// Disconnect power from Quectel   
	gpio_set_level(GSM_EN, 0);
}

/**
*  @brief AT Command Power Down
*/
void at_poweroff() {
	bool power_down = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!send_at_command("AT+QPOWD=1", "OK", RETRIES, CMD_DELAY_MS)) {
            ESP_LOGW(TAG_GSM, "AT command power down failed on attempt %d, retrying...\n", attempt + 1);
        } else {
            power_down = true;
            ESP_LOGI(TAG_GSM, "Modem powered down successfully!\n");       
			gpio_set_level(GSM_EN, 0); // Disconnect power from Quectel    
			break;     
        }
        
    } if (!power_down) {
		error(3);
        ESP_LOGE(TAG_GSM, "AT command power down failed, exiting task...\n");
    }
  return;
}

/**
*  @brief Send AT Commands with response check
*/
bool send_at_command(const char *command, const char *expected_response, int retries, uint32_t timeout_ms) {

    for (int attempt = 0; attempt < retries; attempt++) {
        uart_write_bytes(MODEM_UART_NUM, command, strlen(command));
        uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

        uint64_t start_time = esp_timer_get_time();
        int index = 0;

        while ((esp_timer_get_time() - start_time) < (timeout_ms * 1000)) {
            uint8_t data;
            if (uart_read_bytes(MODEM_UART_NUM, &data, 1, 10 / portTICK_PERIOD_MS) > 0) {
                if (index < BUF_SIZE - 1) {
                    response[index++] = (char)data;
                }
            }
        }
        response[index] = '\0';
        // Copy thingsboard firmware info into a separate buffer
        if (strstr(command, "AT+QHTTPREAD=80")) {
			memset(tb_fw_info, 0, BUF_SIZE);
			strcpy(tb_fw_info, response);
        }
        
        ESP_LOGI(TAG_GSM, "Command: %s, Response: %s", command, response);

        if (strstr(response, expected_response)) {
            return true;
        }

        ESP_LOGW(TAG_GSM, "Attempt %d: Command '%s' failed. Retrying...", attempt + 1, command);
        vTaskDelay(pdMS_TO_TICKS(CMD_DELAY_MS));
    }

    ESP_LOGE(TAG_GSM, "Command '%s' failed after %d retries.", command, retries);
    return false;
}

/**
*  @brief Init uart and power on Modem
*/
bool poweron_modem() {
    uart_init();
    vTaskDelay(pdMS_TO_TICKS(1000));
	esp_rom_gpio_pad_select_gpio(PWR_PIN);
    gpio_set_direction(PWR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PWR_PIN, 0);
	esp_rom_gpio_pad_select_gpio(RESET_PIN);
    gpio_set_direction(RESET_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RESET_PIN, 0);
	gsm_poweron();
 
 	// Quectel GSM Power On Status Check
    bool power = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!send_at_command("AT", "OK", 3, 3000)) {
            ESP_LOGW(TAG_GSM, "Modem is Off on check %d. Retrying...", attempt + 1);
            gsm_poweron();
            vTaskDelay(pdMS_TO_TICKS(50 * (attempt + 1)));
        } else {
            power = true;
            printf("Modem is On, Proceeding...\n");
            break; 
        }
    }
    if (!power) {
		error(3);
        ESP_LOGE(TAG_GSM, "GSM Power Status Check failed after %d retries.", RETRIES);
        return false;
    } 
    
	// Check if URC are set to the right port (uart1)
	if(urc_to_usb) {
		bool urc_restored = false;
		for (int attempt = 0; attempt < RETRIES; attempt++) {
		    if (!send_at_command("AT+QURCCFG=\"urcport\"", "+QURCCFG: \"urcport\"", RETRIES, CMD_DELAY_MS) ||
	        	!send_at_command("AT+QURCCFG=\"urcport\",\"uart1\"", "OK", RETRIES, CMD_DELAY_MS) ||
	        	!send_at_command("AT+QURCCFG=\"urcport\"", "+QURCCFG: \"urcport\",\"uart1\"", RETRIES, CMD_DELAY_MS)) { 
		        ESP_LOGW(TAG_GSM, "Attempt %d: Failed to query, change, and confirm URC port restoration, retrying...", attempt + 1);
		        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
		        continue;
		    } else {
		        ESP_LOGI(TAG_GSM, "URC port successfully restored to UART1 on attempt %d.", attempt + 1);
		        urc_restored = true;
		        urc_to_usb = false;
		        break;
		    }
		}	
		if (!urc_restored) {
		    urc_to_usb = true;
		    error(3);
		    ESP_LOGE(TAG_GSM, "Critical: Failed to restore URC port to UART1 after all recovery attempts.");
		    return false;
		}		
	}
  return true;	
}

/**
*  @brief Latch to Network, check Signal Strength and Activate PDP
*/
bool activate_pdp() {
	// GSM Network Latch Check
    bool latch = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (!send_at_command("ATE0", "OK", RETRIES, CMD_DELAY_MS) ||
        	!send_at_command("AT+QHTTPCFG=\"contextid\",1", "OK", RETRIES, CMD_DELAY_MS) ||
        	!send_at_command("AT+QHTTPCFG=\"responseheader\",0", "OK", RETRIES, CMD_DELAY_MS) || // response header disables (0)
			!send_at_command("AT+CGREG?", "5", RETRIES, CMD_DELAY_MS)) {
            ESP_LOGW(TAG_GSM, "No Network Latch on attempt %d. Retrying...", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1))); // progressive backoff
            continue;
        } else {
            latch = true;
            ESP_LOGI(TAG_GSM, "GSM Network Latch successful!");
            break; 
        }
    }
    if (!latch) {
        ESP_LOGE(TAG_GSM, "GSM Network Latch failed after %d retries. Shutting down and exiting...", RETRIES);
        return false;
    }

	// Check GSM Signal strength before OTA
	bool signal_good = false;
	for (int attempt = 0; attempt < 10; attempt++) {
	    if (!send_at_command("AT+CSQ", "OK", RETRIES, CMD_DELAY_MS)) {
	        ESP_LOGW(TAG_GSM, "Failed to get signal strength on attempt %d. Retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	        continue;
	    } else {	
	        printf("AT+CSQ Response: %s", response);		
	        // Expected response format: +CSQ: <rssi>,<ber>
	        int rssi = -1, ber = -1;	        
	        // Parse the response - look for the CSQ line anywhere in the response
	        char *csq_ptr = strstr(response, "+CSQ:");
	        if (csq_ptr && sscanf(csq_ptr, "+CSQ: %d,%d", &rssi, &ber) == 2) {
	            // Validate RSSI range
	            bool valid =
	                (rssi >= 15 && rssi <= 31) ||  // LTE acceptable range
	                (rssi == 191);                 // TD-SCDMA excellent signal
	
	            if (valid) {
	                printf("Signal strength OK (RSSI=%d)\n", rssi);
	                signal_good = true;
	                break;
	            } else {
	                ESP_LOGW(TAG_GSM, "Weak signal (RSSI=%d), retrying...", rssi);
	                vTaskDelay(pdMS_TO_TICKS(2000)); 
	                continue;
	            }
	        } else {
	            ESP_LOGE(TAG_GSM, "Unexpected CSQ response: %s", response);
	            vTaskDelay(pdMS_TO_TICKS(2000)); 
	            continue;
	        }	 
	    }
	}
	if (!signal_good) {
	    ESP_LOGE(TAG_GSM, "Unstable or weak GSM signal after 10 attempts. Aborting OTA sequence!");
	    return false;
	}

	// Quectel GSM PDP Context configuration and PDP Activation
	char apncommand[60];
	sprintf(apncommand, "AT+QICSGP=1,1,\"%s\",\"\",\"\",1", APN);
    bool success = false;
    for (int attempt = 0; attempt < RETRIES; attempt++) {
        if (!send_at_command(apncommand, "OK", RETRIES, CMD_DELAY_MS) ||
            !send_at_command("AT+QIACT=1", "OK", RETRIES, CMD_DELAY_MS) || 
            !send_at_command("AT+QIACT?", "+QIACT", RETRIES, CMD_DELAY_MS)) {
            ESP_LOGW(TAG_GSM, "GSM setup failed on attempt %d. Retrying...", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1))); 
            continue;
        } else {
            success = true;
            ESP_LOGI(TAG_GSM, "GSM setup (PDP Configuration and Activation) successful!");
            break; 
        }
    }
    if (!success) {
        ESP_LOGE(TAG_GSM, "GSM setup failed after %d retries. Shutting down and exiting...", RETRIES);
        return false;
    }
  return true;
}


/**
*  @brief Deactivate pdp context
*/
bool deactivate_pdp() {
	const int retries_pdp_deactivate = 2;
	bool pdp_deactivated = false;
	for (int attempt = 0; attempt < retries_pdp_deactivate; attempt++) {
	    if (!send_at_command("AT+QIDEACT=1", "OK", RETRIES, CMD_DELAY_MS)) {
	        ESP_LOGW(TAG_GSM, "Failed to deactivate PDP context on attempt %d, retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	    } else {
	        ESP_LOGI(TAG_GSM, "PDP context deactivated successfully on attempt %d.", attempt + 1);
	        pdp_deactivated = true;
	        break;
	    }    
	} if (!pdp_deactivated) {
		error(3);
	    ESP_LOGE(TAG_GSM, "Failed to deactivate PDP context after %d attempts.", retries_pdp_deactivate);
	    return false;
	}
  return true;	
}

/**
*  @brief Power down modem
*/
void powerdown_modem() {
    const int retries_modem_power_down = 2;
    bool modem_powered_down = false;
    for (int attempt = 0; attempt < retries_modem_power_down; attempt++) {
        if (!send_at_command("AT+QPOWD=1", "OK", RETRIES, CMD_DELAY_MS)) {
            ESP_LOGW(TAG_GSM, "Failed to AT power down modem on attempt %d, retrying...\n", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
        } else {
            modem_powered_down = true;
            printf("Modem powered down successfully!\n");
            break;
        }
        
    } if (!modem_powered_down) {
		error(3);
        ESP_LOGE(TAG_GSM, "AT command power down failed after %d retries. Initiating Hardware Power Down...\n", retries_modem_power_down);
        hardware_poweroff();
    }
    
	// Clean up
	uart_driver_delete(MODEM_UART_NUM);	
  return;	
}

/*******************************************************************************************************************************
 FUNCTIONS: THINGSBOARD FIRMWARE GET OTA INFO
********************************************/
bool get_thingsboard_fw_info(void) {
	// Power on and initilaize modem
	if (!poweron_modem() || !activate_pdp()) {
		error(3);
	    ESP_LOGE(TAG_GSM, "Modem initialization failed!");
	    powerdown_modem();
        return false;
    } else {
        printf("Modem initialized successfully!\n");
    }	
    
	/**
	*  @brief Setting API URL for ThingsBoard Firmware Details Request
	*/
	// Derive the ThingsBoard device token from ESP32 Chip ID
	otagetChipIdString(device_token, sizeof(device_token)); 
		
    // Build the ThingsBoard API URL
    char url[512];           
    snprintf(url, sizeof(url),
             "http://%s/api/v1/%s/attributes?sharedKeys=fw_title,fw_version,fw_checksum,fw_checksum_algorithm",
             THINGSBOARD_HOST, device_token);                     
     ESP_LOGI(TAG_GSM, "API URL: %s", url);
	
    char httpget_cmd[100];
    sprintf(httpget_cmd, "AT+QHTTPURL=%d,80", strlen(url));
	
	const int retries_http_url = 3;
	bool http_url_set = false;
	for (int attempt = 0; attempt < retries_http_url; attempt++) {
	    if (!send_at_command(httpget_cmd,"CONNECT", RETRIES, CMD_DELAY_MS)) {
	        ESP_LOGW(TAG_GSM, "Failed to set API url on attempt %d, retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	        continue;
	    } else {
	        uart_write_bytes(MODEM_UART_NUM, url, strlen(url));
	        vTaskDelay(pdMS_TO_TICKS(2000)); 

	        // IMPORTANT: Wait for the final OK response after URL input
	        if (!send_at_command("", "OK", RETRIES, CMD_DELAY_MS)) {
	            ESP_LOGW(TAG_GSM, "URL configuration not confirmed with OK, retrying...");
	            continue;
	        }

	        http_url_set = true;
	        ESP_LOGI(TAG_GSM, "API url set successfully on attempt %d.", attempt + 1);
	        break;
	    }	    
	} if (!http_url_set) {
	    ESP_LOGE(TAG_GSM, "Failed to set API url after %d attempts. Shutting down and exiting...", retries_http_url);
	    powerdown_modem();
	    return false;
	}

	/**
	 * @brief Quectel HTTPS GET Request and Response Read
	 */
	bool get_success = false;
	for (int attempt = 0; attempt < RETRIES; attempt++) {
        if (!send_at_command("AT+QHTTPGET=80", "+QHTTPGET:", RETRIES, 10000)) {
	        ESP_LOGW(TAG_GSM, "HTTPS GET request failed on attempt %d. Retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	        continue;
        } else {
			// Once GET succeeds, try reading response
			for (int attempt = 0; attempt < RETRIES; attempt++) {
		        if (!send_at_command("AT+QHTTPREAD=80", "+QHTTPREAD: 0", RETRIES, 10000)) {
			        ESP_LOGW(TAG_GSM, "HTTPS Read response failed on attempt %d. Retrying...", attempt + 1);
			        vTaskDelay(pdMS_TO_TICKS(2000));
			        continue;
		        } else {
				    get_success = true;
				    ESP_LOGI(TAG_GSM, "HTTPS GET + READ completed successfully on attempt %d.", attempt + 1);				   
				    //printf("Thingsboard Response: %s", tb_fw_info);

	                // Process HTTPS JSON Response and Parse Firmware Info
	                char *json_start = strchr(tb_fw_info, '{');
	                char *json_end = strrchr(tb_fw_info, '}');	                
	                if (!json_start || !json_end || json_end <= json_start) {
	                    ESP_LOGE(TAG_GSM, "Failed to locate JSON in response!");
	                    ESP_LOGE(TAG_GSM, "Full response length: %d", strlen(tb_fw_info));
	                    ESP_LOGE(TAG_GSM, "First 100 chars: %.100s", tb_fw_info);
	                    powerdown_modem();
	                    return false;
	                }	                
	                // Calculate JSON length and copy to clean buffer
	                size_t json_length = json_end - json_start + 1;
	                char json_clean[json_length + 1];
	                memcpy(json_clean, json_start, json_length);
	                json_clean[json_length] = '\0';	                
	                ESP_LOGI(TAG_GSM, "Cleaned JSON: %s", json_clean);	
	                // Parse JSON
	                cJSON *root = cJSON_Parse(json_clean);
	                if (!root) {
	                    const char *error_ptr = cJSON_GetErrorPtr();
	                    if (error_ptr != NULL) {
	                        ESP_LOGE(TAG_GSM, "JSON parsing error before: %s", error_ptr);
	                    }
	                    ESP_LOGE(TAG_GSM, "Failed to parse JSON!");
	                    powerdown_modem();
	                    return false;
	                }				
				    // TB sometimes nests shared attributes under "shared"
				    cJSON *shared = cJSON_GetObjectItem(root, "shared");
				    cJSON *obj = shared ? shared : root;
				
				    cJSON *title = cJSON_GetObjectItem(obj, "fw_title");
				    cJSON *version = cJSON_GetObjectItem(obj, "fw_version");
				    cJSON *checksum = cJSON_GetObjectItem(obj, "fw_checksum");
				    cJSON *algorithm = cJSON_GetObjectItem(obj, "fw_checksum_algorithm");
				
				    memset(&tb_info, 0, sizeof(tb_fw_info_t));
				
				    if (cJSON_IsString(title)) strncpy(tb_info.fw_title, title->valuestring, sizeof(tb_info.fw_title) - 1);
				    if (cJSON_IsString(version)) strncpy(tb_info.fw_version, version->valuestring, sizeof(tb_info.fw_version) - 1);
				    if (cJSON_IsString(checksum)) strncpy(tb_info.fw_checksum, checksum->valuestring, sizeof(tb_info.fw_checksum) - 1);
				    if (cJSON_IsString(algorithm)) strncpy(tb_info.fw_checksum_algorithm, algorithm->valuestring, sizeof(tb_info.fw_checksum_algorithm) - 1);
				
					cJSON_Delete(root);
				
				    ESP_LOGI(TAG_GSM, "Fetched TB Firmware Info:");
				    ESP_LOGI(TAG_GSM, " Title: %s", tb_info.fw_title);
				    ESP_LOGI(TAG_GSM, " Version: %s", tb_info.fw_version);
				    ESP_LOGI(TAG_GSM, " Checksum: %s", tb_info.fw_checksum);
				    ESP_LOGI(TAG_GSM, " Algorithm: %s", tb_info.fw_checksum_algorithm);
				
					// --- Compare with current firmware version and title --- 
					if (strlen(tb_info.fw_title) > 0 && strlen(tb_info.fw_version) > 0) {
					    bool title_mismatch = strcmp(tb_info.fw_title, FIRMWARE_TITLE) != 0;
					    bool version_mismatch = strcmp(tb_info.fw_version, FIRMWARE_VERSION) != 0;
					
					    if (title_mismatch || version_mismatch) {
					        ota_required = true;
					        printf("New firmware available!\n");
					        printf("Current: %s (%s) | Remote: %s (%s)\n",
					                 FIRMWARE_TITLE, FIRMWARE_VERSION,
					                 tb_info.fw_title, tb_info.fw_version);
					    } else {
					        ota_required = false;
					        printf("Firmware up to date. Title: %s | Version: %s\n",
					                 FIRMWARE_TITLE, FIRMWARE_VERSION);
					    }
					} else {
					    ota_required = false;
					    ESP_LOGE(TAG_GSM, "Incomplete firmware attributes received from ThingsBoard.\n");
					}
				    break;
		        }
		    }
		   break;
        }
    }
    if (!get_success) {
	    ESP_LOGE(TAG_GSM, "HTTPS GET failed after %d attempts. Shutting down...", RETRIES);
	    powerdown_modem();
        return false;
    }	
	
	// Deinitialize and power off modem
	if (!deactivate_pdp()) {
		error(3);
	    ESP_LOGE(TAG_GSM, "Modem deinitialization failed!");
	    powerdown_modem();
    } else {
        printf("Modem deinitialized successfully!\n");
        powerdown_modem();     
    }
  return true;	
}
/*******************************************************************************************************************************/



/********************************************************************************************************************************
FUNCTIONS: OTA FUNCTIONS
************************/
/**
 * Initialize GSM OTA update
 */
esp_err_t gsm_ota_begin(gsm_ota_handle_t *handle, int firmware_size, bool bulk_erase) {
    if (handle == NULL || firmware_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize state
    handle->state = ESP_HTTPS_OTA_INIT;
    
    // Get OTA update partition
    handle->update_partition = esp_ota_get_next_update_partition(NULL);
    if (handle->update_partition == NULL) {
        ESP_LOGE(TAG_GSM, "No OTA update partition found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_GSM, "Writing to partition subtype %d at offset 0x%" PRIx32,
             handle->update_partition->subtype, handle->update_partition->address);

    // Allocate buffer for streaming
    handle->buffer_size = 4096; // 4KB chunks
    handle->ota_buffer = malloc(handle->buffer_size);
    if (handle->ota_buffer == NULL) {
        ESP_LOGE(TAG_GSM, "Failed to allocate OTA buffer");
        return ESP_ERR_NO_MEM;
    }

    handle->file_size = firmware_size;
    handle->bytes_received = 0;
    handle->bulk_flash_erase = bulk_erase;

    ESP_LOGI(TAG_GSM, "GSM OTA begin - Firmware size: %d bytes", firmware_size);
    ESP_LOGI(TAG_GSM, "Writing to partition: %s", handle->update_partition->label);

    handle->state = ESP_HTTPS_OTA_BEGIN;
    return ESP_OK;
}

/**
 * Read data from GSM modem with timeout handling
 */
int gsm_ota_read_data(gsm_ota_handle_t *handle, uint8_t *buffer, int max_len, int timeout_ms) {
    uint64_t start_time = esp_timer_get_time();
    const int64_t timeout_us = timeout_ms * 1000;
    
    while ((esp_timer_get_time() - start_time) < timeout_us) {
        int bytes_available = 0;
        if (uart_get_buffered_data_len(MODEM_UART_NUM, (size_t*)&bytes_available) == ESP_OK && bytes_available > 0) {
            int to_read = MIN(bytes_available, max_len);
            int bytes_read = uart_read_bytes(MODEM_UART_NUM, buffer, to_read, pdMS_TO_TICKS(10));
            
            if (bytes_read > 0) {
                return bytes_read;
            }
        }
        
        // Add a small delay to prevent busy waiting, but make it shorter
        vTaskDelay(pdMS_TO_TICKS(10)); // 10 mS responsive checking
    }
    
    return 0; // Timeout
}

/**
 * Verify chip ID from image header
 */
esp_err_t gsm_ota_verify_chip_id(const void *data) {
    esp_image_header_t *image_header = (esp_image_header_t *)data;
    
    if (image_header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        ESP_LOGE(TAG_GSM, "Mismatch chip id, expected %d, found %d", 
                 CONFIG_IDF_FIRMWARE_CHIP_ID, image_header->chip_id);
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

/**
 * Write data to OTA partition with progress reporting
 *
esp_err_t gsm_ota_write_data(gsm_ota_handle_t *handle, const void *data, size_t len) {
    esp_err_t err = esp_ota_write(handle->update_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_GSM, "OTA write failed: %s", esp_err_to_name(err));
        return err;
    }

    handle->bytes_received += len;

    // Log progress every 10% or when complete
    if ((handle->bytes_received % (handle->file_size / 10) == 0) || 
        (handle->bytes_received == handle->file_size)) {
        int progress = (handle->bytes_received * 100) / handle->file_size;
        ESP_LOGI(TAG_GSM, "Download progress: %d%% (%d/%d bytes)", 
                progress, handle->bytes_received, handle->file_size);
        tb_report_ota_status("DOWNLOADING", progress, NULL);
    }

    return ESP_OK;
}*/

/**
 * Write data to OTA partition with progress reporting
 */
esp_err_t gsm_ota_write_data(gsm_ota_handle_t *handle, const void *data, size_t len) {
    esp_err_t err = esp_ota_write(handle->update_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_GSM, "OTA write failed: %s", esp_err_to_name(err));
        return err;
    }

    int previous_received = handle->bytes_received;
    handle->bytes_received += len;

    // Calculate progress percentages
    int previous_progress = (previous_received * 100) / handle->file_size;
    int current_progress = (handle->bytes_received * 100) / handle->file_size;
    
    // Log progress every 10% increment or when complete
    if ((current_progress / 10) > (previous_progress / 10) || 
        (handle->bytes_received == handle->file_size)) {
        ESP_LOGI(TAG_GSM, "Download progress: %d%% (%d/%d bytes)", 
                current_progress, handle->bytes_received, handle->file_size);
        tb_report_ota_status("DOWNLOADING", current_progress, NULL);
    }

    return ESP_OK;
}

// TODO
/**
 * Stream firmware data to OTA partition - FIXED VERSION
 */
esp_err_t gsm_ota_perform(gsm_ota_handle_t *handle) {
    if (handle == NULL || handle->ota_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    switch (handle->state) {
	case ESP_HTTPS_OTA_BEGIN: {
	    // Begin OTA update process
	    const int erase_size = handle->bulk_flash_erase ? 
	        (handle->file_size > 0 ? handle->file_size : OTA_SIZE_UNKNOWN) : 
	        OTA_WITH_SEQUENTIAL_WRITES;
	
	    err = esp_ota_begin(handle->update_partition, erase_size, &handle->update_handle);
	    if (err != ESP_OK) {
	        ESP_LOGE(TAG_GSM, "OTA begin failed: %s", esp_err_to_name(err));
	        return err;
	    }
	
	    handle->bytes_received = 0; // Reset received counter
	    ESP_LOGI(TAG_GSM, "Starting firmware streaming via GSM...");
	
	    // Clear UART buffer completely before sending command
	    ESP_LOGI(TAG_GSM, "Clearing UART buffer before QHTTPREAD...");
	    int bytes_available = 0;
	    if (uart_get_buffered_data_len(MODEM_UART_NUM, (size_t*)&bytes_available) == ESP_OK && bytes_available > 0) {
	        uint8_t *clear_buf = malloc(bytes_available);
	        if (clear_buf) {
	            int cleared = uart_read_bytes(MODEM_UART_NUM, clear_buf, bytes_available, pdMS_TO_TICKS(100));
	            ESP_LOGI(TAG_GSM, "Cleared %d bytes from UART buffer before command", cleared);
	            free(clear_buf);
	        }
	    }
	
	    // Send QHTTPREAD command with 60-second packet interval
	    const char *qhttpread_cmd = "AT+QHTTPREAD=60\r\n";
	    ESP_LOGI(TAG_GSM, "Sending: %s", qhttpread_cmd);
	    uart_write_bytes(MODEM_UART_NUM, qhttpread_cmd, strlen(qhttpread_cmd));
	
	    // STEP 1: Read first chunk and process CONNECT
	    ESP_LOGI(TAG_GSM, "Reading initial data from UART...");
	    int bytes_read = uart_read_bytes(MODEM_UART_NUM, handle->ota_buffer, handle->buffer_size, 100);
	
	    if (bytes_read > 0) {
	        ESP_LOGI(TAG_GSM, "Read %d bytes from UART", bytes_read);
	
	        // Look for CONNECT in the buffer
	        char *connect_ptr = memmem(handle->ota_buffer, bytes_read, "CONNECT", 7);
	        if (connect_ptr) {
	            ESP_LOGI(TAG_GSM, "Found CONNECT in initial data");
	
	            // Calculate start of firmware data (skip CONNECT and any newlines)
	            uint8_t *firmware_start = (uint8_t *)(connect_ptr + 7); // Skip "CONNECT"
	
	            // Skip CR/LF after CONNECT
	            while (firmware_start < ((uint8_t *)handle->ota_buffer + bytes_read) &&
	                   (*firmware_start == '\r' || *firmware_start == '\n')) {
	                firmware_start++;
	            }
	
	            // Compute the number of actual firmware bytes in this first read
	            //int firmware_bytes = (int)(bytes_read - (firmware_start - (uint8_t *)handle->ota_buffer));
				int firmware_bytes = (int)((uint8_t *)handle->ota_buffer + bytes_read - firmware_start);
				ESP_LOGI(TAG_GSM, "First firmware chunk size: %d bytes", firmware_bytes);
	
	            if (firmware_bytes > 0) {
	                ESP_LOGI(TAG_GSM, "Found %d bytes of firmware data after CONNECT", firmware_bytes);
	
	                // Log first few bytes for verification
	                if (firmware_bytes >= 4) {
	                    ESP_LOGI(TAG_GSM, "First 4 firmware bytes: 0x%02X 0x%02X 0x%02X 0x%02X",
	                             firmware_start[0], firmware_start[1],
	                             firmware_start[2], firmware_start[3]);
	                }
	
	                // Write to OTA and update bytes_received
	                err = gsm_ota_write_data(handle, firmware_start, firmware_bytes);
	                if (err != ESP_OK) {
	                    return err;
	                }
	
	                handle->bytes_received += firmware_bytes; // ✅ Correct bytes tracking here 
			        // Debug to verify accumulation
			        ESP_LOGI(TAG_GSM, "DEBUG: After initial write - bytes_received=%d", handle->bytes_received);
	            } else {
	                ESP_LOGW(TAG_GSM, "No firmware data found after CONNECT");
	            }
	            
		        // Debug log
				ESP_LOGI(TAG_GSM, "DEBUG: initial_read=%d, firmware_bytes=%d, header_len=%d, bytes_received=%d, expected_total=%d",
				         bytes_read,
				         firmware_bytes,                // ✅ actual firmware bytes written
				         (int)(firmware_start - (uint8_t *)handle->ota_buffer),
				         handle->bytes_received,
				         handle->file_size);
				 
	         
	            
	        } else {
	            ESP_LOGE(TAG_GSM, "CONNECT not found in initial data!");
	            return ESP_FAIL;
	        }
	
	
	    } else {
	        ESP_LOGE(TAG_GSM, "No data received after QHTTPREAD command");
	        return ESP_FAIL;
	    }
	
	    // Move to in-progress streaming
	    handle->state = ESP_HTTPS_OTA_IN_PROGRESS;
            // Continue to read data - fall through
        }
        
		case ESP_HTTPS_OTA_IN_PROGRESS: {
		    const uint64_t now = esp_timer_get_time();
		    uint64_t last_data_time = now;
		    const uint64_t no_data_timeout_us = 70000000ULL; /* 70s in microseconds */
		    ESP_LOGI(TAG_GSM, "Starting streaming - expected size: %lld", (long long)handle->file_size);
		
		    bool size_known = (handle->file_size > 0);
		    bool finished_by_size = false;
		
		    /* Termination tokens can span reads — keep a small tail to detect split tokens */
		    #define TERM_TAIL_MAX 32
		    uint8_t term_tail[TERM_TAIL_MAX];
		    size_t term_tail_len = 0;
		    uint64_t last_progress_time = esp_timer_get_time(); // in microseconds
		
		    while (1) {
		        if ((esp_timer_get_time() - last_data_time) > no_data_timeout_us) {
		            ESP_LOGE(TAG_GSM, "Timeout - no data received for 70 seconds");
		            return ESP_ERR_TIMEOUT;
		        }
		
		        int bytes_to_read = handle->buffer_size;
		        if (size_known) {
		            int remaining = (int)(handle->file_size - handle->bytes_received);
		            if (remaining <= 0) { finished_by_size = true; break; }
		            bytes_to_read = (remaining < bytes_to_read) ? remaining : bytes_to_read;
		        }
		
		        int bytes_read = gsm_ota_read_data(handle, handle->ota_buffer, bytes_to_read, pdMS_TO_TICKS(100));
		        if (bytes_read > 0) {
		            last_data_time = esp_timer_get_time();
		
		            /* Combine tail + current buffer to reliably detect tokens that span boundaries */
		            size_t combined_max = TERM_TAIL_MAX + (size_t)bytes_read;
		            uint8_t *combined = NULL;
		            if (term_tail_len || bytes_read > 0) {
		                combined = malloc(combined_max);
		                if (!combined) {
		                    ESP_LOGE(TAG_GSM, "Out of memory for combined buffer");
		                    return ESP_ERR_NO_MEM;
		                }
		                memcpy(combined, term_tail, term_tail_len);
		                memcpy(combined + term_tail_len, handle->ota_buffer, bytes_read);
		            }
		
					/* If size unknown, search for explicit modem termination tokens first (prefer full sequences) */
					if (!size_known && combined) {
					    /* prefer full-line tokens to reduce false positives inside binary */
					    void *q0 = memmem(combined, term_tail_len + bytes_read, "+QHTTPREAD: 0\r\n", 15);
					    void *ok_full = memmem(combined, term_tail_len + bytes_read, "\r\nOK\r\n", 6);
					
					    if (q0 || ok_full) {
					        void *term_ptr = q0 ? q0 : ok_full;
					        const char *term_name = q0 ? "+QHTTPREAD: 0" : "OK";
					        
					        int write_len = (int)((uint8_t*)term_ptr - combined);
					        
					        // Write only bytes that came from the current read (not from tail)
					        if (write_len > (int)term_tail_len) {
					            int new_bytes_to_write = write_len - (int)term_tail_len;
					            err = gsm_ota_write_data(handle, handle->ota_buffer, new_bytes_to_write);
					            if (err != ESP_OK) {
					                free(combined);
					                return err;
					            }
					        }
					        
					        free(combined);
					        ESP_LOGI(TAG_GSM, "Found %s -> transfer complete (unknown size mode)", term_name);
					        break;
					    }
					    
					    /* If no termination found, write all new bytes from this read */
					    err = gsm_ota_write_data(handle, handle->ota_buffer, bytes_read);
					    if (err != ESP_OK) {
					        free(combined);
					        return err;
					    }
					    
					} else if (size_known) {
					    /* size known: simply write entire buffer read */
					    err = gsm_ota_write_data(handle, handle->ota_buffer, bytes_read);
					    if (err != ESP_OK) { 
					        if (combined) free(combined); 
					        return err; 
					    }
					} else if (!combined) {
					    /* Fallback for unknown size when malloc failed: write what we have */
					    err = gsm_ota_write_data(handle, handle->ota_buffer, bytes_read);
					    if (err != ESP_OK) return err;
					}

					uint64_t now = esp_timer_get_time();
					if ((now - last_progress_time) >= 2000000ULL) { // 2 seconds = 2,000,000 us
					    last_progress_time = now; // update last progress time					
			            /* Progress logging (avoid logging at 0) */
						if ((handle->bytes_received > 0 && (handle->bytes_received % 5000) == 0) || bytes_read > 100) {
						    if (size_known) {
						        ESP_LOGI(TAG_GSM, "Progress: %d/%d bytes (%.1f%%)",
						                 handle->bytes_received, (int)handle->file_size,  // FIX: %d for file_size
						                 (handle->bytes_received * 100.0) / handle->file_size);
						    } else {
						        ESP_LOGI(TAG_GSM, "Progress: %d bytes received", handle->bytes_received);
						    }
						}
					}

		            /* Safety cap */
		            if (size_known && handle->bytes_received > (handle->file_size * 11) / 10) {
		                ESP_LOGW(TAG_GSM, "Received >110%% expected size (%d > %lld) - stopping", handle->bytes_received, (long long)handle->file_size);
		                if (combined) free(combined);
		                break;
		            }
		
		            /* Update term_tail for next iteration: keep last TERM_TAIL_MAX bytes of combined stream */
		            if (combined) {
		                size_t combined_len = term_tail_len + (size_t)bytes_read;
		                term_tail_len = (combined_len > TERM_TAIL_MAX) ? TERM_TAIL_MAX : combined_len;
		                memcpy(term_tail, combined + (combined_len - term_tail_len), term_tail_len);
		                free(combined);
		            } else {
		                /* If no combined buffer (malloc failed), maintain a simple tail from current buffer */
		                term_tail_len = (bytes_read > (int)TERM_TAIL_MAX) ? TERM_TAIL_MAX : (size_t)bytes_read;
		                memcpy(term_tail, handle->ota_buffer + (bytes_read - (int)term_tail_len), term_tail_len);
		            }
		        } else {
		            vTaskDelay(pdMS_TO_TICKS(10));
		        }
		    } /* end streaming loop */
		
		    /* If we stopped because we reached expected size, wait short time for final OK */
		    if (size_known && finished_by_size) {
		        ESP_LOGI(TAG_GSM, "Expected size reached (%lld). Waiting up to 10s for final OK...", (long long)handle->file_size);
		        const uint64_t ok_timeout_us = 10000ULL * 1000ULL;
		        uint64_t ok_start = esp_timer_get_time();
		        bool ok_received = false;
		        uint8_t ok_buf[64];
		        while ((esp_timer_get_time() - ok_start) < ok_timeout_us) {
		            int r = gsm_ota_read_data(handle, ok_buf, sizeof(ok_buf) - 1, pdMS_TO_TICKS(100));
		            if (r > 0) {
		                ok_buf[r] = '\0';
		                /* prefer full newline-wrapped OK sequence */
		                void *ok_full = memmem(ok_buf, r, "\r\nOK\r\n", 6);
		                void *ok_partial = memmem(ok_buf, r, "\r\nOK", 4);
		                void *ok_alt = memmem(ok_buf, r, "OK\r\n", 4);
		                if (ok_full || ok_partial || ok_alt) {
		                    ok_received = true;
		                    ESP_LOGI(TAG_GSM, "Final OK received");
		                    break;
		                }
		                if (r > 2) {
		                    ESP_LOGW(TAG_GSM, "Unexpected data while waiting for OK: %.32s", ok_buf);
		                }
		            } else {
		                vTaskDelay(pdMS_TO_TICKS(10));
		            }
		        }
		
		        if (!ok_received) {
		            ESP_LOGW(TAG_GSM, "Final OK not observed within timeout; proceeding with OTA finalization");
		        }
		    }
		
		    /* Drain any remaining UART bytes (non-blocking) */
		    ESP_LOGI(TAG_GSM, "Clearing any remaining UART data...");
		    int avail = 0;
		    while (uart_get_buffered_data_len(MODEM_UART_NUM, (size_t*)&avail) == ESP_OK && avail > 0) {
		        /* protect against absurdly large avail values */
		        if (avail > 65536) avail = 65536;
		        uint8_t *tmp = malloc(avail);
		        if (tmp) {
		            int d = uart_read_bytes(MODEM_UART_NUM, tmp, avail, pdMS_TO_TICKS(100));
		            ESP_LOGI(TAG_GSM, "Cleared %d remaining bytes", d);
		            free(tmp);
		        } else {
		            ESP_LOGW(TAG_GSM, "Skipping drain allocation (no memory), breaking drain loop");
		            break;
		        }
		        vTaskDelay(pdMS_TO_TICKS(100));
		    }
		
		    handle->state = ESP_HTTPS_OTA_SUCCESS;
		    ESP_LOGI(TAG_GSM, "GSM OTA streaming completed successfully (received %d bytes)", handle->bytes_received);
		    break;
		}
        
        default:
            ESP_LOGE(TAG_GSM, "Invalid GSM OTA state: %d", handle->state);
            return ESP_ERR_INVALID_STATE;
    }
    return err;
}// TODO


/**
 * Check if OTA download is complete
 */
bool gsm_ota_is_complete_data_received(gsm_ota_handle_t *handle) {
    if (handle == NULL) {
        return false;
    }
    return (handle->bytes_received >= handle->file_size);
}

/**
 * Get number of bytes received
 */
int gsm_ota_get_bytes_received(gsm_ota_handle_t *handle) {
    if (handle == NULL) {
        return -1;
    }
    return handle->bytes_received;
}

/**
 * Complete GSM OTA update
 */
esp_err_t gsm_ota_finish(gsm_ota_handle_t *handle) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    // Free buffer
    if (handle->ota_buffer) {
        free(handle->ota_buffer);
        handle->ota_buffer = NULL;
    }

    // End OTA update if we were in progress or success state
    if (handle->state == ESP_HTTPS_OTA_IN_PROGRESS || handle->state == ESP_HTTPS_OTA_SUCCESS) {
        err = esp_ota_end(handle->update_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_GSM, "OTA end failed: %s", esp_err_to_name(err));
            // Don't return yet, we still need to clean up
        }
    }

    // Set boot partition only if download was successful and OTA end was successful
    if (handle->bytes_received == handle->file_size && err == ESP_OK) {
        err = esp_ota_set_boot_partition(handle->update_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_GSM, "Failed to set boot partition: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG_GSM, "GSM OTA update successful! Device will boot from new firmware on restart.");
            tb_report_ota_status("SUCCESS", 100, NULL);
        }
    } else {
        ESP_LOGE(TAG_GSM, "Incomplete download: %d/%d bytes", handle->bytes_received, handle->file_size);
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
    }

    return err;
}

/**
 * Abort GSM OTA update
 */
esp_err_t gsm_ota_abort(gsm_ota_handle_t *handle) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->ota_buffer) {
        free(handle->ota_buffer);
        handle->ota_buffer = NULL;
    }

    if (handle->state == ESP_HTTPS_OTA_IN_PROGRESS || handle->state == ESP_HTTPS_OTA_SUCCESS) {
        esp_ota_abort(handle->update_handle);
    }

    ESP_LOGI(TAG_GSM, "GSM OTA update aborted");
    tb_report_ota_status("FAILED", 0, "Update aborted");

    return ESP_OK;
}



/**
 *	Update the Firmware Dashboard for OTA Update Tracking TODO
 */
void tb_report_ota_status(const char *state, int progress, const char *error)
{

	/**
	 * @brief Quectel HTTPS GET Request and Response Read
	 *
	bool get_success = false;
	for (int attempt = 0; attempt < RETRIES; attempt++) {
        if (!send_at_command("AT+QHTTPGET=80", "+QHTTPGET:", RETRIES, 10000)) {
	        ESP_LOGW(TAG_GSM, "HTTPS Firmware GET request failed on attempt %d. Retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(2000));
	        continue;
        } else {
			// Once GET succeeds, try reading response
			for (int attempt = 0; attempt < RETRIES; attempt++) {
		        if (!send_at_command("AT+QHTTPREAD=80", "+QHTTPREAD: 0", RETRIES, 30000)) {
			        ESP_LOGW(TAG_GSM, "HTTPS Firmware Read response failed on attempt %d. Retrying...", attempt + 1);
			        vTaskDelay(pdMS_TO_TICKS(500));
			        continue;
		        } else {
				    get_success = true;
				    ESP_LOGI(TAG_GSM, "HTTPS GET + READ completed successfully on attempt %d.", attempt + 1);				   
					//printf("Thingsboard Response: %s", tb_fw_info);
					
					// Create a buffer to stream the binary downloads
					// or just stream all the data to uart so that I can
					// collect all of it in a .bin file and compute the SHA-256 and compare 
					// with the SHA-256 computed by thingsboard. This will confirm if 
					// the download was flawless and reliable.



				    break;
		        }
		    }
		   break;
        }
    }
    if (!get_success) {
	    ESP_LOGE(TAG_GSM, "HTTPS GET failed after %d attempts. Shutting down...", RETRIES);
        return;
    }*/
 
  return;
}


/**
 *	Perform secure gsm ota TODO
 */
bool perform_secure_ota_update() {
	// Power on and initilaize modem
	if (!poweron_modem() || !activate_pdp()) {
		error(3);
	    ESP_LOGE(TAG_GSM, "Modem initialization failed!");
	    powerdown_modem();
        return false;
    } else {
        printf("Modem initialized successfully!\n");
    }
	
    ESP_LOGI(TAG_GSM, "Starting Secure OTA update");
    //tb_report_ota_status("DOWNLOADING", 0, NULL); 
    
    // Construct full firmware URL
    char firmware_url[512];
    snprintf(firmware_url, sizeof(firmware_url),
             "https://%s/api/v1/%s/firmware?title=%s&version=%s",
             THINGSBOARD_HOST, device_token, tb_info.fw_title, tb_info.fw_version);            

    ESP_LOGI(TAG_GSM, "Firmware URL: %s", firmware_url);
	
    char httpget_cmd[100];
    sprintf(httpget_cmd, "AT+QHTTPURL=%d,80", strlen(firmware_url));
	
	const int retries_http_url = 3;
	bool http_url_set = false;
	for (int attempt = 0; attempt < retries_http_url; attempt++) {
	    if (!send_at_command(httpget_cmd,"CONNECT", RETRIES, CMD_DELAY_MS)) {
	        ESP_LOGW(TAG_GSM, "Failed to set API url on attempt %d, retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	        continue;
	    } else {
	        uart_write_bytes(MODEM_UART_NUM, firmware_url, strlen(firmware_url));
	        vTaskDelay(pdMS_TO_TICKS(2000)); 

	        // IMPORTANT: Wait for the final OK response after URL input
	        if (!send_at_command("", "OK", RETRIES, CMD_DELAY_MS)) {
	            ESP_LOGW(TAG_GSM, "URL configuration not confirmed with OK, retrying...");
	            continue;
	        }

	        http_url_set = true;
	        ESP_LOGI(TAG_GSM, "API url set successfully on attempt %d.", attempt + 1);
	        break;
	    }	    
	} if (!http_url_set) {
	    ESP_LOGE(TAG_GSM, "Failed to set API url after %d attempts. Shutting down and exiting...", retries_http_url);
	    powerdown_modem();
	    return false;
	}

	/**
	 * @brief Quectel HTTPS GET Request and Response Read
	 */
    int file_size = 0;
    bool get_success = false;
    for (int attempt = 0; attempt < RETRIES; attempt++) {
        if (!send_at_command("AT+QHTTPGET=80", "+QHTTPGET:", RETRIES, 15000)) {
            ESP_LOGW(TAG_GSM, "HTTPS Firmware Size GET failed on attempt %d", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
            continue;
        } else {      
			// Parse the response to get file size: +QHTTPGET: 0,200,237840
			char *qhttpget_ptr = strstr(response, "+QHTTPGET:");
			if (qhttpget_ptr) {
			    if (sscanf(qhttpget_ptr, "+QHTTPGET: 0,200,%d", &file_size) == 1) {
			        get_success = true;
			        ESP_LOGI(TAG_GSM, "Firmware Size GET successful. File size: %d bytes", file_size);
			        break;
			    } else {
			        ESP_LOGW(TAG_GSM, "Could not parse file size from QHTTPGET response: %s", qhttpget_ptr);
			    }
			} else {
			    ESP_LOGW(TAG_GSM, "QHTTPGET pattern not found in response: %s", response);
			}
        }
    }
    
    if (!get_success || file_size <= 0) {
        ESP_LOGE(TAG_GSM, "Failed to get valid file size");
        powerdown_modem();
        return false;
    }

	/**
	 *  @brief Query current URC port, temporarily disable URCs by routing to USB, and confirm change
	 */
	bool urc_deactivated = false;
	for (int attempt = 0; attempt < RETRIES; attempt++) {
	    if (!send_at_command("AT+QURCCFG=\"urcport\"", "+QURCCFG: \"urcport\"", RETRIES, CMD_DELAY_MS) ||
	        !send_at_command("AT+QURCCFG=\"urcport\",\"usbat\"", "OK", RETRIES, CMD_DELAY_MS) ||
	        !send_at_command("AT+QURCCFG=\"urcport\"", "+QURCCFG: \"urcport\",\"usbat\"", RETRIES, CMD_DELAY_MS)) {     
	        ESP_LOGW(TAG_GSM, "Failed to query, change, and confirm URC port on attempt %d, retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	        continue;
	    } else {
	        ESP_LOGI(TAG_GSM, "URC port successfully queried, changed, and confirmed on attempt %d.", attempt + 1);
	        urc_deactivated = true;
	        break;
	    }    
	}
	if (!urc_deactivated) {
	    error(3);
	    ESP_LOGE(TAG_GSM, "Failed to query, change, and confirm URC port after %d attempts.", RETRIES);
	    return false;
	}


    // Initialize GSM OTA
    gsm_ota_handle_t gsm_ota_handle;
    esp_err_t err = gsm_ota_begin(&gsm_ota_handle, file_size, true); // Use bulk erase for better performance
    if (err != ESP_OK) {
        ESP_LOGE(TAG_GSM, "GSM OTA begin failed");
        powerdown_modem();
        return false;
    }

    // Perform the OTA update
    err = gsm_ota_perform(&gsm_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_GSM, "GSM OTA perform failed");
        gsm_ota_abort(&gsm_ota_handle);
        powerdown_modem();
        return false;
    }

    // Finish OTA
    err = gsm_ota_finish(&gsm_ota_handle);	
	
	/**
	 *  @brief Restore URC port to UART1 after firmware streaming with robust error handling
	 */
	bool urc_restored = false;
	// STEP 1: Normal retry
	for (int attempt = 0; attempt < RETRIES; attempt++) {
	    if (!send_at_command("AT+QURCCFG=\"urcport\"", "+QURCCFG: \"urcport\"", RETRIES, CMD_DELAY_MS) ||
	        !send_at_command("AT+QURCCFG=\"urcport\",\"uart1\"", "OK", RETRIES, CMD_DELAY_MS) ||
	        !send_at_command("AT+QURCCFG=\"urcport\"", "+QURCCFG: \"urcport\",\"uart1\"", RETRIES, CMD_DELAY_MS)) { 
	        ESP_LOGW(TAG_GSM, "Attempt %d: Failed to query, change, and confirm URC port restoration, retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	        continue;
	    } else {
	        ESP_LOGI(TAG_GSM, "URC port successfully restored to UART1 on attempt %d.", attempt + 1);
	        urc_restored = true;
	        urc_to_usb = false;
	        break;
	    }
	}
	// STEP 2: If failed, soft reset the GSM and retry
	if (!urc_restored) {
	    ESP_LOGW(TAG_GSM, "Initial URC port restoration failed, attempting GSM reset...");
	    gsm_reset();
	    vTaskDelay(pdMS_TO_TICKS(5000)); // wait for module to reboot
	    for (int attempt = 0; attempt < RETRIES; attempt++) {
	        if (!send_at_command("AT+QURCCFG=\"urcport\",\"uart1\"", "OK", RETRIES, CMD_DELAY_MS) ||
	        	!send_at_command("AT+QURCCFG=\"urcport\"", "+QURCCFG: \"urcport\",\"uart1\"", RETRIES, CMD_DELAY_MS)) { 
	            ESP_LOGW(TAG_GSM, "Post-reset attempt %d: Failed to restore URC port, retrying...", attempt + 1);
	            vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	            continue;
	        } else {
	            ESP_LOGI(TAG_GSM, "URC port successfully restored to UART1 after GSM reset (attempt %d).", attempt + 1);
	            urc_restored = true;
	            urc_to_usb = false;
	            break;
	        }
	    }
	}
	// STEP 3: If still failed, power cycle the GSM module
	if (!urc_restored) {
	    ESP_LOGW(TAG_GSM, "URC port restoration still failed, performing modem power cycle...");
	    powerdown_modem();
	    vTaskDelay(pdMS_TO_TICKS(500));
	    poweron_modem();
	    for (int attempt = 0; attempt < RETRIES; attempt++) {
	        if (!send_at_command("AT+QURCCFG=\"urcport\",\"uart1\"", "OK", RETRIES, CMD_DELAY_MS) ||
	        	!send_at_command("AT+QURCCFG=\"urcport\"", "+QURCCFG: \"urcport\",\"uart1\"", RETRIES, CMD_DELAY_MS)) { 
	            ESP_LOGW(TAG_GSM, "Post power-cycle attempt %d: Failed to restore URC port, retrying...", attempt + 1);
	            vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1)));
	            continue;
	        } else {
	            ESP_LOGI(TAG_GSM, "URC port successfully restored to UART1 after modem power cycle (attempt %d).", attempt + 1);
	            urc_restored = true;
	            urc_to_usb = false;
	            break;
	        }
	    }
	}
	// Final Failure and Escalation
	if (!urc_restored) {
	    urc_to_usb = true;
	    error(3);
	    ESP_LOGE(TAG_GSM, "Critical: Failed to restore URC port to UART1 after all recovery attempts.");
	    return false;
	}

	// Deinitialize and power off modem
	if (!deactivate_pdp()) {
		error(3);
	    ESP_LOGE(TAG_GSM, "Modem deinitialization failed!");
	    powerdown_modem();
    } else {
        printf("Modem deinitialized successfully!\n");
        powerdown_modem();     
    }
  return true;
}

/*******************************************************************************************************************************/

// Query OTA flag
bool is_ota_required(void) {
    return ota_required;
}

// Identify running partition
void running_partition() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG_GSM, "Currently running partition: %s", running->label); 
    return;
}

/**
*	Query for new firmware and execute ota if found
*/
bool perform_ota() {
    // TB GET attributes and perform ota
    bool tb_fw_info = false;
	for (int attempt = 0; attempt < 3; attempt++) {   
	    if (get_thingsboard_fw_info()) {
	        ESP_LOGI(TAG_GSM, "ThingsBoard Firmware Attributes successfully retrieved.");
	        vTaskDelay(pdMS_TO_TICKS(50));
			    if (is_ota_required()) {
					printf("New Firmware Available, executing ota...\n");
					vTaskDelay(pdMS_TO_TICKS(50));		        
		            bool perform_secure_ota = false;
					for (int attempt = 0; attempt < 3; attempt++) {
						if (!perform_secure_ota_update()) {
							error(3);
						    ESP_LOGE(TAG_GSM, "Ota download failed!");
						    vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1))); // progressive back-off
						    continue;
					    } else {
					        printf("Ota download executed successfully!\n");	
					        perform_secure_ota = true;	
					        // esp_restart(); // Booting to the new OTA slot	TODO				        			          
					        break;      
					    }
					}
				    if (!perform_secure_ota) {
				        ESP_LOGE(TAG_GSM, "Ota download failed after %d retries.\n", RETRIES);
				        ota_flag = false; // reset the ota flag
				        return false;
				    }	
				   ota_flag = false; // reset the ota flag	  		        		        
			    } else {
	        		printf("No update needed, continuing normal operation.");
	        		ota_flag = false; // reset the ota flag
	        		return true;
	    		}
	    	tb_fw_info = true;
	    	break;
	    } else {
	        ESP_LOGE(TAG_GSM, "Failed to fetch firmware attributes from ThingsBoard.");	        
	        continue;
	    }
    }
    if (!tb_fw_info) {
        ESP_LOGE(TAG_GSM, "Failed to fetch firmware attributes from ThingsBoard after %d retries.\n", RETRIES);
        ota_flag = false; // reset the ota flag
        return false;
    }     
    return true;	
}
