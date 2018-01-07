// esp32lights, v1.0
// Luca Dentella, xmas 2017

#include "main.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_err.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "spiffs_vfs.h"

#include "driver/gpio.h"

#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "apps/sntp/sntp.h"

#include "bh1750.h"
#include "cJSON.h"


// Event group for inter-task communication
static EventGroupHandle_t event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// actual relay status
bool relay_status;

// nvs handler
nvs_handle my_handle;

// running configuration
int working_mode;
char p1start[6];
char p1end[6];
char p2start[6];
char p2end[6];
int lux;
bool p1_valid = false;
bool p2_valid = false;
bool lux_valid = false;


// Wifi event handler
static esp_err_t event_handler(void *ctx, system_event_t *event) {
    switch(event->event_id) {
		
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    
	case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(event_group, WIFI_CONNECTED_BIT);
        break;
    
	case SYSTEM_EVENT_STA_DISCONNECTED:
		xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT);
        break;
    
	default:
        break;
    }  
	return ESP_OK;
}


// light sensor read
int get_light_value() {

	float float_value = bh1750_read();
	return (int)float_value;
}


// serve static content from SPIFFS
void spiffs_serve(char* resource, struct netconn *conn) {
	
					
	// check if it exists on SPIFFS
	char full_path[100];
	sprintf(full_path, "/spiffs%s", resource);
	printf("+ Serving static resource: %s\n", full_path);
	struct stat st;
	if (stat(full_path, &st) == 0) {
		netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
		
		// open the file for reading
		FILE* f = fopen(full_path, "r");
		if(f == NULL) {
			printf("Unable to open the file %s\n", full_path);
			return;
		}
		
		// send the file content to the client
		char buffer[1024];
		while(fgets(buffer, 1024, f)) {
			netconn_write(conn, buffer, strlen(buffer), NETCONN_NOCOPY);
		}
		fclose(f);
		fflush(stdout);
	}
	else {
		netconn_write(conn, http_404_hdr, sizeof(http_404_hdr) - 1, NETCONN_NOCOPY);
	}
}
	
