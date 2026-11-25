/**
*	WiFi OTA ESP32-S3-WROOM-1-N8R8
*		Developer: 		Eric Mulwa
*		Date:	   		06/10/2025
*		Last Modified:  06/10/2025
*/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"  
#include "driver/gpio.h"
#include "string.h"
#include <time.h>
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"

/*******************************************************************************************************************************
FIRMWARE: METADATA
*******************/ 
#define FIRMWARE_TITLE    "ota_test"
#define FIRMWARE_VERSION  "v1.0"

/*******************************************************************************************************************************
DEFINITIONS: GLOBAL
*******************/ 
#define HASH_LEN 32
#define LED_GPIO 41
#define OTA_URL_SIZE 256
#define RUN_TIME_BEFORE_OTA_MS 300000  // 5 minutes (300s)
/*******************************************************************************************************************************/


/*******************************************************************************************************************************
WIFI: HTTPS CLIENT OVER SSL & CONFIGURATIONS
*******************************************/ 
char WIFI_SSID[32] = "Geviton";
char WIFI_PASS[64] = "Geviton#4573";
bool networkError = false;
bool apiError = false;
static bool wifi_connected = false;
static int reconnect_attempts = 0;
#define MAX_RECONNECT_ATTEMPTS 4
/*******************************************************************************************************************************/


/*******************************************************************************************************************************
GLOBAL: Global Definitions
**************************/     
static uint32_t storedChipId = 0;
static const char *TAG = "OTA";
/*******************************************************************************************************************************/


/*******************************************************************************************************************************
THINGSBOARD: GET DEFINITIONS
***************************/  
#define MAX_RESPONSE_SIZE 2048
static char http_response_buffer[MAX_RESPONSE_SIZE];
static size_t http_response_length = 0;
// ThingsBoard GET credentials
#define THINGSBOARD_HOST "demo.thingsboard.io"
static char device_token[12];
// OTA variables struct
typedef struct {
    char fw_title[128];
    char fw_version[64];
    char fw_checksum[128];
    char fw_checksum_algorithm[32];
} tb_fw_info_t;
tb_fw_info_t tb_info;
// OTA Flag
static bool ota_required = false;
/*******************************************************************************************************************************/


/*********************************************************************************************************************************
FUNCTIONS: PROTOTYPES
**********************/
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void wifi_connection();
void wifi_disconnect();
esp_err_t _http_event_handler(esp_http_client_event_t *evt);
void perform_secure_ota_update();
// Test Led
void initializations();
void blink_led();
// Thingsboard GET
esp_err_t tb_client_event_handler(esp_http_client_event_handle_t evt);
bool get_thingsboard_fw_info(void);
// Querry if OTA is needed
bool is_ota_required(void);
// perform ota
bool perform_ota();
// CHIP MAC {TB Access Token}
uint32_t getChipId();
void getChipIdString(char *device_token, size_t size);
/*******************************************************************************************************************************/


/*******************************************************************************************************************************
FUNCTION: THE MAIN APPLICATION
*****************************/
void app_main(void) {
    ESP_LOGI(TAG, "Secure OTA Firmware started");
      
	initializations();
    
    // Initialize Wi-Fi connection
    wifi_connection();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Identify running partition
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Currently running partition: %s", running->label); 
    
    // Check if there is an ota update after every minute
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool ota_flag = false;
    
    while (!ota_flag) {
        blink_led();
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) >= RUN_TIME_BEFORE_OTA_MS) {
            ota_flag = true;
            ESP_LOGI(TAG, "OTA flag triggered, starting OTA update...");
            perform_ota();
        }
    }
}
/*******************************************************************************************************************************/

/************************************************************************************************************************************** 
FUNCTIONS: INITIALIZATION
************************/

void initializations() {
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Initialize LED GPIO
    esp_rom_gpio_pad_select_gpio(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);	
}

// LED blinking patterns
void blink_led() {
    static bool led_state = false;
    led_state = !led_state;
    gpio_set_level(LED_GPIO, led_state);
    vTaskDelay(pdMS_TO_TICKS(500));
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
/*********************************************************************************************************************************
FUNCTIONS: WiFi CONNECTION, HANDLING & DISCONNECTION
****************************************************/
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        printf("WiFi connecting ... \n");
        break;
    case WIFI_EVENT_STA_CONNECTED:
        printf("WiFi connected ... \n");
        wifi_connected = true;
        reconnect_attempts = 0; // Reset counter on successful connection
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        printf("WiFi disconnected ... \n");
        wifi_connected = false;
        if (reconnect_attempts < MAX_RECONNECT_ATTEMPTS)
        {
            reconnect_attempts++;
            printf("Reconnecting... Attempt %d/%d\n", reconnect_attempts, MAX_RECONNECT_ATTEMPTS);
            esp_wifi_connect();
        }
        else
        {
            printf("Max reconnection attempts reached.\n");
            //wifi_disconnect();
        }
        break;
    case IP_EVENT_STA_GOT_IP:
        printf("WiFi got IP ... \n\n");
        break;
    default:
        break;
    }
}

