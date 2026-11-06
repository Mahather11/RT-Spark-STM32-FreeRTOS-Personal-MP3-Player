#ifndef SONG_PLAYER_H
#define SONG_PLAYER_H

#include <stddef.h>
#include <stdint.h>

namespace song_player
{

// The song files do not document the units. These options make the caller's
// interpretation explicit rather than treating it as a laboratory fact.
enum class NoteEncoding
{
    FrequencyHz,
    PeriodMilliseconds
};

enum class ZeroNoteBehavior
{
    Rest,
    Invalid
};

enum class TransitionMode
{
    Connected,
    SilenceGap
};

enum class EventKind
{
    Note,
    Rest,
    TransitionSilence,
    End
};

enum class Result
{
    EventReady,
    EndOfSong,
    InvalidSong,
    InvalidConfig,
    InvalidValue,
    DurationOverflow
};

struct SongView
{
    const float* notes;
    const float* beats;
    float tempo;
    size_t length;
};

struct Config
{
    NoteEncoding noteEncoding;
    ZeroNoteBehavior zeroNoteBehavior;
    TransitionMode transitionMode;
    // ASSUMED DEFAULT: tempo is seconds per unit beat, hence 1000 ms/unit.
    // Change this factor if the source format defines another convention.
    double tempoToMilliseconds;
    // Used only in SilenceGap mode. Zero means no additional gap.
    uint32_t transitionGapMilliseconds;
    // Zero disables the corresponding frequency bound.
    double minimumFrequencyHz;
    double maximumFrequencyHz;

    Config();
};

struct Event
{
    EventKind kind;
    size_t noteIndex;
    double frequencyHz; // zero for rests, transition silence, and end
    uint32_t durationMilliseconds;
};

class Sequencer
{
public:
    Sequencer(const SongView& song, const Config& config = Config());

    Result next(Event& event);
    void reset();
    size_t position() const;

private:
    SongView song_;
    Config config_;
    size_t position_;
    bool transitionPending_;
    size_t transitionAfterIndex_;
    bool ended_;
    Result failure_;
};

} // namespace song_player

#endif
