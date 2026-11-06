
#include "stm32f4xx_hal.h"
#include "led_matrix.h"
#include "led_matrix_effect.h"
#include "song_pcm.h"
#include "song_player.h"
#include "song_selection_state.h"
#include "song_catalog.h"
#include "audio_start_gate.h"
#include "st7789_lcd.h"
#include "audio_synth.h"

#include <stdio.h>
#include <math.h>

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

using namespace song_selection;


#define LED_RED    GPIO_PIN_12
#define LED_BLUE   GPIO_PIN_11
#define LED_ONLY_TEST 0
#define LCD_DIAGNOSTIC_MODE 0
#define RCC_LCD_DIAGNOSTIC_MODE 1
// Temporary pin-mapping test: leave codec registers and audio untouched.
#define ES8388_PIN_DIAGNOSTIC_ONLY 0
// Set to 1 temporarily to isolate matrix initialization, DMA, and output.
#define LED_MATRIX_DIAGNOSTIC_MODE 0
// Regression option: the previous 14-event loop remains buildable by setting
// AUDIO_COMPLETE_SONG_PLAYBACK=0; this controls its bounded loop duration.
#ifndef AUDIO_SONG_LOOP_TEST_MS
#define AUDIO_SONG_LOOP_TEST_MS 3000U
#endif
#ifndef AUDIO_COMPLETE_SONG_PLAYBACK
#define AUDIO_COMPLETE_SONG_PLAYBACK 1
#endif
#ifndef AUDIO_SONG_REPEAT_GAP_MS
#define AUDIO_SONG_REPEAT_GAP_MS 1000U
#endif

// Override with -DES8388_I2C_ADDRESS_7BIT=<address> if the physical board
// uses a different ES8388 address strap than the HW1.1 schematic.
#ifndef ES8388_I2C_ADDRESS_7BIT
#define ES8388_I2C_ADDRESS_7BIT 0x10U
#endif
static_assert(ES8388_I2C_ADDRESS_7BIT <= 0x7FU,
              "ES8388_I2C_ADDRESS_7BIT must be a 7-bit address");

constexpr uint32_t kEs8388I2cClockHz = 100000U;
constexpr uint32_t kEs8388ProbeTrials = 3U;
constexpr uint32_t kEs8388ProbeTimeoutMs = 25U;

#define BTN_UP     GPIO_PIN_5
#define BTN_DOWN   GPIO_PIN_1
#define BTN_LEFT   GPIO_PIN_0
#define BTN_RIGHT  GPIO_PIN_4

volatile uint8_t current_song_index = FUR_ELISE_INDEX;
volatile uint8_t candidate_song_index = FUR_ELISE_INDEX;
volatile bool song_playing = true;
volatile uint8_t navigation_feedback_for_ui = 0;
St7789Lcd lcd;
SemaphoreHandle_t lcd_mutex = nullptr;

// Debugger-visible result: 0=not run, 1=ACK, 2=probe failed,
// 3=APB1 clock out of range, 4=HAL I2C initialization failed.
volatile uint8_t es8388_i2c_probe_status = 0;
volatile uint32_t es8388_i2c_probe_pclk1_hz = 0;
volatile uint32_t es8388_i2c_probe_hal_status = HAL_OK;
volatile uint32_t es8388_i2c_probe_hal_error = HAL_I2C_ERROR_NONE;
I2C_HandleTypeDef es8388_i2c_handle = {};
volatile uint32_t es8388_i2c_pf_input_idr = 0;
volatile uint8_t es8388_i2c_pf0_input_high = 0;
volatile uint8_t es8388_i2c_pf1_input_high = 0;

// One-time read-only hardware snapshot, taken after I2C2 initialization and
// immediately before the first address-ready probe.
volatile uint8_t es8388_i2c_hw_snapshot_valid = 0;
volatile uint32_t es8388_i2c_gpio_moder = 0;
volatile uint32_t es8388_i2c_gpio_otyper = 0;
volatile uint32_t es8388_i2c_gpio_pupdr = 0;
volatile uint32_t es8388_i2c_gpio_afr1 = 0;
volatile uint32_t es8388_i2c_gpio_idr = 0;
volatile uint32_t es8388_i2c_cr1 = 0;
volatile uint32_t es8388_i2c_cr2 = 0;
volatile uint32_t es8388_i2c_ccr = 0;
volatile uint32_t es8388_i2c_trise = 0;
volatile uint32_t es8388_i2c_apb1enr = 0;
volatile uint32_t es8388_i2c_snapshot_pclk1_hz = 0;

// Stage C: 0=not attempted (probe-only), 1=in progress,
// 2=initialized and still muted, 3=I2C transaction failed,
// 4=mute write/readback verification failed.
volatile uint8_t es8388_codec_init_status = 0;
volatile uint8_t es8388_codec_failed_register = 0xFFU;
volatile uint32_t es8388_codec_hal_status = HAL_OK;
volatile uint32_t es8388_codec_hal_error = HAL_I2C_ERROR_NONE;
volatile uint8_t es8388_codec_mute_readback = 0;

// Read-only RCC snapshot for inspection in a debugger. The values are captured
// immediately after HAL_Init(), before application peripherals are initialized.
volatile uint32_t rcc_diag_cr = 0;
volatile uint32_t rcc_diag_cfgr = 0;
volatile uint32_t rcc_diag_pllcfgr = 0;
volatile uint32_t rcc_diag_plli2scfgr = 0;
volatile uint32_t rcc_diag_ahb1enr = 0;
volatile uint32_t rcc_diag_apb1enr = 0;
volatile uint32_t rcc_diag_apb2enr = 0;
volatile uint32_t rcc_diag_system_core_clock_hz = 0;
volatile uint32_t rcc_diag_sysclk_hz = 0;
volatile uint32_t rcc_diag_hclk_hz = 0;
volatile uint32_t rcc_diag_pclk1_hz = 0;
volatile uint32_t rcc_diag_pclk2_hz = 0;
volatile uint32_t rcc_diag_calc_hclk_from_registers_hz = 0;
volatile uint32_t rcc_diag_calc_pclk1_from_registers_hz = 0;
volatile uint32_t rcc_diag_calc_pclk2_from_registers_hz = 0;

// Second read-only snapshot, captured after application peripheral setup.
volatile uint32_t rcc_post_init_cr = 0;
volatile uint32_t rcc_post_init_cfgr = 0;
volatile uint32_t rcc_post_init_pllcfgr = 0;
volatile uint32_t rcc_post_init_plli2scfgr = 0;
volatile uint32_t rcc_post_init_ahb1enr = 0;
volatile uint32_t rcc_post_init_apb1enr = 0;
volatile uint32_t rcc_post_init_apb2enr = 0;
volatile uint32_t rcc_post_init_system_core_clock_hz = 0;
volatile uint32_t rcc_post_init_sysclk_hz = 0;
volatile uint32_t rcc_post_init_hclk_hz = 0;
volatile uint32_t rcc_post_init_pclk1_hz = 0;
volatile uint32_t rcc_post_init_pclk2_hz = 0;
volatile uint32_t rcc_post_init_calc_hclk_hz = 0;
volatile uint32_t rcc_post_init_calc_pclk1_hz = 0;
volatile uint32_t rcc_post_init_calc_pclk2_hz = 0;

// Stage A diagnostic only: 0=not attempted, 1=PLLI2S enabled and ready,
// 2=HAL configuration failed, 3=I2S source is not currently PLLI2S.
volatile uint8_t plli2s_clock_diagnostic_status = 0;

// Stage B configures I2S3 and its DMA path only. No transfer is started.
// 0=not attempted, 1=initialized, 2=PLLI2S unavailable,
// 3=DMA initialization failed, 4=I2S initialization failed.
volatile uint8_t i2s3_stage_b_status = 0;
volatile uint8_t i2s3_stage_b_dma_status = 0;
I2S_HandleTypeDef i2s3_handle = {};
DMA_HandleTypeDef i2s3_tx_dma_handle = {};

enum AudioTestStatus : uint8_t
{
    AUDIO_TEST_NOT_READY = 0,
    AUDIO_TEST_READY_MUTED,
    AUDIO_TEST_STARTING,
    AUDIO_TEST_RUNNING,
    AUDIO_TEST_STOPPING,
    AUDIO_TEST_STOPPED_MUTED,
    AUDIO_TEST_FAILED_MUTED,
    AUDIO_TEST_MUTE_UNKNOWN
};

constexpr uint16_t kAudioTestFrames = 4410U;
constexpr uint16_t kAudioTestChannels = 2U;
constexpr uint16_t kAudioTestDmaHalfwords =
    kAudioTestFrames * kAudioTestChannels;
constexpr uint32_t kAudioTestDurationMs = 5000U;
constexpr uint32_t kAudioTestNotificationToggle = 1U;
constexpr uint32_t kAudioSongNotificationStart = 2U;
constexpr uint32_t kAudioSongNotificationPlayPause = 4U;
constexpr uint32_t kAudioStreamErrorNotification = 8U;
#if !AUDIO_COMPLETE_SONG_PLAYBACK
constexpr size_t kAudioSongNoteCount = 14U;
// Exact event-frame total for the first 14 notes under the existing timing
// assumptions: 23,23,23,23,23,23,23,23,45,23,14,23,23,23 ms.
constexpr size_t kAudioSongPhraseFrames = 14770U;
constexpr size_t kAudioSongSilenceFrames = 6615U; // 150 ms guard
constexpr size_t kAudioSongBufferFrames =
    kAudioSongPhraseFrames + kAudioSongSilenceFrames;
constexpr size_t kAudioSongBufferHalfwords = kAudioSongBufferFrames * 2U;
constexpr uint32_t kAudioSongCodecReadAfterStartMs = 50U;
constexpr uint32_t kAudioSongCodecReadSlowThresholdMs = 10U;
// First 14 entries copied from the existing Fur_Elise_note/beat arrays in
// song_def.h. Kept local so this milestone does not link the other songs.
static const float kFurEliseFirstFourteenNotes[kAudioSongNoteCount] = {
    1.516f, 1.607f, 1.516f, 1.607f, 1.516f, 2.024f, 1.702f, 1.911f,
    2.272f, 0.0f, 3.822f, 3.822f, 3.033f, 2.272f};
static const float kFurEliseFirstFourteenBeats[kAudioSongNoteCount] = {
    0.125f, 0.125f, 0.125f, 0.125f, 0.125f, 0.125f, 0.125f, 0.125f,
    0.25f, 0.125f, 0.075f, 0.125f, 0.125f, 0.125f};
constexpr float kFurEliseTempoAssumption = 0.18f;
#endif
// Selected from the first RT-Spark reference because the original Module 8
// timing handout is not present in this project.
constexpr float kTempoScale = 8.0f;
constexpr uint32_t kSongRepeatGapMs = AUDIO_SONG_REPEAT_GAP_MS;
constexpr uint32_t kAudioDmaHalfFrames = 128U;
constexpr uint32_t kAudioDmaFrames = kAudioDmaHalfFrames * 2U;
static_assert(kAudioDmaFrames * 2U <= UINT16_MAX,
              "stereo DMA transfer length must fit HAL's 16-bit count");
static_assert(song_catalog::kSelectableSongCount == song_selection::kSongCount,
              "song catalog and navigation counts must match");
constexpr uint8_t kAudioTestMuteBit = 0x04U;
constexpr uint8_t kAudioTestDacClockRatio = 0x02U; // 256 x LRCK
constexpr uint8_t kAudioTestDacVolume = 0x00U; // 0 dB digital gain
constexpr uint8_t kAudioTestHeadphoneVolume = 0x21U; // -24 dB
constexpr uint8_t kAudioTestIdleDacVolume = 0xC0U; // -96 dB
constexpr uint8_t kAudioTestIdleHeadphoneVolume = 0x00U; // -45 dB
static_assert(kAudioTestDmaHalfwords == 8820U,
              "4410 stereo frames must contain 8820 halfwords");
#if !AUDIO_COMPLETE_SONG_PLAYBACK
static_assert(kAudioSongBufferHalfwords == 42770U &&
                  kAudioSongBufferHalfwords <= UINT16_MAX,
              "melody DMA length must fit and end on a stereo frame");
#endif

alignas(4) static int16_t audio_test_pcm[kAudioTestDmaHalfwords] = {};
#if !AUDIO_COMPLETE_SONG_PLAYBACK
alignas(4) static int16_t audio_song_pcm[kAudioSongBufferHalfwords] = {};
#endif
alignas(4) static int16_t audio_stream_pcm[kAudioDmaFrames * 2U] = {};
volatile uint8_t audio_stream_active = 0U;
enum AudioSongStatus : uint8_t
{
    AUDIO_SONG_IDLE = 0,
    AUDIO_SONG_PREPARING,
    AUDIO_SONG_PLAYING,
    AUDIO_SONG_PAUSED,
    AUDIO_SONG_COMPLETE,
    AUDIO_SONG_FAILED,
    AUDIO_SONG_UNAVAILABLE
};
enum AudioSongDiagnosticStopReason : uint8_t
{
    AUDIO_SONG_DIAG_STOP_NONE = 0,
    AUDIO_SONG_DIAG_STOP_COMPLETE,
    AUDIO_SONG_DIAG_STOP_USER,
    AUDIO_SONG_DIAG_STOP_PAUSE,
    AUDIO_SONG_DIAG_STOP_PREPARE_FAILED,
    AUDIO_SONG_DIAG_STOP_START_FAILED,
    AUDIO_SONG_DIAG_STOP_DMA_ERROR,
    AUDIO_SONG_DIAG_STOP_CURSOR_ERROR,
    AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED,
    AUDIO_SONG_DIAG_STOP_UNSUPPORTED,
    AUDIO_SONG_DIAG_STOP_REPEAT_TIMEOUT
};
volatile uint8_t audio_song_status = AUDIO_SONG_IDLE;
volatile uint8_t audio_song_buffer_ready = 0U;
volatile uint8_t audio_song_complete_for_ui = 0U;
size_t audio_song_frames_written = 0U;
size_t audio_song_dma_start_frame = 0U;
uint16_t audio_song_dma_halfwords = 0U;
volatile uint8_t audio_song_diag_start_requested = 0U;
volatile uint8_t audio_song_diag_prepare_result = 0U;
volatile uint8_t audio_song_diag_output_start_result = 0U;
volatile uint8_t audio_song_diag_transmit_called = 0U;
volatile uint8_t audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_NONE;
volatile uint8_t audio_song_diag_dma_enabled_after_start = 0U;
volatile uint8_t audio_song_diag_i2s_enabled_after_start = 0U;
volatile uint8_t audio_song_diag_mute_readback_valid = 0U;
volatile uint8_t audio_song_diag_mute_readback = 0xFFU;
volatile uint8_t audio_song_diag_early_cursor_valid = 0U;
volatile uint32_t audio_song_diag_frames_prepared = 0U;
volatile uint32_t audio_song_diag_dma_halfwords = 0U;
volatile uint32_t audio_song_diag_tx_hal_status = HAL_OK;
volatile uint32_t audio_song_diag_ndtr_after_start = 0U;
volatile uint32_t audio_song_diag_early_ndtr = 0U;
volatile uint32_t audio_song_diag_early_cursor = 0U;
volatile uint32_t audio_song_diag_stop_elapsed_ms = 0U;
volatile uint32_t audio_song_diag_stop_phrase_frames = 0U;
volatile uint32_t audio_song_diag_stop_ndtr = 0U;
volatile uint32_t audio_song_diag_stop_cursor = 0U;
volatile uint8_t audio_song_diag_stop_dma_enabled = 0U;
volatile uint8_t audio_song_diag_stop_i2s_enabled = 0U;
volatile uint32_t audio_song_diag_stop_hal_i2s_state = 0U;
volatile uint8_t audio_song_diag_stop_mute_valid = 0U;
volatile uint8_t audio_song_diag_stop_mute_readback = 0xFFU;
volatile uint8_t audio_song_diag_snapshot_reason = AUDIO_SONG_DIAG_STOP_NONE;
TickType_t audio_song_diag_start_tick = 0U;
struct AudioSongDmaSnapshot
{
    uint32_t elapsed_ms;
    uint32_t ndtr;
    uint32_t frame_cursor;
    uint32_t hal_i2s_state;
    uint8_t dma_enabled;
    uint8_t i2s_enabled;
    uint8_t valid;
};
volatile AudioSongDmaSnapshot audio_song_diag_dma_start = {};
volatile AudioSongDmaSnapshot audio_song_diag_dma_first_poll = {};
volatile AudioSongDmaSnapshot audio_song_diag_dma_stop = {};
volatile uint32_t audio_song_diag_pcm_halfwords = 0U;
volatile uint32_t audio_song_diag_pcm_nonzero_halfwords = 0U;
volatile int32_t audio_song_diag_pcm_min = 0;
volatile int32_t audio_song_diag_pcm_max = 0;
volatile uint32_t audio_song_diag_pcm_checksum = 0U;
volatile int16_t audio_song_diag_pcm_pairs[8] = {};
volatile uint8_t audio_song_diag_codec_read_attempted = 0U;
volatile uint8_t audio_song_diag_codec_read_result = 0U;
volatile uint8_t audio_song_diag_codec_read_valid = 0U;
volatile uint8_t audio_song_diag_codec_mute_value = 0xFFU;
volatile uint32_t audio_song_diag_codec_read_elapsed_ms = 0U;
volatile uint32_t audio_song_diag_codec_read_hal_status = HAL_OK;
volatile uint32_t audio_song_diag_codec_read_i2c_error = HAL_I2C_ERROR_NONE;
volatile uintptr_t audio_song_diag_dma_source = 0U;
volatile int16_t audio_song_diag_pcm_first[4] = {};
volatile uint8_t audio_song_diag_lcd_page = 0U;
volatile uint8_t audio_test_buffer_ready = 0U;
volatile uint8_t audio_test_status = AUDIO_TEST_NOT_READY;
volatile uint8_t audio_test_mute_confirmed = 0U;
volatile uint8_t audio_test_dma_active = 0U;
volatile uint8_t audio_test_failure_kind = 0U;
volatile uint8_t audio_test_failed_register = 0xFFU;
volatile uint32_t audio_test_i2c_hal_status = HAL_OK;
volatile uint32_t audio_test_i2c_error = HAL_I2C_ERROR_NONE;
volatile uint32_t audio_test_i2s_hal_status = HAL_OK;
volatile uint32_t audio_test_dma_hal_status = HAL_OK;
volatile uint32_t audio_test_dma_recovery_status = HAL_OK;
volatile uint8_t audio_test_dma_error_seen = 0U;
volatile uint32_t audio_test_dma_error_code = 0U;
TaskHandle_t audio_test_control_task_handle = nullptr;

