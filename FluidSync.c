#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "lvgl/lvgl.h"

//* Pumps Pins
#define PUMP_4 15
#define PUMP_3 14
#define PUMP_5 13
#define PUMP_1 12
#define PUMP_2 11 
#define PUMP_6 10

//* Servo Motor Pin
#define SERVO_MOTOR 8
uint carousel_positions[5] = {
    0,      //rest position to enable the user to take off the cups
    625,    //cup 1
    1250,   //cup 2
    2125,   //cup 3
    2819    //cup 4
};
uint slice;
uint channel;

//* Distance Sensor Pin
#define DISTANCE_ECHO 6
#define DISTANCE_TRIG 7

//* Button Pin
#define BUTTON_REQ 9

//* TFT Display Pins
#define TFT_SDA 18
#define TFT_SCL 19
#define TFT_RST 20
#define TFT_DC 21
#define TFT_CS 22
#define SPI_PORT spi0
#define TFT_HOR_RES 240
#define TFT_VER_RES 320

//* TFT display functions and init
void tft_write_cmd(uint8_t cmd) {
    gpio_put(TFT_DC, 0); 
    gpio_put(TFT_CS, 0);
    spi_write_blocking(SPI_PORT, &cmd, 1);
    gpio_put(TFT_CS, 1);
}

void tft_write_data(uint8_t data) {
    gpio_put(TFT_DC, 1);
    gpio_put(TFT_CS, 0);
    spi_write_blocking(SPI_PORT, &data, 1);
    gpio_put(TFT_CS, 1);
}

void tft_hardware_init() {
    spi_init(SPI_PORT, 30000 * 1000);
    gpio_set_function(TFT_SDA, GPIO_FUNC_SPI);
    gpio_set_function(TFT_SCL, GPIO_FUNC_SPI);
    gpio_init(TFT_RST); 
    gpio_set_dir(TFT_RST, GPIO_OUT);
    gpio_init(TFT_DC);  
    gpio_set_dir(TFT_DC, GPIO_OUT);
    gpio_init(TFT_CS);  
    gpio_set_dir(TFT_CS, GPIO_OUT);
    gpio_put(TFT_CS, 1);

    gpio_put(TFT_RST, 1); sleep_ms(10);
    gpio_put(TFT_RST, 0); sleep_ms(50);
    gpio_put(TFT_RST, 1); sleep_ms(150);

    tft_write_cmd(0x11);  // Sleep Out
    sleep_ms(120);

    tft_write_cmd(0x36);  // MADCTL - orientamento e ordine RGB
    tft_write_data(0x00); // Portrait, RGB

    tft_write_cmd(0x3A);  // COLMOD - pixel format
    tft_write_data(0x55); // 16 bit / pixel (RGB565) ← fondamentale per LVGL

    tft_write_cmd(0xB2);  // Porch control
    tft_write_data(0x0C); tft_write_data(0x0C);
    tft_write_data(0x00); tft_write_data(0x33); tft_write_data(0x33);

    tft_write_cmd(0xB7);  // Gate control
    tft_write_data(0x35);

    tft_write_cmd(0xBB);  // VCOM
    tft_write_data(0x19);

    tft_write_cmd(0xC0);  // LCM control
    tft_write_data(0x2C);

    tft_write_cmd(0xC2);  // VDV/VRH enable
    tft_write_data(0x01);

    tft_write_cmd(0xC3);  // VRH set
    tft_write_data(0x12);

    tft_write_cmd(0xC4);  // VDV set
    tft_write_data(0x20);

    tft_write_cmd(0xC6);  // Frame rate (60Hz)
    tft_write_data(0x0F);

    tft_write_cmd(0xD0);  // Power control
    tft_write_data(0xA4); tft_write_data(0xA1);

    tft_write_cmd(0x29);  // Display On
    sleep_ms(10);
}

