#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"

//* test pump pin
#define MOSFET_PIN 15

int main() {

    stdio_init_all();

    gpio_set_function(MOSFET_PIN, GPIO_FUNC_PWM);

    uint slice_num = pwm_gpio_to_slice_num(MOSFET_PIN);
    pwm_set_wrap(slice_num, 65535);
    pwm_set_enabled(slice_num, true);

    pwm_set_gpio_level(MOSFET_PIN, 0);
    sleep_ms(2000); 

    printf("Pompa al 100%% (velocità massima)...\n");
    pwm_set_gpio_level(MOSFET_PIN, 65535);

    while (true) {
        sleep_ms(5000);
    }

    return 0;
}