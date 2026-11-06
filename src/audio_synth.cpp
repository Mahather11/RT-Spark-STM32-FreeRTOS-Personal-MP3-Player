#include "audio_synth.h"

#include <cmath>

namespace audio_synth
{
namespace
{
constexpr uint32_t kWaveBits = 10U;
constexpr uint32_t kWaveSize = 1U << kWaveBits;
constexpr int32_t kEnvelopeFullScale = 32768;
constexpr int16_t kPeakAmplitude = 512;
constexpr float kAttackSeconds = 0.003f;
constexpr float kReleaseSeconds = 0.012f;
constexpr float kNoteGate = 0.90f;
constexpr float kTwoPi = 6.28318530718f;

int16_t waveTable[kWaveSize] = {};
uint32_t sampleRateHz = 0U;
uint32_t phase = 0U;
uint32_t phaseIncrement = 0U;
uint32_t gateFrames = 0U;
int32_t envelope = 0;
int32_t attackStep = 0;
int32_t releaseStep = 0;
int32_t targetEnvelope = 0;

float waveShape(uint32_t index)
{
    const float angle = kTwoPi * static_cast<float>(index) /
                        static_cast<float>(kWaveSize);
    return std::sin(angle) + 0.35f * std::sin(2.0f * angle) +
           0.15f * std::sin(3.0f * angle);
}
} // namespace

void initialize(uint32_t rate)
{
    sampleRateHz = rate;
    float peak = 0.0f;
    for (uint32_t index = 0; index < kWaveSize; ++index)
    {
        const float magnitude = std::fabs(waveShape(index));
        peak = magnitude > peak ? magnitude : peak;
    }
    for (uint32_t index = 0; index < kWaveSize; ++index)
    {
        const float normalized = peak > 0.0f ? waveShape(index) / peak : 0.0f;
        waveTable[index] = static_cast<int16_t>(
            normalized * static_cast<float>(kPeakAmplitude));
    }
    phase = 0U;
    phaseIncrement = 0U;
    gateFrames = 0U;
    envelope = 0;
    targetEnvelope = 0;
    attackStep = rate == 0U ? 0 : static_cast<int32_t>(
        static_cast<float>(kEnvelopeFullScale) /
        (kAttackSeconds * static_cast<float>(rate))) + 1;
    releaseStep = rate == 0U ? 0 : static_cast<int32_t>(
        static_cast<float>(kEnvelopeFullScale) /
        (kReleaseSeconds * static_cast<float>(rate))) + 1;
}

void noteOn(float frequencyHz, uint32_t durationFrames)
{
    if (sampleRateHz == 0U || !std::isfinite(frequencyHz) ||
        frequencyHz <= 0.0f || frequencyHz >= sampleRateHz * 0.5f ||
        durationFrames == 0U)
    {
        noteOff();
        return;
    }
    phaseIncrement = static_cast<uint32_t>(
        static_cast<double>(frequencyHz) * 4294967296.0 /
        static_cast<double>(sampleRateHz));
    gateFrames = static_cast<uint32_t>(durationFrames * kNoteGate);
    if (gateFrames == 0U)
    {
        gateFrames = 1U;
    }
    targetEnvelope = kEnvelopeFullScale;
}

void noteOff()
{
    gateFrames = 0U;
    targetEnvelope = 0;
}

void render(int16_t* stereo, uint32_t frames)
{
    if (stereo == nullptr)
    {
        return;
    }
    for (uint32_t index = 0; index < frames; ++index)
    {
        if (gateFrames > 0U && --gateFrames == 0U)
        {
            targetEnvelope = 0;
        }
        if (envelope < targetEnvelope)
        {
            envelope += attackStep;
            if (envelope > targetEnvelope) envelope = targetEnvelope;
        }
        else if (envelope > targetEnvelope)
        {
            envelope -= releaseStep;
            if (envelope < targetEnvelope) envelope = targetEnvelope;
        }
        const int32_t sample = static_cast<int32_t>(
            (static_cast<int64_t>(waveTable[phase >> (32U - kWaveBits)]) *
             envelope) >> 15U);
        phase += phaseIncrement;
        stereo[index * 2U] = static_cast<int16_t>(sample);
        stereo[index * 2U + 1U] = static_cast<int16_t>(sample);
    }
}

} // namespace audio_synth
