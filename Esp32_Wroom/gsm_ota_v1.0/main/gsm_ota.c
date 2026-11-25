/**
 * WiFi OTA Driver Implementation
 * Developer: Eric Mulwa
 * Date: 06/10/2025
 * Last Modified: 06/10/2025
 */

#include "wifi_ota.h"

/*******************************************************************************************************************************
GLOBAL: Global Definitions
**************************/     
const char *TAG_OTA = "OTA";
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
uint32_t getChipId() {
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
void getChipIdString(char *device_token, size_t size) {
    uint32_t chipId = getChipId();
    snprintf(device_token, size, "%" PRIu32, chipId);  
    printf("Device Serial Number (Chip ID): %s\n", device_token); 
} 


/********************************************************************************************************************************
FUNCTIONS: OTA FUNCTIONS
************************/
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG_OTA, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG_OTA, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG_OTA, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG_OTA, "HEADER: %s: %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG_OTA, "DATA RECEIVED: %d bytes", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG_OTA, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG_OTA, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:  // Add this case
            ESP_LOGI(TAG_OTA, "HTTP_EVENT_REDIRECT, Location: %s", evt->header_value);
            break;
        default:
            ESP_LOGI(TAG_OTA, "Unhandled HTTP event: %d", evt->event_id);
            break;
    }
    return ESP_OK;
}

void perform_secure_ota_update() {
    ESP_LOGI(TAG_OTA, "Starting Secure OTA update");
    tb_report_ota_status("DOWNLOADING", 0, NULL); 
    
    // Construct full firmware URL
    char firmware_url[512];
    snprintf(firmware_url, sizeof(firmware_url),
             "https://%s/api/v1/%s/firmware?title=%s&version=%s",
             THINGSBOARD_HOST, device_token, tb_info.fw_title, tb_info.fw_version);            

    ESP_LOGI(TAG_OTA, "Firmware URL: %s", firmware_url);

    // Configure HTTPS client with certificate verification
    esp_http_client_config_t config = {
        .url = firmware_url,
        .buffer_size = 8 * 1024,
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Using ESP-IDF's certificate bundle
        .timeout_ms = 120000,
    };
    
	esp_https_ota_config_t ota_config = {
	    .http_config = &config,
	    .bulk_flash_erase = true,  // Faster write but may wear flash more
	    .max_http_request_size = 16 * 1024,
	};

    ESP_LOGI(TAG_OTA, "Attempting to download update from %s", config.url);
     // Simulate progress checkpoints for ThingsBoard visualization
    tb_report_ota_status("DOWNLOADING", 20, NULL);
    vTaskDelay(pdMS_TO_TICKS(1000));
    tb_report_ota_status("DOWNLOADING", 40, NULL);
    vTaskDelay(pdMS_TO_TICKS(1000));
    tb_report_ota_status("DOWNLOADING", 60, NULL);
    vTaskDelay(pdMS_TO_TICKS(1000));
    tb_report_ota_status("DOWNLOADING", 80, NULL);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
	// --- Execute OTA ---
    esp_err_t ret = esp_https_ota(&ota_config);
    
    tb_report_ota_status("VERIFYING", 90, NULL);
     
    if (ret == ESP_OK) {
        tb_report_ota_status("SUCCESS", 100, NULL);
        ESP_LOGI(TAG_OTA, "OTA Succeeded, Rebooting...");
        ota_flag = false; // reset the ota flag
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        ESP_LOGE(TAG_OTA, "Firmware upgrade failed: %s", esp_err_to_name(ret));
		tb_report_ota_status("FAILED", -1, esp_err_to_name(ret));
    }
    
    vTaskDelete(NULL);
}
/*******************************************************************************************************************************/


