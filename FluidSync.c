#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "lvgl/lvgl.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "pico/rand.h"

#include "FluidSyncHW.h"
#include "FluidSyncUI.h"

//order arrival interval
#define ORDER_MIN_INTERVAL_MS 5000
#define ORDER_MAX_INTERVAL_MS 30000

#define N_MAX_ORDER 10

#define TIME_PER_PERCENT_MS 429 //for each 1% the pump must go for 429ms, each mix is 100ml

//define core affinity
#define CORE_0 (1 << 0)
#define CORE_1 (1 << 1)

int ui_state;

enum liquid_t {
    LIQ_YELLOW  = 0,  
    LIQ_MAGENTA = 1,  
    LIQ_CYAN    = 2,  
    LIQ_WHITE   = 3, 
    LIQ_BLACK   = 4, 
    LIQ_WATER   = 5   
};

typedef struct { //define the components of a single mix
    int n_components; 
    int pump_output[N_PUMPS]; //define if a pump needs to be activated and the percentage of liquid
} mix;

#define N_TEST_MIXES 6
static const mix test_mixes[N_TEST_MIXES] = {
    { 2, { 30,  0, 70,  0,  0,  0 }},  // 0 Green           ~#16B05B
    { 3, { 25, 15,  0, 60,  0,  0 }},  // 1 Light Orange    ~#FB9B69
    { 3, {  0, 30, 20, 50,  0,  0 }},  // 2 Purple          ~#855AC9
    { 4, { 55, 10, 25,  0, 10,  0 }},  // 3 Dark Green      ~#5F7E1C
    { 5, { 40, 35,  5, 15,  5,  0 }},  // 4 Red             ~#C0452F
    { 6, {  5, 20, 50,  5, 10, 10 }},  // 5 Blue            ~#1E4C8B
};


typedef struct { //define a single pump job
    int pump_id;
    int percentage;
} pump_job_t;

struct output_carousel_t { //struct to manage the status of the output carousel

    int carousel_slot[CAROUSEL_CUPS_SLOTS]; //0=ready; 1=full; 2=not ready->no cup in the slot; 3=empty slot 
    int n_filled_slot;
    int n_ready_slot;
    int n_not_ready_slot;
    int current_ready_slot;

    int bl_cr;                              //track if the carousel is blocked waiting for an action by the user, the carousel might not have ready slot.
    SemaphoreHandle_t carousel_sem;         //Task 1B must be executed after Task 1A, if we have no orders we dont need to check the status of the carousel
    SemaphoreHandle_t user_action_sem;      //carousel wait user action on this sem
    SemaphoreHandle_t carousel_mux;         //mux to modify the status of the slots

}carousel_manager;

struct order_c_buffer{ //struct to manage the order of mixes using a circular buffer

    mix order_queue[N_MAX_ORDER];
    int order_head;
    int order_tail;
    int order_counter;

    int bl_out;                         //signal if task1A output is waiting for a new order
    int bl_user;                        //signal if task1A is waiting for user action
    bool erogating;                     //signal if a mix is been erogated

    SemaphoreHandle_t order_mux;            //mutex access manager
    SemaphoreHandle_t order_sem;        //if no order are placed task1A wait on this sem
    SemaphoreHandle_t calibration_sem;  //task1A wait for carousel calibration on task1B on this sem
    SemaphoreHandle_t mix_done_sem;     //counting sem, task1A must wait all task1C (pump worker) to finish
    SemaphoreHandle_t button_sem;       //if the button is pressed but a mix is been erogated task0B (user action) wait on this semaphore.
    QueueHandle_t pump_queue;

}order_manager;

void carousel_manager_init() {
    carousel_manager.carousel_sem = xSemaphoreCreateBinary();
    carousel_manager.user_action_sem = xSemaphoreCreateBinary();
    carousel_manager.carousel_mux = xSemaphoreCreateBinary();
    xSemaphoreGive(carousel_manager.carousel_mux);


    for(int i=0; i<CAROUSEL_CUPS_SLOTS; i++){
        carousel_manager.carousel_slot[i]=2;
    }

    carousel_manager.n_filled_slot=0;
    carousel_manager.n_ready_slot=0;
    carousel_manager.n_not_ready_slot=CAROUSEL_CUPS_SLOTS;
    carousel_manager.bl_cr=0;
    carousel_manager.current_ready_slot=-1;
}

