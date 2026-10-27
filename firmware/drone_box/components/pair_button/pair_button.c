#include "pair_button.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "secrets.h"

static TaskHandle_t s_task;

// Interrupt handler: does nothing but wake the task. Opening the pairing
// window prints and arms a timer, neither of which belongs in an ISR.
static void IRAM_ATTR on_press(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void button_task(void *arg)
{
    (void)arg;
    int64_t last_us = 0;

    for (;;) {
        // Blocks until the ISR notifies. No polling, no delay loop.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Debounce in software: a mechanical button bounces for a few
        // milliseconds and would otherwise open and immediately re-open the
        // window several times per press.
        const int64_t now = esp_timer_get_time();
        if (now - last_us < PAIR_BUTTON_DEBOUNCE_MS * 1000) {
            continue;
        }
        last_us = now;

        printf("PAIR:  BOOT button pressed\n");
        fflush(stdout);
        secrets_open_pairing(PAIRING_WINDOW_S);
    }
}

void pair_button_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PAIR_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     // the button pulls to ground
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreate(button_task, "pair_btn", 3072, NULL, 5, &s_task);

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PAIR_BUTTON_GPIO, on_press, NULL));

    printf("Pair:  press BOOT on the board to open a %d s pairing window\n",
           PAIRING_WINDOW_S);
    fflush(stdout);
}
