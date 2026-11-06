#include "led_matrix.h"

volatile uint32_t led_matrix_frame_count = 0;
volatile uint32_t led_matrix_transmit_count = 0;
volatile uint32_t led_matrix_transmit_error_count = 0;
volatile uint32_t led_matrix_busy_count = 0;
volatile uint32_t led_matrix_dma_complete_count = 0;
volatile uint32_t led_matrix_dma_error_count = 0;
volatile uint32_t led_matrix_effect_frame_count = 0;
volatile uint32_t led_matrix_timer_clock_hz = 0;
volatile uint32_t led_matrix_period_ticks = 0;
volatile uint32_t led_matrix_zero_duty = 0;
volatile uint32_t led_matrix_one_duty = 0;

namespace
{
constexpr uint16_t kDataSlots = kLedMatrixCount * 24;
constexpr uint16_t kResetSlots = 48;
constexpr uint16_t kBufferSlots = kDataSlots + kResetSlots;
constexpr uint16_t kMatrixEnablePin = GPIO_PIN_2;

TIM_HandleTypeDef timer_handle = {};
DMA_HandleTypeDef dma_handle = {};
uint16_t dma_buffer[kBufferSlots] __attribute__((aligned(4))) = {};
uint16_t zero_duty = 0;
uint16_t one_duty = 0;
volatile bool matrix_busy = false;
bool matrix_initialized = false;


void encode_byte(uint16_t& offset, uint8_t value)
{
    for (int8_t bit = 7; bit >= 0; --bit)
    {
        dma_buffer[offset++] =
            (value & (1U << bit)) != 0 ? one_duty : zero_duty;
    }
}

void encode_pixel(uint8_t index, LedMatrixRgb color)
{
    uint16_t offset = static_cast<uint16_t>(index) * 24;
    encode_byte(offset, color.g);
    encode_byte(offset, color.r);
    encode_byte(offset, color.b);
}
}

extern "C" void DMA1_Stream5_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&dma_handle);
}

extern "C" void HAL_TIM_PWM_PulseFinishedCallback(
    TIM_HandleTypeDef *timer)
{
    if (timer == &timer_handle &&
        timer->Channel == HAL_TIM_ACTIVE_CHANNEL_2)
    {
        HAL_TIM_PWM_Stop_DMA(&timer_handle, TIM_CHANNEL_2);
        __HAL_TIM_SET_COMPARE(&timer_handle, TIM_CHANNEL_2, 0);
        matrix_busy = false;
        ++led_matrix_dma_complete_count;
    }
}

extern "C" void HAL_TIM_ErrorCallback(TIM_HandleTypeDef *timer)
{
        if (timer == &timer_handle &&
            timer->Channel == HAL_TIM_ACTIVE_CHANNEL_2)
        {
            __HAL_TIM_DISABLE_DMA(&timer_handle, TIM_DMA_CC2);
            (void)HAL_TIM_PWM_Stop(&timer_handle, TIM_CHANNEL_2);
            __HAL_TIM_SET_COMPARE(&timer_handle, TIM_CHANNEL_2, 0);
            matrix_busy = false;
            ++led_matrix_dma_error_count;
        }
}