constexpr uint32_t kAudioPllI2sN = 271U;
constexpr uint32_t kAudioPllI2sR = 2U;

constexpr uint32_t kI2s3AudioFrequencyHz = I2S_AUDIOFREQ_44K;

constexpr uint8_t kAhbPrescalerShift[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};
constexpr uint8_t kApbPrescalerShift[8] = {0, 0, 0, 0, 1, 2, 3, 4};

void capture_rcc_clock_diagnostics()
{
    rcc_diag_cr = RCC->CR;
    rcc_diag_cfgr = RCC->CFGR;
    rcc_diag_pllcfgr = RCC->PLLCFGR;
    rcc_diag_plli2scfgr = RCC->PLLI2SCFGR;
    rcc_diag_ahb1enr = RCC->AHB1ENR;
    rcc_diag_apb1enr = RCC->APB1ENR;
    rcc_diag_apb2enr = RCC->APB2ENR;
    rcc_diag_system_core_clock_hz = SystemCoreClock;

    // HAL's SYSCLK getter decodes RCC source/PLL fields. The bus getters use
    // SystemCoreClock and the current prescaler fields; all are read-only.
    rcc_diag_sysclk_hz = HAL_RCC_GetSysClockFreq();
    rcc_diag_hclk_hz = HAL_RCC_GetHCLKFreq();
    rcc_diag_pclk1_hz = HAL_RCC_GetPCLK1Freq();
    rcc_diag_pclk2_hz = HAL_RCC_GetPCLK2Freq();

    // Independently derive bus frequencies from the captured CFGR prescalers.
    // SYSCLK is still a nominal HAL calculation based on the selected source
    // and the project's HSI_VALUE/HSE_VALUE constants, not a physical measure.
    rcc_diag_calc_hclk_from_registers_hz = rcc_diag_sysclk_hz >>
        kAhbPrescalerShift[(rcc_diag_cfgr >> 4U) & 0x0FU];
    rcc_diag_calc_pclk1_from_registers_hz =
        rcc_diag_calc_hclk_from_registers_hz >>
        kApbPrescalerShift[(rcc_diag_cfgr >> 10U) & 0x07U];
    rcc_diag_calc_pclk2_from_registers_hz =
        rcc_diag_calc_hclk_from_registers_hz >>
        kApbPrescalerShift[(rcc_diag_cfgr >> 13U) & 0x07U];
}

void capture_post_init_rcc_clock_diagnostics()
{
    rcc_post_init_cr = RCC->CR;
    rcc_post_init_cfgr = RCC->CFGR;
    rcc_post_init_pllcfgr = RCC->PLLCFGR;
    rcc_post_init_plli2scfgr = RCC->PLLI2SCFGR;
    rcc_post_init_ahb1enr = RCC->AHB1ENR;
    rcc_post_init_apb1enr = RCC->APB1ENR;
    rcc_post_init_apb2enr = RCC->APB2ENR;
    rcc_post_init_system_core_clock_hz = SystemCoreClock;

    rcc_post_init_sysclk_hz = HAL_RCC_GetSysClockFreq();
    rcc_post_init_hclk_hz = HAL_RCC_GetHCLKFreq();
    rcc_post_init_pclk1_hz = HAL_RCC_GetPCLK1Freq();
    rcc_post_init_pclk2_hz = HAL_RCC_GetPCLK2Freq();

    rcc_post_init_calc_hclk_hz = rcc_post_init_sysclk_hz >>
        kAhbPrescalerShift[(rcc_post_init_cfgr >> 4U) & 0x0FU];
    rcc_post_init_calc_pclk1_hz = rcc_post_init_calc_hclk_hz >>
        kApbPrescalerShift[(rcc_post_init_cfgr >> 10U) & 0x07U];
    rcc_post_init_calc_pclk2_hz = rcc_post_init_calc_hclk_hz >>
        kApbPrescalerShift[(rcc_post_init_cfgr >> 13U) & 0x07U];
}

void configure_plli2s_clock_diagnostic()
{
    // On STM32F407, RCC_CFGR.I2SSRC=0 selects PLLI2S. Preserve that source
    // field and all other RCC settings; do not silently switch clock sources.
    if ((RCC->CFGR & RCC_CFGR_I2SSRC) != 0U)
    {
        plli2s_clock_diagnostic_status = 3;
        return;
    }

    RCC_PeriphCLKInitTypeDef peripheral_clocks = {};
    peripheral_clocks.PeriphClockSelection = RCC_PERIPHCLK_PLLI2S;
    peripheral_clocks.PLLI2S.PLLI2SN = kAudioPllI2sN;
    peripheral_clocks.PLLI2S.PLLI2SR = kAudioPllI2sR;

    if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clocks) == HAL_OK &&
        __HAL_RCC_GET_FLAG(RCC_FLAG_PLLI2SRDY) != RESET)
    {
        plli2s_clock_diagnostic_status = 1;
    }
    else
    {
        plli2s_clock_diagnostic_status = 2;
    }
}

extern "C" void HAL_I2S_MspInit(I2S_HandleTypeDef *i2s)
{
    if (i2s == nullptr || i2s->Instance != SPI3)
    {
        return;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_SPI3_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {};
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF6_SPI3;
    gpio.Pin = GPIO_PIN_7;
    HAL_GPIO_Init(GPIOC, &gpio); // I2S3_MCK
    gpio.Pin = GPIO_PIN_3 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOB, &gpio); // I2S3_CK and I2S3_SD
    gpio.Pin = GPIO_PIN_15;
    HAL_GPIO_Init(GPIOA, &gpio); // I2S3_WS

    // PB3 and PA15 are JTAG pins. Configuring them for I2S disables those
    // JTAG signals/SWO; the independent PA13/PA14 SWD interface remains.
    i2s3_tx_dma_handle.Instance = DMA1_Stream7;
    i2s3_tx_dma_handle.Init.Channel = DMA_CHANNEL_0;
    i2s3_tx_dma_handle.Init.Direction = DMA_MEMORY_TO_PERIPH;
    i2s3_tx_dma_handle.Init.PeriphInc = DMA_PINC_DISABLE;
    i2s3_tx_dma_handle.Init.MemInc = DMA_MINC_ENABLE;
    i2s3_tx_dma_handle.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    i2s3_tx_dma_handle.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    i2s3_tx_dma_handle.Init.Mode = DMA_CIRCULAR;
    i2s3_tx_dma_handle.Init.Priority = DMA_PRIORITY_HIGH;
    i2s3_tx_dma_handle.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    i2s3_tx_dma_handle.Init.MemBurst = DMA_MBURST_SINGLE;
    i2s3_tx_dma_handle.Init.PeriphBurst = DMA_PBURST_SINGLE;
    i2s3_stage_b_dma_status =
        HAL_DMA_Init(&i2s3_tx_dma_handle) == HAL_OK ? 1U : 0U;
    if (i2s3_stage_b_dma_status == 0U)
    {
        return;
    }

    __HAL_LINKDMA(i2s, hdmatx, i2s3_tx_dma_handle);
    HAL_NVIC_SetPriority(DMA1_Stream7_IRQn,
                         configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream7_IRQn);
}

extern "C" void DMA1_Stream7_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&i2s3_tx_dma_handle);
}

extern "C" void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *i2s)
{
    if (i2s != nullptr && i2s->Instance == SPI3 &&
        audio_stream_active != 0U)
    {
        audio_synth::render(audio_stream_pcm, kAudioDmaHalfFrames);
    }
}

extern "C" void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *i2s)
{
    if (i2s != nullptr && i2s->Instance == SPI3 &&
        audio_stream_active != 0U)
    {
        audio_synth::render(
            &audio_stream_pcm[kAudioDmaHalfFrames * 2U],
            kAudioDmaHalfFrames);
    }
}

extern "C" void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *i2s)
{
    if (i2s != nullptr && i2s->Instance == SPI3)
    {
        audio_test_dma_error_seen = 1U;
        audio_test_dma_error_code = i2s->ErrorCode;
        if (audio_test_control_task_handle != nullptr)
        {
            BaseType_t higherPriorityTaskWoken = pdFALSE;
            (void)xTaskNotifyFromISR(
                audio_test_control_task_handle,
                kAudioStreamErrorNotification, eSetBits,
                &higherPriorityTaskWoken);
            portYIELD_FROM_ISR(higherPriorityTaskWoken);
        }
    }
}

void configure_i2s3_stage_b()
{
    if (plli2s_clock_diagnostic_status != 1U ||
        (RCC->CR & RCC_CR_PLLI2SRDY) == 0U)
    {
        i2s3_stage_b_status = 2U;
        return;
    }

    i2s3_stage_b_dma_status = 0U;
    i2s3_handle.Instance = SPI3;
    i2s3_handle.Init.Mode = I2S_MODE_MASTER_TX;
    i2s3_handle.Init.Standard = I2S_STANDARD_PHILIPS;
    i2s3_handle.Init.DataFormat = I2S_DATAFORMAT_16B;
    i2s3_handle.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
    i2s3_handle.Init.AudioFreq = kI2s3AudioFrequencyHz;
    i2s3_handle.Init.CPOL = I2S_CPOL_LOW;
    i2s3_handle.Init.ClockSource = I2S_CLOCK_PLL;
    i2s3_handle.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;

    if (HAL_I2S_Init(&i2s3_handle) != HAL_OK)
    {
        i2s3_stage_b_status = i2s3_stage_b_dma_status == 0U ? 3U : 4U;
        return;
    }

    // HAL_I2S_Init leaves I2SE clear. No DMA transfer, clocks, or audio data
    // begin until a later explicit transmit call (not part of Stage B).
    i2s3_stage_b_status = i2s3_stage_b_dma_status == 1U ? 1U : 3U;
}

void draw_rcc_lcd_page(const char *title,
                       const uint32_t registers[7],
                       const uint32_t frequencies_hz[4],
                       uint8_t plli2s_status = 0,
                       uint8_t i2s3_status = 0,
                       bool show_codec_status = false)
{
    static const char *const register_names[7] = {
        "CR", "CFGR", "PLLCFGR", "PLLI2SCFGR",
        "AHB1ENR", "APB1ENR", "APB2ENR"};
    static const char *const frequency_names[4] = {
        "SYSCLK", "HCLK", "PCLK1", "PCLK2"};

    lcd.clear(0x0000);
    lcd.drawText(6, 4, title, 0x07E0, 0x0000);
    lcd.drawText(6, 14, "RAW RCC HEX", 0x07E0, 0x0000);
    for (uint8_t index = 0; index < 7; ++index)
    {
        char line[28] = {};
        snprintf(line, sizeof(line), "%s %08lX", register_names[index],
                 static_cast<unsigned long>(registers[index]));
        lcd.drawText(6, static_cast<uint16_t>(24U + index * 10U),
                     line, 0xFFFF, 0x0000);
    }

    lcd.drawText(6, 100, "CALC FREQ HZ", 0x07E0, 0x0000);
    for (uint8_t index = 0; index < 4; ++index)
    {
        char line[32] = {};
        snprintf(line, sizeof(line), "%s %lu", frequency_names[index],
                 static_cast<unsigned long>(frequencies_hz[index]));
        lcd.drawText(6, static_cast<uint16_t>(112U + index * 10U),
                     line, 0xFFFF, 0x0000);
    }

    const bool plli2s_enabled =
        (registers[0] & RCC_CR_PLLI2SON) != 0U;
    const bool plli2s_ready =
        (registers[0] & RCC_CR_PLLI2SRDY) != 0U;
    char pll_status[32] = {};
    snprintf(pll_status, sizeof(pll_status), "PLLI2S %s / %s",
             plli2s_enabled ? "ON" : "OFF",
             plli2s_ready ? "READY" : "NOT READY");
    lcd.drawText(6, 158, pll_status, 0xFFFF, 0x0000);
    if (plli2s_status != 0U)
    {
        const char *configuration_status =
            plli2s_status == 1U ? "CONFIG: OK" :
            (plli2s_status == 2U ? "CONFIG: HAL ERROR" :
                                   "CONFIG: SOURCE NOT PLLI2S");
        lcd.drawText(6, 170, configuration_status, 0xFFFF, 0x0000);
    }
    if (i2s3_status != 0U)
    {
        const char *i2s_status_text =
            i2s3_status == 1U ? "I2S3/DMA READY (IDLE)" :
            (i2s3_status == 2U ? "I2S3 PLLI2S NOT READY" :
             (i2s3_status == 3U ? "I2S3 DMA INIT ERROR" :
                                  "I2S3 INIT ERROR"));
        lcd.drawText(6, 182, i2s_status_text, 0xFFFF, 0x0000);
    }
    if (show_codec_status)
    {
        const char *codec_status_text =
            es8388_i2c_probe_status == 1U ?
                (es8388_codec_init_status == 2U ?
                    "I2C ACK / CODEC MUTED" :
                 (es8388_codec_init_status == 3U ||
                  es8388_codec_init_status == 4U) ?
                    "I2C ACK / INIT FAILED" :
                    "I2C ACK ONLY") :
            (es8388_i2c_probe_status == 2U ? "I2C PROBE FAILED" :
                                             "I2C PROBE ERROR");
        lcd.drawText(6, 194, codec_status_text, 0xFFFF, 0x0000);
        if (es8388_i2c_probe_status == 2U)
        {
            char probe_failure_line[32] = {};
            snprintf(probe_failure_line, sizeof(probe_failure_line),
                     "HAL %lu ERR %08lX",
                     static_cast<unsigned long>(es8388_i2c_probe_hal_status),
                     static_cast<unsigned long>(es8388_i2c_probe_hal_error));
            lcd.drawText(6, 206, probe_failure_line, 0xF800, 0x0000);
        }
        else if (es8388_codec_init_status == 3U ||
            es8388_codec_init_status == 4U)
        {
            char failure_line[32] = {};
            snprintf(failure_line, sizeof(failure_line),
                     "FAIL R%02X H%lu E%08lX",
                     es8388_codec_failed_register,
                     static_cast<unsigned long>(es8388_codec_hal_status),
                     static_cast<unsigned long>(es8388_codec_hal_error));
            lcd.drawText(6, 206, failure_line, 0xF800, 0x0000);
        }
    }
}

