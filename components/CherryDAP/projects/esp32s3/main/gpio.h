#pragma once
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

enum
{
    GPIO_INPUT_MODE = 0, /*!< INPUT NO PULL */
    GPIO_OUTPUT_MODE,    /*!< OUTPUT MODE NO PULL */
    GPIO_INPUT_PP_MODE,  /*!< INPUT PULL UP */
};

static inline void gpio_set_mode(uint32_t pin, uint32_t mode)
{
    if (mode == GPIO_INPUT_MODE)
    {
        gpio_ll_output_disable(&GPIO, pin);
    }
    else if (mode == GPIO_OUTPUT_MODE)
    {
        gpio_ll_output_enable(&GPIO, pin);
    }
    else if (mode == GPIO_INPUT_PP_MODE)
    {
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
        gpio_ll_output_disable(&GPIO, pin);
    }
}

static inline void gpio_write(uint32_t pin, uint32_t level)
{
    gpio_ll_set_level(&GPIO, pin, level);
}

static inline int gpio_read(uint32_t pin)
{
    return gpio_ll_get_level(&GPIO, pin);
}
