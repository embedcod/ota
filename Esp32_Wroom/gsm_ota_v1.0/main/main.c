/**
*	GSM OTA ESP32-WROOM-32
*		Developer: 		Eric Mulwa
*		Date:	   		01/11/2025
*		Last Modified:  01/11/2025
*/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "string.h"
#include <time.h>
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "wifi_ota.h"

/*******************************************************************************************************************************
DEFINITIONS: GLOBAL
*******************/ 
#define LED_GPIO 41
#define RUN_TIME_BEFORE_OTA_MS 60000  // 1 minute(s)
/*******************************************************************************************************************************/


/*******************************************************************************************************************************
WIFI: HTTPS CLIENT OVER SSL & CONFIGURATIONS
*******************************************/ 
char WIFI_SSID[32] = "Geviton";
char WIFI_PASS[64] = "Geviton#4573";
bool networkError = false;
bool apiError = false;
bool wifi_connected = false;
int reconnect_attempts = 0;
#define MAX_RECONNECT_ATTEMPTS 4
/*******************************************************************************************************************************/


/*******************************************************************************************************************************
GLOBAL: Global Definitions
**************************/     
static const char *TAG = "OTA";
bool ota_flag = false;
/*******************************************************************************************************************************/


/*********************************************************************************************************************************
FUNCTIONS: PROTOTYPES
**********************/
void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void wifi_connection();
void wifi_disconnect();
// Test Led
void blink_led();
/*******************************************************************************************************************************/


/*******************************************************************************************************************************
FUNCTION: THE MAIN APPLICATION
*****************************/
void app_main(void) {
    ESP_LOGI(TAG, "Secure OTA Firmware started");
      
	initialize_nvs();
    
    // Connect to WiFi
    wifi_connection();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Identify running partition
    running_partition();
    
    // Check if there is an ota update after every minute
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
   
    while (!ota_flag) {
        blink_led();
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) >= RUN_TIME_BEFORE_OTA_MS) {
            ota_flag = true;
            ESP_LOGI(TAG, "OTA flag triggered, starting OTA update...");
            perform_ota();
            start_time = xTaskGetTickCount() * portTICK_PERIOD_MS; // reset timer
        }
    }
}
/*******************************************************************************************************************************/

/******************************************************************************************************************************** 
FUNCTIONS: INITIALIZATION
************************/

// LED blinking patterns
void blink_led() {
    // Initialize LED GPIO
    esp_rom_gpio_pad_select_gpio(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);	
	
    static bool led_state = false;
    led_state = !led_state;
    gpio_set_level(LED_GPIO, led_state);
    vTaskDelay(pdMS_TO_TICKS(2000));
}
/*********************************************************************************************************************************
FUNCTIONS: WiFi CONNECTION, HANDLING & DISCONNECTION
****************************************************/
void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
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

/*******************************************************************************************************************************/