void draw_es8388_i2c_register_page(
    const char *title,
    const char *const register_names[],
    const uint32_t register_values[],
    uint8_t register_count,
    uint32_t pclk1_hz)
{
    lcd.clear(0x0000);
    lcd.drawText(6, 6, title, 0x07E0, 0x0000);
    lcd.drawText(6, 22, "POST-INIT / PRE-PROBE", 0xFFFF, 0x0000);
    char pin_sample_line[32] = {};
    snprintf(pin_sample_line, sizeof(pin_sample_line),
             "INPUT PF0:%s PF1:%s",
             es8388_i2c_pf0_input_high != 0U ? "HIGH" : "LOW",
             es8388_i2c_pf1_input_high != 0U ? "HIGH" : "LOW");
    lcd.drawText(6, 34, pin_sample_line, 0xFFE0, 0x0000);
    if (es8388_i2c_hw_snapshot_valid == 0U)
    {
        lcd.drawText(6, 58, "SNAPSHOT NOT CAPTURED", 0xF800, 0x0000);
        return;
    }

    for (uint8_t index = 0; index < register_count; ++index)
    {
        char line[28] = {};
        snprintf(line, sizeof(line), "%s %08lX", register_names[index],
                 static_cast<unsigned long>(register_values[index]));
        lcd.drawText(6, static_cast<uint16_t>(56U + index * 20U),
                     line, 0xFFFF, 0x0000);
    }
    char pclk_line[28] = {};
    snprintf(pclk_line, sizeof(pclk_line), "PCLK1 HZ %lu",
             static_cast<unsigned long>(pclk1_hz));
    lcd.drawText(6, static_cast<uint16_t>(56U + register_count * 20U),
                 pclk_line, 0xFFE0, 0x0000);

    char probe_line[32] = {};
    if (es8388_i2c_probe_status == 1U)
    {
        snprintf(probe_line, sizeof(probe_line), "PROBE ACK ADDR %02X",
                 static_cast<unsigned>(ES8388_I2C_ADDRESS_7BIT));
        lcd.drawText(6, 184, probe_line, 0x07E0, 0x0000);
    }
    else if (es8388_i2c_probe_status == 2U)
    {
        snprintf(probe_line, sizeof(probe_line), "PROBE FAILED H%lu E%08lX",
                 static_cast<unsigned long>(es8388_i2c_probe_hal_status),
                 static_cast<unsigned long>(es8388_i2c_probe_hal_error));
        lcd.drawText(6, 184, probe_line, 0xF800, 0x0000);
    }
    else
    {
        snprintf(probe_line, sizeof(probe_line), "PROBE STATUS %u",
                 static_cast<unsigned>(es8388_i2c_probe_status));
        lcd.drawText(6, 184, probe_line, 0xF800, 0x0000);
    }
}

extern "C" void boot_diag_stage(uint8_t stage)
{
#if LCD_DIAGNOSTIC_MODE
    HAL_GPIO_WritePin(GPIOF, LED_BLUE, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_SET);
    HAL_Delay(500);
    for (uint8_t pulse = 0; pulse < stage; ++pulse)
    {
        HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_RESET);
        HAL_Delay(120);
        HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_SET);
        HAL_Delay(180);
    }
    HAL_Delay(1000);
#else
    (void)stage;
#endif
}

extern "C" void xPortSysTickHandler(void);
extern "C" void SysTick_Handler(void)
{
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        xPortSysTickHandler();
    }
}

void hardware_init()
{
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {};

    // Onboard LEDs
    gpio.Pin = LED_RED | LED_BLUE;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOF, &gpio);

    // Four directional buttons
    gpio.Pin = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &gpio);

    HAL_GPIO_WritePin(
        GPIOF, LED_RED | LED_BLUE, GPIO_PIN_RESET);
}

void capture_es8388_i2c_hardware_snapshot()
{
    es8388_i2c_gpio_moder = GPIOF->MODER;
    es8388_i2c_gpio_otyper = GPIOF->OTYPER;
    es8388_i2c_gpio_pupdr = GPIOF->PUPDR;
    es8388_i2c_gpio_afr1 = GPIOF->AFR[0];
    es8388_i2c_gpio_idr = GPIOF->IDR;
    es8388_i2c_cr1 = I2C2->CR1;
    es8388_i2c_cr2 = I2C2->CR2;
    es8388_i2c_ccr = I2C2->CCR;
    es8388_i2c_trise = I2C2->TRISE;
    es8388_i2c_apb1enr = RCC->APB1ENR;
    es8388_i2c_snapshot_pclk1_hz = HAL_RCC_GetPCLK1Freq();
    es8388_i2c_hw_snapshot_valid = 1U;
}

void es8388_i2c_probe()
{
    __HAL_RCC_GPIOF_CLK_ENABLE();

    // Sample the schematic's I2C2 pins while they are high-impedance inputs.
    // No internal pull is enabled; the external board pull-ups determine level.
    GPIO_InitTypeDef gpio = {};
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOF, &gpio);
    es8388_i2c_pf_input_idr = GPIOF->IDR;
    es8388_i2c_pf0_input_high =
        (es8388_i2c_pf_input_idr & GPIO_PIN_0) != 0U ? 1U : 0U;
    es8388_i2c_pf1_input_high =
        (es8388_i2c_pf_input_idr & GPIO_PIN_1) != 0U ? 1U : 0U;

    es8388_i2c_probe_pclk1_hz = HAL_RCC_GetPCLK1Freq();
    if (es8388_i2c_probe_pclk1_hz < 2000000U ||
        es8388_i2c_probe_pclk1_hz > 42000000U)
    {
        es8388_i2c_probe_status = 3;
        return;
    }

    __HAL_RCC_I2C2_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF4_I2C2;
    HAL_GPIO_Init(GPIOF, &gpio);

    es8388_i2c_handle.Instance = I2C2;
    es8388_i2c_handle.Init.ClockSpeed = kEs8388I2cClockHz;
    es8388_i2c_handle.Init.DutyCycle = I2C_DUTYCYCLE_2;
    es8388_i2c_handle.Init.OwnAddress1 = 0;
    es8388_i2c_handle.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    es8388_i2c_handle.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    es8388_i2c_handle.Init.OwnAddress2 = 0;
    es8388_i2c_handle.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    es8388_i2c_handle.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    const HAL_StatusTypeDef i2c_init_status =
        HAL_I2C_Init(&es8388_i2c_handle);
    capture_es8388_i2c_hardware_snapshot();
    if (i2c_init_status != HAL_OK)
    {
        es8388_i2c_probe_status = 4;
        return;
    }

    const uint16_t address = static_cast<uint16_t>(
        ES8388_I2C_ADDRESS_7BIT << 1U);
    const HAL_StatusTypeDef probe_hal_status = HAL_I2C_IsDeviceReady(
        &es8388_i2c_handle,
        address,
        kEs8388ProbeTrials,
        kEs8388ProbeTimeoutMs);
    es8388_i2c_probe_hal_status =
        static_cast<uint32_t>(probe_hal_status);
    es8388_i2c_probe_hal_error =
        probe_hal_status == HAL_OK ? HAL_I2C_ERROR_NONE :
                                     HAL_I2C_GetError(&es8388_i2c_handle);
    es8388_i2c_probe_status = probe_hal_status == HAL_OK ? 1U : 2U;
}

bool es8388_record_i2c_failure(uint8_t reg, HAL_StatusTypeDef status)
{
    es8388_codec_failed_register = reg;
    es8388_codec_hal_status = static_cast<uint32_t>(status);
    es8388_codec_hal_error = HAL_I2C_GetError(&es8388_i2c_handle);
    es8388_codec_init_status = 3U;
    return false;
}

bool es8388_read_register(uint8_t reg, uint8_t &value)
{
    const uint16_t address = static_cast<uint16_t>(
        ES8388_I2C_ADDRESS_7BIT << 1U);
    const HAL_StatusTypeDef status = HAL_I2C_Mem_Read(
        &es8388_i2c_handle, address, reg, I2C_MEMADD_SIZE_8BIT,
        &value, 1U, kEs8388ProbeTimeoutMs);
    if (status != HAL_OK)
    {
        return es8388_record_i2c_failure(reg, status);
    }
    return true;
}

bool es8388_write_register(uint8_t reg, uint8_t value)
{
    const uint16_t address = static_cast<uint16_t>(
        ES8388_I2C_ADDRESS_7BIT << 1U);
    const HAL_StatusTypeDef status = HAL_I2C_Mem_Write(
        &es8388_i2c_handle, address, reg, I2C_MEMADD_SIZE_8BIT,
        &value, 1U, kEs8388ProbeTimeoutMs);
    if (status != HAL_OK)
    {
        return es8388_record_i2c_failure(reg, status);
    }
    return true;
}

bool es8388_codec_initialize_muted()
{
    if (es8388_i2c_probe_status != 1U)
    {
        return false;
    }

    es8388_codec_init_status = 1U;
    es8388_codec_failed_register = 0xFFU;
    es8388_codec_hal_status = HAL_OK;
    es8388_codec_hal_error = HAL_I2C_ERROR_NONE;

    uint8_t mute_register = 0;
    if (!es8388_read_register(0x19U, mute_register))
    {
        return false;
    }

    mute_register = static_cast<uint8_t>(mute_register | 0x04U);
    uint8_t mute_readback = 0;
    if (!es8388_write_register(0x19U, mute_register) ||
        !es8388_read_register(0x19U, mute_readback))
    {
        return false;
    }
    es8388_codec_mute_readback = mute_readback;
    if ((es8388_codec_mute_readback & 0x04U) == 0U)
    {
        es8388_codec_failed_register = 0x19U;
        es8388_codec_hal_status = HAL_OK;
        es8388_codec_hal_error = HAL_I2C_GetError(&es8388_i2c_handle);
        es8388_codec_init_status = 4U;
        return false;
    }

    struct RegisterWrite
    {
        uint8_t reg;
        uint8_t value;
    };
    static const RegisterWrite initialization_sequence[] = {
        {0x04U, 0xC0U},
        {0x08U, 0x00U},
        {0x02U, 0xF3U},
        {0x00U, 0x05U},
        {0x01U, 0x40U},
        {0x17U, 0x18U},
        {0x1AU, 0xC0U},
        {0x1BU, 0xC0U},
        {0x27U, 0x90U},
        {0x2AU, 0x90U},
        {0x2EU, 0x00U},
        {0x2FU, 0x00U},
        {0x02U, 0xAAU},
        {0x04U, 0x30U}};

    for (const RegisterWrite &write : initialization_sequence)
    {
        if (!es8388_write_register(write.reg, write.value))
        {
            return false;
        }
    }

    if (!es8388_read_register(0x19U, mute_readback))
    {
        return false;
    }
    es8388_codec_mute_readback = mute_readback;
    if ((es8388_codec_mute_readback & 0x04U) == 0U)
    {
        es8388_codec_failed_register = 0x19U;
        es8388_codec_hal_status = HAL_OK;
        es8388_codec_hal_error = HAL_I2C_GetError(&es8388_i2c_handle);
        es8388_codec_init_status = 4U;
        return false;
    }

    es8388_codec_init_status = 2U;
    return true;
}

const char *audio_test_status_text(uint8_t status)
{
    switch (status)
    {
    case AUDIO_TEST_READY_MUTED: return "HOLD UP+DOWN: SINE TEST";
    case AUDIO_TEST_STARTING: return "SINE: STARTING";
    case AUDIO_TEST_RUNNING: return "SINE: RUNNING (STOP CHORD)";
    case AUDIO_TEST_STOPPING: return "SINE: STOPPING";
    case AUDIO_TEST_STOPPED_MUTED: return "SINE: STOPPED MUTED";
    case AUDIO_TEST_FAILED_MUTED: return "SINE: FAILED / MUTED";
    case AUDIO_TEST_MUTE_UNKNOWN: return "SINE: MUTE UNKNOWN";
    default: return "SINE: NOT READY";
    }
}

const char *audio_song_status_text(uint8_t status)
{
    switch (status)
    {
    case AUDIO_SONG_PREPARING: return "MELODY: PREPARING";
    case AUDIO_SONG_PLAYING: return "MELODY: PLAYING (UP PAUSE)";
    case AUDIO_SONG_PAUSED: return "MELODY: PAUSED (UP RESUME)";
    case AUDIO_SONG_COMPLETE: return "MELODY: STOPPED MUTED";
    case AUDIO_SONG_FAILED: return "MELODY: FAILED / CHECK MUTE";
    case AUDIO_SONG_UNAVAILABLE: return "MELODY: UNAVAILABLE";
    default: return nullptr;
    }
}

const char *player_playback_status_text()
{
    switch (audio_song_status)
    {
    case AUDIO_SONG_PREPARING: return "STARTING";
    case AUDIO_SONG_PLAYING: return "PLAYING";
    case AUDIO_SONG_PAUSED: return "PAUSED";
    case AUDIO_SONG_COMPLETE: return "STOPPED";
    case AUDIO_SONG_FAILED: return "AUDIO ERROR";
    case AUDIO_SONG_UNAVAILABLE: return "UNAVAILABLE";
    default: return song_playing ? "PLAYING" : "PAUSED";
    }
}

bool player_audio_is_playing()
{
    switch (audio_song_status)
    {
    case AUDIO_SONG_PREPARING:
    case AUDIO_SONG_PAUSED:
    case AUDIO_SONG_COMPLETE:
    case AUDIO_SONG_FAILED:
    case AUDIO_SONG_UNAVAILABLE:
        return false;
    case AUDIO_SONG_PLAYING:
        return true;
    default:
        // Preserve the existing player indication while the sine test is
        // active or the melody controller is idle.
        return song_playing;
    }
}

const char *audio_song_diag_stop_reason_text(uint8_t reason)
{
    switch (reason)
    {
    case AUDIO_SONG_DIAG_STOP_COMPLETE: return "DONE";
    case AUDIO_SONG_DIAG_STOP_USER: return "USER";
    case AUDIO_SONG_DIAG_STOP_PAUSE: return "PAUSE";
    case AUDIO_SONG_DIAG_STOP_PREPARE_FAILED: return "PREP";
    case AUDIO_SONG_DIAG_STOP_START_FAILED: return "START";
    case AUDIO_SONG_DIAG_STOP_DMA_ERROR: return "DMAERR";
    case AUDIO_SONG_DIAG_STOP_CURSOR_ERROR: return "CURSOR";
    case AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED: return "STOPERR";
    case AUDIO_SONG_DIAG_STOP_UNSUPPORTED: return "UNSUP";
    case AUDIO_SONG_DIAG_STOP_REPEAT_TIMEOUT: return "TEST END";
    default: return "NONE";
    }
}

