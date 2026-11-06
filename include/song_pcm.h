#ifndef SONG_PCM_H
#define SONG_PCM_H

#include <stddef.h>
#include <stdint.h>

#include "song_player.h"

namespace song_pcm
{

enum class Result
{
    EventRendered,
    EndOfSong,
    InvalidConfiguration,
    InvalidEvent,
    CapacityExceeded
};

struct Config
{
    uint32_t sampleRateHz;
    int16_t peakAmplitude;
    uint32_t trailingSilenceMilliseconds;

    Config();
};

// output is interleaved stereo. Capacity and reported positions use frames,
// so callers can never supply a half-stereo-frame offset accidentally.
class Renderer
{
public:
    Renderer(int16_t* output, size_t capacityFrames,
             const Config& config = Config());

    Result append(const song_player::Event& event);
    size_t framesWritten() const;

private:
    bool appendSilence(size_t frames);

    int16_t* output_;
    size_t capacityFrames_;
    size_t framesWritten_;
    Config config_;
    double phaseRadians_;
    bool ended_;
    Result failure_;
};

} // namespace song_pcm

#endif
