#ifndef AUDIO_START_GATE_H
#define AUDIO_START_GATE_H

#include <stdint.h>

namespace audio_start_gate
{

enum class Decision
{
    WaitForPlay,
    StartPlayback,
    StopPlayback,
    PlaybackError
};

// Pending/repeated start notifications cannot bypass the authoritative play
// state. The caller re-evaluates this decision after each task notification.
inline Decision decide(bool playing, bool stopRequested, bool dmaError)
{
    if (dmaError) return Decision::PlaybackError;
    if (stopRequested) return Decision::StopPlayback;
    return playing ? Decision::StartPlayback : Decision::WaitForPlay;
}

inline bool isSongChange(uint8_t activeSong, uint8_t requestedSong)
{
    return activeSong != requestedSong;
}

} // namespace audio_start_gate

#endif