void draw_audio_test_status_line(uint16_t y)
{
    lcd.fillRect(6, y, 228, 10, 0x0000);
    const uint8_t song_status = audio_song_status;
    const char *song_text = audio_song_status_text(song_status);
    if (song_status != AUDIO_SONG_IDLE &&
        audio_song_diag_start_requested != 0U)
    {
        char diagnostic_line[40] = {};
        switch (audio_song_diag_lcd_page % 16U)
        {
        case 0U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "REQ%u PREP%u FR%lu",
                static_cast<unsigned>(audio_song_diag_start_requested),
                static_cast<unsigned>(audio_song_diag_prepare_result),
                static_cast<unsigned long>(audio_song_diag_frames_prepared));
            break;
        case 1U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "OK%u C%u T%lu D%u I%u M%02X",
                static_cast<unsigned>(audio_song_diag_output_start_result),
                static_cast<unsigned>(audio_song_diag_transmit_called),
                static_cast<unsigned long>(audio_song_diag_tx_hal_status),
                static_cast<unsigned>(
                    audio_song_diag_dma_enabled_after_start),
                static_cast<unsigned>(
                    audio_song_diag_i2s_enabled_after_start),
                audio_song_diag_mute_readback_valid != 0U ?
                    static_cast<unsigned>(audio_song_diag_mute_readback) :
                    0xFFU);
            break;
        case 2U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "SRC%08lX H%lu",
                static_cast<unsigned long>(audio_song_diag_dma_source),
                static_cast<unsigned long>(audio_song_diag_dma_halfwords));
            break;
        case 3U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "ND%lu C%lu R%s",
                static_cast<unsigned long>(audio_song_diag_early_ndtr),
                static_cast<unsigned long>(audio_song_diag_early_cursor),
                audio_song_diag_stop_reason_text(
                    audio_song_diag_stop_reason));
            break;
        case 4U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "END%lums F%lu %s",
                static_cast<unsigned long>(
                    audio_song_diag_stop_elapsed_ms),
                static_cast<unsigned long>(
                    audio_song_diag_stop_phrase_frames),
                audio_song_diag_stop_reason_text(
                    audio_song_diag_snapshot_reason));
            break;
        case 5U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "N%lu C%lu D%u I%u H%lu M%02X",
                static_cast<unsigned long>(audio_song_diag_stop_ndtr),
                static_cast<unsigned long>(audio_song_diag_stop_cursor),
                static_cast<unsigned>(
                    audio_song_diag_stop_dma_enabled),
                static_cast<unsigned>(
                    audio_song_diag_stop_i2s_enabled),
                static_cast<unsigned long>(
                    audio_song_diag_stop_hal_i2s_state),
                audio_song_diag_stop_mute_valid != 0U ?
                    static_cast<unsigned>(
                        audio_song_diag_stop_mute_readback) : 0xFFU);
            break;
        case 6U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "PCM H%lu NZ%lu",
                static_cast<unsigned long>(
                    audio_song_diag_pcm_halfwords),
                static_cast<unsigned long>(
                    audio_song_diag_pcm_nonzero_halfwords));
            break;
        case 7U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "PCM MIN%ld MAX%ld",
                static_cast<long>(audio_song_diag_pcm_min),
                static_cast<long>(audio_song_diag_pcm_max));
            break;
        case 8U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "PCM HASH%08lX",
                static_cast<unsigned long>(audio_song_diag_pcm_checksum));
            break;
        case 9U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "P0 %d/%d P1 %d/%d",
                static_cast<int>(audio_song_diag_pcm_pairs[0]),
                static_cast<int>(audio_song_diag_pcm_pairs[1]),
                static_cast<int>(audio_song_diag_pcm_pairs[2]),
                static_cast<int>(audio_song_diag_pcm_pairs[3]));
            break;
        case 10U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "P2 %d/%d P3 %d/%d",
                static_cast<int>(audio_song_diag_pcm_pairs[4]),
                static_cast<int>(audio_song_diag_pcm_pairs[5]),
                static_cast<int>(audio_song_diag_pcm_pairs[6]),
                static_cast<int>(audio_song_diag_pcm_pairs[7]));
            break;
        case 11U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "START %lums N%lu C%lu D%u I%u H%lu",
                static_cast<unsigned long>(
                    audio_song_diag_dma_start.elapsed_ms),
                static_cast<unsigned long>(
                    audio_song_diag_dma_start.ndtr),
                static_cast<unsigned long>(
                    audio_song_diag_dma_start.frame_cursor),
                static_cast<unsigned>(
                    audio_song_diag_dma_start.dma_enabled),
                static_cast<unsigned>(
                    audio_song_diag_dma_start.i2s_enabled),
                static_cast<unsigned long>(
                    audio_song_diag_dma_start.hal_i2s_state));
            break;
        case 12U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "POLL %lums N%lu C%lu D%u I%u H%lu",
                static_cast<unsigned long>(
                    audio_song_diag_dma_first_poll.elapsed_ms),
                static_cast<unsigned long>(
                    audio_song_diag_dma_first_poll.ndtr),
                static_cast<unsigned long>(
                    audio_song_diag_dma_first_poll.frame_cursor),
                static_cast<unsigned>(
                    audio_song_diag_dma_first_poll.dma_enabled),
                static_cast<unsigned>(
                    audio_song_diag_dma_first_poll.i2s_enabled),
                static_cast<unsigned long>(
                    audio_song_diag_dma_first_poll.hal_i2s_state));
            break;
        case 13U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "I2C R%u V%02X %lums",
                static_cast<unsigned>(audio_song_diag_codec_read_result),
                audio_song_diag_codec_read_valid != 0U ?
                    static_cast<unsigned>(
                        audio_song_diag_codec_mute_value) : 0xFFU,
                static_cast<unsigned long>(
                    audio_song_diag_codec_read_elapsed_ms));
            break;
        default:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "I2C HAL%lu ERR%08lX",
                static_cast<unsigned long>(
                    audio_song_diag_codec_read_hal_status),
                static_cast<unsigned long>(
                    audio_song_diag_codec_read_i2c_error));
            break;
        case 15U:
            snprintf(diagnostic_line, sizeof(diagnostic_line),
                "STATE %s", player_playback_status_text());
            break;
        }
        const uint16_t diagnostic_color =
            audio_song_diag_stop_reason ==
                AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED ||
            audio_song_diag_stop_reason ==
                AUDIO_SONG_DIAG_STOP_PREPARE_FAILED ||
            audio_song_diag_stop_reason == AUDIO_SONG_DIAG_STOP_DMA_ERROR ||
            audio_song_diag_stop_reason == AUDIO_SONG_DIAG_STOP_CURSOR_ERROR ||
            audio_song_diag_stop_reason == AUDIO_SONG_DIAG_STOP_START_FAILED ?
                0xF800 : 0xFFE0;
        lcd.drawText(6, y, diagnostic_line, diagnostic_color, 0x0000);
        return;
    }
    if (song_text != nullptr)
    {
        const uint16_t song_color = song_status == AUDIO_SONG_PLAYING ?
            0x07E0 : song_status == AUDIO_SONG_FAILED ? 0xF800 : 0xFFE0;
        lcd.drawText(6, y, song_text, song_color, 0x0000);
        return;
    }
    const uint8_t status = audio_test_status;
    const uint16_t color =
        status == AUDIO_TEST_RUNNING ? 0x07E0 :
        (status == AUDIO_TEST_FAILED_MUTED ||
         status == AUDIO_TEST_MUTE_UNKNOWN) ? 0xF800 : 0xFFE0;
    lcd.drawText(6, y, audio_test_status_text(status), color, 0x0000);
}

void wait_diagnostic_page_with_audio_status(uint32_t duration_ms)
{
    const TickType_t started = xTaskGetTickCount();
    const TickType_t duration = pdMS_TO_TICKS(duration_ms);
    uint8_t last_status = 0xFFU;
    uint8_t last_song_status = 0xFFU;
    while ((xTaskGetTickCount() - started) < duration)
    {
        const uint8_t status = audio_test_status;
        const uint8_t song_status = audio_song_status;
        if (status != last_status || song_status != last_song_status)
        {
            draw_audio_test_status_line(218U);
            last_status = status;
            last_song_status = song_status;
        }
        vTaskDelay(pdMS_TO_TICKS(100U));
    }
}

void prepare_audio_test_buffer()
{
    constexpr float kTwoPi = 6.2831853071795864769f;
    constexpr float kAmplitude = 512.0f;
    for (uint16_t frame = 0; frame < kAudioTestFrames; ++frame)
    {
        const float phase = kTwoPi * 44.0f *
            static_cast<float>(frame) / kAudioTestFrames;
        const int16_t sample = static_cast<int16_t>(
            lroundf(kAmplitude * sinf(phase)));
        audio_test_pcm[frame * 2U] = sample;
        audio_test_pcm[frame * 2U + 1U] = sample;
    }
    audio_test_buffer_ready = 1U;
}

#if !AUDIO_COMPLETE_SONG_PLAYBACK
bool capture_audio_song_pcm_diagnostics()
{
    const size_t phrase_halfwords = kAudioSongPhraseFrames * 2U;
    if (audio_song_frames_written < kAudioSongPhraseFrames ||
        phrase_halfwords > kAudioSongBufferHalfwords)
    {
        return false;
    }

    int16_t minimum = 32767;
    int16_t maximum = -32768;
    uint32_t nonzero_halfwords = 0U;
    uint32_t checksum = 2166136261UL; // FNV-1a over little-endian halfwords
    for (size_t index = 0; index < phrase_halfwords; ++index)
    {
        const int16_t sample = audio_song_pcm[index];
        const uint16_t bits = static_cast<uint16_t>(sample);
        minimum = sample < minimum ? sample : minimum;
        maximum = sample > maximum ? sample : maximum;
        nonzero_halfwords += sample != 0 ? 1U : 0U;
        checksum = (checksum ^ (bits & 0xFFU)) * 16777619UL;
        checksum = (checksum ^ (bits >> 8U)) * 16777619UL;
        if (index < 8U)
        {
            audio_song_diag_pcm_pairs[index] = sample;
        }
    }

    audio_song_diag_pcm_halfwords =
        static_cast<uint32_t>(phrase_halfwords);
    audio_song_diag_pcm_nonzero_halfwords = nonzero_halfwords;
    audio_song_diag_pcm_min = minimum;
    audio_song_diag_pcm_max = maximum;
    audio_song_diag_pcm_checksum = checksum;
    return true;
}