bool led_matrix_init()
{
    if (matrix_initialized)
    {
        return true;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    const uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    const uint32_t timer_clock =
        ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) ?
        pclk1 : pclk1 * 2U;
    const uint32_t period_ticks =
        (timer_clock + 400000UL) / 800000UL;
    if (period_ticks < 8U || period_ticks > 65535U)
    {
        return false;
    }
    zero_duty = static_cast<uint16_t>(
        (period_ticks * 24U + 50U) / 100U);
    one_duty = static_cast<uint16_t>(
        (period_ticks * 48U + 50U) / 100U);
    led_matrix_timer_clock_hz = timer_clock;
    led_matrix_period_ticks = period_ticks;
    led_matrix_zero_duty = zero_duty;
    led_matrix_one_duty = one_duty;

    GPIO_InitTypeDef data_gpio = {};
    data_gpio.Pin = GPIO_PIN_7;
    data_gpio.Mode = GPIO_MODE_AF_PP;
    data_gpio.Pull = GPIO_NOPULL;
    data_gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    data_gpio.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOA, &data_gpio);

    GPIO_InitTypeDef enable_gpio = {};
    enable_gpio.Pin = kMatrixEnablePin;
    enable_gpio.Mode = GPIO_MODE_OUTPUT_PP;
    enable_gpio.Pull = GPIO_NOPULL;
    enable_gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOF, &enable_gpio);
    // The official matrix enable is active-low.
    HAL_GPIO_WritePin(GPIOF, kMatrixEnablePin, GPIO_PIN_RESET);

    timer_handle.Instance = TIM3;
    timer_handle.Init.Prescaler = 0;
    timer_handle.Init.CounterMode = TIM_COUNTERMODE_UP;
    timer_handle.Init.Period = static_cast<uint32_t>(period_ticks - 1U);
    timer_handle.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    timer_handle.Init.RepetitionCounter = 0;
    timer_handle.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&timer_handle) != HAL_OK)
    {
        return false;
    }

    TIM_MasterConfigTypeDef master = {};
    master.MasterOutputTrigger = TIM_TRGO_RESET;
    master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(
            &timer_handle, &master) != HAL_OK)
    {
        return false;
    }

    TIM_OC_InitTypeDef output = {};
    output.OCMode = TIM_OCMODE_PWM1;
    output.Pulse = 0;
    output.OCPolarity = TIM_OCPOLARITY_HIGH;
    output.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(
            &timer_handle, &output, TIM_CHANNEL_2) != HAL_OK)
    {
        return false;
    }

    dma_handle.Instance = DMA1_Stream5;
    dma_handle.Init.Channel = DMA_CHANNEL_5;
    dma_handle.Init.Direction = DMA_MEMORY_TO_PERIPH;
    dma_handle.Init.PeriphInc = DMA_PINC_DISABLE;
    dma_handle.Init.MemInc = DMA_MINC_ENABLE;
    dma_handle.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    dma_handle.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    dma_handle.Init.Mode = DMA_NORMAL;
    dma_handle.Init.Priority = DMA_PRIORITY_HIGH;
    dma_handle.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&dma_handle) != HAL_OK)
    {
        return false;
    }
    __HAL_LINKDMA(&timer_handle, hdma[TIM_DMA_ID_CC2], dma_handle);

    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);

    for (uint16_t i = kDataSlots; i < kBufferSlots; ++i)
    {
        dma_buffer[i] = 0;
    }
    matrix_initialized = true;
    return led_matrix_clear();
}

bool led_matrix_set_color(uint8_t index, LedMatrixRgb color)
{
    if (!matrix_initialized || index >= kLedMatrixCount || matrix_busy)
    {
        return false;
    }
    encode_pixel(index, color);
    return true;
}

bool led_matrix_fill(LedMatrixRgb color)
{
    if (!matrix_initialized || matrix_busy)
    {
        return false;
    }
    for (uint8_t index = 0; index < kLedMatrixCount; ++index)
    {
        encode_pixel(index, color);
    }
    return true;
}

bool led_matrix_submit_frame(const LedMatrixRgb *colors, uint8_t count)
{
    if (!matrix_initialized || colors == nullptr ||
        count > kLedMatrixCount || matrix_busy)
    {
        if (matrix_busy)
        {
            ++led_matrix_busy_count;
        }
        return false;
    }
    for (uint8_t index = 0; index < kLedMatrixCount; ++index)
    {
        encode_pixel(index, index < count ? colors[index] : LedMatrixRgb{0, 0, 0});
    }
    ++led_matrix_frame_count;
    return true;
}

bool led_matrix_clear()
{
    return led_matrix_fill({0, 0, 0});
}

bool led_matrix_transmit()
{
    if (!matrix_initialized || matrix_busy)
    {
        if (matrix_busy)
        {
            ++led_matrix_busy_count;
        }
        return false;
    }
    matrix_busy = true;
    if (HAL_TIM_PWM_Start_DMA(
            &timer_handle, TIM_CHANNEL_2,
            reinterpret_cast<uint32_t *>(dma_buffer),
            kBufferSlots) != HAL_OK)
    {
        matrix_busy = false;
        ++led_matrix_transmit_error_count;
        return false;
    }
    ++led_matrix_transmit_count;
    return true;
}

bool led_matrix_is_busy()
{
    return matrix_busy;
}
