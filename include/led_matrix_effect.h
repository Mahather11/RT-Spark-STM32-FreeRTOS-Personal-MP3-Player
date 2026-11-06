#ifndef LED_MATRIX_EFFECT_H
#define LED_MATRIX_EFFECT_H

#include <stdint.h>

struct MatrixEffectRgb
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

constexpr uint8_t kMatrixEffectLedCount = 19;

enum class LedMatrixEffectMode : uint8_t
{
    Playing,
    Paused,
    Candidate,
    Confirmed
};

constexpr uint8_t kMatrixPlayingMinimum = 8;
constexpr uint8_t kMatrixPlayingMaximum = 40;
constexpr uint8_t kMatrixPausedMaximum = 8;

inline uint8_t led_matrix_triangle(uint16_t phase, uint8_t amplitude)
{
    const uint16_t position = phase % 32U;
    const uint16_t distance = position <= 16U ?
        position : 32U - position;
    return static_cast<uint8_t>(
        (distance * amplitude) / 16U);
}

inline uint8_t led_matrix_pulse(uint16_t phase)
{
    return led_matrix_triangle(phase, 20);
}

inline MatrixEffectRgb led_matrix_color_for(
    LedMatrixEffectMode mode, uint8_t index, uint16_t phase)
{
    if (mode == LedMatrixEffectMode::Paused)
    {
        const uint8_t cycle = static_cast<uint8_t>((phase / 32U) % 4U);
        return {
            static_cast<uint8_t>(1U + (cycle & 1U)),
            static_cast<uint8_t>(cycle == 2U ? 1U : 0U),
            static_cast<uint8_t>(6U + (cycle == 1U || cycle == 2U ? 2U : 1U))};
    }

    const uint8_t hue = static_cast<uint8_t>(
        (static_cast<uint16_t>(index) * 256U / kMatrixEffectLedCount +
         phase) & 0xFFU);
    const uint8_t segment = static_cast<uint8_t>(hue / 43U);
    const uint8_t within = static_cast<uint8_t>(hue % 43U);
    const uint8_t rising = static_cast<uint8_t>(within * 32U / 43U);
    const uint8_t falling = static_cast<uint8_t>(32U - rising);
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    switch (segment)
    {
    case 0:
        red = 32;
        green = rising;
        blue = 0;
        break;
    case 1:
        red = falling;
        green = 32;
        blue = 0;
        break;
    case 2:
        red = 0;
        green = 32;
        blue = rising;
        break;
    case 3:
        red = 0;
        green = falling;
        blue = 32;
        break;
    case 4:
        red = rising;
        green = 0;
        blue = 32;
        break;
    default:
        red = 32;
        green = 0;
        blue = falling;
        break;
    }

    if (mode == LedMatrixEffectMode::Candidate)
    {
        return {
            static_cast<uint8_t>(4U + (index == (phase % kMatrixEffectLedCount) ?
                18U : red / 4U)),
            static_cast<uint8_t>(2U + green / 5U),
            static_cast<uint8_t>(4U + blue / 4U)};
    }

    if (mode == LedMatrixEffectMode::Confirmed)
    {
        const uint8_t pulse = led_matrix_pulse(phase);
        return {
            static_cast<uint8_t>(red / 2U + pulse),
            static_cast<uint8_t>(green / 2U + pulse),
            static_cast<uint8_t>(blue / 2U + pulse)};
    }

    const uint8_t pulse = led_matrix_pulse(phase);
    const uint8_t brightness = static_cast<uint8_t>(
        kMatrixPlayingMinimum + pulse);
    return {
        static_cast<uint8_t>((red * brightness) / 32U),
        static_cast<uint8_t>((green * brightness) / 32U),
        static_cast<uint8_t>((blue * brightness) / 32U)};
}

#endif