void order_manager_init() {
    order_manager.order_mux = xSemaphoreCreateBinary();
    xSemaphoreGive(order_manager.order_mux);
    order_manager.order_sem = xSemaphoreCreateBinary();
    order_manager.calibration_sem = xSemaphoreCreateBinary();
    order_manager.button_sem = xSemaphoreCreateBinary();
    order_manager.mix_done_sem = xSemaphoreCreateCounting(N_PUMPS, 0);

    order_manager.pump_queue = xQueueCreate(N_PUMPS, sizeof(pump_job_t));

    order_manager.bl_out= order_manager.bl_user =0;

    for(int i=0;i<N_MAX_ORDER;i++){
        order_manager.order_queue[i].n_components=0;
        for(int j=0; j<N_PUMPS; j++){
            order_manager.order_queue[i].pump_output[j]=0;
        }
    }

    order_manager.order_counter = order_manager.order_head = order_manager.order_tail =0;

    order_manager.erogating = false;
}

bool push_order(mix m){
    if (order_manager.order_counter==N_MAX_ORDER){
        return false;
    }
    order_manager.order_queue[order_manager.order_head]=m;
    order_manager.order_head = (order_manager.order_head +1)%N_MAX_ORDER;
    order_manager.order_counter++;
    return true;
}

bool pop_order(mix *m){
    if(order_manager.order_counter==0){
        return false;
    }
    *m = order_manager.order_queue[order_manager.order_tail];
    order_manager.order_tail=(order_manager.order_tail+1)%N_MAX_ORDER;
    order_manager.order_counter--;
    return true;
}

void pump_worker_task1C(void *pvParameters) { //single thread of a pump, pumps managed by a thread pool, Core1 highest priority task to reduce latency on start and stop erogation
    pump_job_t job;

    while(1) {
        if (xQueueReceive(order_manager.pump_queue, &job, portMAX_DELAY) == pdPASS) {

            hw_pump_on(job.pump_id);
            printf("    PUMP %d STARTED\n", job.pump_id+1);

            int delay_ms = job.percentage * TIME_PER_PERCENT_MS;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));

            hw_pump_off(job.pump_id);
            printf("    PUMP %d STOPPED\n", job.pump_id+1);

            xSemaphoreGive(order_manager.mix_done_sem);
        }
    }
}

void dispenser_task1A(void *pvParameters){
    while(1){

        //check if we have new orders
        xSemaphoreTake(order_manager.order_mux, portMAX_DELAY);
        if(order_manager.order_counter==0){
            order_manager.bl_out++;
            xSemaphoreGive(order_manager.order_mux);
            xSemaphoreTake(order_manager.order_sem, portMAX_DELAY); //wait for an order to arrive
        }else{
            xSemaphoreGive(order_manager.order_mux);
        }

        //task1A wakes up task1B and waits for carousel calibration
        xSemaphoreGive(carousel_manager.carousel_sem);
        xSemaphoreTake(order_manager.calibration_sem, portMAX_DELAY);

        //carousel is on position, we can start to erogate the mix
        xSemaphoreTake(order_manager.order_mux, portMAX_DELAY);
        mix out_mix;
        pop_order(&out_mix);

        for(int i = 0; i < N_PUMPS; i++) {
            if(out_mix.pump_output[i] > 0) {
                pump_job_t job = { .pump_id = i, .percentage = out_mix.pump_output[i] };
                xQueueSend(order_manager.pump_queue, &job, portMAX_DELAY);
            }
        }

        xSemaphoreGive(order_manager.order_mux);


        //wait for all task1C to finish
        for(int i = 0; i < out_mix.n_components; i++) {
            xSemaphoreTake(order_manager.mix_done_sem, portMAX_DELAY);
        }


        xSemaphoreTake(carousel_manager.carousel_mux, portMAX_DELAY);
        carousel_manager.carousel_slot[carousel_manager.current_ready_slot] = 1;
        carousel_manager.n_ready_slot--;
        carousel_manager.n_filled_slot++;
        order_manager.erogating = false;

        printf("MIX READY IN SLOT %d!\n", carousel_manager.current_ready_slot+1);

        //if a user is waiting for a mix to finish i want that his request is served next.
        if(order_manager.bl_user>0){
            order_manager.bl_user--;
            xSemaphoreGive(order_manager.button_sem); //token passing task0B: gli passa carousel_mux
        }else{
            xSemaphoreGive(carousel_manager.carousel_mux);
        }
    }
}

