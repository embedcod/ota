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
// OTA Flag
bool ota_required = false;
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
            vTaskDelay(pdMS_TO_TICKS(50)); 
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
            vTaskDelay(pdMS_TO_TICKS(2000)); 
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
	        vTaskDelay(pdMS_TO_TICKS(2000)); 
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
            vTaskDelay(pdMS_TO_TICKS(2000)); 
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
	    at_poweroff();
	    return false;
	}

	/**
	 * @brief Quectel HTTPS GET Request and Response Read
	 */
	bool get_success = false;
	for (int attempt = 0; attempt < RETRIES; attempt++) {
        if (!send_at_command("AT+QHTTPGET=80", "+QHTTPGET:", RETRIES, 10000)) {
	        ESP_LOGW(TAG_GSM, "HTTPS GET request failed on attempt %d. Retrying...", attempt + 1);
	        vTaskDelay(pdMS_TO_TICKS(2000));
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
 *	Update the Firmware Dashboard for OTA Update Tracking TODO
 */
void tb_report_ota_status(const char *state, int progress, const char *error)
{

 
  return;
}


/**
 *	Perform secure gsm ota TODO
 */
void perform_secure_ota_update() {
	// Power on and initilaize modem
	if (!poweron_modem() || !activate_pdp()) {
		error(3);
	    ESP_LOGE(TAG_GSM, "Modem initialization failed!");
	    powerdown_modem();
        return;
    } else {
        printf("Modem initialized successfully!\n");
    }
	
	/**
	*  @brief Setting API URL for ThingsBoard Firmware Details Request
	*/
	// Derive the ThingsBoard device token from ESP32 Chip ID
	otagetChipIdString(device_token, sizeof(device_token)); 


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
	    at_poweroff();
	    return;
	}


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


	
	/**
	 * @brief Quectel HTTPS GET Request and Response Read
	 */
    int file_size = 0;
    bool get_success = false;
    for (int attempt = 0; attempt < RETRIES; attempt++) {
        if (!send_at_command("AT+QHTTPGET=80", "+QHTTPGET:", RETRIES, 15000)) {
            ESP_LOGW(TAG_GSM, "HTTPS Firmware GET failed on attempt %d", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        } else {      
			// Parse the response to get file size: +QHTTPGET: 0,200,237840
			char *qhttpget_ptr = strstr(response, "+QHTTPGET:");
			if (qhttpget_ptr) {
			    if (sscanf(qhttpget_ptr, "+QHTTPGET: 0,200,%d", &file_size) == 1) {
			        get_success = true;
			        ESP_LOGI(TAG_GSM, "Firmware GET successful. File size: %d bytes", file_size);
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
        return;
    }


	
	
	
	
	
	
	
	
	
	
	// Deinitialize and power off modem
	if (!deactivate_pdp()) {
		error(3);
	    ESP_LOGE(TAG_GSM, "Modem Deinitialization failed!");
	    powerdown_modem();
    } else {
        printf("Modem initialized successfully!\n");
        powerdown_modem();
    }
   return;
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
*	Querry for new firmware and execute ota if found
*/
bool perform_ota() {
    // TB GET attributes and perform ota
    if (get_thingsboard_fw_info()) {
        ESP_LOGI(TAG_GSM, "ThingsBoard Firmware Attributes successfully retrieved.");
        vTaskDelay(pdMS_TO_TICKS(50));
		    if (is_ota_required()) {
				printf("New Firmware Available, executing ota...\n");
				vTaskDelay(pdMS_TO_TICKS(50));
		        // xTaskCreate(&perform_secure_ota_update, "ota_update_task", 8192, NULL, 5, NULL); // OTA task has the highest priority TODO
		        perform_secure_ota_update();
		    } else {
        		printf("No update needed, continuing normal operation.");
        		ota_flag = false; // reset the ota flag
        		return true;
    		}
    } else {
        ESP_LOGE(TAG_GSM, "Failed to fetch firmware attributes from ThingsBoard.");
        ota_flag = false; // reset the ota flag
        powerdown_modem(); // Power off modem and deinitialize uart
        return false;
    }
    return true;	
}
