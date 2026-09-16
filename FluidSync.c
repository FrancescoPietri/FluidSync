#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "lvgl/lvgl.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define N_MAX_ORDER 10

//* Pumps Pins
#define N_PUMPS 6
#define PUMP_4 15
#define PUMP_3 14
#define PUMP_5 13
#define PUMP_1 12
#define PUMP_2 11 
#define PUMP_6 10

//* Servo Motor Pin
#define CAROUSEL_SERVO 8
#define CAROUSEL_CUPS_SLOTS 4
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
#define DISTANCE_ECHO 7
#define DISTANCE_TRIG 6

//* Button Pin
#define UNLOAD_BUTTON 9

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
    gpio_set_function(CAROUSEL_SERVO, GPIO_FUNC_PWM);
    slice   = pwm_gpio_to_slice_num(CAROUSEL_SERVO);
    channel = pwm_gpio_to_channel(CAROUSEL_SERVO);

    pwm_set_clkdiv(slice, 100.0f);  
    pwm_set_wrap(slice, 24999);     
    pwm_set_enabled(slice, true);
}

//* Button init
void init_req_button() {
    gpio_init(UNLOAD_BUTTON);
    gpio_set_dir(UNLOAD_BUTTON, GPIO_IN);
    gpio_pull_up(UNLOAD_BUTTON);
}

void init_distance_sensor() {
    gpio_init(DISTANCE_ECHO);
    gpio_set_dir(DISTANCE_ECHO, GPIO_IN);

    gpio_init(DISTANCE_TRIG);
    gpio_set_dir(DISTANCE_TRIG, GPIO_OUT);
    gpio_put(DISTANCE_TRIG, 0);
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
    init_distance_sensor();
}

float get_distance() {
    
    gpio_put(DISTANCE_TRIG, 1);
    sleep_us(10);
    gpio_put(DISTANCE_TRIG, 0);

    uint32_t timeout = time_us_32() + 30000; 
    while (gpio_get(DISTANCE_ECHO) == 0) {
        if (time_us_32() > timeout) return -1.0f; 
    }
    uint32_t start_time = time_us_32();

    timeout = time_us_32() + 30000;
    while (gpio_get(DISTANCE_ECHO) == 1) {
        if (time_us_32() > timeout) return -1.0f;
    }
    uint32_t end_time = time_us_32();

    uint32_t pulse_duration = end_time - start_time;

    return (pulse_duration * 0.0343f) / 2.0f;
}

typedef struct {
    uint n_components; //number of mixed liquids
    uint pump_output[N_PUMPS]; //define if a pump needs to be activated [0=off; 1=on]
    int in_queue;
} mix;

struct output_carousel_t {
    int carousel_slot[CAROUSEL_CUPS_SLOTS]; //0=ready; 1=full; 2=not ready, no cup in the slot
    int n_filled_slot;
    int n_ready_slot;
    int n_not_ready_slot;
    int bl_cr;
    SemaphoreHandle_t carousel_sem; //carousel manager attende istruzioni da pumps manager su questo mux
    SemaphoreHandle_t carousel_mux; 
} output_carousel;

void carousel_init() {
    output_carousel.carousel_sem = xSemaphoreCreateBinary();
    output_carousel.carousel_mux = xSemaphoreCreateMutex();
}

typedef struct{
    mix order_queue[N_MAX_ORDER];
    int order_head;
    int order_tail;
    int order_counter;
}  order_c_buffer;

struct manager_t {
    order_c_buffer order_buffer;
    SemaphoreHandle_t m_mux; //mutex access manager
    SemaphoreHandle_t order_sem; //if no order are placed task1A wait on this mux 
    SemaphoreHandle_t calibration_sem; 
    int bl_out; //check if task1A output is waiting
} manager;

void manager_init() {
    manager.m_mux = xSemaphoreCreateMutex();
    manager.order_sem = xSemaphoreCreateBinary();
    manager.calibration_sem = xSemaphoreCreateBinary();
}