bool prepare_fur_elise_phrase()
{
    if (audio_song_buffer_ready != 0U)
    {
        audio_song_diag_frames_prepared =
            static_cast<uint32_t>(audio_song_frames_written);
        audio_song_diag_prepare_result = 1U;
        if (!capture_audio_song_pcm_diagnostics())
        {
            audio_song_diag_prepare_result = 2U;
            return false;
        }
        for (size_t sample = 0; sample < 4U; ++sample)
        {
            audio_song_diag_pcm_first[sample] = audio_song_pcm[sample];
        }
        return true;
    }
    if (i2s3_tx_dma_handle.Instance != nullptr &&
        (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U)
    {
        audio_song_diag_prepare_result = 2U;
        return false;
    }

    audio_song_status = AUDIO_SONG_PREPARING;
    song_player::Config sequence_config;
    // ASSUMPTIONS, not confirmed lab requirements: note constants are periods
    // in milliseconds; tempo is seconds per beat unit; No is a timed rest;
    // note changes are connected. The first 14 Fur Elise entries are used.
    sequence_config.noteEncoding =
        song_player::NoteEncoding::PeriodMilliseconds;
    sequence_config.zeroNoteBehavior = song_player::ZeroNoteBehavior::Rest;
    sequence_config.transitionMode = song_player::TransitionMode::Connected;
    sequence_config.tempoToMilliseconds = 1000.0;
    const song_player::SongView song = {
        kFurEliseFirstFourteenNotes, kFurEliseFirstFourteenBeats,
        kFurEliseTempoAssumption,
        kAudioSongNoteCount};
    song_player::Sequencer sequencer(song, sequence_config);
    song_pcm::Config pcm_config;
    pcm_config.sampleRateHz = kI2s3AudioFrequencyHz;
    pcm_config.peakAmplitude = 512;
    pcm_config.trailingSilenceMilliseconds = 150U;
    song_pcm::Renderer renderer(
        audio_song_pcm, kAudioSongBufferFrames, pcm_config);

    for (;;)
    {
        song_player::Event event;
        const song_player::Result sequence_result = sequencer.next(event);
        if (sequence_result == song_player::Result::EndOfSong)
        {
            if (renderer.append(event) != song_pcm::Result::EndOfSong)
            {
                audio_song_diag_prepare_result = 2U;
                audio_song_status = AUDIO_SONG_FAILED;
                return false;
            }
            break;
        }
        if (sequence_result != song_player::Result::EventReady ||
            renderer.append(event) != song_pcm::Result::EventRendered)
        {
            audio_song_diag_prepare_result = 2U;
            audio_song_status = AUDIO_SONG_FAILED;
            return false;
        }
        taskYIELD();
    }

    audio_song_frames_written = renderer.framesWritten();
    if (audio_song_frames_written != kAudioSongBufferFrames)
    {
        audio_song_diag_frames_prepared =
            static_cast<uint32_t>(audio_song_frames_written);
        audio_song_diag_prepare_result = 2U;
        audio_song_status = AUDIO_SONG_FAILED;
        return false;
    }
    audio_song_diag_frames_prepared =
        static_cast<uint32_t>(audio_song_frames_written);
    for (size_t sample = 0; sample < 4U; ++sample)
    {
        audio_song_diag_pcm_first[sample] = audio_song_pcm[sample];
    }
    if (!capture_audio_song_pcm_diagnostics())
    {
        audio_song_diag_prepare_result = 2U;
        audio_song_status = AUDIO_SONG_FAILED;
        return false;
    }
    audio_song_buffer_ready = 1U;
    audio_song_diag_prepare_result = 1U;
    return true;
}

#endif

void audio_test_record_failure(uint8_t reg, uint8_t kind,
                               HAL_StatusTypeDef status,
                               uint32_t error)
{
    if (audio_test_failed_register == 0xFFU)
    {
        audio_test_failed_register = reg;
        audio_test_failure_kind = kind;
        if (kind == 3U)
        {
            audio_test_i2s_hal_status = static_cast<uint32_t>(status);
        }
        else
        {
            audio_test_i2c_hal_status = static_cast<uint32_t>(status);
            audio_test_i2c_error = error;
        }
    }
}

bool audio_test_read_register(uint8_t reg, uint8_t &value)
{
    const uint16_t address = static_cast<uint16_t>(
        ES8388_I2C_ADDRESS_7BIT << 1U);
    const HAL_StatusTypeDef status = HAL_I2C_Mem_Read(
        &es8388_i2c_handle, address, reg, I2C_MEMADD_SIZE_8BIT,
        &value, 1U, kEs8388ProbeTimeoutMs);
    if (status != HAL_OK)
    {
        audio_test_record_failure(reg, 1U, status,
            HAL_I2C_GetError(&es8388_i2c_handle));
        return false;
    }
    return true;
}

bool audio_test_write_register(uint8_t reg, uint8_t value)
{
    const uint16_t address = static_cast<uint16_t>(
        ES8388_I2C_ADDRESS_7BIT << 1U);
    const HAL_StatusTypeDef status = HAL_I2C_Mem_Write(
        &es8388_i2c_handle, address, reg, I2C_MEMADD_SIZE_8BIT,
        &value, 1U, kEs8388ProbeTimeoutMs);
    if (status != HAL_OK)
    {
        audio_test_record_failure(reg, 1U, status,
            HAL_I2C_GetError(&es8388_i2c_handle));
        return false;
    }
    return true;
}

bool audio_test_write_verify(uint8_t reg, uint8_t value)
{
    uint8_t readback = 0U;
    if (!audio_test_write_register(reg, value) ||
        !audio_test_read_register(reg, readback))
    {
        return false;
    }
    if (readback != value)
    {
        audio_test_record_failure(reg, 2U, HAL_OK, HAL_I2C_ERROR_NONE);
        return false;
    }
    return true;
}

bool audio_test_set_mute_and_verify(bool muted)
{
    uint8_t value = 0U;
    if (!audio_test_read_register(0x19U, value))
    {
        audio_test_mute_confirmed = 0U;
        return false;
    }
    value = muted ? static_cast<uint8_t>(value | kAudioTestMuteBit) :
                    static_cast<uint8_t>(value & ~kAudioTestMuteBit);
    if (!audio_test_write_register(0x19U, value))
    {
        audio_test_mute_confirmed = 0U;
        return false;
    }
    uint8_t readback = 0U;
    if (!audio_test_read_register(0x19U, readback))
    {
        audio_test_mute_confirmed = 0U;
        if (!muted && audio_song_diag_start_requested != 0U &&
            audio_song_status != AUDIO_SONG_IDLE)
        {
            audio_song_diag_mute_readback_valid = 0U;
        }
        return false;
    }
    if (!muted && audio_song_diag_start_requested != 0U &&
        audio_song_status != AUDIO_SONG_IDLE)
    {
        audio_song_diag_mute_readback = readback;
        audio_song_diag_mute_readback_valid = 1U;
    }
    const bool verified =
        ((readback & kAudioTestMuteBit) != 0U) == muted;
    audio_test_mute_confirmed =
        (readback & kAudioTestMuteBit) != 0U ? 1U : 0U;
    if (!verified)
    {
        audio_test_record_failure(0x19U, 2U, HAL_OK,
                                  HAL_I2C_ERROR_NONE);
    }
    return verified;
}

bool audio_test_restore_idle_attenuation()
{
    bool restored = true;
    restored = audio_test_write_verify(
        0x1AU, kAudioTestIdleDacVolume) && restored;
    restored = audio_test_write_verify(
        0x1BU, kAudioTestIdleDacVolume) && restored;
    restored = audio_test_write_verify(
        0x2EU, kAudioTestIdleHeadphoneVolume) && restored;
    restored = audio_test_write_verify(
        0x2FU, kAudioTestIdleHeadphoneVolume) && restored;
    return restored;
}

bool audio_test_stop_and_restore(bool failed)
{
    audio_test_status = AUDIO_TEST_STOPPING;
    const bool mute_verified = audio_test_set_mute_and_verify(true);
    HAL_StatusTypeDef stop_status = HAL_OK;
    HAL_StatusTypeDef recovery_status = HAL_OK;
    if (audio_test_dma_active != 0U ||
        (i2s3_handle.Instance != nullptr &&
         (i2s3_handle.Instance->I2SCFGR & SPI_I2SCFGR_I2SE) != 0U))
    {
        stop_status = HAL_I2S_DMAStop(&i2s3_handle);
        if (stop_status != HAL_OK ||
            (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U)
        {
            // HAL_I2S_DMAStop() already makes one bounded HAL_DMA_Abort()
            // attempt. Retry through HAL; if its state is no longer BUSY, HAL
            // rejects the abort without force-clearing EN.
            if ((i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U)
            {
                recovery_status = HAL_DMA_Abort(&i2s3_tx_dma_handle);
            }
            else if ((i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) == 0U &&
                     stop_status != HAL_OK)
            {
                // The stream is already verified disabled, so retry the
                // HAL I2S stop sequence to re-check peripheral shutdown.
                recovery_status = HAL_I2S_DMAStop(&i2s3_handle);
            }
        }
        audio_test_dma_active =
            (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U ? 1U : 0U;
    }
    audio_test_dma_hal_status = static_cast<uint32_t>(stop_status);
    audio_test_dma_recovery_status = static_cast<uint32_t>(recovery_status);
    const bool attenuation_restored = audio_test_restore_idle_attenuation();
    const bool dma_still_enabled =
        (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U;
    const bool i2s_still_enabled = i2s3_handle.Instance != nullptr &&
        (i2s3_handle.Instance->I2SCFGR & SPI_I2SCFGR_I2SE) != 0U;
    if (stop_status != HAL_OK || dma_still_enabled || i2s_still_enabled ||
        !attenuation_restored)
    {
        failed = true;
        if ((stop_status != HAL_OK || dma_still_enabled || i2s_still_enabled) &&
            audio_test_failed_register == 0xFFU)
        {
            audio_test_record_failure(0xFFU, 3U,
                stop_status == HAL_OK ? HAL_ERROR : stop_status,
                HAL_I2C_ERROR_NONE);
        }
    }
    if (!mute_verified)
    {
        audio_test_status = AUDIO_TEST_MUTE_UNKNOWN;
    }
    else if (failed)
    {
        audio_test_status = AUDIO_TEST_FAILED_MUTED;
    }
    else
    {
        audio_test_status = AUDIO_TEST_STOPPED_MUTED;
    }
    return audio_test_status == AUDIO_TEST_STOPPED_MUTED;
}

void capture_audio_song_dma_snapshot(
    volatile AudioSongDmaSnapshot &snapshot,
    size_t start_frame,
    uint16_t transfer_halfwords,
    TickType_t start_tick)
{
    const TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
    snapshot.elapsed_ms =
        static_cast<uint32_t>(elapsed_ticks) * 1000U /
        configTICK_RATE_HZ;
    snapshot.valid = 0U;
    snapshot.frame_cursor = UINT32_MAX;
    snapshot.ndtr = UINT32_MAX;
    snapshot.dma_enabled = 0U;
    snapshot.i2s_enabled = 0U;
    snapshot.hal_i2s_state = static_cast<uint32_t>(i2s3_handle.State);

    if (i2s3_tx_dma_handle.Instance == nullptr)
    {
        return;
    }

    const uint32_t remaining_halfwords =
        i2s3_tx_dma_handle.Instance->NDTR;
    snapshot.ndtr = remaining_halfwords;
    snapshot.dma_enabled =
        (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U ? 1U : 0U;
    snapshot.i2s_enabled =
        i2s3_handle.Instance != nullptr &&
        (i2s3_handle.Instance->I2SCFGR & SPI_I2SCFGR_I2SE) != 0U ? 1U : 0U;
    if (remaining_halfwords > transfer_halfwords)
    {
        return;
    }

    const size_t transferred_frames =
        (transfer_halfwords - remaining_halfwords) / 2U;
    const size_t cursor = start_frame + transferred_frames;
    snapshot.frame_cursor = static_cast<uint32_t>(cursor);
    snapshot.valid = cursor <= audio_song_frames_written ? 1U : 0U;
}

void reset_audio_song_dma_snapshot(
    volatile AudioSongDmaSnapshot &snapshot)
{
    snapshot.elapsed_ms = 0U;
    snapshot.ndtr = 0U;
    snapshot.frame_cursor = 0U;
    snapshot.hal_i2s_state = 0U;
    snapshot.dma_enabled = 0U;
    snapshot.i2s_enabled = 0U;
    snapshot.valid = 0U;
}

bool audio_output_start_pcm(const int16_t *pcm, size_t frame_count)
{
    audio_test_failed_register = 0xFFU;
    audio_test_failure_kind = 0U;
    audio_test_i2c_hal_status = HAL_OK;
    audio_test_i2c_error = HAL_I2C_ERROR_NONE;
    audio_test_i2s_hal_status = HAL_OK;
    audio_test_dma_hal_status = HAL_OK;
    audio_test_dma_recovery_status = HAL_OK;
    audio_test_status = AUDIO_TEST_STARTING;

    if (pcm == nullptr || frame_count == 0U ||
        frame_count > static_cast<size_t>(UINT16_MAX / 2U) ||
        es8388_i2c_probe_status != 1U ||
        es8388_codec_init_status != 2U ||
        (es8388_codec_mute_readback & kAudioTestMuteBit) == 0U ||
        i2s3_stage_b_status != 1U || i2s3_stage_b_dma_status != 1U ||
        i2s3_handle.State != HAL_I2S_STATE_READY ||
        i2s3_tx_dma_handle.Instance != DMA1_Stream7 ||
        i2s3_tx_dma_handle.Init.Channel != DMA_CHANNEL_0 ||
        (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U)
    {
        audio_test_record_failure(0xFFU, 4U, HAL_ERROR,
                                  HAL_I2C_ERROR_NONE);
        audio_test_status = audio_test_mute_confirmed != 0U ?
            AUDIO_TEST_FAILED_MUTED : AUDIO_TEST_MUTE_UNKNOWN;
        return false;
    }

    uint8_t mute_readback = 0U;
    if (!audio_test_read_register(0x19U, mute_readback) ||
        (mute_readback & kAudioTestMuteBit) == 0U)
    {
        audio_test_mute_confirmed = 0U;
        if (audio_test_failed_register == 0xFFU)
        {
            audio_test_record_failure(0x19U, 2U, HAL_OK,
                                      HAL_I2C_ERROR_NONE);
        }
        if (audio_test_failure_kind == 2U &&
            audio_test_i2c_hal_status == HAL_OK &&
            audio_test_i2c_error == HAL_I2C_ERROR_NONE)
        {
            // A successful read showing mute clear is corrected and verified
            // before returning. A failed read cannot establish mute state.
            if (audio_test_set_mute_and_verify(true))
            {
                (void)audio_test_restore_idle_attenuation();
                audio_test_status = AUDIO_TEST_FAILED_MUTED;
                return false;
            }
        }
        audio_test_status = AUDIO_TEST_MUTE_UNKNOWN;
        return false;
    }
    audio_test_mute_confirmed = 1U;

    if (!audio_test_write_verify(0x18U, kAudioTestDacClockRatio) ||
        !audio_test_write_verify(0x1AU, kAudioTestDacVolume) ||
        !audio_test_write_verify(0x1BU, kAudioTestDacVolume) ||
        !audio_test_write_verify(0x2EU, kAudioTestHeadphoneVolume) ||
        !audio_test_write_verify(0x2FU, kAudioTestHeadphoneVolume))
    {
        audio_test_stop_and_restore(true);
        return false;
    }

    audio_test_dma_error_seen = 0U;
    audio_test_dma_error_code = 0U;
    const uint16_t halfwords = static_cast<uint16_t>(frame_count * 2U);
    if (audio_song_diag_start_requested != 0U &&
        audio_song_status != AUDIO_SONG_IDLE)
    {
        audio_song_diag_transmit_called = 1U;
        audio_song_diag_dma_start.valid = 0U;
        audio_song_diag_dma_first_poll.valid = 0U;
        audio_song_diag_codec_read_attempted = 0U;
        audio_song_diag_codec_read_result = 0U;
        audio_song_diag_codec_read_valid = 0U;
        audio_song_diag_codec_mute_value = 0xFFU;
        audio_song_diag_codec_read_elapsed_ms = 0U;
        audio_song_diag_codec_read_hal_status = HAL_OK;
        audio_song_diag_codec_read_i2c_error = HAL_I2C_ERROR_NONE;
    }
    const HAL_StatusTypeDef transmit_status = HAL_I2S_Transmit_DMA(
        &i2s3_handle,
        const_cast<uint16_t *>(reinterpret_cast<const uint16_t *>(pcm)),
        halfwords);
    audio_test_dma_hal_status = static_cast<uint32_t>(transmit_status);
    if (audio_song_diag_start_requested != 0U &&
        audio_song_status != AUDIO_SONG_IDLE)
    {
        audio_song_diag_tx_hal_status =
            static_cast<uint32_t>(transmit_status);
        audio_song_diag_dma_enabled_after_start =
            (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U ? 1U : 0U;
        audio_song_diag_i2s_enabled_after_start =
            (i2s3_handle.Instance->I2SCFGR & SPI_I2SCFGR_I2SE) != 0U ? 1U : 0U;
        audio_song_diag_ndtr_after_start =
            i2s3_tx_dma_handle.Instance->NDTR;
        if (transmit_status == HAL_OK)
        {
            audio_song_diag_start_tick = xTaskGetTickCount();
            capture_audio_song_dma_snapshot(
                audio_song_diag_dma_start,
                audio_song_dma_start_frame,
                audio_song_dma_halfwords,
                audio_song_diag_start_tick);
        }
    }
    if (transmit_status != HAL_OK ||
        i2s3_handle.State != HAL_I2S_STATE_BUSY_TX ||
        (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) == 0U ||
        (i2s3_handle.Instance->I2SCFGR & SPI_I2SCFGR_I2SE) == 0U)
    {
        audio_test_record_failure(0xFFU, 3U,
                                  transmit_status == HAL_OK ?
                                      HAL_ERROR : transmit_status,
                                  HAL_I2C_ERROR_NONE);
        audio_test_dma_active =
            (i2s3_tx_dma_handle.Instance->CR & DMA_SxCR_EN) != 0U ? 1U : 0U;
        audio_test_stop_and_restore(true);
        return false;
    }
    audio_test_dma_active = 1U;

    if (!audio_test_set_mute_and_verify(false))
    {
        audio_test_stop_and_restore(true);
        return false;
    }
    audio_test_status = AUDIO_TEST_RUNNING;
    return true;
}

bool audio_test_start()
{
    return audio_test_buffer_ready != 0U &&
        audio_output_start_pcm(audio_test_pcm, kAudioTestFrames);
}

#if !AUDIO_COMPLETE_SONG_PLAYBACK
bool audio_song_start_from_frame(size_t frame)
{
    if (audio_song_buffer_ready == 0U ||
        frame >= kAudioSongPhraseFrames ||
        frame >= audio_song_frames_written)
    {
        return false;
    }

    audio_song_dma_start_frame = frame;
#if AUDIO_SONG_LOOP_TEST_MS > 0U
    // Loop only the phrase. Keep the renderer's trailing silence outside the
    // DMA span so the bounded diagnostic repeats audible content continuously.
    const size_t remaining_frames = kAudioSongPhraseFrames - frame;
#else
    const size_t remaining_frames = audio_song_frames_written - frame;
#endif
    const size_t halfwords = remaining_frames * 2U;
    if (halfwords == 0U || halfwords > UINT16_MAX)
    {
        return false;
    }
    audio_song_dma_halfwords = static_cast<uint16_t>(halfwords);
    const int16_t *const source = &audio_song_pcm[frame * 2U];
    audio_song_diag_dma_source = reinterpret_cast<uintptr_t>(source);
    audio_song_diag_dma_halfwords =
        static_cast<uint32_t>(audio_song_dma_halfwords);
    const bool started = audio_output_start_pcm(source, remaining_frames);
    audio_song_diag_output_start_result = started ? 1U : 2U;
    if (!started && audio_song_diag_stop_reason == AUDIO_SONG_DIAG_STOP_NONE)
    {
        audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_START_FAILED;
    }
    return started;
}

#endif

class UpDownAudioChord
{
public:
    ButtonEvents filter(const ButtonEvents &events,
                        const ButtonLevels &stable,
        TickType_t now_tick,
                        bool &toggle_test)
    {
        ButtonEvents navigation_events = events;
        navigation_events.button1Pressed = false;
        navigation_events.button1Released = false;
        navigation_events.button3Pressed = false;
        navigation_events.button3Released = false;
        toggle_test = false;

        if (candidate_)
        {
            if (!triggered_ && !stable.button1High &&
                !stable.button3High &&
                static_cast<TickType_t>(now_tick - chord_started_tick_) >=
                    kChordHoldMs)
            {
                toggle_test = true;
                triggered_ = true;
            }
            if (stable.button1High && stable.button3High)
            {
                // Once both debounced presses formed a chord, consume them
                // even if the deliberate hold was shorter than the trigger.
                candidate_ = false;
                triggered_ = false;
                up_pending_ = false;
                down_pending_ = false;
            }
            return navigation_events;
        }

        if (events.button1Pressed)
        {
            up_pending_ = true;
            up_pressed_tick_ = now_tick;
        }
        if (events.button3Pressed)
        {
            down_pending_ = true;
            down_pressed_tick_ = now_tick;
        }

        // Unsigned TickType_t subtraction wraps naturally. The shorter of
        // the two modular distances is the elapsed gap for this short window.
        const TickType_t up_to_down =
            static_cast<TickType_t>(up_pressed_tick_ - down_pressed_tick_);
        const TickType_t down_to_up =
            static_cast<TickType_t>(down_pressed_tick_ - up_pressed_tick_);
        const TickType_t difference =
            up_to_down < down_to_up ? up_to_down : down_to_up;
        if (up_pending_ && down_pending_ &&
            !stable.button1High && !stable.button3High &&
            difference <= kChordWindowMs)
        {
            candidate_ = true;
            triggered_ = false;
            chord_started_tick_ = now_tick;
            return navigation_events;
        }

        if (up_pending_ &&
            (events.button1Released ||
             static_cast<TickType_t>(now_tick - up_pressed_tick_) >
                 kChordWindowMs))
        {
            navigation_events.button1Pressed = true;
            up_pending_ = false;
        }
        if (down_pending_ &&
            (events.button3Released ||
             static_cast<TickType_t>(now_tick - down_pressed_tick_) >
                 kChordWindowMs))
        {
            navigation_events.button3Pressed = true;
            down_pending_ = false;
        }
        return navigation_events;
    }

private:
    static constexpr TickType_t kChordWindowMs = 180U;
    static constexpr TickType_t kChordHoldMs = 700U;
    bool up_pending_ = false;
    bool down_pending_ = false;
    bool candidate_ = false;
    bool triggered_ = false;
    TickType_t up_pressed_tick_ = 0U;
    TickType_t down_pressed_tick_ = 0U;
    TickType_t chord_started_tick_ = 0U;
};

void led_only_test()
{
    __HAL_RCC_GPIOF_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {};
    gpio.Pin = LED_BLUE | LED_RED;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOF, &gpio);

    // The official RT-Spark LEDs are common-anode: low is on, high is off.
    for (;;)
    {
        HAL_GPIO_WritePin(GPIOF, LED_BLUE, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_SET);
        HAL_Delay(5000);

        HAL_GPIO_WritePin(GPIOF, LED_BLUE | LED_RED, GPIO_PIN_SET);
        HAL_Delay(5000);

        HAL_GPIO_WritePin(GPIOF, LED_BLUE, GPIO_PIN_SET);
        HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_RESET);
        HAL_Delay(5000);

        HAL_GPIO_WritePin(GPIOF, LED_BLUE | LED_RED, GPIO_PIN_SET);
        HAL_Delay(5000);
    }
}

void update_lcd_leds_thread(void *)
{
    static const char *const song_names[kSongCount] = {
        "FUR ELISE",
        "CANON IN D",
        "MINUET IN G",
        "TURKISH MARCH",
        "NOCT E FLAT",
        "WALTZ NO 2",
        "NOCT C SHARP",
        "SYMPHONY NO 40"};
    uint32_t feedback_visible_until = 0;
    bool feedback_expired = true;

    const auto draw_song_row = [&](uint8_t index)
    {
        const uint16_t x = (index % 2U) == 0 ? 6 : 126;
        const uint16_t y = static_cast<uint16_t>(
            96 + (index / 2U) * 10);
        const bool selected = index == candidate_song_index;
        lcd.fillRect(x, y, 114, 10, 0x0000);
        char line[24] = {};
        line[0] = selected ? '>' : ' ';
        line[1] = static_cast<char>('1' + index);
        line[2] = ' ';
        uint8_t position = 3;
        for (const char *name = song_names[index];
             *name != '\0' && position < sizeof(line) - 1; ++name)
        {
            line[position++] = *name;
        }
        line[position] = '\0';
        lcd.drawText(x, y, line, selected ? 0xFFFF : 0x7BEF, 0x0000);
    };

    const auto draw_selected_track = [&]()
    {
        const uint8_t selected_song = candidate_song_index < kSongCount ?
            candidate_song_index : 0;
        lcd.fillRect(6, 72, 228, 12, 0x0000);
        lcd.drawText(6, 72, "SELECTED:", 0x07E0, 0x0000);
        lcd.drawText(66, 72, song_names[selected_song],
                     0xFFFF, 0x0000);
    };

    const auto draw_status = [&]()
    {
        lcd.fillRect(6, 58, 228, 10, 0x0000);
        lcd.drawText(6, 58, player_playback_status_text(),
                     0xFFE0, 0x0000);
    };

    const auto draw_feedback = [&]()
    {
        const uint32_t now_ms =
            static_cast<uint32_t>(xTaskGetTickCount());
        const uint8_t feedback_code =
            now_ms < feedback_visible_until ? navigation_feedback_for_ui : 0;
        const char *feedback = "";
        if (feedback_code == 1)
        {
            feedback = "SELECTED TRACK";
        }
        else if (feedback_code == 2)
        {
            feedback = "TRACK PLAYING";
        }
        lcd.fillRect(6, 136, 228, 10, 0x0000);
        lcd.drawText(6, 136, feedback, 0xF81F, 0x0000);
    };

    const auto draw_static_screen = [&]()
    {
        lcd.clear(0x0000);
        lcd.drawTextScaled(6, 2, "PERSONAL MP3", 0xFFFF, 0x0000, 2);
        lcd.drawText(6, 20, "PLAYER", 0xFFFF, 0x0000);
        lcd.drawText(6, 30, "NOW PLAYING", 0x07E0, 0x0000);
        const uint8_t playing_song = current_song_index < kSongCount ?
            current_song_index : 0;
        lcd.fillRect(6, 38, 228, 16, 0x0000);
        lcd.drawTextScaled(6, 38, song_names[playing_song],
                           0xFFFF, 0x0000, 2);
        draw_status();
        draw_selected_track();
        lcd.drawText(6, 86, "TRACK LIST", 0x07E0, 0x0000);
        for (uint8_t index = 0; index < kSongCount; ++index)
        {
            draw_song_row(index);
        }
        draw_feedback();
        lcd.drawText(6, 148, "DOWN: MOVE DOWN", 0xFFFF, 0x0000);
        lcd.drawText(6, 160, "LEFT/RIGHT: MOVE ACROSS",
                     0xFFFF, 0x0000);
        lcd.drawText(6, 172, "HOLD RIGHT: PLAY SELECTED",
                     0xFFFF, 0x0000);
        lcd.drawText(6, 184, "UP: PLAY / PAUSE", 0xFFFF, 0x0000);
        draw_audio_test_status_line(218U);
    };

#if RCC_LCD_DIAGNOSTIC_MODE
    const uint32_t initial_registers[7] = {
        rcc_diag_cr, rcc_diag_cfgr, rcc_diag_pllcfgr,
        rcc_diag_plli2scfgr, rcc_diag_ahb1enr,
        rcc_diag_apb1enr, rcc_diag_apb2enr};
    const uint32_t initial_frequencies_hz[4] = {
        rcc_diag_sysclk_hz, rcc_diag_calc_hclk_from_registers_hz,
        rcc_diag_calc_pclk1_from_registers_hz,
        rcc_diag_calc_pclk2_from_registers_hz};
    const uint32_t post_init_registers[7] = {
        rcc_post_init_cr, rcc_post_init_cfgr, rcc_post_init_pllcfgr,
        rcc_post_init_plli2scfgr, rcc_post_init_ahb1enr,
        rcc_post_init_apb1enr, rcc_post_init_apb2enr};
    const uint32_t post_init_frequencies_hz[4] = {
        rcc_post_init_sysclk_hz, rcc_post_init_calc_hclk_hz,
        rcc_post_init_calc_pclk1_hz, rcc_post_init_calc_pclk2_hz};

    static const char *const gpio_register_names[5] = {
        "GPIOF MODER", "GPIOF OTYPER", "GPIOF PUPDR",
        "GPIOF AFR[0]", "GPIOF IDR"};
    const uint32_t gpio_register_values[5] = {
        es8388_i2c_gpio_moder, es8388_i2c_gpio_otyper,
        es8388_i2c_gpio_pupdr, es8388_i2c_gpio_afr1,
        es8388_i2c_gpio_idr};
    static const char *const i2c_register_names[5] = {
        "I2C2 CR1", "I2C2 CR2", "I2C2 CCR", "I2C2 TRISE",
        "RCC APB1ENR"};
    const uint32_t i2c_register_values[5] = {
        es8388_i2c_cr1, es8388_i2c_cr2, es8388_i2c_ccr,
        es8388_i2c_trise, es8388_i2c_apb1enr};

    draw_rcc_lcd_page("RCC AFTER HAL", initial_registers,
                      initial_frequencies_hz);
    wait_diagnostic_page_with_audio_status(10000U);
    draw_rcc_lcd_page("RCC AFTER PLLI2S", post_init_registers,
                      post_init_frequencies_hz,
                      plli2s_clock_diagnostic_status,
                      i2s3_stage_b_status, true);
    wait_diagnostic_page_with_audio_status(10000U);
    draw_es8388_i2c_register_page(
        "I2C2 GPIOF SNAPSHOT", gpio_register_names,
        gpio_register_values, 5U, es8388_i2c_snapshot_pclk1_hz);
    wait_diagnostic_page_with_audio_status(10000U);
    draw_es8388_i2c_register_page(
        "I2C2 PERIPHERAL SNAP", i2c_register_names,
        i2c_register_values, 5U, es8388_i2c_snapshot_pclk1_hz);
    wait_diagnostic_page_with_audio_status(10000U);
#endif

    draw_static_screen();
    uint8_t last_song = current_song_index;
    uint8_t last_candidate = candidate_song_index;
    bool last_playing = song_playing;
    uint8_t last_feedback = navigation_feedback_for_ui;
    uint8_t last_audio_status = audio_test_status;
    uint8_t last_audio_song_status = audio_song_status;
    uint8_t last_song_complete = audio_song_complete_for_ui;
    TickType_t last_audio_diag_page_tick = xTaskGetTickCount();
    for (;;)
    {
        const TickType_t now_tick = xTaskGetTickCount();
        const uint32_t now_ms = static_cast<uint32_t>(now_tick);
        if (last_feedback != navigation_feedback_for_ui)
        {
            feedback_visible_until = now_ms + pdMS_TO_TICKS(3000);
            feedback_expired = false;
        }
        const bool feedback_timeout =
                last_feedback != 0 && !feedback_expired &&
                static_cast<int32_t>(now_ms - feedback_visible_until) >= 0;
        if (last_song != current_song_index ||
            last_candidate != candidate_song_index ||
            last_playing != song_playing ||
            last_song_complete != audio_song_complete_for_ui ||
            last_feedback != navigation_feedback_for_ui ||
            feedback_timeout || last_audio_status != audio_test_status ||
            last_audio_song_status != audio_song_status)
        {
            if (last_song != current_song_index)
            {
                lcd.fillRect(6, 38, 228, 16, 0x0000);
                const uint8_t playing_song = current_song_index < kSongCount ?
                    current_song_index : 0;
                lcd.drawTextScaled(6, 38, song_names[playing_song],
                                   0xFFFF, 0x0000, 2);
            }
            if (last_candidate != candidate_song_index)
            {
                draw_song_row(last_candidate);
                draw_song_row(candidate_song_index);
                draw_selected_track();
            }
            if (last_playing != song_playing ||
                last_song_complete != audio_song_complete_for_ui ||
                last_audio_song_status != audio_song_status)
            {
                draw_status();
            }
            if (last_feedback != navigation_feedback_for_ui ||
                feedback_timeout)
            {
                draw_feedback();
            }
            if (last_audio_status != audio_test_status ||
                last_audio_song_status != audio_song_status)
            {
                draw_audio_test_status_line(218U);
            }
            last_song = current_song_index;
            last_candidate = candidate_song_index;
            last_playing = song_playing;
            last_song_complete = audio_song_complete_for_ui;
            last_feedback = navigation_feedback_for_ui;
            last_audio_status = audio_test_status;
            last_audio_song_status = audio_song_status;
            if (feedback_timeout)
            {
                feedback_expired = true;
            }
        }
        if (audio_song_status != AUDIO_SONG_IDLE &&
            audio_song_diag_start_requested != 0U &&
            static_cast<TickType_t>(now_tick - last_audio_diag_page_tick) >=
                pdMS_TO_TICKS(1000U))
        {
            audio_song_diag_lcd_page =
                static_cast<uint8_t>((audio_song_diag_lcd_page + 1U) % 16U);
            draw_audio_test_status_line(218U);
            last_audio_diag_page_tick = now_tick;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void show_lcd_diagnostic_state()
{
    const uint8_t state =
        static_cast<uint8_t>(lcd_diagnostic_state);
    const uint8_t flash_count =
        (state >= 1 && state <= 5) ? state : 10;

    // The RT-Spark RGB LED is common-anode: low is on, high is off.
    HAL_GPIO_WritePin(GPIOF, LED_BLUE, GPIO_PIN_SET);
    for (;;)
    {
        HAL_GPIO_WritePin(GPIOF, LED_BLUE, GPIO_PIN_SET);
        HAL_Delay(2000);
        for (uint8_t flash = 0; flash < flash_count; ++flash)
        {
            HAL_GPIO_WritePin(GPIOF, LED_BLUE, GPIO_PIN_RESET);
            HAL_Delay(200);
            HAL_GPIO_WritePin(GPIOF, LED_BLUE, GPIO_PIN_SET);
            HAL_Delay(300);
        }
        HAL_Delay(2000);
    }
}

void polling_buttons(void *)
{
    ButtonDebouncer debouncer;
    DirectSongNavigation navigation(
        current_song_index, song_playing);
    UpDownAudioChord audio_chord;

    for (;;)
    {
        const ButtonLevels raw_levels = {
            HAL_GPIO_ReadPin(GPIOC, BTN_UP) == GPIO_PIN_SET,
            HAL_GPIO_ReadPin(GPIOC, BTN_LEFT) == GPIO_PIN_SET,
            HAL_GPIO_ReadPin(GPIOC, BTN_DOWN) == GPIO_PIN_SET,
            HAL_GPIO_ReadPin(GPIOC, BTN_RIGHT) == GPIO_PIN_SET};
        const bool any_pressed =
            !raw_levels.button1High ||
            !raw_levels.button2High ||
            !raw_levels.button3High ||
            !raw_levels.button4High;
        const TickType_t now_tick = xTaskGetTickCount();
        const uint32_t now_ms = static_cast<uint32_t>(now_tick);
        const ButtonEvents events =
            debouncer.update(raw_levels, now_ms);
        const ButtonLevels stable_levels =
            debouncer.stableLevels();
        bool toggle_audio_test = false;
        const ButtonEvents navigation_events = audio_chord.filter(
            events, stable_levels, now_tick, toggle_audio_test);
        if (toggle_audio_test && audio_test_control_task_handle != nullptr)
        {
            // Task context only; no FreeRTOS API is called from the DMA ISR.
            (void)xTaskNotify(audio_test_control_task_handle,
                              kAudioTestNotificationToggle, eSetBits);
        }
        const DirectNavigationChange change = navigation.update(
            navigation_events, stable_levels, now_ms);
        candidate_song_index = navigation.candidateSong();
        if (change.selectionConfirmed)
        {
            current_song_index = candidate_song_index;
            // Long RIGHT makes DirectSongNavigation playing. Publish that
            // state before waking the controller, which uses this global to
            // decide whether to start immediately or wait paused.
            song_playing = navigation.isPlaying();
            audio_song_diag_start_requested = 1U;
            audio_song_diag_lcd_page = 0U;
            audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_NONE;
            navigation_feedback_for_ui = 2;
            audio_song_complete_for_ui = 0U;
            if (audio_test_control_task_handle != nullptr)
            {
                (void)xTaskNotify(audio_test_control_task_handle,
                                  kAudioSongNotificationStart, eSetBits);
            }
        }
        if (change.playPauseChanged)
        {
            song_playing = navigation.isPlaying();
            if (audio_test_control_task_handle != nullptr &&
                (audio_song_status == AUDIO_SONG_PREPARING ||
                 audio_song_status == AUDIO_SONG_PLAYING ||
                 audio_song_status == AUDIO_SONG_PAUSED))
            {
                (void)xTaskNotify(audio_test_control_task_handle,
                                  kAudioSongNotificationPlayPause, eSetBits);
            }
            else if (audio_song_complete_for_ui != 0U)
            {
                audio_song_complete_for_ui = 0U;
            }
        }
        if (change.candidateChanged)
        {
            navigation_feedback_for_ui = 1;
        }

        HAL_GPIO_WritePin(
            GPIOF,
            LED_BLUE,
            any_pressed ?
                GPIO_PIN_SET : GPIO_PIN_RESET);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

#if !AUDIO_COMPLETE_SONG_PLAYBACK
bool audio_song_read_frame_cursor(size_t start_frame,
                                  uint16_t transfer_halfwords,
                                  size_t &frame_cursor)
{
    if (i2s3_tx_dma_handle.Instance == nullptr)
    {
        return false;
    }
    const uint32_t remaining_halfwords =
        i2s3_tx_dma_handle.Instance->NDTR;
    if (remaining_halfwords > transfer_halfwords)
    {
        return false;
    }
    // Floor a possible in-progress stereo pair to the preceding complete
    // frame. DMA offsets and resumes therefore always use two halfwords.
    const size_t transferred_frames =
        (transfer_halfwords - remaining_halfwords) / 2U;
    frame_cursor = start_frame + transferred_frames;
    return frame_cursor <= audio_song_frames_written;
}

#if AUDIO_SONG_LOOP_TEST_MS > 0U
bool audio_song_stop_loop_test(uint32_t elapsed_playback_ms)
{
    capture_audio_song_dma_snapshot(
        audio_song_diag_dma_stop, audio_song_dma_start_frame,
        audio_song_dma_halfwords, audio_song_diag_start_tick);
    audio_song_diag_stop_elapsed_ms = elapsed_playback_ms;
    audio_song_diag_stop_phrase_frames =
        static_cast<uint32_t>(kAudioSongPhraseFrames);
    audio_song_diag_stop_ndtr = audio_song_diag_dma_stop.ndtr;
    audio_song_diag_stop_cursor = audio_song_diag_dma_stop.frame_cursor;
    audio_song_diag_stop_dma_enabled = audio_song_diag_dma_stop.dma_enabled;
    audio_song_diag_stop_i2s_enabled = audio_song_diag_dma_stop.i2s_enabled;
    audio_song_diag_stop_hal_i2s_state =
        audio_song_diag_dma_stop.hal_i2s_state;
    audio_song_diag_stop_mute_valid = audio_song_diag_mute_readback_valid;
    audio_song_diag_stop_mute_readback = audio_song_diag_mute_readback;
    audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_REPEAT_TIMEOUT;
    audio_song_diag_snapshot_reason = AUDIO_SONG_DIAG_STOP_REPEAT_TIMEOUT;
    if (audio_test_stop_and_restore(false))
    {
        audio_song_status = AUDIO_SONG_COMPLETE;
        audio_song_complete_for_ui = 1U;
        return true;
    }
    audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED;
    audio_song_status = AUDIO_SONG_FAILED;
    return false;
}
#endif

void audio_song_control_loop()
{
    audio_song_status = AUDIO_SONG_PREPARING;
    audio_song_complete_for_ui = 0U;
    audio_song_diag_prepare_result = 0U;
    audio_song_diag_output_start_result = 0U;
    audio_song_diag_transmit_called = 0U;
    audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_NONE;
    audio_song_diag_frames_prepared = 0U;
    audio_song_diag_dma_halfwords = 0U;
    audio_song_diag_tx_hal_status = HAL_OK;
    audio_song_diag_dma_enabled_after_start = 0U;
    audio_song_diag_i2s_enabled_after_start = 0U;
    audio_song_diag_mute_readback_valid = 0U;
    audio_song_diag_ndtr_after_start = 0U;
    audio_song_diag_early_ndtr = 0U;
    audio_song_diag_early_cursor = 0U;
    audio_song_diag_early_cursor_valid = 0U;
    audio_song_diag_stop_elapsed_ms = 0U;
    audio_song_diag_stop_phrase_frames = 0U;
    audio_song_diag_stop_ndtr = 0U;
    audio_song_diag_stop_cursor = 0U;
    audio_song_diag_stop_dma_enabled = 0U;
    audio_song_diag_stop_i2s_enabled = 0U;
    audio_song_diag_stop_hal_i2s_state = 0U;
    audio_song_diag_stop_mute_valid = 0U;
    audio_song_diag_stop_mute_readback = 0xFFU;
    audio_song_diag_snapshot_reason = AUDIO_SONG_DIAG_STOP_NONE;
    reset_audio_song_dma_snapshot(audio_song_diag_dma_start);
    reset_audio_song_dma_snapshot(audio_song_diag_dma_first_poll);
    reset_audio_song_dma_snapshot(audio_song_diag_dma_stop);
    audio_song_diag_codec_read_attempted = 0U;
    audio_song_diag_codec_read_result = 0U;
    audio_song_diag_codec_read_valid = 0U;
    audio_song_diag_codec_mute_value = 0xFFU;
    audio_song_diag_codec_read_elapsed_ms = 0U;
    audio_song_diag_codec_read_hal_status = HAL_OK;
    audio_song_diag_codec_read_i2c_error = HAL_I2C_ERROR_NONE;
    if (current_song_index != FUR_ELISE_INDEX)
    {
        audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_UNSUPPORTED;
        audio_song_status = AUDIO_SONG_UNAVAILABLE;
        audio_song_complete_for_ui = 1U;
        return;
    }
    if (!prepare_fur_elise_phrase())
    {
        (void)audio_test_stop_and_restore(true);
        audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_PREPARE_FAILED;
        audio_song_status = AUDIO_SONG_FAILED;
        return;
    }

    size_t resume_frame = 0U;
#if AUDIO_SONG_LOOP_TEST_MS > 0U
    TickType_t active_segment_started_tick = 0U;
    TickType_t accumulated_playback_ticks = 0U;
#endif
    bool paused = !song_playing;
    bool start_requested = song_playing;
    for (;;)
    {
        if (current_song_index != FUR_ELISE_INDEX)
        {
            if (!audio_test_stop_and_restore(false))
            {
                audio_song_diag_stop_reason =
                    AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED;
                audio_song_status = AUDIO_SONG_FAILED;
                return;
            }
            audio_song_complete_for_ui = 1U;
            audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_UNSUPPORTED;
            audio_song_status = AUDIO_SONG_UNAVAILABLE;
            return;
        }

        if (start_requested)
        {
            audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_NONE;
            if (!audio_song_start_from_frame(resume_frame))
            {
                (void)audio_test_stop_and_restore(true);
                audio_song_diag_stop_reason =
                    AUDIO_SONG_DIAG_STOP_START_FAILED;
                audio_song_status = AUDIO_SONG_FAILED;
                return;
            }
            start_requested = false;
            paused = false;
#if AUDIO_SONG_LOOP_TEST_MS > 0U
            // audio_song_diag_start_tick is captured immediately after the
            // successful HAL DMA start. Paused time is excluded from the limit.
            active_segment_started_tick = audio_song_diag_start_tick;
#endif
            audio_song_status = AUDIO_SONG_PLAYING;
        }

#if AUDIO_SONG_LOOP_TEST_MS > 0U
        if (!paused &&
            static_cast<TickType_t>(accumulated_playback_ticks +
                (xTaskGetTickCount() - active_segment_started_tick)) >=
                pdMS_TO_TICKS(AUDIO_SONG_LOOP_TEST_MS))
        {
            const TickType_t elapsed_ticks = static_cast<TickType_t>(
                accumulated_playback_ticks +
                (xTaskGetTickCount() - active_segment_started_tick));
            const uint32_t elapsed_playback_ms = static_cast<uint32_t>(
                (static_cast<uint64_t>(elapsed_ticks) * 1000U) /
                configTICK_RATE_HZ);
            (void)audio_song_stop_loop_test(elapsed_playback_ms);
            uint32_t stale_chord = 0U;
            (void)xTaskNotifyWait(0U, kAudioTestNotificationToggle,
                                  &stale_chord, 0U);
            return;
        }
#endif

        if (paused)
        {
            uint32_t notification = 0U;
            (void)xTaskNotifyWait(0U, UINT32_MAX, &notification,
                                  portMAX_DELAY);
            if ((notification & kAudioTestNotificationToggle) != 0U)
            {
                if (!audio_test_stop_and_restore(false))
                {
                    audio_song_diag_stop_reason =
                        AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED;
                    audio_song_status = AUDIO_SONG_FAILED;
                    return;
                }
                audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_USER;
                audio_song_status = AUDIO_SONG_COMPLETE;
                audio_song_complete_for_ui = 1U;
                return;
            }
            if ((notification & kAudioSongNotificationStart) != 0U)
            {
                if (current_song_index != FUR_ELISE_INDEX)
                {
                    audio_song_diag_stop_reason =
                        AUDIO_SONG_DIAG_STOP_UNSUPPORTED;
                    audio_song_status = AUDIO_SONG_UNAVAILABLE;
                    return;
                }
                resume_frame = 0U;
#if AUDIO_SONG_LOOP_TEST_MS > 0U
                accumulated_playback_ticks = 0U;
#endif
                start_requested = true;
                continue;
            }
            if ((notification & kAudioSongNotificationPlayPause) != 0U &&
                song_playing)
            {
                start_requested = true;
                continue;
            }
            continue;
        }

        if (audio_test_dma_error_seen != 0U)
        {
            (void)audio_test_stop_and_restore(true);
            audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_DMA_ERROR;
            audio_song_status = AUDIO_SONG_FAILED;
            return;
        }

        size_t frame_cursor = 0U;
        if (!audio_song_read_frame_cursor(
                audio_song_dma_start_frame, audio_song_dma_halfwords,
                frame_cursor))
        {
            (void)audio_test_stop_and_restore(true);
            audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_CURSOR_ERROR;
            audio_song_status = AUDIO_SONG_FAILED;
            return;
        }
        if (audio_song_diag_early_cursor_valid == 0U)
        {
            capture_audio_song_dma_snapshot(
                audio_song_diag_dma_first_poll,
                audio_song_dma_start_frame,
                audio_song_dma_halfwords,
                audio_song_diag_start_tick);
            audio_song_diag_early_ndtr =
                audio_song_diag_dma_first_poll.ndtr;
            audio_song_diag_early_cursor =
                audio_song_diag_dma_first_poll.frame_cursor;
            audio_song_diag_early_cursor_valid = 1U;
        }
        if (audio_song_diag_codec_read_attempted == 0U &&
            frame_cursor < kAudioSongPhraseFrames)
        {
            const TickType_t elapsed_ticks =
                xTaskGetTickCount() - audio_song_diag_start_tick;
            const uint32_t elapsed_ms =
                static_cast<uint32_t>(elapsed_ticks) * 1000U /
                configTICK_RATE_HZ;
            if (elapsed_ms >= kAudioSongCodecReadAfterStartMs)
            {
                audio_song_diag_codec_read_attempted = 1U;
                uint8_t mute_value = 0xFFU;
                const uint32_t read_started = HAL_GetTick();
                const bool read_ok =
                    audio_test_read_register(0x19U, mute_value);
                const uint32_t read_elapsed = HAL_GetTick() - read_started;
                audio_song_diag_codec_read_elapsed_ms = read_elapsed;
                audio_song_diag_codec_read_hal_status = read_ok ?
                    HAL_OK : audio_test_i2c_hal_status;
                audio_song_diag_codec_read_i2c_error = read_ok ?
                    HAL_I2C_ERROR_NONE : audio_test_i2c_error;
                audio_song_diag_codec_read_valid = read_ok ? 1U : 0U;
                if (read_ok)
                {
                    audio_song_diag_codec_mute_value = mute_value;
                    audio_song_diag_codec_read_result =
                        read_elapsed > kAudioSongCodecReadSlowThresholdMs ?
                            2U : 1U;
                }
                else
                {
                    audio_song_diag_codec_read_result = 3U;
                }
            }
        }
#if AUDIO_SONG_LOOP_TEST_MS == 0U
        if (frame_cursor >= kAudioSongPhraseFrames)
        {
            // Snapshot the transfer state before the bounded mute/stop path
            // changes DMA, I2S, or codec state.
            capture_audio_song_dma_snapshot(
                audio_song_diag_dma_stop,
                audio_song_dma_start_frame,
                audio_song_dma_halfwords,
                audio_song_diag_start_tick);
            audio_song_diag_stop_elapsed_ms =
                audio_song_diag_dma_stop.elapsed_ms;
            audio_song_diag_stop_phrase_frames =
                static_cast<uint32_t>(kAudioSongPhraseFrames);
            audio_song_diag_stop_ndtr =
                audio_song_diag_dma_stop.ndtr;
            audio_song_diag_stop_cursor =
                audio_song_diag_dma_stop.frame_cursor;
            audio_song_diag_stop_dma_enabled =
                audio_song_diag_dma_stop.dma_enabled;
            audio_song_diag_stop_i2s_enabled =
                audio_song_diag_dma_stop.i2s_enabled;
            audio_song_diag_stop_hal_i2s_state =
                audio_song_diag_dma_stop.hal_i2s_state;
            audio_song_diag_stop_mute_valid =
                audio_song_diag_mute_readback_valid;
            audio_song_diag_stop_mute_readback =
                audio_song_diag_mute_readback;
            audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_COMPLETE;
            audio_song_diag_snapshot_reason =
                AUDIO_SONG_DIAG_STOP_COMPLETE;
            if (audio_test_stop_and_restore(false))
            {
                audio_song_status = AUDIO_SONG_COMPLETE;
                audio_song_complete_for_ui = 1U;
            }
            else
            {
                audio_song_diag_stop_reason =
                    AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED;
                audio_song_status = AUDIO_SONG_FAILED;
            }
            uint32_t stale_chord = 0U;
            (void)xTaskNotifyWait(0U, kAudioTestNotificationToggle,
                                  &stale_chord, 0U);
            return;
        }
#endif

        uint32_t notification = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notification,
                              pdMS_TO_TICKS(2U));
        if ((notification & kAudioTestNotificationToggle) != 0U)
        {
            if (audio_test_stop_and_restore(false))
            {
                audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_USER;
                audio_song_status = AUDIO_SONG_COMPLETE;
                audio_song_complete_for_ui = 1U;
            }
            else
            {
                audio_song_diag_stop_reason =
                    AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED;
                audio_song_status = AUDIO_SONG_FAILED;
            }
            return;
        }
        if ((notification & kAudioSongNotificationStart) != 0U)
        {
            if (!audio_test_stop_and_restore(false))
            {
                audio_song_diag_stop_reason =
                    AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED;
                audio_song_status = AUDIO_SONG_FAILED;
                return;
            }
            if (current_song_index != FUR_ELISE_INDEX)
            {
                audio_song_diag_stop_reason =
                    AUDIO_SONG_DIAG_STOP_UNSUPPORTED;
                audio_song_status = AUDIO_SONG_UNAVAILABLE;
                return;
            }
            resume_frame = 0U;
#if AUDIO_SONG_LOOP_TEST_MS > 0U
            accumulated_playback_ticks = 0U;
#endif
            start_requested = true;
            continue;
        }
        if ((notification & kAudioSongNotificationPlayPause) != 0U &&
            !song_playing)
        {
            const TickType_t pause_requested_tick = xTaskGetTickCount();
            if (!audio_test_stop_and_restore(false))
            {
                audio_song_diag_stop_reason =
                    AUDIO_SONG_DIAG_STOP_SHUTDOWN_FAILED;
                audio_song_status = AUDIO_SONG_FAILED;
                return;
            }
#if AUDIO_SONG_LOOP_TEST_MS > 0U
            accumulated_playback_ticks = static_cast<TickType_t>(
                accumulated_playback_ticks +
                (pause_requested_tick - active_segment_started_tick));
#endif
            if (!audio_song_read_frame_cursor(
                    audio_song_dma_start_frame, audio_song_dma_halfwords,
                    resume_frame))
            {
                audio_song_diag_stop_reason =
                    AUDIO_SONG_DIAG_STOP_CURSOR_ERROR;
                audio_song_status = AUDIO_SONG_FAILED;
                return;
            }
            if (resume_frame >= kAudioSongPhraseFrames)
            {
#if AUDIO_SONG_LOOP_TEST_MS > 0U
                // A circular DMA stream can be observed at NDTR==0 for one
                // instant; resume at this transfer's loop origin.
                resume_frame = audio_song_dma_start_frame;
#else
                audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_COMPLETE;
                audio_song_status = AUDIO_SONG_COMPLETE;
                audio_song_complete_for_ui = 1U;
                return;
#endif
            }
            paused = true;
            audio_song_diag_stop_reason = AUDIO_SONG_DIAG_STOP_PAUSE;
            audio_song_status = AUDIO_SONG_PAUSED;
        }
    }
}

#else
static void set_stream_note(const song_player::Event& event)
{
    taskENTER_CRITICAL();
    if (event.kind == song_player::EventKind::Note)
    {
        const uint64_t frame_count =
            static_cast<uint64_t>(event.durationMilliseconds) *
            kI2s3AudioFrequencyHz / 1000U;
        audio_synth::noteOn(
            static_cast<float>(event.frequencyHz),
            frame_count > UINT32_MAX ? UINT32_MAX :
                                      static_cast<uint32_t>(frame_count));
    }
    else
    {
        audio_synth::noteOff();
    }
    taskEXIT_CRITICAL();
}

// Returns: 0 = event elapsed, 1 = resume current event, 2 = new song,
// 3 = stop request. Pausing silences the synthesizer and resumes by replaying
// the current event from its start, avoiding an arbitrary DMA cursor position.
static uint8_t wait_for_song_event(
    uint32_t durationMs, uint8_t activeSongIndex)
{
    TickType_t remaining = pdMS_TO_TICKS(durationMs);
    if (remaining == 0U) remaining = 1U;
    while (remaining > 0U)
    {
        const TickType_t before = xTaskGetTickCount();
        uint32_t notification = 0U;
        const TickType_t waitTicks = remaining > pdMS_TO_TICKS(10U) ?
            pdMS_TO_TICKS(10U) : remaining;
        const BaseType_t notified = xTaskNotifyWait(
            0U, UINT32_MAX, &notification, waitTicks);
        const TickType_t elapsed = xTaskGetTickCount() - before;
        remaining = elapsed >= remaining ? 0U : remaining - elapsed;
        if (audio_test_dma_error_seen != 0U ||
            (notification & kAudioStreamErrorNotification) != 0U)
            return 4U;
        if (notified != pdTRUE)
        {
            if (remaining == 0U) return 0U;
            continue;
        }
        if ((notification & kAudioTestNotificationToggle) != 0U) return 3U;
        if ((notification & kAudioSongNotificationStart) != 0U)
        {
            if (audio_start_gate::isSongChange(
                    activeSongIndex, current_song_index))
                return 2U;
            // Ignore a duplicate confirmation for the already active song.
            notification &= ~kAudioSongNotificationStart;
        }
        if ((notification & kAudioSongNotificationPlayPause) != 0U &&
            !song_playing)
        {
            taskENTER_CRITICAL();
            audio_synth::noteOff();
            taskEXIT_CRITICAL();
            if (!audio_test_set_mute_and_verify(true)) return 4U;
            audio_song_status = AUDIO_SONG_PAUSED;
            for (;;)
            {
                notification = 0U;
                (void)xTaskNotifyWait(0U, UINT32_MAX, &notification,
                                      portMAX_DELAY);
                if ((notification & kAudioTestNotificationToggle) != 0U)
                    return 3U;
                if (audio_test_dma_error_seen != 0U ||
                    (notification & kAudioStreamErrorNotification) != 0U)
                    return 4U;
                if ((notification & kAudioSongNotificationStart) != 0U)
                {
                    if (audio_start_gate::isSongChange(
                            activeSongIndex, current_song_index))
                        return 2U;
                    notification &= ~kAudioSongNotificationStart;
                }
                if ((notification & kAudioSongNotificationPlayPause) != 0U &&
                    song_playing)
                {
                    audio_song_status = AUDIO_SONG_PLAYING;
                    return 1U;
                }
            }
        }
    }
    return 0U;
}

static bool start_audio_stream()
{
    for (size_t index = 0; index < kAudioDmaFrames * 2U; ++index)
        audio_stream_pcm[index] = 0;
    audio_stream_active = 1U;
    if (!audio_output_start_pcm(audio_stream_pcm, kAudioDmaFrames))
    {
        audio_stream_active = 0U;
        return false;
    }
    return true;
}

static audio_start_gate::Decision wait_for_play_state()
{
    for (;;)
    {
        audio_start_gate::Decision decision = audio_start_gate::decide(
            song_playing, false, audio_test_dma_error_seen != 0U);
        if (decision != audio_start_gate::Decision::WaitForPlay)
            return decision;

        // Keep the task-visible state paused while consuming any number of
        // song-start notifications. Those notifications may update the chosen
        // index, but only the play state is allowed to open the audio path.
        audio_song_status = AUDIO_SONG_PAUSED;
        uint32_t notification = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notification, portMAX_DELAY);
        decision = audio_start_gate::decide(
            song_playing,
            (notification & kAudioTestNotificationToggle) != 0U,
            audio_test_dma_error_seen != 0U ||
                (notification & kAudioStreamErrorNotification) != 0U);
        if (decision != audio_start_gate::Decision::WaitForPlay)
            return decision;
    }
}

void audio_song_control_loop()
{
    bool streamStarted = false;
    bool streamMuted = false;
    for (;;)
    {
        const audio_start_gate::Decision startDecision =
            wait_for_play_state();
        if (startDecision == audio_start_gate::Decision::StopPlayback)
        {
            if (streamStarted)
            {
                audio_stream_active = 0U;
                (void)audio_test_stop_and_restore(false);
            }
            audio_song_status = AUDIO_SONG_COMPLETE;
            audio_song_complete_for_ui = 1U;
            return;
        }
        if (startDecision == audio_start_gate::Decision::PlaybackError)
        {
            if (streamStarted)
            {
                audio_stream_active = 0U;
                (void)audio_test_stop_and_restore(true);
            }
            audio_song_status = AUDIO_SONG_FAILED;
            return;
        }
        if (!streamStarted)
        {
            if (!start_audio_stream())
            {
                audio_song_status = AUDIO_SONG_FAILED;
                return;
            }
            streamStarted = true;
        }

        uint8_t songIndex = current_song_index;
        if (songIndex >= song_selection::kSongCount) songIndex = 0U;
        Song* const selectedSong = song_catalog::get(songIndex);
        if (selectedSong == nullptr)
        {
            audio_song_status = AUDIO_SONG_UNAVAILABLE;
            return;
        }
        Song& song = *selectedSong;
        song_player::SongView view = {
            song.note, song.beat, song.tempo,
            static_cast<size_t>(song.length)};
        song_player::Config config;
        config.noteEncoding = song_player::NoteEncoding::PeriodMilliseconds;
        config.zeroNoteBehavior = song_player::ZeroNoteBehavior::Rest;
        config.transitionMode = song_player::TransitionMode::Connected;
        // Reference interpretation: duration seconds = beat * tempo * 8.
        config.tempoToMilliseconds = 1000.0 * kTempoScale;
        song_player::Sequencer sequencer(view, config);
        audio_song_status = AUDIO_SONG_PLAYING;
        bool restartSong = false;

        for (;;)
        {
            song_player::Event event;
            const song_player::Result result = sequencer.next(event);
            if (result == song_player::Result::EndOfSong)
            {
                taskENTER_CRITICAL();
                audio_synth::noteOff();
                taskEXIT_CRITICAL();
                const uint8_t control = wait_for_song_event(
                    kSongRepeatGapMs, songIndex);
                if (control == 4U)
                {
                    audio_stream_active = 0U;
                    (void)audio_test_stop_and_restore(true);
                    audio_song_status = AUDIO_SONG_FAILED;
                    return;
                }
                if (control == 3U)
                {
                    audio_stream_active = 0U;
                    (void)audio_test_stop_and_restore(false);
                    audio_song_status = AUDIO_SONG_COMPLETE;
                    audio_song_complete_for_ui = 1U;
                    return;
                }
                if (control == 2U)
                {
                    restartSong = true;
                    break;
                }
                // Pause during the repeat gap waits silently, then begins at
                // the first event when playback resumes.
                if (control == 1U)
                {
                    streamMuted = true;
                    const uint8_t resume = wait_for_song_event(
                        kSongRepeatGapMs, songIndex);
                    if (resume == 4U)
                    {
                        audio_stream_active = 0U;
                        (void)audio_test_stop_and_restore(true);
                        audio_song_status = AUDIO_SONG_FAILED;
                        return;
                    }
                    if (resume == 3U)
                    {
                        audio_stream_active = 0U;
                        (void)audio_test_stop_and_restore(false);
                        audio_song_status = AUDIO_SONG_COMPLETE;
                        audio_song_complete_for_ui = 1U;
                        return;
                    }
                    if (resume == 2U) restartSong = true;
                }
                break;
            }
            if (result != song_player::Result::EventReady)
            {
                audio_stream_active = 0U;
                (void)audio_test_stop_and_restore(true);
                audio_song_status = AUDIO_SONG_FAILED;
                return;
            }

            set_stream_note(event);
            if (streamMuted)
            {
                if (!audio_test_set_mute_and_verify(false))
                {
                    audio_stream_active = 0U;
                    (void)audio_test_stop_and_restore(true);
                    audio_song_status = AUDIO_SONG_FAILED;
                    return;
                }
                streamMuted = false;
            }
            uint8_t control = wait_for_song_event(
                event.durationMilliseconds, songIndex);
            if (control == 3U)
            {
                taskENTER_CRITICAL();
                audio_synth::noteOff();
                taskEXIT_CRITICAL();
                audio_stream_active = 0U;
                (void)audio_test_stop_and_restore(false);
                audio_song_status = AUDIO_SONG_COMPLETE;
                audio_song_complete_for_ui = 1U;
                return;
            }
            if (control == 4U)
            {
                audio_stream_active = 0U;
                (void)audio_test_stop_and_restore(true);
                audio_song_status = AUDIO_SONG_FAILED;
                return;
            }
            if (control == 2U)
            {
                taskENTER_CRITICAL();
                audio_synth::noteOff();
                taskEXIT_CRITICAL();
                if (!audio_test_set_mute_and_verify(true))
                {
                    audio_stream_active = 0U;
                    (void)audio_test_stop_and_restore(true);
                    audio_song_status = AUDIO_SONG_FAILED;
                    return;
                }
                streamMuted = true;
                vTaskDelay(pdMS_TO_TICKS(10U));
                restartSong = true;
                break;
            }
            while (control == 1U)
            {
                streamMuted = true;
                set_stream_note(event);
                if (!audio_test_set_mute_and_verify(false))
                {
                    audio_stream_active = 0U;
                    (void)audio_test_stop_and_restore(true);
                    audio_song_status = AUDIO_SONG_FAILED;
                    return;
                }
                streamMuted = false;
                control = wait_for_song_event(
                    event.durationMilliseconds, songIndex);
                if (control == 2U) restartSong = true;
                if (control == 4U)
                {
                    audio_stream_active = 0U;
                    (void)audio_test_stop_and_restore(true);
                    audio_song_status = AUDIO_SONG_FAILED;
                    return;
                }
                if (control == 3U)
                {
                    taskENTER_CRITICAL();
                    audio_synth::noteOff();
                    taskEXIT_CRITICAL();
                    audio_stream_active = 0U;
                    (void)audio_test_stop_and_restore(false);
                    audio_song_status = AUDIO_SONG_COMPLETE;
                    audio_song_complete_for_ui = 1U;
                    return;
                }
                if (restartSong) break;
            }
            if (restartSong) break;
        }
        if (restartSong) continue;
        // End-of-song gap elapsed; construct a fresh sequencer at event zero.
    }
}
#endif

void adjust_volume(void *)
{
    uint8_t pulse_count = 0;
    if (es8388_i2c_probe_status == 1)
    {
        pulse_count = 1;
    }
    else if (es8388_i2c_probe_status == 2)
    {
        pulse_count = 3;
    }
    else if (es8388_i2c_probe_status == 3 ||
             es8388_i2c_probe_status == 4)
    {
        pulse_count = 4;
    }

    if (pulse_count > 0)
    {
        // GPIOF pin 12 is the existing active-low onboard red LED. Save and
        // restore its normal output level after this one-time startup report.
        const GPIO_PinState normal_red_level =
            HAL_GPIO_ReadPin(GPIOF, LED_RED);
        HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_SET);
        vTaskDelay(pdMS_TO_TICKS(150));

        for (uint8_t pulse = 0; pulse < pulse_count; ++pulse)
        {
            HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_RESET);
            vTaskDelay(pdMS_TO_TICKS(180));
            HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_SET);
            if (pulse + 1U < pulse_count)
            {
                vTaskDelay(pdMS_TO_TICKS(220));
            }
        }

        HAL_GPIO_WritePin(GPIOF, LED_RED, normal_red_level);
    }

    for (;;)
    {
        uint32_t notification = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notification, portMAX_DELAY);
        if ((notification & kAudioSongNotificationStart) != 0U)
        {
            audio_song_control_loop();
            continue;
        }
        if ((notification & kAudioTestNotificationToggle) == 0U)
        {
            continue;
        }

        audio_song_status = AUDIO_SONG_IDLE;
        audio_song_complete_for_ui = 0U;

        if (audio_test_status != AUDIO_TEST_RUNNING)
        {
            if (!audio_test_start())
            {
                continue;
            }

            const TickType_t started = xTaskGetTickCount();
            const TickType_t maximum_duration =
                pdMS_TO_TICKS(kAudioTestDurationMs);
            bool dma_failed = false;
            bool start_song_after_test = false;
            while ((xTaskGetTickCount() - started) < maximum_duration)
            {
                if (audio_test_dma_error_seen != 0U)
                {
                    dma_failed = true;
                    break;
                }
                const TickType_t elapsed = xTaskGetTickCount() - started;
                const TickType_t remaining = maximum_duration - elapsed;
                const TickType_t wait_ticks = remaining >
                    pdMS_TO_TICKS(20U) ? pdMS_TO_TICKS(20U) : remaining;
                uint32_t stop_notification = 0U;
                const BaseType_t notified = xTaskNotifyWait(
                        0U,
                        kAudioTestNotificationToggle |
                            kAudioSongNotificationStart,
                        &stop_notification,
                        wait_ticks);
                if ((stop_notification & kAudioSongNotificationStart) != 0U)
                {
                    // A song confirmation is deferred until the sine test's
                    // original five-second window finishes.
                    start_song_after_test = true;
                }
                if (notified == pdTRUE &&
                    (stop_notification &
                     kAudioTestNotificationToggle) != 0U)
                {
                    break;
                }
            }
            const bool stopped_safely =
                audio_test_stop_and_restore(dma_failed);
            // A chord delivered while timeout cleanup was running is stale
            // stop control. Consume it before returning to the idle wait.
            uint32_t stale_notification = 0U;
            (void)xTaskNotifyWait(0U, kAudioTestNotificationToggle,
                                  &stale_notification, 0U);
            if (start_song_after_test && stopped_safely)
            {
                uint32_t start_notification = 0U;
                (void)xTaskNotifyWait(0U, kAudioSongNotificationStart,
                                      &start_notification, 0U);
                audio_song_control_loop();
            }
        }
        else
        {
            (void)audio_test_stop_and_restore(false);
            // Discard any duplicate/coalesced stop event generated before the
            // controller reaches idle. A later fresh chord remains actionable.
            uint32_t stale_notification = 0U;
            (void)xTaskNotifyWait(0U, kAudioTestNotificationToggle,
                                  &stale_notification, 0U);
        }
    }
}

void led_matrix_thread(void *)
{
    if (!led_matrix_init())
    {
        vTaskDelete(nullptr);
        return;
    }

#if LED_MATRIX_DIAGNOSTIC_MODE
    const LedMatrixRgb diagnostic_colors[] = {
        {24, 0, 0},
        {0, 24, 0},
        {0, 0, 24},
        {0, 0, 0}};
    for (;;)
    {
        for (const LedMatrixRgb color : diagnostic_colors)
        {
            if (led_matrix_fill(color))
            {
                led_matrix_transmit();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
#else
    const LedMatrixRgb startup_colors[] = {
        {24, 0, 0},
        {0, 24, 0},
        {0, 0, 24},
        {18, 18, 18}};
    for (const LedMatrixRgb color : startup_colors)
    {
        if (led_matrix_fill(color))
        {
            led_matrix_transmit();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    char text[40];
    snprintf(text, sizeof(text), "TIM3 %lu MHZ P%lu Z%lu O%lu",
             (unsigned long)(led_matrix_timer_clock_hz / 1000000UL),
             (unsigned long)led_matrix_period_ticks,
             (unsigned long)led_matrix_zero_duty,
             (unsigned long)led_matrix_one_duty);
    lcd.drawText(6, 200, text, 0xFFFF, 0x0000);

    uint16_t phase = 0;
    uint8_t last_candidate = candidate_song_index;
    uint8_t last_song = current_song_index;
    uint8_t candidate_effect_frames = 0;
    uint8_t confirmation_effect_frames = 0;
    uint8_t paused_frame_divider = 0;
    LedMatrixRgb frame[kLedMatrixCount] = {};
    for (;;)
    {
        const uint8_t candidate = candidate_song_index;
        const uint8_t song = current_song_index;
        const bool playing = player_audio_is_playing();
        if (candidate != last_candidate)
        {
            candidate_effect_frames = 8;
            last_candidate = candidate;
        }
        if (song != last_song)
        {
            confirmation_effect_frames = 14;
            last_song = song;
        }

        LedMatrixEffectMode mode = playing ?
            LedMatrixEffectMode::Playing : LedMatrixEffectMode::Paused;
        if (confirmation_effect_frames > 0)
        {
            mode = LedMatrixEffectMode::Confirmed;
        }
        else if (candidate_effect_frames > 0)
        {
            mode = LedMatrixEffectMode::Candidate;
        }

        if (mode == LedMatrixEffectMode::Candidate)
        {
            phase = static_cast<uint16_t>(
                static_cast<uint16_t>(candidate) * kLedMatrixCount +
                candidate_effect_frames * 2U);
        }
        for (uint8_t index = 0; index < kLedMatrixCount; ++index)
        {
            const MatrixEffectRgb color =
                led_matrix_color_for(mode, index, phase);
            frame[index] = {color.r, color.g, color.b};
        }
        ++led_matrix_effect_frame_count;
        if (led_matrix_submit_frame(frame, kLedMatrixCount))
        {
            led_matrix_transmit();
        }

        if (confirmation_effect_frames > 0)
        {
            --confirmation_effect_frames;
        }
        else if (candidate_effect_frames > 0)
        {
            --candidate_effect_frames;
        }
        if (mode == LedMatrixEffectMode::Paused)
        {
            ++paused_frame_divider;
            if (paused_frame_divider >= 10)
            {
                paused_frame_divider = 0;
                phase = static_cast<uint16_t>(phase + 1U);
            }
        }
        else
        {
            phase = static_cast<uint16_t>(phase + 3U);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
}

int main()
{
    HAL_Init();
    capture_rcc_clock_diagnostics();
#if LED_ONLY_TEST
    led_only_test();
#else
    hardware_init();
    boot_diag_stage(1);

    // HAL LCD delays must run before the first FreeRTOS API call: in this
    // port, uxCriticalNesting starts at 0xaaaaaaaa and can leave BASEPRI
    // raised before the scheduler initializes it.
    boot_diag_stage(3);
    const bool lcd_initialized = lcd.initialize();
    boot_diag_stage(7);
    if (!lcd_initialized)
    {
#if LCD_DIAGNOSTIC_MODE
        show_lcd_diagnostic_state();
#else
        HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_SET);
        HAL_GPIO_WritePin(GPIOF, LED_BLUE, GPIO_PIN_RESET);
        while (1)
        {
            __set_BASEPRI(0);
            HAL_Delay(200);
        }
#endif
    }

    lcd_mutex = xSemaphoreCreateMutex();
    __set_BASEPRI(0);
    boot_diag_stage(2);
    if (lcd_mutex == nullptr)
    {
        // The ID diagnostic did not run: state remains NotRun (0), reported
        // as ten blue flashes. Keep red steadily on for this failure.
        HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_RESET);
        __set_BASEPRI(0);
        show_lcd_diagnostic_state();
    }
    lcd.attachMutex(lcd_mutex);

    // Probe once after LCD setup and before FreeRTOS task creation. A missing
    // codec leaves the normal LCD, button, and matrix startup path running.
    es8388_i2c_probe();
#if !ES8388_PIN_DIAGNOSTIC_ONLY
    if (es8388_i2c_probe_status == 1U)
    {
        es8388_codec_initialize_muted();
    }
#endif
    configure_plli2s_clock_diagnostic();
    configure_i2s3_stage_b();
    if (i2s3_stage_b_status == 1U && i2s3_stage_b_dma_status == 1U)
    {
        audio_synth::initialize(kI2s3AudioFrequencyHz);
    }
    capture_post_init_rcc_clock_diagnostics();
    prepare_audio_test_buffer();
    audio_test_mute_confirmed =
        es8388_codec_init_status == 2U &&
        (es8388_codec_mute_readback & kAudioTestMuteBit) != 0U ? 1U : 0U;
    audio_test_status =
        audio_test_buffer_ready != 0U &&
        audio_test_mute_confirmed != 0U &&
        i2s3_stage_b_status == 1U &&
        i2s3_stage_b_dma_status == 1U ?
            AUDIO_TEST_READY_MUTED : AUDIO_TEST_NOT_READY;

#if LCD_DIAGNOSTIC_MODE
    // Capture the ID first, then make the existing persistent red-fill
    // attempt before taking over the red LED with the diagnostic pattern.
    HAL_GPIO_WritePin(GPIOF, LED_RED, GPIO_PIN_SET);
    boot_diag_stage(8);
    lcd.showTestScreen();
    __set_BASEPRI(0);
    boot_diag_stage(9);
    boot_diag_stage(10);
    show_lcd_diagnostic_state();
#else
    BaseType_t red_task = xTaskCreate(
        update_lcd_leds_thread, "lcd_leds",
        configMINIMAL_STACK_SIZE + 128,
        nullptr, 2, nullptr);

    BaseType_t button_task = xTaskCreate(
        polling_buttons, "buttons",
        configMINIMAL_STACK_SIZE + 128,
        nullptr, 3, nullptr);

    BaseType_t volume_task = xTaskCreate(
        adjust_volume, "volume",
        configMINIMAL_STACK_SIZE + 128,
        nullptr, 1, &audio_test_control_task_handle);

    BaseType_t matrix_task = xTaskCreate(
        led_matrix_thread, "matrix",
        configMINIMAL_STACK_SIZE + 128,
        nullptr, 1, nullptr);

    if (red_task != pdPASS ||
        button_task != pdPASS ||
        volume_task != pdPASS ||
        matrix_task != pdPASS)
    {
        while (1)
        {
            __set_BASEPRI(0);
            HAL_GPIO_TogglePin(GPIOF, LED_RED);
            HAL_Delay(200);
        }
    }

    vTaskStartScheduler();
#endif

    while (1) {}
#endif
}
