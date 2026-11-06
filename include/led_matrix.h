#ifndef LED_MATRIX_H
#define LED_MATRIX_H

#include "stm32f4xx_hal.h"

#include <stdint.h>

struct LedMatrixRgb
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

constexpr uint8_t kLedMatrixCount = 19;

bool led_matrix_init();
bool led_matrix_set_color(uint8_t index, LedMatrixRgb color);
bool led_matrix_fill(LedMatrixRgb color);
bool led_matrix_submit_frame(const LedMatrixRgb *colors, uint8_t count);
bool led_matrix_clear();
bool led_matrix_transmit();
bool led_matrix_is_busy();

extern volatile uint32_t led_matrix_frame_count;
extern volatile uint32_t led_matrix_transmit_count;
extern volatile uint32_t led_matrix_transmit_error_count;
extern volatile uint32_t led_matrix_busy_count;
extern volatile uint32_t led_matrix_dma_complete_count;
extern volatile uint32_t led_matrix_dma_error_count;
extern volatile uint32_t led_matrix_effect_frame_count;
extern volatile uint32_t led_matrix_timer_clock_hz;
extern volatile uint32_t led_matrix_period_ticks;
extern volatile uint32_t led_matrix_zero_duty;
extern volatile uint32_t led_matrix_one_duty;

#endif