void pumps_manager(void *pvParameters){
    while(1){
        //controlla se ci sono ordini -> prendo  m_mux e controllo order_counter -> se non ci sono mi metto in attesa su order_mux
        xSemaphoreTake(manager.m_mux, portMAX_DELAY); //accedo al manager
        if(manager.order_buffer.order_counter==0){
            manager.bl_out++; 
            xSemaphoreGive(manager.m_mux);
            xSemaphoreTake(manager.order_sem, portMAX_DELAY); //aspetto un ordine e mi blocco
            xSemaphoreGive(output_carousel.carousel_sem);
        }else{
            //pumps manager e' sicuro che ci sia un ordine da erogare
            //sveglia in carosello in modo che possa calibrarsi e posizionare un bicchiere libero
            xSemaphoreGive(manager.m_mux);
            xSemaphoreGive(output_carousel.carousel_sem);
        }

        //pumps manager attende che carousel manager faccia la calibrazione.
        xSemaphoreTake(manager.calibration_sem, portMAX_DELAY);
        xSemaphoreTake(manager.m_mux, portMAX_DELAY);

        //il bicchiere e' in posizione possiamo estrarre il mix da erogare.
        mix out_mix;
        pop_order(&manager.order_buffer, &out_mix);

        
    }
}

void carousel_manager(void *pvParameters){
    while(1){
        xSemaphoreTake(output_carousel.carousel_sem, portMAX_DELAY); //si mette in attesa di ricere istruzioni da parte di pumps_manager
        xSemaphoreTake(output_carousel.carousel_mux, portMAX_DELAY);
        do{
            if(output_carousel.n_ready_slot==0){
                output_carousel.bl_cr++;
                xSemaphoreGive(output_carousel.carousel_mux);
                xSemaphoreTake(output_carousel.carousel_sem, portMAX_DELAY); //si mette in attesa che l'utente scarichi il carosello
                //token passing da taskC.0
                output_carousel.bl_cr--; 
            }
            for(int i=0; i<CAROUSEL_CUPS_SLOTS; i++){
                //move to slot i
                switch(output_carousel.carousel_slot[i]){
                    case 0: 
                        //increment ready slot, remember this as last ready slot
                        break;
                    case 1: 
                        //skip
                        break;
                    case 2:
                        if(get_distance()<15.0){
                            output_carousel.carousel_slot[i]=0;
                            //increment ready slot, decrement not ready slot, remember this as last ready slot
                        }
                }
            }
        }while(output_carousel.n_ready_slot==0);
        //move to last noted ready slot and wakeup pumps manager
        xSemaphoreGive(output_carousel.carousel_mux);
        xSemaphoreGive(manager.calibration_sem);
    }     
}


bool push_order(order_c_buffer *cb, mix m){
    if (cb->order_head==N_MAX_ORDER){
        return false;
    }
    cb->order_queue[cb->order_head]=m;
    cb->order_head = (cb->order_head +1)%N_MAX_ORDER;
    cb->order_counter++;
    return true;
}

bool pop_order(order_c_buffer *cb, mix *m){
    if(cb->order_tail==0){
        return false;
    }
    *m = cb->order_queue[cb->order_tail];
    cb->order_tail=(cb->order_tail+1)%N_MAX_ORDER;
    cb->order_counter--;
    return true;
}

int main(){

}

/*
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

        printf("distance: %f\n", get_distance());

        lv_tick_inc(5);
        lv_timer_handler();

        if (absolute_time_diff_us(get_absolute_time(), next_servo_time) <= 0) {
            pwm_set_chan_level(slice, channel, carousel_positions[servo_index]);
            servo_index = (servo_index + 1) % 5;
            next_servo_time = make_timeout_time_ms(1000);
        }

        bool current_state = gpio_get(UNLOAD_BUTTON);

        if (!current_state) {
            lv_obj_set_style_bg_color(rect, lv_color_hex(0x00FF00), LV_PART_MAIN);
        }else{
            lv_obj_set_style_bg_color(rect, lv_color_hex(0xFF0000), LV_PART_MAIN);
        }

        sleep_ms(5);
    }

    return 0;
}
*/