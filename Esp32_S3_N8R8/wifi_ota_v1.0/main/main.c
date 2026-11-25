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

#define HASH_LEN 32
#define LED_GPIO 41
#define OTA_URL_SIZE 256
#define RUN_TIME_BEFORE_OTA_MS 20000  // 20 seconds

static const char *TAG = "OTA";

// Wi-Fi Config
char WIFI_SSID[32] = "Geviton";
char WIFI_PASS[64] = "Geviton#4573";
bool networkError = false;
bool apiError = false;
static bool wifi_connected = false;
static int reconnect_attempts = 0;
#define MAX_RECONNECT_ATTEMPTS 4

//const char *URL = "https://github.com/embedcod/ESP-OTA-Test/blob/main/app-template.bin";
const char *URL = "https://raw.githubusercontent.com/embedcod/ESP-OTA-Test/main/app-template.bin";

// ========== Your Wi-Fi Functions ========== //
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void wifi_connection();
void wifi_disconnect();


// LED blinking patterns
void blink_led() {
    static bool led_state = false;
    led_state = !led_state;
    gpio_set_level(LED_GPIO, led_state);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

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
    
    // Configure HTTPS client with certificate verification
    esp_http_client_config_t config = {
        .url = URL,
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

void app_main(void) {
    ESP_LOGI(TAG, "Secure OTA Firmware started");
      
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
    
    // Initialize Wi-Fi connection
    wifi_connection();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Identify running partition
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Currently running partition: %s", running->label);

    // Run until OTA flag triggers
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool ota_flag = false;

    while (!ota_flag) {
        blink_led();
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) >= RUN_TIME_BEFORE_OTA_MS) {
            ota_flag = true;
            ESP_LOGI(TAG, "OTA flag triggered, starting OTA update...");
            xTaskCreate(&perform_secure_ota_update, "ota_update_task", 8192, NULL, 5, NULL);
        }
    }
}

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