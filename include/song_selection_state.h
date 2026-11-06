#ifndef SONG_SELECTION_STATE_H
#define SONG_SELECTION_STATE_H

#include <stdint.h>

namespace song_selection
{

constexpr uint8_t kSongCount = 8;
constexpr uint32_t kConfirmationTimeoutMs = 5000;
constexpr uint32_t kSelectionEntryLongPressMs = 1000;
constexpr uint32_t kSelectionArmedCaptureWindowMs = 50;
constexpr uint32_t kRightLongPressMs = 900;

// These indices intentionally match the first eight Song objects in song_def.h.
enum SongIndex : uint8_t
{
    FUR_ELISE_INDEX = 0,
    CANNON_IN_D_INDEX = 1,
    MINUET_IN_G_MAJOR_INDEX = 2,
    TURKISH_MARCH_INDEX = 3,
    NOCTRUNE_IN_E_FLAT_INDEX = 4,
    WALTZ_NO2_INDEX = 5,
    NOCTRUNE_IN_C_SHARP_MAJOR_INDEX = 6,
    SYMPHONY_NO40_INDEX = 7
};

struct ButtonLevels
{
    // These are electrical levels: true is HIGH and false is LOW.
    bool button1High;
    bool button2High;
    bool button3High;
    bool button4High;
};

struct DebouncedButtonState
{
    ButtonLevels levels;
    bool allButtonsReleased;
};

struct ButtonEvents
{
    bool button1Pressed = false;
    bool button1Released = false;
    bool button2Pressed = false;
    bool button2Released = false;
    bool button3Pressed = false;
    bool button3Released = false;
    bool button4Pressed = false;
    bool button4Released = false;
};

class ButtonDebouncer
{
public:
    explicit ButtonDebouncer(uint32_t debounceMs = 30);

    ButtonEvents update(const ButtonLevels& rawLevels, uint32_t nowMs);
    ButtonLevels stableLevels() const;

private:
    struct Channel
    {
        bool stableHigh;
        bool candidateHigh;
        uint32_t candidateSinceMs;
    };

    ButtonEvents updateChannel(
        Channel& channel,
        bool rawHigh,
        uint32_t nowMs,
        bool& pressed,
        bool& released);

    uint32_t debounceMs_;
    Channel channels_[4];
};

enum class SelectionState
{
    Idle,
    // A selection button was pressed; Button 1 must capture before all
    // selection buttons are released, otherwise the interaction is canceled.
    SelectionArmed,
    AwaitingRelease,
    AwaitingConfirmation
};

struct StateChange
{
    bool candidateCaptured = false;
    bool selectionConfirmed = false;
    bool selectionTimedOut = false;
    bool playPauseChanged = false;
};

class SongSelectionStateMachine
{
public:
    explicit SongSelectionStateMachine(
        uint8_t initialSong = 0,
        bool initiallyPlaying = true);

    StateChange update(
        const DebouncedButtonState& debounced,
        const ButtonEvents& events,
        uint32_t nowMs);

    uint8_t currentSong() const;
    uint8_t pendingSong() const;
    SelectionState state() const;
    bool isPlaying() const;

    static uint8_t selectionFromElectricalLevels(
        const ButtonLevels& levels);

private:
    uint8_t currentSong_;
    uint8_t pendingSong_;
    SelectionState state_;
    bool playing_;
    bool selectionButtonActivity_;
    bool button1LongPressHandled_;
    uint32_t selectionArmedAtMs_;
    uint32_t button1PressedAtMs_;
    uint32_t candidateCapturedAtMs_;
};

uint8_t selectionFromElectricalLevels(const ButtonLevels& levels);

struct DirectNavigationChange
{
    bool candidateChanged = false;
    bool selectionConfirmed = false;
    bool playPauseChanged = false;
};

class DirectSongNavigation
{
public:
    explicit DirectSongNavigation(
        uint8_t initialSong = 0,
        bool initiallyPlaying = true);

    DirectNavigationChange update(
        const ButtonEvents& events,
        const ButtonLevels& stableLevels,
        uint32_t nowMs);

    uint8_t candidateSong() const;
    bool isPlaying() const;

private:
    void moveDown();
    void moveLeft();
    void moveRight();

    uint8_t candidateSong_;
    bool playing_;
    bool rightPressActive_;
    bool rightLongPressHandled_;
    uint32_t rightPressedAtMs_;
};

} // namespace song_selection

#endif