void carousel_task1B(void *pvParameters){
    while(1){
        xSemaphoreTake(carousel_manager.carousel_sem, portMAX_DELAY); //si mette in attesa di ricere istruzioni da parte di pumps_manager
        xSemaphoreTake(carousel_manager.carousel_mux, portMAX_DELAY);
        do{
            if(carousel_manager.n_ready_slot==0 && carousel_manager.current_ready_slot!=-1){
                carousel_manager.bl_cr++;
                ui_state = UI_NO_CUPS;
                xSemaphoreGive(carousel_manager.carousel_mux);
                xSemaphoreTake(carousel_manager.user_action_sem, portMAX_DELAY); //si mette in attesa che l'utente scarichi il carosello
                //token passing da taskC.0
                carousel_manager.bl_cr--;
            }
            carousel_manager.current_ready_slot=0;
            for(int i=0; i<CAROUSEL_CUPS_SLOTS; i++){
                switch(carousel_manager.carousel_slot[i]){
                    case 0:
                        carousel_manager.current_ready_slot=i;
                        break;
                    case 1:
                        break;
                    case 2:
                        hw_carousel_move_to_slot(i);
                        if(hw_cup_present()){
                            carousel_manager.carousel_slot[i]=0;
                            carousel_manager.n_ready_slot++;
                            carousel_manager.n_not_ready_slot--;
                            carousel_manager.current_ready_slot=i;
                        }else{
                            carousel_manager.carousel_slot[i]=3;
                        }
                        break;
                    case 3:
                        break;
                }
            }
        }while(carousel_manager.n_ready_slot==0);
        //move to last noted ready slot and wakeup pumps manager
        printf("MOVING TO SLOT %d\n",  carousel_manager.current_ready_slot+1);
        hw_carousel_move_to_slot(carousel_manager.current_ready_slot);
        order_manager.erogating = true; //user must wait the end of erogation before accessing the carousel
        xSemaphoreGive(carousel_manager.carousel_mux);
        xSemaphoreGive(order_manager.calibration_sem);
    }
}

static int random_delay(unsigned int *seed, int min, int max){
    return min + (int)rand_r(seed) % (max - min + 1);
}

void orders_task0A(void *pvParameters){ //generate orders at random intervals
    int seed = get_rand_32();

    while(1){
        vTaskDelay(pdMS_TO_TICKS(random_delay(&seed, ORDER_MIN_INTERVAL_MS, ORDER_MAX_INTERVAL_MS)));
        xSemaphoreTake(order_manager.order_mux, portMAX_DELAY);
        int rand_mix = (int)rand_r(&seed) % (N_TEST_MIXES);

        if(push_order(test_mixes[rand_mix])){
            printf("ORDER PUSHED \n");
            if(order_manager.bl_out>0){
                order_manager.bl_out--;
                xSemaphoreGive(order_manager.order_sem); //wakes up task dispenser that is waiting for a new order
            }
        }else{
            printf("ORDER QUEUE FULL \n");
        }

        xSemaphoreGive(order_manager.order_mux);
    }
}

