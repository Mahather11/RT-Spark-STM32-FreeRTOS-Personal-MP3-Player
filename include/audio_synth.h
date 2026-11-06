#ifndef AUDIO_SYNTH_H
#define AUDIO_SYNTH_H

#include <stdint.h>

namespace audio_synth
{

// Initialize the small wavetable and envelope for the active I2S rate.
void initialize(uint32_t sampleRateHz);

// Set the note for durationFrames. Calls that change synthesizer state must
// be serialized against render() by the hardware integration (DMA IRQ mask).
void noteOn(float frequencyHz, uint32_t durationFrames);
void noteOff();

// Write interleaved 16-bit stereo frames. Safe to call from the DMA IRQ.
void render(int16_t* stereo, uint32_t frames);

} // namespace audio_synth

#endif
