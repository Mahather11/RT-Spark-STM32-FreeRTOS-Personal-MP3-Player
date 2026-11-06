#include "song_selection_state.h"

namespace song_selection
{

ButtonDebouncer::ButtonDebouncer(uint32_t debounceMs)
    : debounceMs_(debounceMs),
      channels_{
          {true, true, 0},
          {true, true, 0},
          {true, true, 0},
          {true, true, 0}}
{
}

ButtonEvents ButtonDebouncer::update(
    const ButtonLevels& rawLevels,
    uint32_t nowMs)
{
    ButtonEvents events;
    updateChannel(channels_[0], rawLevels.button1High, nowMs,
                  events.button1Pressed, events.button1Released);
    updateChannel(channels_[1], rawLevels.button2High, nowMs,
                  events.button2Pressed, events.button2Released);
    updateChannel(channels_[2], rawLevels.button3High, nowMs,
                  events.button3Pressed, events.button3Released);
    updateChannel(channels_[3], rawLevels.button4High, nowMs,
                  events.button4Pressed, events.button4Released);
    return events;
}

ButtonLevels ButtonDebouncer::stableLevels() const
{
    return {
        channels_[0].stableHigh,
        channels_[1].stableHigh,
        channels_[2].stableHigh,
        channels_[3].stableHigh};
}

ButtonEvents ButtonDebouncer::updateChannel(
    Channel& channel,
    bool rawHigh,
    uint32_t nowMs,
    bool& pressed,
    bool& released)
{
    if (rawHigh == channel.stableHigh)
    {
        channel.candidateHigh = rawHigh;
        return {};
    }

    if (rawHigh != channel.candidateHigh)
    {
        channel.candidateHigh = rawHigh;
        channel.candidateSinceMs = nowMs;
        return {};
    }

    if (static_cast<uint32_t>(nowMs - channel.candidateSinceMs) <
        debounceMs_)
    {
        return {};
    }

    channel.stableHigh = rawHigh;
    pressed = !rawHigh;
    released = rawHigh;
    return {};
}

SongSelectionStateMachine::SongSelectionStateMachine(
    uint8_t initialSong,
    bool initiallyPlaying)
    : currentSong_(initialSong < kSongCount ? initialSong : 0),
      pendingSong_(0),
      state_(SelectionState::Idle),
      playing_(initiallyPlaying),
      selectionButtonActivity_(false),
      button1LongPressHandled_(false),
      selectionArmedAtMs_(0),
      button1PressedAtMs_(0),
      candidateCapturedAtMs_(0)
{
}

StateChange SongSelectionStateMachine::update(
    const DebouncedButtonState& debounced,
    const ButtonEvents& events,
    uint32_t nowMs)
{
    StateChange change;

    const ButtonLevels& levels = debounced.levels;
    const bool selectionButtonPressed =
        events.button2Pressed || events.button3Pressed ||
        events.button4Pressed;
    const bool selectionLevelsAllHigh =
        levels.button2High && levels.button3High &&
        levels.button4High;

    if ((state_ == SelectionState::AwaitingRelease ||
         state_ == SelectionState::AwaitingConfirmation) &&
        static_cast<uint32_t>(nowMs - candidateCapturedAtMs_) >=
            kConfirmationTimeoutMs)
    {
        state_ = SelectionState::Idle;
        selectionButtonActivity_ = false;
        button1LongPressHandled_ = false;
        change.selectionTimedOut = true;
        return change;
    }

    if (selectionButtonPressed)
    {
        selectionButtonActivity_ = true;
        if (state_ == SelectionState::Idle)
        {
            state_ = SelectionState::SelectionArmed;
            selectionArmedAtMs_ = nowMs;
        }
    }

    if ((state_ == SelectionState::SelectionArmed ||
         state_ == SelectionState::Idle) &&
        events.button1Pressed &&
        (!selectionLevelsAllHigh ||
         (state_ == SelectionState::SelectionArmed &&
          static_cast<uint32_t>(nowMs - selectionArmedAtMs_) <
              kSelectionArmedCaptureWindowMs)))
    {
        pendingSong_ = selectionFromElectricalLevels(levels);
        candidateCapturedAtMs_ = nowMs;
        state_ = SelectionState::AwaitingRelease;
        change.candidateCaptured = true;
        return change;
    }

    if (state_ == SelectionState::SelectionArmed &&
        selectionLevelsAllHigh &&
        !events.button1Pressed &&
        static_cast<uint32_t>(nowMs - selectionArmedAtMs_) >=
            kSelectionArmedCaptureWindowMs)
    {
        state_ = SelectionState::Idle;
        selectionButtonActivity_ = false;
        return change;
    }

    if (state_ == SelectionState::AwaitingRelease)
    {
        if (debounced.allButtonsReleased)
        {
            state_ = SelectionState::AwaitingConfirmation;
        }
        return change;
    }

    if (state_ == SelectionState::AwaitingConfirmation)
    {
        if (events.button1Pressed)
        {
            currentSong_ = pendingSong_;
            state_ = SelectionState::Idle;
            selectionButtonActivity_ = false;
            change.selectionConfirmed = true;
        }
        return change;
    }

    if (events.button1Pressed)
    {
        button1PressedAtMs_ = nowMs;
        button1LongPressHandled_ = false;
    }

    if (levels.button1High == false &&
        !button1LongPressHandled_ &&
        static_cast<uint32_t>(nowMs - button1PressedAtMs_) >=
            kSelectionEntryLongPressMs)
    {
        pendingSong_ = selectionFromElectricalLevels(levels);
        candidateCapturedAtMs_ = nowMs;
        state_ = SelectionState::AwaitingRelease;
        button1LongPressHandled_ = true;
        change.candidateCaptured = true;
        return change;
    }

    if (!events.button1Pressed)
    {
        return change;
    }

    playing_ = !playing_;
    change.playPauseChanged = true;

    return change;
}

uint8_t SongSelectionStateMachine::currentSong() const
{
    return currentSong_;
}

uint8_t SongSelectionStateMachine::pendingSong() const
{
    return pendingSong_;
}

SelectionState SongSelectionStateMachine::state() const
{
    return state_;
}

bool SongSelectionStateMachine::isPlaying() const
{
    return playing_;
}

uint8_t SongSelectionStateMachine::selectionFromElectricalLevels(
    const ButtonLevels& levels)
{
    return static_cast<uint8_t>(
        (levels.button2High ? 1U : 0U) << 2U |
        (levels.button3High ? 1U : 0U) << 1U |
        (levels.button4High ? 1U : 0U));
}

uint8_t selectionFromElectricalLevels(const ButtonLevels& levels)
{
    return SongSelectionStateMachine::selectionFromElectricalLevels(levels);
}

DirectSongNavigation::DirectSongNavigation(
    uint8_t initialSong,
    bool initiallyPlaying)
    : candidateSong_(initialSong < kSongCount ? initialSong : 0),
      playing_(initiallyPlaying),
      rightPressActive_(false),
      rightLongPressHandled_(false),
      rightPressedAtMs_(0)
{
}

DirectNavigationChange DirectSongNavigation::update(
    const ButtonEvents& events,
    const ButtonLevels& stableLevels,
    uint32_t nowMs)
{
    DirectNavigationChange change;

    if (events.button1Pressed)
    {
        playing_ = !playing_;
        change.playPauseChanged = true;
    }
    if (events.button3Pressed)
    {
        moveDown();
        change.candidateChanged = true;
    }
    if (events.button2Pressed)
    {
        moveLeft();
        change.candidateChanged = true;
    }
    if (events.button4Pressed)
    {
        rightPressActive_ = true;
        rightLongPressHandled_ = false;
        rightPressedAtMs_ = nowMs;
    }

    if (rightPressActive_ &&
        !rightLongPressHandled_ &&
        !stableLevels.button4High &&
        static_cast<uint32_t>(nowMs - rightPressedAtMs_) >=
            kRightLongPressMs)
    {
        rightLongPressHandled_ = true;
        playing_ = true;
        change.selectionConfirmed = true;
    }

    if (events.button4Released && rightPressActive_)
    {
        if (!rightLongPressHandled_)
        {
            moveRight();
            change.candidateChanged = true;
        }
        rightPressActive_ = false;
    }

    return change;
}

uint8_t DirectSongNavigation::candidateSong() const
{
    return candidateSong_;
}

bool DirectSongNavigation::isPlaying() const
{
    return playing_;
}

void DirectSongNavigation::moveDown()
{
    candidateSong_ = static_cast<uint8_t>(
        (candidateSong_ + 2U) % kSongCount);
}

void DirectSongNavigation::moveLeft()
{
    if ((candidateSong_ % 2U) != 0)
    {
        --candidateSong_;
    }
}

void DirectSongNavigation::moveRight()
{
    if ((candidateSong_ % 2U) == 0)
    {
        ++candidateSong_;
    }
}

} // namespace song_selection
