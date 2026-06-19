#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"

//* test pump pin
#define PUMP_4 15 //4
#define PUMP_3 14 //3
#define PUMP_5 13 //5
#define PUMP_1 12 //1
#define PUMP_2 11 //2
#define PUMP_6 10 //6

//* test motor pin
#define S_MOTOR 8


//* test distance sensor pin
#define D_ECHO 7
#define D_TRIG 6


int main() {

    stdio_init_all();

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

    gpio_put(PUMP_4, 0);
    gpio_put(PUMP_3, 1);
    gpio_put(PUMP_5, 0);
    gpio_put(PUMP_1, 0);
    gpio_put(PUMP_2, 0);
    gpio_put(PUMP_6, 0);

    gpio_set_function(S_MOTOR, GPIO_FUNC_PWM);
    uint slice   = pwm_gpio_to_slice_num(S_MOTOR);
    uint channel = pwm_gpio_to_channel(S_MOTOR);

    pwm_set_clkdiv(slice, 100.0f);  
    pwm_set_wrap(slice, 24999);     
    pwm_set_enabled(slice, true);


    uint levels[5] = {312, 1031, 1875, 2718, 3750};

    while (true) {
        for (int i = 0; i < 5; i++) {
            pwm_set_chan_level(slice, channel, levels[i]);
            sleep_ms(1000);
        }
    }

    return 0;
}