void wifi_connection()
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_initiation);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

	wifi_config_t wifi_configuration = { 0 }; // Initialize struct to avoid garbage values
	strcpy((char *)wifi_configuration.sta.ssid, WIFI_SSID);
	strcpy((char *)wifi_configuration.sta.password, WIFI_PASS);
	
	esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_start();
	esp_wifi_connect();
    printf("Waiting for WiFi reconnection to timeout...\n");
	while (reconnect_attempts < MAX_RECONNECT_ATTEMPTS && !wifi_connected) {
	    printf(".");
	    vTaskDelay(pdMS_TO_TICKS(300));
	} 
}

void wifi_disconnect()
{
    printf("Disconnecting WiFi...\n");
    
    // Unregister WiFi event handlers to prevent duplicate handling
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);

    // Stop and deinit WiFi properly
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    // Destroy the default network interface
    esp_netif_destroy(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));

    // Delete the event loop
    esp_event_loop_delete_default();

    wifi_connected = false;
    reconnect_attempts = 0;
    printf("WiFi disconnected and resources freed.\n");
}
/*******************************************************************************************************************************/


/********************************************************************************************************************************
FUNCTIONS: OTA FUNCTIONS
************************/
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HEADER: %s: %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "DATA RECEIVED: %d bytes", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:  // Add this case
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT, Location: %s", evt->header_value);
            break;
        default:
            ESP_LOGI(TAG, "Unhandled HTTP event: %d", evt->event_id);
            break;
    }
    return ESP_OK;
}

void perform_secure_ota_update() {
    ESP_LOGI(TAG, "Starting Secure OTA update");
    
    // Construct full firmware URL
    char firmware_url[512];
    snprintf(firmware_url, sizeof(firmware_url),
             "https://%s/api/v1/%s/firmware?title=%s&version=%s",
             THINGSBOARD_HOST, device_token, tb_info.fw_title, tb_info.fw_version);            

    ESP_LOGI(TAG, "Firmware URL: %s", firmware_url);

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

    ESP_LOGI(TAG, "Attempting to download update from %s", config.url);
    esp_err_t ret = esp_https_ota(&ota_config);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Succeeded, Rebooting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed: %s", esp_err_to_name(ret));
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
                ESP_LOGE(TAG, "Response too large for buffer!\n");
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
        ESP_LOGE(TAG, "Wi-Fi not connected. Cannot query ThingsBoard.");
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
        ESP_LOGE(TAG, "Failed to initialize HTTP client!");
        return false;
    }

    // Set headers
    esp_http_client_set_header(client, "accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    // Execute GET request
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status Code: %d", status_code);

    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to get firmware attributes. HTTP %d", status_code);
        esp_http_client_cleanup(client);
        return false;
    }

    // Verify we received data
    if (http_response_length == 0) {
        ESP_LOGE(TAG, "Empty response received from ThingsBoard.");
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI(TAG, "Response: %.100s", http_response_buffer);

    // Parse JSON
    cJSON *root = cJSON_Parse(http_response_buffer);
    if (!root) {
        ESP_LOGE(TAG, "JSON parsing error!");
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

    ESP_LOGI(TAG, "Fetched TB Firmware Info:");
    ESP_LOGI(TAG, " Title: %s", tb_info.fw_title);
    ESP_LOGI(TAG, " Version: %s", tb_info.fw_version);
    ESP_LOGI(TAG, " Checksum: %s", tb_info.fw_checksum);
    ESP_LOGI(TAG, " Algorithm: %s", tb_info.fw_checksum_algorithm);

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

// Function to query OTA flag
bool is_ota_required(void) {
    return ota_required;
}


/**
*	Querry for new firmware repeatdely and excute ota if found
*/
bool perform_ota() {
    // TB GET attributes and perform ota
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (get_thingsboard_fw_info()) {
        ESP_LOGI(TAG, "ThingsBoard Firmware Attributes successfully retrieved.");
        vTaskDelay(pdMS_TO_TICKS(50));
		    if (is_ota_required()) {
				printf("New Firmware Available, executing ota...\n");
				vTaskDelay(pdMS_TO_TICKS(50));
		        xTaskCreate(&perform_secure_ota_update, "ota_update_task", 8192, NULL, 5, NULL);
		    } else {
        		ESP_LOGI("OTA", "No update needed, continuing normal operation.");
    		}
    } else {
        ESP_LOGE(TAG, "Failed to fetch firmware attributes from ThingsBoard.");
        return false;
    }
    return true;	
}
/*******************************************************************************************************************************/