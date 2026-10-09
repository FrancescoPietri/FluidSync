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

//order arrival interval
#define ORDER_MIN_INTERVAL_MS 5000
#define ORDER_MAX_INTERVAL_MS 30000
#define USER_BUTTON_GPIO   9
#define BUTTON_POLL_MS     10
#define BUTTON_DEBOUNCE_MS 30


#define N_MAX_ORDER 10

#define TIME_PER_PERCENT_MS 429 //for each 1% the pump must go for 429ms, each mix is 100ml

//* Servo Motor Pin
#define CAROUSEL_SERVO 8
#define CAROUSEL_CUPS_SLOTS 4
#define N_PUMPS 6

//define core affinity
#define CORE_0 (1 << 0)
#define CORE_1 (1 << 1)

enum liquid_t {
    LIQ_YELLOW  = 0,  
    LIQ_MAGENTA = 1,  
    LIQ_CYAN    = 2,  
    LIQ_WHITE   = 3, 
    LIQ_BLACK   = 4, 
    LIQ_WATER   = 5   
};

typedef struct { //define the components of a single mix
    uint n_components; 
    uint pump_output[N_PUMPS]; //define if a pump needs to be activated and the percentage of liquid
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
    uint8_t pump_id;
    uint16_t percentage;
} pump_job_t;

struct output_carousel_t { //struct to manage the status of the output carousel

    int carousel_slot[CAROUSEL_CUPS_SLOTS]; //0=ready; 1=full; 2=not ready->no cup in the slot
    int n_filled_slot;
    int n_ready_slot;
    int n_not_ready_slot;
    int current_ready_slot;

    int bl_cr;                              //track if the carousel is blocked waiting for an action by the user, the carousel might not have ready slot.
    SemaphoreHandle_t carousel_sem;         //Task 1B must be executed after Task 1A, if we have no orders we dont need to check the status of the carousel
    SemaphoreHandle_t user_action_sem;      //carousel wait user action on this sem
    SemaphoreHandle_t carousel_mux;         //mux to modify the status of the slots

}carousel_manager;

struct order_c_buffer{ //struct to manage the order of mixes using a classic circular buffer

    mix order_queue[N_MAX_ORDER];
    int order_head;
    int order_tail;
    int order_counter;


    bool order_wait_user_action;        //if the user is taking action on the carousel the machine must stop, no order can be erogated
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

    order_manager.order_wait_user_action = false;
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

            printf("    PUMP %d STARTED\n", job.pump_id);

            uint32_t delay_ms = job.percentage * TIME_PER_PERCENT_MS;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));

            printf("    PUMP %d STOPPED\n", job.pump_id);

            xSemaphoreGive(order_manager.mix_done_sem);
        }
    }
}

void pumps_task1A(void *pvParameters){
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

        printf("MIX READY IN SLOT %d!\n", carousel_manager.current_ready_slot);

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
            if(carousel_manager.n_ready_slot==0){
                carousel_manager.bl_cr++;
                xSemaphoreGive(carousel_manager.carousel_mux);
                xSemaphoreTake(carousel_manager.user_action_sem, portMAX_DELAY); //si mette in attesa che l'utente scarichi il carosello
                //token passing da taskC.0
                carousel_manager.bl_cr--;
            }
            for(int i=0; i<CAROUSEL_CUPS_SLOTS; i++){
                //move to slot i
                switch(carousel_manager.carousel_slot[i]){
                    case 0:
                        carousel_manager.current_ready_slot=i;
                        break;
                    case 1:
                        break;
                    case 2:
                        if((rand()%20)<15){
                            carousel_manager.carousel_slot[i]=0;
                            carousel_manager.n_ready_slot++;
                            carousel_manager.n_not_ready_slot--;
                            carousel_manager.current_ready_slot=i;
                        }
                }
            }
        }while(carousel_manager.n_ready_slot==0);
        //move to last noted ready slot and wakeup pumps manager
        printf("MOVING TO SLOT %d",  carousel_manager.current_ready_slot);
        order_manager.erogating = true; //protetto da carousel_mux: da qui l'utente aspetta la fine del mix
        xSemaphoreGive(carousel_manager.carousel_mux);
        xSemaphoreGive(order_manager.calibration_sem);
    }
}