static void http_server_netconn_serve(struct netconn *conn) {

	struct netbuf *inbuf;
	char *buf;
	u16_t buflen;
	err_t err;

	err = netconn_recv(conn, &inbuf);

	if (err == ERR_OK) {
	  
		// get the request and terminate the string
		netbuf_data(inbuf, (void**)&buf, &buflen);
		buf[buflen] = '\0';
		
		// get the request body and the first line
		char* body = strstr(buf, "\r\n\r\n");
		char *request_line = strtok(buf, "\n");
		
		if(request_line) {
				
			// dynamic page: setConfig
			if(strstr(request_line, "POST /setConfig")) {
			
				cJSON *root = cJSON_Parse(body);
				cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
				
				if(strstr(mode_item->valuestring, "manual")) {
					
					working_mode = MODE_MANUAL;
					err = nvs_set_i32(my_handle, "mode", MODE_MANUAL);		
					if(err != ESP_OK) {
						printf("Unable to store new working mode in NVS\n");
					}
					
					cJSON *status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
					if(strstr(status_item->valuestring, "on")) {
						printf("! Manual mode, turning the relay ON\n");
						gpio_set_level(CONFIG_RELAY_PIN, RELAY_ON);
						relay_status = true;	
					}
					else {
						printf("! Manual mode, turning the relay OFF\n");
						gpio_set_level(CONFIG_RELAY_PIN, RELAY_OFF);
						relay_status = false;	
					}
				}
				
				else if(strstr(mode_item->valuestring, "time")) {
				
					working_mode = MODE_TIME;
					err = nvs_set_i32(my_handle, "mode", MODE_TIME);		
					if(err != ESP_OK) {
						printf("Unable to store new working mode in NVS\n");
					}
					cJSON *p1start_item = cJSON_GetObjectItemCaseSensitive(root, "p1start");
					cJSON *p1end_item = cJSON_GetObjectItemCaseSensitive(root, "p1end");
					strcpy(p1start, p1start_item->valuestring);
					strcpy(p1end, p1end_item->valuestring);
					p1_valid = true;
					
					err = nvs_set_str(my_handle, "p1start", p1start);
					if(err != ESP_OK) {
						printf("Unable to store new p1start value in NVS\n");
					}
					err = nvs_set_str(my_handle, "p1end", p1end);
					if(err != ESP_OK) {
						printf("Unable to store new p1end value in NVS\n");
					}
					
					err = nvs_erase_key(my_handle, "p2start");
					if(err != ESP_OK) {
						printf("Unable to clear p2start value in NVS\n");
					}
					err = nvs_erase_key(my_handle, "p2end");
					if(err != ESP_OK) {
						printf("Unable to clear p2end value in NVS\n");
					}
					
					if(strstr(mode_item->valuestring, "timeP2")) {
						
						cJSON *p2start_item = cJSON_GetObjectItemCaseSensitive(root, "p2start");
						cJSON *p2end_item = cJSON_GetObjectItemCaseSensitive(root, "p2end");
						strcpy(p2start, p2start_item->valuestring);
						strcpy(p2end, p2end_item->valuestring);
						strcpy(p2end, p2end_item->valuestring);
						p2_valid = true;
						
						err = nvs_set_str(my_handle, "p2start", p2start);
						if(err != ESP_OK) {
							printf("Unable to store new p2start value in NVS\n");
						}
						err = nvs_set_str(my_handle, "p2end", p2end);
						if(err != ESP_OK) {
							printf("Unable to store new p2end value in NVS\n");
						}
					}
				}
				
				else if(strstr(mode_item->valuestring, "light")) {
					
					working_mode = MODE_LIGHT;
					err = nvs_set_i32(my_handle, "mode", MODE_LIGHT);		
					if(err != ESP_OK) {
						printf("Unable to store new working mode in NVS\n");
					}
					
					cJSON *lux_item = cJSON_GetObjectItemCaseSensitive(root, "lux");
					lux = atoi(lux_item->valuestring);
					lux_valid = true;
					err = nvs_set_i32(my_handle, "lux", lux);		
					if(err != ESP_OK) {
						printf("Unable to store new lux value in NVS\n");
					}
				}
			}
			
			// dynamic page: getConfig
			else if(strstr(request_line, "GET /getConfig")) {
			
				cJSON *root = cJSON_CreateObject();
				if(working_mode == MODE_MANUAL)	cJSON_AddStringToObject(root, "mode", "manual");
				else if(working_mode == MODE_TIME) cJSON_AddStringToObject(root, "mode", "time");
				else if(working_mode == MODE_LIGHT) cJSON_AddStringToObject(root, "mode", "light");
				
				if(relay_status == true) cJSON_AddStringToObject(root, "status", "on");
				else cJSON_AddStringToObject(root, "status", "off");
				
				if(p1_valid) {
					cJSON_AddStringToObject(root, "p1start", p1start);
					cJSON_AddStringToObject(root, "p1end", p1end);
				}
				if(p2_valid) {
					cJSON_AddStringToObject(root, "p2start", p2start);
					cJSON_AddStringToObject(root, "p2end", p2end);
				}
	
				if(lux_valid) cJSON_AddNumberToObject(root, "lux", lux);
				
				char *rendered = cJSON_Print(root);
				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
				netconn_write(conn, rendered, strlen(rendered), NETCONN_NOCOPY);
			}
			
			// dynamic page: getLight
			else if(strstr(request_line, "GET /getLight")) {
			
				int light_value = get_light_value();		
				cJSON *root = cJSON_CreateObject();
				cJSON_AddNumberToObject(root, "lux", light_value);
				char *rendered = cJSON_Print(root);
				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
				netconn_write(conn, rendered, strlen(rendered), NETCONN_NOCOPY);
			}
			
			// default page -> redirect to index.html
			else if(strstr(request_line, "GET / ")) {
				spiffs_serve("/index.html", conn);
			}
			// static content, get it from SPIFFS
			else {
				
				// get the requested resource
				char* method = strtok(request_line, " ");
				char* resource = strtok(NULL, " ");
				spiffs_serve(resource, conn);
			}
		}
	}
	
	// close the connection and free the buffer
	netconn_close(conn);
	netbuf_delete(inbuf);
}