/*******************************************************************************************************************************
 FUNCTIONS: THINGSBOARD FIRMWARE GET OTA INFO
********************************************/
esp_err_t tb_client_event_handler(esp_http_client_event_handle_t evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            printf("HTTP_EVENT_ON_DATA, len=%d\n", evt->data_len);
            // Check if we have space to store this chunk
            if (http_response_length + evt->data_len < MAX_RESPONSE_SIZE) {
                memcpy(http_response_buffer + http_response_length, evt->data, evt->data_len);
                http_response_length += evt->data_len;
            } else {
                ESP_LOGE(TAG_OTA, "Response too large for buffer!\n");
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            printf("HTTP_EVENT_ON_FINISH\n");
            // Ensure null termination
            if (http_response_length < MAX_RESPONSE_SIZE) {
                http_response_buffer[http_response_length] = '\0';
            }
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

bool get_thingsboard_fw_info(void) {
	
    if (!wifi_connected) {
        ESP_LOGE(TAG_OTA, "Wi-Fi not connected. Cannot query ThingsBoard.");
        return false;
    }
    
	// check the esp mac address and use it as TB Token
	getChipIdString(device_token, sizeof(device_token)); 

    // Clear response buffer
    http_response_length = 0;
    memset(http_response_buffer, 0, sizeof(http_response_buffer));

    // Build the API URL
    char url[512];
    snprintf(url, sizeof(url),
             "https://%s/api/v1/%s/attributes?sharedKeys=fw_title,fw_version,fw_checksum,fw_checksum_algorithm",
             THINGSBOARD_HOST, device_token);

    // Configure HTTPS client
    esp_http_client_config_t config_get = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .event_handler = tb_client_event_handler,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config_get);
    if (client == NULL) {
        ESP_LOGE(TAG_OTA, "Failed to initialize HTTP client!");
        return false;
    }

    // Set headers
    esp_http_client_set_header(client, "accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    // Execute GET request
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_OTA, "HTTP GET request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG_OTA, "HTTP Status Code: %d", status_code);

    if (status_code != 200) {
        ESP_LOGE(TAG_OTA, "Failed to get firmware attributes. HTTP %d", status_code);
        esp_http_client_cleanup(client);
        return false;
    }

    // Verify we received data
    if (http_response_length == 0) {
        ESP_LOGE(TAG_OTA, "Empty response received from ThingsBoard.");
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI(TAG_OTA, "Response: %.100s", http_response_buffer);

    // Parse JSON
    cJSON *root = cJSON_Parse(http_response_buffer);
    if (!root) {
        ESP_LOGE(TAG_OTA, "JSON parsing error!");
        esp_http_client_cleanup(client);
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
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG_OTA, "Fetched TB Firmware Info:");
    ESP_LOGI(TAG_OTA, " Title: %s", tb_info.fw_title);
    ESP_LOGI(TAG_OTA, " Version: %s", tb_info.fw_version);
    ESP_LOGI(TAG_OTA, " Checksum: %s", tb_info.fw_checksum);
    ESP_LOGI(TAG_OTA, " Algorithm: %s", tb_info.fw_checksum_algorithm);

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
	        ESP_LOGI("OTA", "Firmware up to date. Title: %s | Version: %s\n",
	                 FIRMWARE_TITLE, FIRMWARE_VERSION);
	    }
	} else {
	    ota_required = false;
	    ESP_LOGE("OTA", "Incomplete firmware attributes received from ThingsBoard.\n");
	}
	return true;
}

// Query OTA flag
bool is_ota_required(void) {
    return ota_required;
}

// Identify running partition
void running_partition() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG_OTA, "Currently running partition: %s", running->label); 
}

/**
*	Querry for new firmware and execute ota if found
*/
bool perform_ota() {
    // TB GET attributes and perform ota
    if (get_thingsboard_fw_info()) {
        ESP_LOGI(TAG_OTA, "ThingsBoard Firmware Attributes successfully retrieved.");
        vTaskDelay(pdMS_TO_TICKS(50));
		    if (is_ota_required()) {
				printf("New Firmware Available, executing ota...\n");
				vTaskDelay(pdMS_TO_TICKS(50));
		        xTaskCreate(&perform_secure_ota_update, "ota_update_task", 8192, NULL, 5, NULL);
		    } else {
        		ESP_LOGI("OTA", "No update needed, continuing normal operation.");
        		ota_flag = false; // reset the ota flag
        		return true;
    		}
    } else {
        ESP_LOGE(TAG_OTA, "Failed to fetch firmware attributes from ThingsBoard.");
        return false;
    }
    return true;	
}


