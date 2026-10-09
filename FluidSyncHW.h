#pragma once

#include <stdbool.h>
#include <stdint.h>

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
#define SENSOR_SETTLE_MS   100 //extra wait after the servo, the carousel stops vibrating
#define SENSOR_READINGS    5
#define SENSOR_GAP_MS      60  //min time between two pings
#define SENSOR_MIN_VALID   3   //less valid readings than this -> no cup

//* Distance Sensor Pin
#define DISTANCE_ECHO 7
#define DISTANCE_TRIG 6
#define CUP_MAX_DISTANCE_CM 15.0f 

//* Button Pin
#define UNLOAD_BUTTON 9

//* TFT Display Pins
#define TFT_SDA 18
#define TFT_SCL 19
#define TFT_RST 20
#define TFT_DC 21
#define TFT_CS 22
#define SPI_PORT spi0
#define TFT_HOR_RES 320 //landscape: long side horizontal
#define TFT_VER_RES 240



void hw_init(void);

//pompe: id 0..5 = PUMP_1..PUMP_6 = yellow, magenta, cyan, white, black, water
void hw_pump_on(uint8_t pump_id);
void hw_pump_off(uint8_t pump_id);
void hw_all_pumps_off(void);
//true if the pump is on; *on_ms (if not NULL) = how long it has been on. Read-only, for the display
bool hw_pump_is_on(uint8_t pump_id, uint32_t *on_ms);

void hw_carousel_move_to_slot(int slot); //slot 0..3
void hw_carousel_move_to_rest(void);     

//check if a cup is present, based on ultrasonic sensor, distance < 15
bool hw_cup_present(void);

bool hw_button_is_pressed(void);

//display: call ONLY from the display task (LVGL is not thread-safe). The screen itself is in FluidSyncUI.c
void hw_display_init(void);
void hw_display_tick(uint32_t elapsed_ms);
