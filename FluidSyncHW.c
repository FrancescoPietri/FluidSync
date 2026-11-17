//FluidSync - livello hardware (driver e init), dal codice hardware di Francesco
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "lvgl/lvgl.h"

#include "FreeRTOS.h"
#include "task.h"

#include "FluidSyncHW.h"
#include "FluidSyncUI.h"

static const uint pump_pins[N_PUMPS] = { PUMP_1, PUMP_2, PUMP_3, PUMP_4, PUMP_5, PUMP_6 };

static const uint carousel_positions[CAROUSEL_CUPS_SLOTS + 1] = {
    0,      //rest position to enable the user to take off the cups
    915,    //cup 1
    1646,   //cup 2
    2390,   //cup 3
    3130    //cup 4
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
    tft_write_data(0x60); // Landscape (MV|MX), RGB. Upside down? use 0xA0. Mirrored? use 0x20 or 0xE0

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

    tft_write_cmd(0x36);  // VDV/VRH enable
    tft_write_data(0xA0);

    tft_write_cmd(0xC3);  // VRH set
    tft_write_data(0x12);

    tft_write_cmd(0xC4);  // VDV set
    tft_write_data(0x20);

    tft_write_cmd(0xC6);  // Frame rate (60Hz)
    tft_write_data(0x0F);

    tft_write_cmd(0xD0);  // Power control
    tft_write_data(0xA4); tft_write_data(0xA1);

    tft_write_cmd(0x21);  // Display Inversion On: this ST7789 panel shows inverted colors without it (white bg, green<->purple)

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

void hw_carousel_release(void){
    pwm_set_chan_level(slice, channel, 0); //no pulse -> servo goes limp
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
static volatile bool pump_is_on[N_PUMPS];
static volatile uint32_t pump_on_since_ms[N_PUMPS];

void hw_pump_on(uint8_t pump_id){
    pump_on_since_ms[pump_id] = to_ms_since_boot(get_absolute_time());
    pump_is_on[pump_id] = true;
    gpio_put(pump_pins[pump_id], 1);
}

void hw_pump_off(uint8_t pump_id){
    gpio_put(pump_pins[pump_id], 0);
    pump_is_on[pump_id] = false;
}

bool hw_pump_is_on(uint8_t pump_id, uint32_t *on_ms){
    bool on = pump_is_on[pump_id];
    if (on_ms) *on_ms = on ? to_ms_since_boot(get_absolute_time()) - pump_on_since_ms[pump_id] : 0;
    return on;
}

void hw_all_pumps_off(void){
    for(int i = 0; i < N_PUMPS; i++) hw_pump_off(i);
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

    //ECHO must be low before a new ping (the previous echo may still be in progress)
    uint32_t t_idle = time_us_32();
    while (gpio_get(DISTANCE_ECHO) == 1) {
        if (time_us_32() - t_idle > 30000) return -1.0f;
    }

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

bool hw_cup_present(float *dist_cm){
    if (dist_cm) *dist_cm = -1.0f;
    float d[SENSOR_READINGS];
    int n = 0;

    vTaskDelay(pdMS_TO_TICKS(SENSOR_SETTLE_MS));
    for (int i = 0; i < SENSOR_READINGS; i++) {
        float x = get_distance();
        if (x > 0.0f) d[n++] = x; //-1 = timeout, discarded
        vTaskDelay(pdMS_TO_TICKS(SENSOR_GAP_MS)); //the task blocks, the others run
    }

    if (n < SENSOR_MIN_VALID) {
        printf("distance: no echo (%d/%d valid)\n", n, SENSOR_READINGS);
        return false;
    }

    //insertion sort, then take the middle value
    for (int i = 1; i < n; i++) {
        float v = d[i];
        int j = i - 1;
        while (j >= 0 && d[j] > v) { d[j + 1] = d[j]; j--; }
        d[j + 1] = v;
    }
    float median = d[n / 2];
    if (dist_cm) *dist_cm = median;
    printf("distance: %.1f cm (median of %d)\n", median, n);
    return median < CUP_MAX_DISTANCE_CM;
}

//* Button
bool hw_button_is_pressed(void){
    return gpio_get(UNLOAD_BUTTON) == 0;
}

//* Display
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

    ui_create(); //screen layout in FluidSyncUI.c
}

void hw_display_tick(uint32_t elapsed_ms){
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
}

//* WiFi: on-board ESP8285 with AT firmware on UART0. The ESP8285 runs the AP, DHCP and TCP; here only the AT protocol.
#define ESP_UART uart0
#define ESP_TRACE 0 //1 = print the AT traffic on the USB serial (debugging)
#define esp_trace(...) do{ if(ESP_TRACE) printf(__VA_ARGS__); }while(0)

//RX is interrupt-driven into a ring buffer: the 32-byte UART FIFO alone overflows while we send a long reply
//(a 2KB chunk takes ~180ms) and the browser's next request arrives meanwhile.
#define ESP_RX_BUF 2048
static volatile uint8_t esp_rx[ESP_RX_BUF];
static volatile uint16_t esp_rx_head, esp_rx_tail; //written by the IRQ / by esp_getc only

static void esp_rx_irq(void){
    while(uart_is_readable(ESP_UART)){
        uint8_t c = uart_getc(ESP_UART);
        uint16_t next = (esp_rx_head + 1) % ESP_RX_BUF;
        if(next != esp_rx_tail){ esp_rx[esp_rx_head] = c; esp_rx_head = next; } //full: byte dropped
    }
}

//next byte from the ESP, -1 on timeout
static int esp_getc(uint32_t timeout_ms){
    TickType_t start = xTaskGetTickCount();
    while(esp_rx_tail == esp_rx_head){
        if(xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return -1;
        vTaskDelay(1);
    }
    uint8_t c = esp_rx[esp_rx_tail];
    esp_rx_tail = (esp_rx_tail + 1) % ESP_RX_BUF;
    return c;
}

//Incoming requests ("+IPD") can arrive at any time, also while we wait for the answer to a command
//(a browser asks for /cfg while the page is still being sent): they are queued here, never dropped.
#define ESP_PENDING 4   //the ESP keeps max 5 connections, one is the one being served
#define ESP_REQ_MAX 256 //start of the request kept (the request line is all we parse)
static struct { int id; char req[ESP_REQ_MAX]; } pending[ESP_PENDING];
static int pending_n;

//reads the payload of a "+IPD,id,len:" header into the pending queue
static void esp_stash_request(const char *header){
    int id, len, n = 0;
    if(sscanf(header, "+IPD,%d,%d:", &id, &len) != 2) return;
    esp_trace("esp< +IPD id=%d len=%d\n", id, len);
    char *dst = pending_n < ESP_PENDING ? pending[pending_n].req : NULL; //queue full: drain and drop
    for(int i = 0; i < len; i++){
        int c = esp_getc(1000);
        if(c < 0) break;
        if(dst && n < ESP_REQ_MAX - 1) dst[n++] = (char)c;
    }
    if(!dst){ printf("wifi: request queue full, request dropped\n"); return; }
    dst[n] = 0;
    pending[pending_n++].id = id;
}

//one line from the ESP without "\r\n", -1 on timeout. The CIPSEND prompt is returned as the line ">".
//A request is queued (see above) and returned as an empty line, so the caller keeps waiting for its answer.
static int esp_readline(char *buf, int size, uint32_t timeout_ms){
    int n = 0;
    while(1){
        int c = esp_getc(timeout_ms);
        if(c < 0) return -1;
        if(c == '\r') continue;
        if(c == '\n'){ if(n == 0) continue; break; }
        if(c == '>' && n == 0){ buf[n++] = '>'; break; }
        if(n < size - 1) buf[n++] = (char)c;
        if(c == ':' && n > 5 && strncmp(buf, "+IPD,", 5) == 0){
            buf[n] = 0;
            esp_stash_request(buf);
            n = 0;
            break;
        }
    }
    buf[n] = 0;
    if(n) esp_trace("esp< %s\n", buf);
    return n;
}

//send a command and wait for OK. Unrelated lines in between (CONNECT, CLOSED, ...) are skipped
static bool esp_cmd(const char *cmd, uint32_t timeout_ms){
    char line[96];
    esp_trace("esp> %s\n", cmd);
    uart_puts(ESP_UART, cmd);
    uart_puts(ESP_UART, "\r\n");
    while(esp_readline(line, sizeof line, timeout_ms) >= 0){
        if(strcmp(line, "OK") == 0 || strcmp(line, "no change") == 0) return true;
        if(strcmp(line, "ERROR") == 0 || strcmp(line, "FAIL") == 0) return false;
    }
    return false;
}

bool hw_wifi_start_ap(const char *ssid, const char *password){
    uart_init(ESP_UART, ESP_BAUD);
    gpio_set_function(ESP_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(ESP_RX_PIN, GPIO_FUNC_UART);
    irq_set_exclusive_handler(UART0_IRQ, esp_rx_irq); //runs on this task's core
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(ESP_UART, true, false);
    vTaskDelay(pdMS_TO_TICKS(1500)); //ESP8285 boot time

    //most of these boards ship without AT firmware: keep retrying and say so on the USB serial
    while(!esp_cmd("AT", 500)){
        printf("ESP8285 does not answer AT on GP%d/GP%d @%d: flash ESP-AT firmware on it\n", ESP_TX_PIN, ESP_RX_PIN, ESP_BAUD);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    char sap[128];
    snprintf(sap, sizeof sap, "AT+CWSAP=\"%s\",\"%s\",5,3", ssid, password); //channel 5, WPA2-PSK
    return esp_cmd("ATE0", 1000)               //no echo
        && esp_cmd("AT+CWMODE=2", 2000)        //access point
        && esp_cmd(sap, 5000)
        && esp_cmd("AT+CIPMUX=1", 1000)        //needed by the server
        && esp_cmd("AT+CIPSERVER=1,80", 2000)
        && esp_cmd("AT+CIPSTO=10", 1000);      //drop idle connections (ESP holds max 5)
}

int hw_wifi_wait_request(char *req, int size, uint32_t timeout_ms){
    char line[96];
    while(pending_n == 0) //CONNECT/CLOSED/... lines are skipped, requests are queued by esp_readline
        if(esp_readline(line, sizeof line, timeout_ms) < 0) return -1;

    int id = pending[0].id;
    snprintf(req, size, "%s", pending[0].req);
    printf("wifi: [%d] %.*s\n", id, (int)strcspn(req, "\r\n"), req); //request line, for debugging on the USB serial
    pending_n--;
    memmove(&pending[0], &pending[1], pending_n * sizeof pending[0]);
    return id;
}

#define ESP_MAX_SEND 2048 //AT+CIPSEND limit per call

bool hw_wifi_send(int id, const char *data, int len){
    char cmd[32];
    while(len > 0){
        int chunk = len < ESP_MAX_SEND ? len : ESP_MAX_SEND;
        snprintf(cmd, sizeof cmd, "AT+CIPSEND=%d,%d\r\n", id, chunk);
        esp_trace("esp> %s", cmd);
        uart_puts(ESP_UART, cmd);
        while(1){ //wait for the ">" prompt: the ESP is ready for the payload
            if(esp_readline(cmd, sizeof cmd, 2000) < 0) return false;
            if(strcmp(cmd, ">") == 0) break;
            if(strcmp(cmd, "ERROR") == 0) return false; //connection already closed by the client
        }
        uart_write_blocking(ESP_UART, (const uint8_t *)data, chunk);
        while(1){
            if(esp_readline(cmd, sizeof cmd, 3000) < 0) return false;
            if(strcmp(cmd, "SEND OK") == 0) break;
            if(strcmp(cmd, "SEND FAIL") == 0 || strcmp(cmd, "ERROR") == 0) return false;
        }
        data += chunk;
        len -= chunk;
    }
    return true;
}

void hw_wifi_close(int id){
    char cmd[24];
    snprintf(cmd, sizeof cmd, "AT+CIPCLOSE=%d", id);
    esp_cmd(cmd, 2000);
}

//* Config storage: last 4KB sector of the flash, far after the program
#define CFG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CFG_FLASH_MAX    1024 //bytes reserved for the config (multiple of FLASH_PAGE_SIZE)

static uint8_t cfg_page[CFG_FLASH_MAX]; //flash is programmed from RAM

static void cfg_flash_write(void *param){
    flash_range_erase(CFG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CFG_FLASH_OFFSET, cfg_page, CFG_FLASH_MAX);
}

void hw_config_load(void *dst, size_t len){
    memcpy(dst, (const void *)(XIP_BASE + CFG_FLASH_OFFSET), len < CFG_FLASH_MAX ? len : CFG_FLASH_MAX);
}

bool hw_config_save(const void *src, size_t len){
    extern char __flash_binary_end;
    if(len > CFG_FLASH_MAX || (uintptr_t)&__flash_binary_end > XIP_BASE + CFG_FLASH_OFFSET) return false; //would overwrite the program
    memset(cfg_page, 0xFF, sizeof cfg_page);
    memcpy(cfg_page, src, len);
    //pauses the other core and the interrupts for ~50ms (erase + program)
    return flash_safe_execute(cfg_flash_write, NULL, 1000) == PICO_OK;
}