void tft_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    
    tft_write_cmd(0x2A); // (X)
    tft_write_data(area->x1 >> 8); tft_write_data(area->x1 & 0xFF);
    tft_write_data(area->x2 >> 8); tft_write_data(area->x2 & 0xFF);

    tft_write_cmd(0x2B); // (Y)
    tft_write_data(area->y1 >> 8); tft_write_data(area->y1 & 0xFF);
    tft_write_data(area->y2 >> 8); tft_write_data(area->y2 & 0xFF);

    tft_write_cmd(0x2C); 

    uint32_t width = (area->x2 - area->x1 + 1);
    uint32_t height = (area->y2 - area->y1 + 1);
    uint32_t num_pixels = width * height;
    
    gpio_put(TFT_DC, 1); 
    gpio_put(TFT_CS, 0); 

    spi_write_blocking(SPI_PORT, (uint8_t *)color_p, num_pixels * 2);

    gpio_put(TFT_CS, 1); 

    lv_disp_flush_ready(disp_drv);
}

//* Pumps init
void init_pumps() {
    gpio_init(PUMP_4);
    gpio_init(PUMP_3);
    gpio_init(PUMP_5);
    gpio_init(PUMP_1);
    gpio_init(PUMP_2);
    gpio_init(PUMP_6);

    gpio_set_dir(PUMP_4, GPIO_OUT);
    gpio_set_dir(PUMP_3, GPIO_OUT);
    gpio_set_dir(PUMP_5, GPIO_OUT);
    gpio_set_dir(PUMP_1, GPIO_OUT);
    gpio_set_dir(PUMP_2, GPIO_OUT);
    gpio_set_dir(PUMP_6, GPIO_OUT);
}

//* Carousel servo init
void init_servo_motor() {
    gpio_set_function(SERVO_MOTOR, GPIO_FUNC_PWM);
    slice   = pwm_gpio_to_slice_num(SERVO_MOTOR);
    channel = pwm_gpio_to_channel(SERVO_MOTOR);

    pwm_set_clkdiv(slice, 100.0f);  
    pwm_set_wrap(slice, 24999);     
    pwm_set_enabled(slice, true);
}

void init_req_button() {
    gpio_init(BUTTON_REQ);
    gpio_set_dir(BUTTON_REQ, GPIO_IN);
    gpio_pull_up(BUTTON_REQ);
}

//* Full system init
void init_fluidsync() {
    stdio_init_all();
    init_pumps();
    init_servo_motor();
    tft_hardware_init();
    lv_init();
    spi_init(SPI_PORT, 10000 * 1000);
    init_req_button();
}

int main() {

    init_fluidsync();

    gpio_put(PUMP_4, 0);
    gpio_put(PUMP_3, 1);
    gpio_put(PUMP_5, 0);
    gpio_put(PUMP_1, 0);
    gpio_put(PUMP_2, 0);
    gpio_put(PUMP_6, 0); 

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[TFT_HOR_RES * 10]; 
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, TFT_HOR_RES * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = tft_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.hor_res = TFT_HOR_RES;
    disp_drv.ver_res = TFT_VER_RES;
    lv_disp_drv_register(&disp_drv);

    lv_obj_t *rect = lv_obj_create(lv_scr_act());
    lv_obj_set_size(rect, 200, 100);                        
    lv_obj_align(rect, LV_ALIGN_CENTER, 0, 0);               

    static lv_style_t style_rect;
    lv_style_init(&style_rect);
    lv_style_set_bg_color(&style_rect, lv_color_hex(0xFF0000)); 
    lv_style_set_bg_opa(&style_rect, LV_OPA_COVER);             
    lv_style_set_border_width(&style_rect, 0);                  
    lv_style_set_radius(&style_rect, 0);                        

    lv_obj_add_style(rect, &style_rect, 0);

    static uint servo_index = 0;
    static absolute_time_t next_servo_time;
    next_servo_time = make_timeout_time_ms(1000);

    while (true) {
        lv_tick_inc(5);
        lv_timer_handler();

        if (absolute_time_diff_us(get_absolute_time(), next_servo_time) <= 0) {
            pwm_set_chan_level(slice, channel, carousel_positions[servo_index]);
            servo_index = (servo_index + 1) % 5;
            next_servo_time = make_timeout_time_ms(1000);
        }

        bool current_state = gpio_get(BUTTON_REQ);

        if (!current_state) {
            lv_obj_set_style_bg_color(rect, lv_color_hex(0x00FF00), LV_PART_MAIN);
        }else{
            lv_obj_set_style_bg_color(rect, lv_color_hex(0xFF0000), LV_PART_MAIN);
        }

        sleep_ms(5);
    }

    return 0;
}