static void http_server(void *pvParameters) {
	
	struct netconn *conn, *newconn;
	err_t err;
	conn = netconn_new(NETCONN_TCP);
	netconn_bind(conn, NULL, 80);
	netconn_listen(conn);
	printf("* HTTP Server listening\n");
	do {
		err = netconn_accept(conn, &newconn);
		if (err == ERR_OK) {
			http_server_netconn_serve(newconn);
			netconn_delete(newconn);
		}
		vTaskDelay(10); //allows task to be pre-empted
	} while(err == ERR_OK);
	netconn_close(conn);
	netconn_delete(conn);
}

static void monitoring_task(void *pvParameters) {
	
	printf("* Monitoring task started\n");
	
	while(1) {
	
		// run every 1000ms
		vTaskDelay(1000 / portTICK_PERIOD_MS);
		
		// working mode manual? nothing to do...
		if(working_mode == MODE_MANUAL) continue;

		// working mode time? Check if the current time matches a start/stop time
		if(working_mode == MODE_TIME) {
		
			time_t now = 0;
			time(&now);
			char actual_time[6];
			strftime(actual_time, 6, "%H:%M", localtime(&now));
			
			if(p1_valid) {
				if((strcmp(actual_time, p1start) == 0) && relay_status == false) {
					printf("! Actual time (%s) after p1start (%s), turning the relay ON\n", actual_time, p1start);
					gpio_set_level(CONFIG_RELAY_PIN, RELAY_ON);
					relay_status = true;
				}
				if((strcmp(actual_time, p1end) == 0) && relay_status == true) {
					printf("! Actual time (%s) after p1end (%s), turning the relay OFF\n", actual_time, p1end);
					gpio_set_level(CONFIG_RELAY_PIN, RELAY_OFF);
					relay_status = false;
				}
			}
			
			if(p2_valid) {
				if((strcmp(actual_time, p2start) == 0) && relay_status == false) {
					printf("! Actual time (%s) after p2start (%s), turning the relay ON\n", actual_time, p2start);
					gpio_set_level(CONFIG_RELAY_PIN, RELAY_ON);
					relay_status = true;
				}
				if((strcmp(actual_time, p2end) == 0) && relay_status == true) {
					printf("! Actual time (%s) after p2end (%s), turning the relay OFF\n", actual_time, p2end);
					gpio_set_level(CONFIG_RELAY_PIN, RELAY_OFF);
					relay_status = false;
				}
			}
		}
		
		// working mode light? Check if the light value is lower then the threshold
		if(working_mode == MODE_LIGHT && lux_valid) {
			
			int actual_light_value = get_light_value();
			if(actual_light_value < lux) {
				if(relay_status == false) {
					printf("! Actual light value (%d) under the threshold (%d), turning the relay ON\n", actual_light_value, lux);
					gpio_set_level(CONFIG_RELAY_PIN, RELAY_ON);
					relay_status = true;	
				}
			}
			else {
				if(relay_status == true) {
					printf("! Actual light value (%d) above the threshold (%d), turning the relay OFF\n", actual_light_value, lux);
					gpio_set_level(CONFIG_RELAY_PIN, RELAY_OFF);
					relay_status = false;
				}
			}
		}
	}
}

// setup the NVS partition
void nvs_setup() {
	
	esp_err_t err = nvs_flash_init();
	if(err == ESP_ERR_NVS_NO_FREE_PAGES) {
		const esp_partition_t* nvs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
		if(!nvs_partition) {
			printf("error: unable to find a NVS partition\n");
			while(1) vTaskDelay(10 / portTICK_PERIOD_MS);
		}
		err = (esp_partition_erase_range(nvs_partition, 0, nvs_partition->size));
		if(err != ESP_OK) {
			printf("error: unable to erase the NVS partition\n");
			while(1) vTaskDelay(10 / portTICK_PERIOD_MS);
		}
		err = nvs_flash_init();
		if(err != ESP_OK) {		
			printf("error: unable to initialize the NVS partition\n");
			while(1) vTaskDelay(10 / portTICK_PERIOD_MS);
		}
	}
	
	printf("* NVS configured\n");
}

// setup and start the wifi connection
void wifi_setup() {
	
	event_group = xEventGroupCreate();
		
	tcpip_adapter_init();

	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

	wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

	wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
	
	printf("* Wifi configured and started\n");
}