static uint32_t pausetta(unsigned int *seed, uint32_t min, uint32_t max){
    return min + (uint32_t)rand_r(seed) % (max - min + 1);
}

void orders_task0A(void *pvParameters){
    unsigned int seed = get_rand_32();
    uint32_t order_id = 0;

    while(1){
        vTaskDelay(pdMS_TO_TICKS(pausetta(&seed, ORDER_MIN_INTERVAL_MS, ORDER_MAX_INTERVAL_MS)));
        xSemaphoreTake(order_manager.order_mux, portMAX_DELAY);
        uint8_t rand_mix = (uint8_t)rand_r(&seed) % (N_TEST_MIXES);

        if(push_order(test_mixes[rand_mix])){
            printf("ORDER PUSHED \n");
            if(order_manager.bl_out>0){
                order_manager.bl_out--;
                xSemaphoreGive(order_manager.order_sem); //segnalo l'arrivo di un nuovo ordine
            }
        }else{
            printf("ORDER QUEUE FULL \n");
        }

        xSemaphoreGive(order_manager.order_mux);
    }
}

static bool button_is_pressed(void){
    return gpio_get(USER_BUTTON_GPIO) == 0;
}

static void wait_button_click(void){
    while(1){
        while(!button_is_pressed()) vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
        if(button_is_pressed()) break;
    }
    while(button_is_pressed()) vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
}


void user_button0B(void *pvParameters){
    while (1)
    {
        wait_button_click();

        xSemaphoreTake(carousel_manager.carousel_mux, portMAX_DELAY);
        if(order_manager.erogating){
            order_manager.bl_user++;
            xSemaphoreGive(carousel_manager.carousel_mux);
            xSemaphoreTake(order_manager.button_sem, portMAX_DELAY);
            //token passing from task1A to task0B of carousel mux
        }

        xSemaphoreTake(order_manager.order_mux, portMAX_DELAY);


        for(int i=0; i<CAROUSEL_CUPS_SLOTS; i++){ //i assume the user do something on the carousel, so the state of each slot is not ready
            carousel_manager.carousel_slot[i]=2;
        }

        carousel_manager.n_filled_slot=0;
        carousel_manager.n_ready_slot=0;
        carousel_manager.n_not_ready_slot=CAROUSEL_CUPS_SLOTS;
        carousel_manager.current_ready_slot=-1;

        printf("USER USING CAROUSEL \n");

        wait_button_click();

        if(carousel_manager.bl_cr>0){
            xSemaphoreGive(carousel_manager.user_action_sem);
            xSemaphoreGive(order_manager.order_mux);
        }else{
            xSemaphoreGive(order_manager.order_mux);
            xSemaphoreGive(carousel_manager.carousel_mux);
        }

    }
}

int main() {
    stdio_init_all();
    srand(time(NULL));

    gpio_init(USER_BUTTON_GPIO);
    gpio_set_dir(USER_BUTTON_GPIO, GPIO_IN);
    gpio_pull_up(USER_BUTTON_GPIO);

    carousel_manager_init();
    order_manager_init();
    //pumps_hardware_init();

    xTaskCreateAffinitySet(pumps_task1A, "Pump_Manager", 512, NULL, 3, CORE_1, NULL);
    xTaskCreateAffinitySet(carousel_task1B, "Carousel_Manager", 512, NULL, 3, CORE_1, NULL);

    xTaskCreateAffinitySet(pump_worker_task1C, "Worker_1", 256, NULL, 2, CORE_1, NULL);
    xTaskCreateAffinitySet(pump_worker_task1C, "Worker_2", 256, NULL, 2, CORE_1, NULL);

    xTaskCreateAffinitySet(orders_task0A, "Order_Generator", 256, NULL, 3, CORE_0, NULL);
    xTaskCreateAffinitySet(user_button0B, "User_Action", 256, NULL, 2, CORE_0, NULL);


    vTaskStartScheduler();

    return 0;
}
    