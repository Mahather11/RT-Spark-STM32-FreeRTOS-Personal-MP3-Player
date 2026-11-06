#include "song_pcm.h"

#include <cmath>

namespace song_pcm
{
namespace
{
const double kTwoPi = 6.283185307179586476925286766559;

bool durationToFrames(uint32_t durationMs, uint32_t sampleRateHz,
                      size_t& frames)
{
    const uint64_t scaled = static_cast<uint64_t>(durationMs) * sampleRateHz;
    const uint64_t rounded = (scaled + 500U) / 1000U;
    if (rounded > static_cast<uint64_t>(static_cast<size_t>(-1)))
    {
        return false;
    }
    frames = static_cast<size_t>(rounded);
    return true;
}
} // namespace

Config::Config()
    : sampleRateHz(44100U),
      peakAmplitude(512),
      trailingSilenceMilliseconds(100U)
{
}

Renderer::Renderer(int16_t* output, size_t capacityFrames,
                   const Config& config)
    : output_(output),
      capacityFrames_(capacityFrames),
      framesWritten_(0),
      config_(config),
      phaseRadians_(0.0),
      ended_(false),
      failure_(Result::EventRendered)
{
}

bool Renderer::appendSilence(size_t frames)
{
    if (frames > capacityFrames_ - framesWritten_ ||
        (frames > 0 && output_ == NULL))
    {
        return false;
    }
    for (size_t frame = 0; frame < frames; ++frame)
    {
        const size_t sample = (framesWritten_ + frame) * 2U;
        output_[sample] = 0;
        output_[sample + 1U] = 0;
    }
    framesWritten_ += frames;
    phaseRadians_ = 0.0;
    return true;
}

Result Renderer::append(const song_player::Event& event)
{
    if (failure_ != Result::EventRendered)
    {
        return failure_;
    }
    if (config_.sampleRateHz == 0U || config_.peakAmplitude <= 0 ||
        config_.peakAmplitude > 32767 ||
        (capacityFrames_ > 0 && output_ == NULL))
    {
        failure_ = Result::InvalidConfiguration;
        return failure_;
    }
    if (ended_)
    {
        return Result::EndOfSong;
    }

    if (event.kind == song_player::EventKind::End)
    {
        size_t tailFrames = 0;
        if (!durationToFrames(config_.trailingSilenceMilliseconds,
                              config_.sampleRateHz, tailFrames) ||
            !appendSilence(tailFrames))
        {
            failure_ = Result::CapacityExceeded;
            return failure_;
        }
        ended_ = true;
        return Result::EndOfSong;
    }

    size_t eventFrames = 0;
    if (event.durationMilliseconds == 0U ||
        !durationToFrames(event.durationMilliseconds, config_.sampleRateHz,
                          eventFrames) ||
        eventFrames == 0U)
    {
        failure_ = Result::InvalidEvent;
        return failure_;
    }
    if (eventFrames > capacityFrames_ - framesWritten_)
    {
        failure_ = Result::CapacityExceeded;
        return failure_;
    }

    if (event.kind == song_player::EventKind::Rest ||
        event.kind == song_player::EventKind::TransitionSilence)
    {
        if (event.frequencyHz != 0.0 || !std::isfinite(event.frequencyHz))
        {
            failure_ = Result::InvalidEvent;
            return failure_;
        }
        if (!appendSilence(eventFrames))
        {
            failure_ = Result::CapacityExceeded;
            return failure_;
        }
        return Result::EventRendered;
    }
    if (event.kind != song_player::EventKind::Note ||
        !std::isfinite(event.frequencyHz) || event.frequencyHz <= 0.0 ||
        event.frequencyHz >= static_cast<double>(config_.sampleRateHz) / 2.0)
    {
        failure_ = Result::InvalidEvent;
        return failure_;
    }

    const double phaseStep = kTwoPi * event.frequencyHz /
        static_cast<double>(config_.sampleRateHz);
    for (size_t frame = 0; frame < eventFrames; ++frame)
    {
        const int16_t sampleValue = static_cast<int16_t>(
            std::lround(static_cast<double>(config_.peakAmplitude) *
                        std::sin(phaseRadians_)));
        const size_t sample = (framesWritten_ + frame) * 2U;
        output_[sample] = sampleValue;
        output_[sample + 1U] = sampleValue;
        phaseRadians_ += phaseStep;
        if (phaseRadians_ >= kTwoPi)
        {
            phaseRadians_ -= kTwoPi;
        }
    }
    framesWritten_ += eventFrames;
    return Result::EventRendered;
}

size_t Renderer::framesWritten() const
{
    return framesWritten_;
}

} // namespace song_pcm
