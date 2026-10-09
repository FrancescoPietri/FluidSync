//FluidSync - livello hardware (driver e init), dal codice hardware di Francesco
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "lvgl/lvgl.h"

#include "FreeRTOS.h"
#include "task.h"

#include "FluidSyncHW.h"

static const uint pump_pins[N_PUMPS] = { PUMP_1, PUMP_2, PUMP_3, PUMP_4, PUMP_5, PUMP_6 };

static const uint carousel_positions[CAROUSEL_CUPS_SLOTS + 1] = {
    0,      //rest position to enable the user to take off the cups
    625,    //cup 1
    1250,   //cup 2
    2125,   //cup 3
    2819    //cup 4
};

static uint slice;
static uint channel;


//* TFT display functions and init
static void tft_write_cmd(uint8_t cmd) {
    gpio_put(TFT_DC, 0);
    gpio_put(TFT_CS, 0);
    spi_write_blocking(SPI_PORT, &cmd, 1);
    gpio_put(TFT_CS, 1);
}

static void tft_write_data(uint8_t data) {
    gpio_put(TFT_DC, 1);
    gpio_put(TFT_CS, 0);
    spi_write_blocking(SPI_PORT, &data, 1);
    gpio_put(TFT_CS, 1);
}

static void tft_hardware_init() {
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
    tft_write_data(0x55); // 16 bit / pixel (RGB565) <- fondamentale per LVGL

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

static void tft_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {

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
static void init_pumps() {
    for(int i = 0; i < N_PUMPS; i++){
        gpio_init(pump_pins[i]);
        gpio_set_dir(pump_pins[i], GPIO_OUT);
        gpio_put(pump_pins[i], 0); 
    }
}

//* Carousel servo init
static void init_servo_motor() {
    gpio_set_function(CAROUSEL_SERVO, GPIO_FUNC_PWM);
    slice   = pwm_gpio_to_slice_num(CAROUSEL_SERVO);
    channel = pwm_gpio_to_channel(CAROUSEL_SERVO);

    pwm_set_clkdiv(slice, 100.0f);  //125 MHz / 100 = 1,25 MHz
    pwm_set_wrap(slice, 24999);     //-> 50 Hz
    pwm_set_enabled(slice, true);
}

//* Button init
static void init_req_button() {
    gpio_init(UNLOAD_BUTTON);
    gpio_set_dir(UNLOAD_BUTTON, GPIO_IN);
    gpio_pull_up(UNLOAD_BUTTON);
}

static void init_distance_sensor() {
    gpio_init(DISTANCE_ECHO);
    gpio_set_dir(DISTANCE_ECHO, GPIO_IN);

    gpio_init(DISTANCE_TRIG);
    gpio_set_dir(DISTANCE_TRIG, GPIO_OUT);
    gpio_put(DISTANCE_TRIG, 0);
}

//* Full system init
void hw_init(void) {
    stdio_init_all();
    init_pumps();
    init_servo_motor();
    tft_hardware_init();
    lv_init();
    spi_init(SPI_PORT, 10000 * 1000);
    init_req_button();
    init_distance_sensor();
}

//* Pumps
void hw_pump_on(uint8_t pump_id){
    gpio_put(pump_pins[pump_id], 1);
}

void hw_pump_off(uint8_t pump_id){
    gpio_put(pump_pins[pump_id], 0);
}

void hw_all_pumps_off(void){
    for(int i = 0; i < N_PUMPS; i++) gpio_put(pump_pins[i], 0);
}

//* Carousel
static void carousel_set_position(uint index){
    pwm_set_chan_level(slice, channel, carousel_positions[index]);
    vTaskDelay(pdMS_TO_TICKS(SERVO_SETTLE_MS)); 
}

void hw_carousel_move_to_slot(int slot){
    carousel_set_position(slot + 1); 
}

void hw_carousel_move_to_rest(void){
    carousel_set_position(0);
}

//* Distance sensor
static float get_distance() {

    gpio_put(DISTANCE_TRIG, 1);
    sleep_us(10);
    gpio_put(DISTANCE_TRIG, 0);

    uint32_t t0 = time_us_32();
    while (gpio_get(DISTANCE_ECHO) == 0) {
        if (time_us_32() - t0 > 30000) return -1.0f;
    }
    uint32_t start_time = time_us_32();

    while (gpio_get(DISTANCE_ECHO) == 1) {
        if (time_us_32() - start_time > 30000) return -1.0f;
    }
    uint32_t end_time = time_us_32();

    uint32_t pulse_duration = end_time - start_time;

    return (pulse_duration * 0.0343f) / 2.0f;
}

bool hw_cup_present(void){
    float d = get_distance();
    printf("distance: %f\n", d);
    return d > 0.0f && d < CUP_MAX_DISTANCE_CM;
}

//* Button
bool hw_button_is_pressed(void){
    return gpio_get(UNLOAD_BUTTON) == 0;
}

//* Display
static lv_obj_t *rect;
static lv_obj_t *label;

void hw_display_init(void){
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

    rect = lv_obj_create(lv_scr_act());
    lv_obj_set_size(rect, 200, 100);
    lv_obj_align(rect, LV_ALIGN_CENTER, 0, 0);

    static lv_style_t style_rect;
    lv_style_init(&style_rect);
    lv_style_set_bg_opa(&style_rect, LV_OPA_COVER);
    lv_style_set_border_width(&style_rect, 0);
    lv_style_set_radius(&style_rect, 0);
    lv_obj_add_style(rect, &style_rect, 0);

    label = lv_label_create(rect);
    lv_obj_center(label);

    hw_display_set_state(UI_RUNNING);
}

void hw_display_set_state(ui_state_t state){
    switch(state){
        case UI_WAIT_MIX:
            lv_obj_set_style_bg_color(rect, lv_color_hex(0xFFD000), LV_PART_MAIN); //giallo
            lv_label_set_text(label, "ATTENDI FINE MIX");
            break;
        case UI_USER_TURN:
            lv_obj_set_style_bg_color(rect, lv_color_hex(0x00FF00), LV_PART_MAIN); //verde
            lv_label_set_text(label, "TOGLI I BICCHIERI\nE PREMI");
            break;
        default:
            lv_obj_set_style_bg_color(rect, lv_color_hex(0x404040), LV_PART_MAIN); //grigio
            lv_label_set_text(label, "IN FUNZIONE");
            break;
    }
}

void hw_display_tick(uint32_t elapsed_ms){
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
}
