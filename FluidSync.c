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

#define N_MAX_ORDER 10

#define TIME_PER_PERCENT_MS 429 //for each 1% the pump must go for 429ms, each mix is 100ml

//* Servo Motor Pin
#define CAROUSEL_SERVO 8
#define CAROUSEL_CUPS_SLOTS 4
#define N_PUMPS 6

//define core affinity
#define CORE_0 (1 << 0)
#define CORE_1 (1 << 1)

typedef struct { //define the components of a single mix
    uint n_components; //number of mixed liquids
    uint pump_output[N_PUMPS]; //define if a pump needs to be activated and the percentage of liquid 
} mix;

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
    int bl_cr; //track if the carousel is blocked waiting for an action by the user (waked up by the button)
    SemaphoreHandle_t carousel_sem; //process carousel manager waits for action on carousel on this sem
    SemaphoreHandle_t user_action_sem;
    SemaphoreHandle_t carousel_mux; //mux to modify the status of the slots

}carousel_manager;

struct order_c_buffer{ //struct to manage the order of mixes using a classic circular buffer
    mix order_queue[N_MAX_ORDER];
    int order_head;
    int order_tail;
    int order_counter;
    SemaphoreHandle_t m_mux; //mutex access manager
    SemaphoreHandle_t order_sem; //if no order are placed task1A wait on this mux 
    SemaphoreHandle_t calibration_sem; 
    SemaphoreHandle_t mix_done_sem;
    QueueHandle_t pump_queue;
    int bl_out; //check if task1A output is waiting

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
    order_manager.m_mux = xSemaphoreCreateMutex();
    order_manager.order_sem = xSemaphoreCreateBinary();
    order_manager.calibration_sem = xSemaphoreCreateBinary();
    order_manager.mix_done_sem = xSemaphoreCreateCounting(N_PUMPS, 0);
    
    order_manager.pump_queue = xQueueCreate(N_PUMPS, sizeof(pump_job_t));
    
    order_manager.bl_out=0;

    for(int i=0;i<N_MAX_ORDER;i++){
        order_manager.order_queue[i].n_components=0;
        for(int j=0; j<N_PUMPS; j++){
            order_manager.order_queue[i].pump_output[j]=0;
        }
    }

    order_manager.order_counter = order_manager.order_head = order_manager.order_tail =0;
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

void order_input_task0A(void *pvParameters){
    
}

void pump_worker_task1C(void *pvParameters) {
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
        //controlla se ci sono ordini -> prendo  m_mux e controllo order_counter -> se non ci sono mi metto in attesa su order_mux
        xSemaphoreTake(order_manager.m_mux, portMAX_DELAY); //accedo al order_manager
        if(order_manager.order_counter==0){
            order_manager.bl_out++; 
            xSemaphoreGive(order_manager.m_mux);
            xSemaphoreTake(order_manager.order_sem, portMAX_DELAY); //aspetto un ordine e mi blocco
        }else{
            //pumps manager e' sicuro che ci sia un ordine da erogare
            //sveglia in carosello in modo che possa calibrarsi e posizionare un bicchiere libero
            xSemaphoreGive(order_manager.m_mux);
        }

        //pumps manager attende che carousel manager faccia la calibrazione.
        xSemaphoreGive(carousel_manager.carousel_sem);
        xSemaphoreTake(order_manager.calibration_sem, portMAX_DELAY);
        xSemaphoreTake(order_manager.m_mux, portMAX_DELAY);

        //il bicchiere e' in posizione possiamo estrarre il mix da erogare.
        mix out_mix;
        pop_order(&out_mix);

        int components_to_pour = 0;
        
        for(int i = 0; i < N_PUMPS; i++) {
            if(out_mix.pump_output[i] > 0) {
                pump_job_t job = { .pump_id = i, .percentage = out_mix.pump_output[i] };
                xQueueSend(order_manager.pump_queue, &job, portMAX_DELAY);
                components_to_pour++;
            }
        }

        xSemaphoreGive(order_manager.m_mux);

        for(int i = 0; i < components_to_pour; i++) {
            xSemaphoreTake(order_manager.mix_done_sem, portMAX_DELAY);
        }
        
    
        xSemaphoreTake(carousel_manager.carousel_mux, portMAX_DELAY);
        carousel_manager.carousel_slot[carousel_manager.current_ready_slot] = 1;
        carousel_manager.n_ready_slot--;
        carousel_manager.n_filled_slot++;
        xSemaphoreGive(carousel_manager.carousel_mux);
        
        printf("MIX READY IN SLOT %d!\n", carousel_manager.current_ready_slot);
        
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
        xSemaphoreGive(carousel_manager.carousel_mux);
        xSemaphoreGive(order_manager.calibration_sem);
    }     
}

int main() {
    stdio_init_all();
    srand(time(NULL));

    carousel_manager_init();
    order_manager_init();
    //pumps_hardware_init();

    xTaskCreateAffinitySet(pumps_task1A, "Pump_Manager", 512, NULL, 3, CORE_1, NULL);
    xTaskCreateAffinitySet(carousel_task1B, "Carousel_Manager", 512, NULL, 3, CORE_1, NULL);
 
    xTaskCreateAffinitySet(pump_worker_task1C, "Worker_1", 256, NULL, 2, CORE_1, NULL);
    xTaskCreateAffinitySet(pump_worker_task1C, "Worker_2", 256, NULL, 2, CORE_1, NULL);

    vTaskStartScheduler();

    return 0;
}