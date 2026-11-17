#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

//* Pumps Pins
#define N_PUMPS 6
#define PUMP_1 12
#define PUMP_2 11
#define PUMP_3 14
#define PUMP_4 15
#define PUMP_5 13
#define PUMP_6 10

#define CAROUSEL_CUPS_SLOTS 4

//* Carousel Motor Pin
#define CAROUSEL_SERVO 8
#define SERVO_SETTLE_MS 700 
#define SENSOR_SETTLE_MS   100 
#define SENSOR_READINGS    8
#define SENSOR_GAP_MS      60  
#define SENSOR_MIN_VALID   3   

//* Distance Sensor Pin
#define DISTANCE_ECHO 7
#define DISTANCE_TRIG 6
#define CUP_MAX_DISTANCE_CM 16.0f 

//* Button Pin
#define UNLOAD_BUTTON 9
#define BUTTON_POLL_MS     50
#define BUTTON_DEBOUNCE_MS 30

//* WiFi: ESP8285 (AT firmware) on UART0
#define ESP_TX_PIN 0 //RP2040 TX -> ESP RX
#define ESP_RX_PIN 1 //RP2040 RX <- ESP TX
#define ESP_BAUD   115200
#define HW_WIFI_AP_IP "192.168.4.1" //ESP8285 default AP address

//* TFT Display Pins
#define TFT_SDA 18
#define TFT_SCL 19
#define TFT_RST 20
#define TFT_DC 21
#define TFT_CS 22
#define SPI_PORT spi0
#define TFT_HOR_RES 320 //landscape: long side horizontal
#define TFT_VER_RES 240
#define DISPLAY_PERIOD_MS 30



void hw_init(void);

//pumps: id 0..5 = PUMP_1..PUMP_6, the liquid in each one is configured from the web page
void hw_pump_on(uint8_t pump_id);
void hw_pump_off(uint8_t pump_id);
void hw_all_pumps_off(void);
//true if the pump is on; *on_ms (if not NULL) = how long it has been on. Read-only, for the display
bool hw_pump_is_on(uint8_t pump_id, uint32_t *on_ms);

void hw_carousel_move_to_slot(int slot); //slot 0..3
void hw_carousel_move_to_rest(void);     
void hw_carousel_release(void);  

//check if a cup is present, based on ultrasonic sensor, distance < CUP_MAX_DISTANCE_CM
//*dist_cm (if not NULL) = measured median distance, -1 if no valid echo
bool hw_cup_present(float *dist_cm);

bool hw_button_is_pressed(void);

//display: call ONLY from the display task (LVGL is not thread-safe). The screen itself is in FluidSyncUI.c
void hw_display_init(void);
void hw_display_tick(uint32_t elapsed_ms);

//WiFi: call ONLY from one task (the UART is not shared). Blocks until the ESP answers AT, false if the AP/server setup fails.
//Password min 8 chars, no quotes or commas.
bool hw_wifi_start_ap(const char *ssid, const char *password);
//waits for an HTTP request on port 80: returns the connection id (-1 on timeout), req = start of the request
int hw_wifi_wait_request(char *req, int size, uint32_t timeout_ms);
//sends data on connection id (any length, split in ESP-sized chunks), false if the connection is gone
bool hw_wifi_send(int id, const char *data, int len);
void hw_wifi_close(int id);

//config storage in the last flash sector. load copies the raw bytes (the caller checks them),
//save pauses both cores ~50ms: do not call while pumps are running (their timing would stretch)
void hw_config_load(void *dst, size_t len);
bool hw_config_save(const void *src, size_t len);