// configure the output PIN
void gpio_setup() {
	
	// configure the relay pin as GPIO, output
	gpio_pad_select_gpio(CONFIG_RELAY_PIN);
    gpio_set_direction(CONFIG_RELAY_PIN, GPIO_MODE_OUTPUT);
	
	// set initial status = OFF
	gpio_set_level(CONFIG_RELAY_PIN, RELAY_OFF);
	relay_status = false;
	
	printf("* Relay PIN configured\n");
}

// read the configuration from NVS
void read_config() {

	// open the partition in R/W mode
	esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
	if (err != ESP_OK) {
		printf("Error: unable to open the NVS partition\n");
		while(1) vTaskDelay(10 / portTICK_PERIOD_MS);
	}
	
	// get the configuration values
	err = nvs_get_i32(my_handle, "mode", &working_mode);
	if(err != ESP_OK) {
		printf("No working mode configured, default MANUAL\n\n");
		working_mode = MODE_MANUAL;
	}
	size_t string_size;
	err = nvs_get_str(my_handle, "p1start", p1start, &string_size);
	if(err == ESP_OK) {
		err = nvs_get_str(my_handle, "p1end", p1end, &string_size);
		if(err == ESP_OK) p1_valid = true;
	}
	err = nvs_get_str(my_handle, "p2start", p2start, &string_size);
	if(err == ESP_OK) {
		err = nvs_get_str(my_handle, "p2end", p2end, &string_size);
		if(err == ESP_OK) p2_valid = true;
	}
	err = nvs_get_i32(my_handle, "lux", &lux);
	if(err == ESP_OK) lux_valid = true;
	
	printf("* Configuration read from NVS\n");
}

// print the configuration to console
void print_config() {
	
	// print the configuration
	printf("- CONFIGURATION ------------------------------\n");
	printf(" Working mode:\t");
	if(working_mode == MODE_MANUAL) printf("MANUAL\n");
	else if(working_mode == MODE_TIME) printf("TIME\n");
	else if(working_mode == MODE_LIGHT) printf("LIGHT\n");
	if(p1_valid) {
		printf(" P1 start:\t%s\n", p1start);
		printf(" P1 end:\t%s\n", p1end);
	}
	if(p2_valid) {
		printf(" P2 start:\t%s\n", p2start);
		printf(" P2 end:\t%s\n", p2end);
	}
	if(lux_valid) printf(" Lux value:\t%d\n", lux);
	printf("----------------------------------------------\n\n");
}

// get time from NTP server
void get_time() {

	// configure the NTP server
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
	
	// configure the timezone
	setenv("TZ", CONFIG_TIMEZONE, 1);
    tzset();
	
	time_t now = 0;
    struct tm timeinfo = {0};
    while(timeinfo.tm_year < (2016 - 1900)) {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
	
	char actual_time[6];
	strftime(actual_time, 6, "%H:%M", localtime(&now));
	printf("* Time (%s) set from NTP server\n", actual_time);
}


// Main application
void app_main()
{
	// log only errors
	esp_log_level_set("*", ESP_LOG_ERROR);
	
	printf("esp32lights v1.0\n\n");

	// initialize the different modules and components
	vfs_spiffs_register();
	gpio_setup();
	nvs_setup();
	wifi_setup();
	bh1750_init();
	printf("* BH1750 sensor initialized\n");
	
	// read the configuration from NVS
	printf("\n");
	read_config();
	print_config();
	
	// wait for connection to the wifi network
	xEventGroupWaitBits(event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
	printf("* Connected to the wifi network\n");
	
	// print the local IP address
	tcpip_adapter_ip_info_t ip_info;
	ESP_ERROR_CHECK(tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info));
	printf("- NETWORK PARAMETERS -------------------------\n");
	printf(" IP Address:\t%s\n", ip4addr_ntoa(&ip_info.ip));
	printf(" Subnet mask:\t%s\n", ip4addr_ntoa(&ip_info.netmask));
	printf(" Gateway:\t%s\n", ip4addr_ntoa(&ip_info.gw));	
	printf("----------------------------------------------\n\n");
	
	// start SNTP and wait for correct time
	get_time();
	
	printf("\n");
	
	// start the HTTP Server task
    xTaskCreate(&http_server, "http_server", 20000, NULL, 5, NULL);
	
	// start the monitoring task
	xTaskCreate(&monitoring_task, "monitoring_task", 2048, NULL, 5, NULL);
	
	printf("\n");
}
