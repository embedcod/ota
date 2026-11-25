#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "string.h"
#include <time.h>
#include "nvs.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "gsm_ota.h"

/*******************************************************************************************************************************
DEFINITIONS: GLOBAL
*******************/ 
#define LED_GPIO 15
#define RUN_TIME_BEFORE_OTA_MS 20000  // 20 second (s)
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
// Test Led
void blink_led();
void error(int count);

/*******************************************************************************************************************************
FUNCTION: THE MAIN APPLICATION
*****************************/
void app_main(void) {
    ESP_LOGI(TAG, "Secure GSM OTA Firmware started");
    vTaskDelay(pdMS_TO_TICKS(2000));
    
	initialize_nvs();

    // Identify running partition
    running_partition();
    
    // Check if there is an ota update after every minute
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
   
    while (1) {
        blink_led();
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) >= RUN_TIME_BEFORE_OTA_MS) {
            ota_flag = true;
            ESP_LOGI(TAG, "GSM OTA flag triggered, starting OTA update...");
            // Gsm ota execution
            bool ota_executed = false;
			for (int attempt = 0; attempt < 3; attempt++) {
				if (!perform_ota()) {
					error(3);
				    ESP_LOGE(TAG_GSM, "Ota update execution failed!");
				    vTaskDelay(pdMS_TO_TICKS(4000));
				    continue;
			    } else {
			        printf("Ota update executed successfully!\n");	
			        ota_executed = true;	  
			        break;      
			    }
			}
		    if (!ota_executed) {
		        ESP_LOGE(TAG_GSM, "Gsm Ota failed after %d retries. Exiting & restarting device...", RETRIES);
		        // esp_restart();
		    }
			 
			 
            start_time = xTaskGetTickCount() * portTICK_PERIOD_MS; // reset timer
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}


// LED blinking patterns
void blink_led() {
    // Initialize LED GPIO
    esp_rom_gpio_pad_select_gpio(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);	
	   
    for (int i = 0; i < 3; i++) {
	    gpio_set_level(LED_GPIO, 1);
	    vTaskDelay(pdMS_TO_TICKS(150));
	    gpio_set_level(LED_GPIO, 0);
	    vTaskDelay(pdMS_TO_TICKS(150));
    }    
}

 /**
 * @brief error indicator. 
 */
 void error(int count) {
    for (int i = 0; i < count; i++) {
	    gpio_set_level(LED_GPIO, 1);
	    vTaskDelay(pdMS_TO_TICKS(200));
	    gpio_set_level(LED_GPIO, 0);
	    vTaskDelay(pdMS_TO_TICKS(200));
    }
}