static void wait_button_click(void){
    while(1){
        while(!hw_button_is_pressed()){
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
        if(hw_button_is_pressed()) break;
    }

    while(hw_button_is_pressed()){
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
}


void user_button_task0B(void *pvParameters){
    while (1)
    {
        wait_button_click();

        ui_state = UI_WAIT_MIX;
        xSemaphoreTake(carousel_manager.carousel_mux, portMAX_DELAY);

        if(order_manager.erogating){ //if an order is been erogated we cant access the carousel.
            order_manager.bl_user++;
            xSemaphoreGive(carousel_manager.carousel_mux);
            xSemaphoreTake(order_manager.button_sem, portMAX_DELAY);
            //token passing from task1A to task0B of carousel mux
        }

        //xSemaphoreTake(order_manager.order_mux, portMAX_DELAY);


        for(int i=0; i<CAROUSEL_CUPS_SLOTS; i++){ //i assume the user do something on the carousel, so the state of each slot is not ready
            carousel_manager.carousel_slot[i]=2;
        }

        carousel_manager.n_filled_slot=0;
        carousel_manager.n_ready_slot=0;
        carousel_manager.n_not_ready_slot=CAROUSEL_CUPS_SLOTS;
        carousel_manager.current_ready_slot=-1;

        hw_carousel_move_to_rest();
        hw_carousel_release();
        ui_state = UI_USER_TURN;

        printf("USER USING CAROUSEL \n");

        wait_button_click();
        ui_state = UI_RUNNING;

        if(carousel_manager.bl_cr>0){
            xSemaphoreGive(carousel_manager.user_action_sem);
            //xSemaphoreGive(order_manager.order_mux);
        }else{
            //xSemaphoreGive(order_manager.order_mux);
            xSemaphoreGive(carousel_manager.carousel_mux);
        }

    }
}

void display_task0C(void *pvParameters){
    ui_snapshot_t s = {0};
    bool was_on[N_PUMPS] = {false};
    bool mix_known = false;       //mix of the current erogation already read
    bool was_erogating = false;
    uint32_t mixes_done = 0;      //counted here: every end of an erogation is a finished mix
    uint32_t since_refresh = UI_REFRESH_MS;

    while(1){
        if(since_refresh >= UI_REFRESH_MS){
            since_refresh = 0;
            bool erogating = order_manager.erogating;

            //new erogation: the mix is not known yet (it is popped only after the carousel is in position)
            if(erogating && !was_erogating){
                mix_known = false;
                for(int i = 0; i < N_PUMPS; i++){ s.mix_pct[i] = 0; s.pump_state[i] = UI_PUMP_UNUSED; was_on[i] = false; }
            }
            if(!erogating && was_erogating) mixes_done++;
            was_erogating = erogating;

            bool any_on = false;
            for(int i = 0; i < N_PUMPS; i++) if(hw_pump_is_on(i, NULL)) any_on = true;

            //first pump on: pumps_task1A has already popped the mix, it is the slot just before order_tail
            if(erogating && !mix_known && any_on){
                const mix *m = &order_manager.order_queue[(order_manager.order_tail + N_MAX_ORDER - 1) % N_MAX_ORDER];
                for(int i = 0; i < N_PUMPS; i++){
                    s.mix_pct[i] = m->pump_output[i];
                    s.pump_state[i] = m->pump_output[i] > 0 ? UI_PUMP_WAITING : UI_PUMP_UNUSED;
                }
                mix_known = true;
            }

            //pump states and progress, from the pump GPIOs
            uint32_t total = 0, poured = 0;
            for(int i = 0; i < N_PUMPS; i++){
                uint32_t on_ms;
                bool on = hw_pump_is_on(i, &on_ms);
                if(mix_known && s.mix_pct[i] > 0){
                    if(on) s.pump_state[i] = UI_PUMP_RUNNING;
                    else if(was_on[i]) s.pump_state[i] = UI_PUMP_DONE;
                }
                was_on[i] = was_on[i] || on;

                total += s.mix_pct[i];
                if(s.pump_state[i] == UI_PUMP_DONE) poured += s.mix_pct[i];
                else if(s.pump_state[i] == UI_PUMP_RUNNING){
                    uint32_t p = on_ms / TIME_PER_PERCENT_MS;
                    poured += p < s.mix_pct[i] ? p : s.mix_pct[i];
                }
            }
            s.fill_pct = total ? poured * 100 / total : 0;

            s.state = (ui_state_t)ui_state;
            s.dispensing = erogating;
            s.active_slot = carousel_manager.current_ready_slot;
            for(int i = 0; i < CAROUSEL_CUPS_SLOTS; i++) s.slot[i] = carousel_manager.carousel_slot[i];
            s.queue_len = order_manager.order_counter;
            s.queue_max = N_MAX_ORDER;
            s.mixes_done = mixes_done;

            ui_update(&s);
        }
        hw_display_tick(DISPLAY_PERIOD_MS);
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_PERIOD_MS));
        since_refresh += DISPLAY_PERIOD_MS;
    }
}


int main() {
    hw_init();
    hw_display_init();

    carousel_manager_init();
    order_manager_init();

    xTaskCreateAffinitySet(dispenser_task1A, "Pump_Manager", 512, NULL, 3, CORE_1, NULL);
    xTaskCreateAffinitySet(carousel_task1B, "Carousel_Manager", 512, NULL, 3, CORE_1, NULL);

    xTaskCreateAffinitySet(pump_worker_task1C, "Worker_1", 256, NULL, 4, CORE_1, NULL);
    xTaskCreateAffinitySet(pump_worker_task1C, "Worker_2", 256, NULL, 4, CORE_1, NULL);

    xTaskCreateAffinitySet(orders_task0A, "Order_Generator", 512, NULL, 3, CORE_0, NULL);
    xTaskCreateAffinitySet(user_button_task0B, "User_Action", 512, NULL, 4, CORE_0, NULL);
    xTaskCreateAffinitySet(display_task0C, "Display", 2048, NULL, 0, CORE_0, NULL);

    vTaskStartScheduler();

    return 0;
}
    