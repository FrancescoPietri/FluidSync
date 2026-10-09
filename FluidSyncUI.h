//FluidSync - user interface (LVGL), landscape 320x240
//Only LVGL drawing lives here: no FreeRTOS, no GPIO, no access to the logic structs.
//The logic fills a ui_snapshot_t and the display task passes it to ui_update().
//LVGL is not thread-safe: ui_create/ui_update must be called ONLY by display_task0D (or main before the scheduler).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "FluidSyncHW.h" //N_PUMPS, CAROUSEL_CUPS_SLOTS

//Status of machine, chosen by the logic
typedef enum {
    UI_RUNNING = 0,   //machine running, nothing to do for the user
    UI_WAIT_MIX,      //user pressed during a mix: wait, do not touch the carousel
    UI_USER_TURN,     //carousel is at rest: load/remove cups, then press again
    UI_NO_CUPS        //carousel found no empty cup: press the button to load cups
} ui_state_t;

//state of a pump in the current mix
enum {
    UI_PUMP_UNUSED = 0,  //not part of this mix
    UI_PUMP_WAITING,     //job queued, waiting for a free worker
    UI_PUMP_RUNNING,
    UI_PUMP_DONE
};

typedef struct {
    ui_state_t state;
    bool dispensing;                     //a mix is being poured in active_slot
    int active_slot;                     //slot under the nozzle, -1 = none
    int slot[CAROUSEL_CUPS_SLOTS];       //same codes as carousel_slot: 0=empty cup ready, 1=full, 2=no cup
    uint8_t mix_pct[N_PUMPS];            //current (or last) mix, percentage of each liquid
    uint8_t pump_state[N_PUMPS];         //UI_PUMP_*
    uint8_t fill_pct;                    //progress of the current mix, 0..100
    int queue_len;                       //orders waiting
    int queue_max;
    uint32_t mixes_done;
} ui_snapshot_t;

void ui_create(void);                      //build the screen, after the display driver is registered
void ui_update(const ui_snapshot_t *s);    //redraw only what changed