/**
 *	Update the Firmware Dashboard for OTA Update Tracking
 */
void tb_report_ota_status(const char *state, int progress, const char *error)
{
    // Construct full firmware URL
    char firmware_url[512];
    snprintf(firmware_url, sizeof(firmware_url),
             "https://%s/api/v1/%s/firmware?title=%s&version=%s",
             THINGSBOARD_HOST, device_token, tb_info.fw_title, tb_info.fw_version);  
	
    // --- TELEMETRY Payload (dashboard live updates) ---
    cJSON *telemetry = cJSON_CreateObject();
   
    // Core OTA progress/state updates
    if (state) cJSON_AddStringToObject(telemetry, "fw_state", state);   // DOWNLOADING, VERIFYING, SUCCESS, FAILED
    cJSON_AddNumberToObject(telemetry, "fw_progress", progress);        // 0–100% progress
    
    // Current firmware metadata
    cJSON_AddStringToObject(telemetry, "current_fw_title", FIRMWARE_TITLE);
    cJSON_AddStringToObject(telemetry, "current_fw_version", FIRMWARE_VERSION);

    // Target firmware metadata
    cJSON_AddStringToObject(telemetry, "target_fw_title", tb_info.fw_title);
    cJSON_AddStringToObject(telemetry, "target_fw_version", tb_info.fw_version);

    // Timestamp for target firmware set
    // cJSON_AddNumberToObject(telemetry, "target_fw_ts", (long long)time(NULL) * 1000);

    // Optional error message
    if (error) cJSON_AddStringToObject(telemetry, "fw_error", error);

    // --- ATTRIBUTES Payload (static values) ---
    cJSON *attributes = cJSON_CreateObject();
    if (tb_info.fw_checksum[0] != '\0')
    cJSON_AddStringToObject(attributes, "fw_checksum", tb_info.fw_checksum);
    cJSON_AddStringToObject(attributes, "fw_url", firmware_url);
    cJSON_AddStringToObject(attributes, "deviceName", device_token); // Mac ID as Name

    // --- Convert to strings ---
    char *telemetry_str = cJSON_PrintUnformatted(telemetry);
    char *attributes_str = cJSON_PrintUnformatted(attributes);

    // --- URLs for ThingsBoard API ---
    char url_telemetry[256];
    char url_attributes[256];

    snprintf(url_telemetry, sizeof(url_telemetry), "https://%s/api/v1/%s/telemetry", THINGSBOARD_HOST, device_token);
    snprintf(url_attributes, sizeof(url_attributes), "https://%s/api/v1/%s/attributes", THINGSBOARD_HOST, device_token);

    // --- Send Telemetry ---
    esp_http_client_config_t cfg_t = {
        .url = url_telemetry,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000
    };
    esp_http_client_handle_t client_t = esp_http_client_init(&cfg_t);
    esp_http_client_set_header(client_t, "Content-Type", "application/json");
    esp_http_client_set_post_field(client_t, telemetry_str, strlen(telemetry_str));
    esp_http_client_perform(client_t);
    esp_http_client_cleanup(client_t);

    // --- Send Attributes (once per report) ---
    esp_http_client_config_t cfg_a = {
        .url = url_attributes,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000
    };
    esp_http_client_handle_t client_a = esp_http_client_init(&cfg_a);
    esp_http_client_set_header(client_a, "Content-Type", "application/json");
    esp_http_client_set_post_field(client_a, attributes_str, strlen(attributes_str));
    esp_http_client_perform(client_a);
    esp_http_client_cleanup(client_a);

    // --- Logging ---
    ESP_LOGI(TAG_OTA, "Reported OTA state: %s | Progress: %d%%", state ? state : "N/A", progress);

    // --- Cleanup ---
    cJSON_Delete(telemetry);
    cJSON_Delete(attributes);
    free(telemetry_str);
    free(attributes_str);
}
/*******************************************************************************************************************************/