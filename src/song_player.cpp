#include "song_player.h"

#include <cmath>
#include <limits>

namespace song_player
{

Config::Config()
    : noteEncoding(NoteEncoding::PeriodMilliseconds),
      zeroNoteBehavior(ZeroNoteBehavior::Rest),
      transitionMode(TransitionMode::Connected),
      tempoToMilliseconds(1000.0),
      transitionGapMilliseconds(0),
      minimumFrequencyHz(0.0),
      maximumFrequencyHz(0.0)
{
}

Sequencer::Sequencer(const SongView& song, const Config& config)
    : song_(song),
      config_(config),
      position_(0),
      transitionPending_(false),
      transitionAfterIndex_(0),
      ended_(false),
      failure_(Result::EventReady)
{
}

Result Sequencer::next(Event& event)
{
    if (failure_ != Result::EventReady)
    {
        return failure_;
    }

    if (config_.noteEncoding != NoteEncoding::FrequencyHz &&
        config_.noteEncoding != NoteEncoding::PeriodMilliseconds)
    {
        failure_ = Result::InvalidConfig;
        return failure_;
    }
    if (config_.zeroNoteBehavior != ZeroNoteBehavior::Rest &&
        config_.zeroNoteBehavior != ZeroNoteBehavior::Invalid)
    {
        failure_ = Result::InvalidConfig;
        return failure_;
    }
    if (config_.transitionMode != TransitionMode::Connected &&
        config_.transitionMode != TransitionMode::SilenceGap)
    {
        failure_ = Result::InvalidConfig;
        return failure_;
    }
    if (!std::isfinite(config_.tempoToMilliseconds) ||
        config_.tempoToMilliseconds <= 0.0 ||
        !std::isfinite(config_.minimumFrequencyHz) ||
        !std::isfinite(config_.maximumFrequencyHz) ||
        config_.minimumFrequencyHz < 0.0 ||
        config_.maximumFrequencyHz < 0.0 ||
        (config_.minimumFrequencyHz > 0.0 &&
         config_.maximumFrequencyHz > 0.0 &&
         config_.minimumFrequencyHz > config_.maximumFrequencyHz))
    {
        failure_ = Result::InvalidConfig;
        return failure_;
    }
    if (song_.length > 0 && (song_.notes == NULL || song_.beats == NULL))
    {
        failure_ = Result::InvalidSong;
        return failure_;
    }
    if (!std::isfinite(song_.tempo) || song_.tempo <= 0.0f)
    {
        failure_ = Result::InvalidSong;
        return failure_;
    }

    if (transitionPending_)
    {
        transitionPending_ = false;
        event = {EventKind::TransitionSilence, transitionAfterIndex_, 0.0,
                 config_.transitionGapMilliseconds};
        return Result::EventReady;
    }

    if (ended_ || position_ >= song_.length)
    {
        ended_ = true;
        event = {EventKind::End, song_.length, 0.0, 0};
        return Result::EndOfSong;
    }

    const size_t index = position_;
    const double noteValue = song_.notes[index];
    const double beat = song_.beats[index];
    if (!std::isfinite(noteValue) || !std::isfinite(beat) || beat <= 0.0)
    {
        failure_ = Result::InvalidValue;
        return failure_;
    }

    const double duration = static_cast<double>(song_.tempo) * beat *
        config_.tempoToMilliseconds;
    if (!std::isfinite(duration) || duration < 0.5 ||
        duration >= static_cast<double>(
            std::numeric_limits<uint32_t>::max()) + 0.5)
    {
        failure_ = duration >= static_cast<double>(
            std::numeric_limits<uint32_t>::max()) + 0.5 ?
            Result::DurationOverflow : Result::InvalidValue;
        return failure_;
    }
    const uint32_t durationMs = static_cast<uint32_t>(duration + 0.5);

    EventKind kind;
    double frequency = 0.0;
    if (noteValue == 0.0)
    {
        if (config_.zeroNoteBehavior == ZeroNoteBehavior::Invalid)
        {
            failure_ = Result::InvalidValue;
            return failure_;
        }
        kind = EventKind::Rest;
    }
    else
    {
        if (noteValue < 0.0)
        {
            failure_ = Result::InvalidValue;
            return failure_;
        }
        frequency = config_.noteEncoding == NoteEncoding::FrequencyHz ?
            noteValue : 1000.0 / noteValue;
        if (!std::isfinite(frequency) || frequency <= 0.0 ||
            (config_.minimumFrequencyHz > 0.0 &&
             frequency < config_.minimumFrequencyHz) ||
            (config_.maximumFrequencyHz > 0.0 &&
             frequency > config_.maximumFrequencyHz))
        {
            failure_ = Result::InvalidValue;
            return failure_;
        }
        kind = EventKind::Note;
    }

    ++position_;
    event = {kind, index, frequency, durationMs};
    if (kind == EventKind::Note && position_ < song_.length &&
        config_.transitionMode == TransitionMode::SilenceGap &&
        config_.transitionGapMilliseconds > 0)
    {
        transitionPending_ = true;
        transitionAfterIndex_ = index;
    }
    return Result::EventReady;
}

void Sequencer::reset()
{
    position_ = 0;
    transitionPending_ = false;
    transitionAfterIndex_ = 0;
    ended_ = false;
    failure_ = Result::EventReady;
}

size_t Sequencer::position() const
{
    return position_;
}

} // namespace song_player
