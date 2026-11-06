#include <unity.h>

#include "led_matrix_effect.h"
#include "audio_synth.h"
#include "audio_start_gate.h"
#include "song_catalog.h"
#include "song_pcm.h"
#include "song_player.h"
#include "song_selection_state.h"

using namespace song_selection;
constexpr double kReferenceTempoScale = 8.0;

void setUp() {}
void tearDown() {}

void test_song_catalog_maps_the_eight_selectable_course_songs()
{
    const int expectedLengths[8] = {72, 88, 90, 176, 116, 135, 64, 168};
    TEST_ASSERT_TRUE(song_catalog::allCourseLengthsMatch());
    for (uint8_t index = 0; index < song_catalog::kSelectableSongCount; ++index)
    {
        Song* const selected = song_catalog::get(index);
        TEST_ASSERT_NOT_NULL(selected);
        TEST_ASSERT_EQUAL_INT(expectedLengths[index], selected->length);
        TEST_ASSERT_EQUAL_PTR(selected, song_catalog::getCourseSong(index));
    }
    TEST_ASSERT_NULL(song_catalog::get(song_catalog::kSelectableSongCount));
    TEST_ASSERT_NOT_NULL(song_catalog::getCourseSong(8U));
    TEST_ASSERT_NOT_NULL(song_catalog::getCourseSong(9U));
    TEST_ASSERT_NULL(song_catalog::getCourseSong(10U));
}

void test_initial_audio_gate_keeps_paused_song_silent()
{
    using audio_start_gate::Decision;
    TEST_ASSERT_EQUAL(static_cast<int>(Decision::WaitForPlay),
        static_cast<int>(audio_start_gate::decide(false, false, false)));
    // Repeated song-start notifications do not override a paused state.
    TEST_ASSERT_EQUAL(static_cast<int>(Decision::WaitForPlay),
        static_cast<int>(audio_start_gate::decide(false, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(Decision::StartPlayback),
        static_cast<int>(audio_start_gate::decide(true, false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(Decision::StopPlayback),
        static_cast<int>(audio_start_gate::decide(false, true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(Decision::PlaybackError),
        static_cast<int>(audio_start_gate::decide(true, false, true)));
    TEST_ASSERT_FALSE(audio_start_gate::isSongChange(3U, 3U));
    TEST_ASSERT_TRUE(audio_start_gate::isSongChange(3U, 4U));
}

void test_course_song_sequencer_reaches_every_declared_event()
{
    for (uint8_t index = 0; index < song_catalog::kSelectableSongCount; ++index)
    {
        Song* const song = song_catalog::get(index);
        song_player::SongView view = {
            song->note, song->beat, song->tempo,
            static_cast<size_t>(song->length)};
        song_player::Config config;
        config.noteEncoding = song_player::NoteEncoding::PeriodMilliseconds;
        config.zeroNoteBehavior = song_player::ZeroNoteBehavior::Rest;
        config.tempoToMilliseconds = 1000.0 * kReferenceTempoScale;
        song_player::Sequencer sequencer(view, config);
        size_t events = 0U;
        song_player::Event event;
        while (sequencer.next(event) == song_player::Result::EventReady)
        {
            TEST_ASSERT_TRUE(event.durationMilliseconds > 0U);
            ++events;
        }
        TEST_ASSERT_EQUAL_UINT32(song->length, events);
        TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EndOfSong),
                          static_cast<int>(sequencer.next(event)));
        sequencer.reset();
        TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                          static_cast<int>(sequencer.next(event)));
        TEST_ASSERT_EQUAL_UINT32(0U, event.noteIndex);
    }
}

void test_reference_timing_and_zero_note_rest()
{
    Song* const song = song_catalog::get(0U);
    song_player::SongView view = {
        song->note, song->beat, song->tempo,
        static_cast<size_t>(song->length)};
    song_player::Config config;
    config.noteEncoding = song_player::NoteEncoding::PeriodMilliseconds;
    config.zeroNoteBehavior = song_player::ZeroNoteBehavior::Rest;
    config.tempoToMilliseconds = 1000.0 * kReferenceTempoScale;
    song_player::Sequencer sequencer(view, config);
    song_player::Event event;
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(sequencer.next(event)));
    TEST_ASSERT_FLOAT_WITHIN(0.02, 1000.0 / 1.516, event.frequencyHz);
    TEST_ASSERT_EQUAL_UINT32(180U, event.durationMilliseconds);
    for (uint8_t index = 1U; index < 9U; ++index)
        TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                          static_cast<int>(sequencer.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(sequencer.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::EventKind::Rest),
                      static_cast<int>(event.kind));
    TEST_ASSERT_EQUAL_UINT32(180U, event.durationMilliseconds);
}

void test_streaming_synth_emits_bounded_stereo_and_releases()
{
    audio_synth::initialize(44100U);
    int16_t samples[768U * 2U] = {};
    audio_synth::noteOn(440.0f, 256U);
    audio_synth::render(samples, 256U);
    int16_t peak = 0;
    bool nonzero = false;
    for (size_t frame = 0; frame < 256U; ++frame)
    {
        TEST_ASSERT_EQUAL_INT16(samples[frame * 2U], samples[frame * 2U + 1U]);
        const int16_t value = samples[frame * 2U];
        const int16_t magnitude = value < 0 ? static_cast<int16_t>(-value) : value;
        peak = magnitude > peak ? magnitude : peak;
        nonzero = nonzero || value != 0;
    }
    TEST_ASSERT_TRUE(nonzero);
    TEST_ASSERT_TRUE(peak <= 512);
    audio_synth::noteOff();
    audio_synth::render(&samples[512U], 256U);
    audio_synth::render(&samples[1024U], 256U);
    TEST_ASSERT_EQUAL_INT16(0, samples[1534U]);
    TEST_ASSERT_EQUAL_INT16(0, samples[1535U]);
}

void test_song_player_period_conversion_and_assumed_duration()
{
    const float notes[] = {2.0f};
    const float beats[] = {0.5f};
    song_player::SongView song = {notes, beats, 0.2f, 1};
    song_player::Sequencer player(song);
    song_player::Event event;

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::EventKind::Note),
                      static_cast<int>(event.kind));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 500.0, event.frequencyHz);
    TEST_ASSERT_EQUAL_UINT32(100, event.durationMilliseconds);
}

void test_song_player_frequency_encoding_is_configurable()
{
    const float notes[] = {440.0f};
    const float beats[] = {1.0f};
    song_player::SongView song = {notes, beats, 1.0f, 1};
    song_player::Config config;
    config.noteEncoding = song_player::NoteEncoding::FrequencyHz;
    config.tempoToMilliseconds = 500.0;
    song_player::Sequencer player(song, config);
    song_player::Event event;

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 440.0, event.frequencyHz);
    TEST_ASSERT_EQUAL_UINT32(500, event.durationMilliseconds);
}

void test_zero_note_is_a_duration_bearing_rest_by_default()
{
    const float notes[] = {0.0f};
    const float beats[] = {0.25f};
    song_player::SongView song = {notes, beats, 0.4f, 1};
    song_player::Sequencer player(song);
    song_player::Event event;

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::EventKind::Rest),
                      static_cast<int>(event.kind));
    TEST_ASSERT_EQUAL_UINT32(100, event.durationMilliseconds);
    TEST_ASSERT_TRUE(event.frequencyHz == 0.0);
}

void test_null_arrays_are_rejected_for_nonempty_song()
{
    const float beats[] = {1.0f};
    song_player::SongView noNotes = {NULL, beats, 1.0f, 1};
    song_player::SongView noBeats = {beats, NULL, 1.0f, 1};
    song_player::Event event;
    song_player::Sequencer first(noNotes);
    song_player::Sequencer second(noBeats);

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::InvalidSong),
                      static_cast<int>(first.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::InvalidSong),
                      static_cast<int>(second.next(event)));
}

void test_invalid_note_beat_and_nonfinite_values_are_rejected()
{
    const float badNotes[] = {-1.0f};
    const float goodNotes[] = {1.0f};
    const float badBeats[] = {0.0f};
    const float nanNotes[] = {NAN};
    const float infBeats[] = {INFINITY};
    song_player::Event event;

    song_player::SongView negative = {badNotes, goodNotes, 1.0f, 1};
    song_player::SongView zeroBeat = {goodNotes, badBeats, 1.0f, 1};
    song_player::SongView nanNote = {nanNotes, goodNotes, 1.0f, 1};
    song_player::SongView infBeat = {goodNotes, infBeats, 1.0f, 1};
    song_player::Sequencer a(negative);
    song_player::Sequencer b(zeroBeat);
    song_player::Sequencer c(nanNote);
    song_player::Sequencer d(infBeat);

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::InvalidValue),
                      static_cast<int>(a.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::InvalidValue),
                      static_cast<int>(b.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::InvalidValue),
                      static_cast<int>(c.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::InvalidValue),
                      static_cast<int>(d.next(event)));
}

void test_invalid_tempo_and_duration_overflow_are_rejected()
{
    const float notes[] = {1.0f};
    const float beats[] = {1.0f};
    song_player::Event event;
    song_player::SongView invalidTempo = {notes, beats, NAN, 1};
    song_player::SongView overflow = {notes, beats, 3.0e38f, 1};
    song_player::Sequencer invalid(invalidTempo);
    song_player::Sequencer tooLong(overflow);

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::InvalidSong),
                      static_cast<int>(invalid.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::DurationOverflow),
                      static_cast<int>(tooLong.next(event)));
}

void test_frequency_bounds_and_zero_policy_are_configurable()
{
    const float notes[] = {30.0f, 0.0f};
    const float beats[] = {1.0f, 1.0f};
    song_player::SongView song = {notes, beats, 1.0f, 2};
    song_player::Config config;
    config.noteEncoding = song_player::NoteEncoding::FrequencyHz;
    config.minimumFrequencyHz = 40.0;
    config.zeroNoteBehavior = song_player::ZeroNoteBehavior::Invalid;
    song_player::Sequencer player(song, config);
    song_player::Event event;

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::InvalidValue),
                      static_cast<int>(player.next(event)));
}

void test_transition_gap_and_connected_mode()
{
    const float notes[] = {440.0f, 220.0f};
    const float beats[] = {1.0f, 1.0f};
    song_player::SongView song = {notes, beats, 0.1f, 2};
    song_player::Config config;
    config.noteEncoding = song_player::NoteEncoding::FrequencyHz;
    config.transitionMode = song_player::TransitionMode::SilenceGap;
    config.transitionGapMilliseconds = 7;
    song_player::Sequencer player(song, config);
    song_player::Event event;

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::EventKind::Note),
                      static_cast<int>(event.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::EventKind::TransitionSilence),
                      static_cast<int>(event.kind));
    TEST_ASSERT_EQUAL_UINT32(7, event.durationMilliseconds);
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::EventKind::Note),
                      static_cast<int>(event.kind));
}

void test_end_of_song_is_stable_and_reset_replays_within_bounds()
{
    const float notes[] = {440.0f};
    const float beats[] = {1.0f};
    song_player::SongView song = {notes, beats, 0.1f, 1};
    song_player::Config config;
    config.noteEncoding = song_player::NoteEncoding::FrequencyHz;
    song_player::Sequencer player(song, config);
    song_player::Event event;

    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_EQUAL_UINT32(1, player.position());
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EndOfSong),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::EventKind::End),
                      static_cast<int>(event.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EndOfSong),
                      static_cast<int>(player.next(event)));
    player.reset();
    TEST_ASSERT_EQUAL_UINT32(0, player.position());
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EventReady),
                      static_cast<int>(player.next(event)));
    TEST_ASSERT_EQUAL_UINT32(0, event.noteIndex);
}

void test_empty_song_ends_without_dereferencing_arrays()
{
    song_player::SongView empty = {NULL, NULL, 1.0f, 0};
    song_player::Sequencer player(empty);
    song_player::Event event;
    TEST_ASSERT_EQUAL(static_cast<int>(song_player::Result::EndOfSong),
                      static_cast<int>(player.next(event)));
}

void test_pcm_capacity_stereo_alignment_and_exact_sample_counts()
{
    int16_t output[12] = {};
    song_pcm::Config config;
    config.sampleRateHz = 1000U;
    config.peakAmplitude = 100;
    config.trailingSilenceMilliseconds = 0U;
    song_pcm::Renderer renderer(output, 6U, config);
    song_player::Event note = {
        song_player::EventKind::Note, 0U, 100.0, 3U};
    song_player::Event rest = {
        song_player::EventKind::Rest, 1U, 0.0, 2U};
    song_player::Event end = {
        song_player::EventKind::End, 2U, 0.0, 0U};

    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::EventRendered),
                      static_cast<int>(renderer.append(note)));
    TEST_ASSERT_EQUAL_UINT32(3, renderer.framesWritten());
    TEST_ASSERT_EQUAL_INT16(output[0], output[1]);
    TEST_ASSERT_EQUAL_INT16(output[2], output[3]);
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::EventRendered),
                      static_cast<int>(renderer.append(rest)));
    TEST_ASSERT_EQUAL_UINT32(5, renderer.framesWritten());
    TEST_ASSERT_EQUAL_INT16(0, output[6]);
    TEST_ASSERT_EQUAL_INT16(0, output[7]);
    TEST_ASSERT_EQUAL_INT16(0, output[8]);
    TEST_ASSERT_EQUAL_INT16(0, output[9]);
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::EndOfSong),
                      static_cast<int>(renderer.append(end)));
    TEST_ASSERT_EQUAL_UINT32(5, renderer.framesWritten());
}

void test_pcm_capacity_failure_does_not_overrun_buffer()
{
    int16_t output[4] = {123, 123, 123, 123};
    song_pcm::Config config;
    config.sampleRateHz = 1000U;
    config.trailingSilenceMilliseconds = 0U;
    song_pcm::Renderer renderer(output, 2U, config);
    song_player::Event note = {
        song_player::EventKind::Note, 0U, 100.0, 3U};

    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::CapacityExceeded),
                      static_cast<int>(renderer.append(note)));
    TEST_ASSERT_EQUAL_UINT32(0, renderer.framesWritten());
    TEST_ASSERT_EQUAL_INT16(123, output[0]);
    TEST_ASSERT_EQUAL_INT16(123, output[3]);
}

void test_pcm_rejects_invalid_events_and_invalid_configuration()
{
    int16_t output[16] = {};
    song_pcm::Config config;
    config.sampleRateHz = 1000U;
    song_pcm::Renderer renderer(output, 8U, config);
    song_player::Event badFrequency = {
        song_player::EventKind::Note, 0U, INFINITY, 2U};
    song_player::Event zeroDuration = {
        song_player::EventKind::Rest, 1U, 0.0, 0U};
    song_player::Event nyquist = {
        song_player::EventKind::Note, 2U, 500.0, 2U};
    song_player::Event malformedRest = {
        song_player::EventKind::Rest, 3U, 10.0, 2U};
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::InvalidEvent),
                      static_cast<int>(renderer.append(badFrequency)));

    song_pcm::Renderer zeroDurationRenderer(output, 8U, config);
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::InvalidEvent),
                      static_cast<int>(zeroDurationRenderer.append(zeroDuration)));
    song_pcm::Renderer nyquistRenderer(output, 8U, config);
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::InvalidEvent),
                      static_cast<int>(nyquistRenderer.append(nyquist)));
    song_pcm::Renderer restRenderer(output, 8U, config);
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::InvalidEvent),
                      static_cast<int>(restRenderer.append(malformedRest)));

    song_pcm::Config badConfig;
    badConfig.sampleRateHz = 0U;
    song_pcm::Renderer invalid(output, 8U, badConfig);
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::InvalidConfiguration),
                      static_cast<int>(invalid.append(zeroDuration)));
}

void test_pcm_44100_hz_rounding_and_tail_capacity_are_exact()
{
    int16_t output[2044] = {};
    song_pcm::Config config;
    config.sampleRateHz = 44100U;
    config.peakAmplitude = 512;
    config.trailingSilenceMilliseconds = 10U;
    song_pcm::Renderer exact(output, 1022U, config);
    song_player::Event note = {
        song_player::EventKind::Note, 0U, 440.0, 23U};
    song_player::Event end = {
        song_player::EventKind::End, 1U, 0.0, 0U};

    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::EventRendered),
                      static_cast<int>(exact.append(note)));
    TEST_ASSERT_EQUAL_UINT32(1014, exact.framesWritten());
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::CapacityExceeded),
                      static_cast<int>(exact.append(end)));
    TEST_ASSERT_EQUAL_UINT32(1014, exact.framesWritten());
}

void test_pcm_end_event_appends_tail_once_and_rejects_later_events()
{
    int16_t output[12] = {};
    song_pcm::Config config;
    config.sampleRateHz = 1000U;
    config.trailingSilenceMilliseconds = 4U;
    song_pcm::Renderer renderer(output, 6U, config);
    song_player::Event note = {
        song_player::EventKind::Note, 0U, 100.0, 2U};
    song_player::Event end = {
        song_player::EventKind::End, 1U, 0.0, 0U};

    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::EventRendered),
                      static_cast<int>(renderer.append(note)));
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::EndOfSong),
                      static_cast<int>(renderer.append(end)));
    TEST_ASSERT_EQUAL_UINT32(6, renderer.framesWritten());
    for (size_t frame = 2; frame < 6; ++frame)
    {
        TEST_ASSERT_EQUAL_INT16(0, output[frame * 2U]);
        TEST_ASSERT_EQUAL_INT16(0, output[frame * 2U + 1U]);
    }
    TEST_ASSERT_EQUAL(static_cast<int>(song_pcm::Result::EndOfSong),
                      static_cast<int>(renderer.append(end)));
    TEST_ASSERT_EQUAL_UINT32(6, renderer.framesWritten());
}

void test_fur_elise_first_fourteen_events_render_expected_duration()
{
    // Mirrors the firmware's explicitly assumed period-ms and tempo scaling;
    // these units/formula are not confirmed laboratory requirements.
    const float notes[] = {
        1.516f, 1.607f, 1.516f, 1.607f, 1.516f, 2.024f, 1.702f,
        1.911f, 2.272f, 0.0f, 3.822f, 3.822f, 3.033f, 2.272f};
    const float beats[] = {
        0.125f, 0.125f, 0.125f, 0.125f, 0.125f, 0.125f, 0.125f,
        0.125f, 0.25f, 0.125f, 0.075f, 0.125f, 0.125f, 0.125f};
    song_player::SongView song = {notes, beats, 0.18f, 14U};
    song_player::Config sequenceConfig;
    sequenceConfig.noteEncoding =
        song_player::NoteEncoding::PeriodMilliseconds;
    sequenceConfig.zeroNoteBehavior = song_player::ZeroNoteBehavior::Rest;
    sequenceConfig.transitionMode = song_player::TransitionMode::Connected;
    sequenceConfig.tempoToMilliseconds = 1000.0;
    song_player::Sequencer sequencer(song, sequenceConfig);

    int16_t output[21385U * 2U] = {};
    song_pcm::Config pcmConfig;
    pcmConfig.sampleRateHz = 44100U;
    pcmConfig.peakAmplitude = 512;
    pcmConfig.trailingSilenceMilliseconds = 150U;
    song_pcm::Renderer renderer(output, 21385U, pcmConfig);

    size_t eventCount = 0U;
    size_t restCount = 0U;
    for (;;)
    {
        song_player::Event event;
        const song_player::Result result = sequencer.next(event);
        if (result == song_player::Result::EndOfSong)
        {
            TEST_ASSERT_EQUAL(
                static_cast<int>(song_pcm::Result::EndOfSong),
                static_cast<int>(renderer.append(event)));
            break;
        }
        TEST_ASSERT_EQUAL(
            static_cast<int>(song_player::Result::EventReady),
            static_cast<int>(result));
        if (event.kind == song_player::EventKind::Rest)
        {
            ++restCount;
        }
        TEST_ASSERT_EQUAL(
            static_cast<int>(song_pcm::Result::EventRendered),
            static_cast<int>(renderer.append(event)));
        ++eventCount;
    }

    TEST_ASSERT_EQUAL_UINT32(14U, eventCount);
    TEST_ASSERT_EQUAL_UINT32(1U, restCount);
    TEST_ASSERT_EQUAL_UINT32(21385U, renderer.framesWritten());
    TEST_ASSERT_EQUAL_INT16(0, output[10097U * 2U]);
    TEST_ASSERT_EQUAL_INT16(0, output[10097U * 2U + 1U]);
}

static ButtonLevels levels(
    bool button1High,
    bool button2High,
    bool button3High,
    bool button4High)
{
    return {button1High, button2High, button3High, button4High};
}

static ButtonEvents button1Press()
{
    ButtonEvents events;
    events.button1Pressed = true;
    return events;
}

static ButtonEvents selectionPress()
{
    ButtonEvents events;
    events.button2Pressed = true;
    return events;
}

static ButtonEvents downPress()
{
    ButtonEvents events;
    events.button3Pressed = true;
    return events;
}

static ButtonEvents upPress()
{
    ButtonEvents events;
    events.button1Pressed = true;
    return events;
}

static ButtonEvents rightPress()
{
    ButtonEvents events;
    events.button4Pressed = true;
    return events;
}

static ButtonEvents rightRelease()
{
    ButtonEvents events;
    events.button4Released = true;
    return events;
}

static ButtonEvents leftPress()
{
    ButtonEvents events;
    events.button2Pressed = true;
    return events;
}

static DebouncedButtonState debounced(
    const ButtonLevels& input)
{
    return {
        input,
        input.button1High && input.button2High &&
            input.button3High && input.button4High};
}

void test_all_eight_raw_binary_combinations()
{
    TEST_ASSERT_EQUAL_UINT8(0, FUR_ELISE_INDEX);
    TEST_ASSERT_EQUAL_UINT8(1, CANNON_IN_D_INDEX);
    TEST_ASSERT_EQUAL_UINT8(2, MINUET_IN_G_MAJOR_INDEX);
    TEST_ASSERT_EQUAL_UINT8(3, TURKISH_MARCH_INDEX);
    TEST_ASSERT_EQUAL_UINT8(4, NOCTRUNE_IN_E_FLAT_INDEX);
    TEST_ASSERT_EQUAL_UINT8(5, WALTZ_NO2_INDEX);
    TEST_ASSERT_EQUAL_UINT8(6, NOCTRUNE_IN_C_SHARP_MAJOR_INDEX);
    TEST_ASSERT_EQUAL_UINT8(7, SYMPHONY_NO40_INDEX);

    for (uint8_t expected = 0; expected < kSongCount; ++expected)
    {
        ButtonLevels input = levels(
            true,
            (expected & 0b100U) != 0,
            (expected & 0b010U) != 0,
            (expected & 0b001U) != 0);
        TEST_ASSERT_EQUAL_UINT8(expected, selectionFromElectricalLevels(input));
    }
}

void test_low_low_low_selects_first_song()
{
    TEST_ASSERT_EQUAL_UINT8(
        0,
        selectionFromElectricalLevels(levels(false, false, false, false)));
}

void test_debouncer_emits_press_and_release_only_after_stability()
{
    ButtonDebouncer debouncer(10);
    ButtonLevels released = levels(true, true, true, true);
    ButtonLevels pressed = levels(false, true, true, true);

    TEST_ASSERT_FALSE(debouncer.update(pressed, 0).button1Pressed);
    TEST_ASSERT_FALSE(debouncer.update(pressed, 9).button1Pressed);
    TEST_ASSERT_TRUE(debouncer.update(pressed, 10).button1Pressed);
    TEST_ASSERT_FALSE(debouncer.update(pressed, 11).button1Pressed);
    TEST_ASSERT_FALSE(debouncer.update(released, 15).button1Released);
    TEST_ASSERT_TRUE(debouncer.update(released, 25).button1Released);
}

void test_selection_requires_capture_release_then_confirmation()
{
    SongSelectionStateMachine machine(3, true);
    ButtonLevels candidate = levels(false, false, true, false); // 010? PC0=0, PC1=1, PC4=0 => 010

    StateChange captured = machine.update(
        debounced(candidate), button1Press(), 100);
    TEST_ASSERT_TRUE(captured.candidateCaptured);
    TEST_ASSERT_EQUAL_UINT8(2, machine.pendingSong());
    TEST_ASSERT_EQUAL(SelectionState::AwaitingRelease, machine.state());

    StateChange notConfirmed = machine.update(
        debounced(levels(true, true, true, true)), ButtonEvents{}, 101);
    TEST_ASSERT_FALSE(notConfirmed.selectionConfirmed);
    TEST_ASSERT_EQUAL(SelectionState::AwaitingConfirmation, machine.state());

    StateChange confirmed = machine.update(
        debounced(levels(false, true, true, true)), button1Press(), 102);
    TEST_ASSERT_TRUE(confirmed.selectionConfirmed);
    TEST_ASSERT_EQUAL_UINT8(2, machine.currentSong());
    TEST_ASSERT_EQUAL(SelectionState::Idle, machine.state());
}

void test_selection_timeout_preserves_current_song()
{
    SongSelectionStateMachine machine(6, true);
    StateChange captured = machine.update(
        debounced(levels(false, false, false, true)),
        button1Press(), 1000);
    TEST_ASSERT_TRUE(captured.candidateCaptured);

    StateChange timedOut = machine.update(
        debounced(levels(false, false, false, true)), ButtonEvents{},
        1000 + kConfirmationTimeoutMs);
    TEST_ASSERT_TRUE(timedOut.selectionTimedOut);
    TEST_ASSERT_EQUAL_UINT8(6, machine.currentSong());
    TEST_ASSERT_EQUAL(SelectionState::Idle, machine.state());
    TEST_ASSERT_TRUE(machine.isPlaying());
}

void test_button_one_toggles_play_pause_without_selection_activity()
{
    SongSelectionStateMachine machine(0, true);

    StateChange paused = machine.update(
        debounced(levels(false, true, true, true)), button1Press(), 10);
    TEST_ASSERT_TRUE(paused.playPauseChanged);
    TEST_ASSERT_FALSE(machine.isPlaying());

    StateChange playing = machine.update(
        debounced(levels(false, true, true, true)), button1Press(), 20);
    TEST_ASSERT_TRUE(playing.playPauseChanged);
    TEST_ASSERT_TRUE(machine.isPlaying());
}

void test_high_high_high_can_be_captured_after_selection_mode_is_armed()
{
    SongSelectionStateMachine machine(0, true);
    machine.update(
        debounced(levels(true, false, true, true)), selectionPress(), 10);
    machine.update(
        debounced(levels(true, true, true, true)), ButtonEvents{}, 20);

    StateChange captured = machine.update(
        debounced(levels(false, true, true, true)), button1Press(), 30);
    TEST_ASSERT_TRUE(captured.candidateCaptured);
    TEST_ASSERT_EQUAL_UINT8(7, machine.pendingSong());
}

void test_timeout_in_confirmation_state()
{
    SongSelectionStateMachine machine(4, false);
    machine.update(
        debounced(levels(false, false, true, true)), button1Press(), 100);
    machine.update(
        debounced(levels(true, true, true, true)), ButtonEvents{}, 101);

    StateChange timedOut = machine.update(
        debounced(levels(true, true, true, true)), ButtonEvents{},
        101 + kConfirmationTimeoutMs);
    TEST_ASSERT_TRUE(timedOut.selectionTimedOut);
    TEST_ASSERT_EQUAL_UINT8(4, machine.currentSong());
    TEST_ASSERT_FALSE(machine.isPlaying());
}

void test_timeout_survives_counter_wraparound()
{
    SongSelectionStateMachine machine(5, true);
    const uint32_t start = UINT32_MAX - 1000U;
    machine.update(
        debounced(levels(false, false, true, true)), button1Press(), start);

    StateChange timedOut = machine.update(
        debounced(levels(false, false, true, true)), ButtonEvents{},
        start + kConfirmationTimeoutMs);
    TEST_ASSERT_TRUE(timedOut.selectionTimedOut);
    TEST_ASSERT_EQUAL_UINT8(5, machine.currentSong());
}

void test_stray_selection_button_does_not_change_later_play_pause()
{
    SongSelectionStateMachine machine(0, true);
    machine.update(
        debounced(levels(true, false, true, true)), selectionPress(), 10);
    machine.update(
        debounced(levels(true, true, true, true)), ButtonEvents{}, 20);

    StateChange toggled = machine.update(
        debounced(levels(false, true, true, true)), button1Press(), 100);
    TEST_ASSERT_TRUE(toggled.playPauseChanged);
    TEST_ASSERT_FALSE(toggled.candidateCaptured);
    TEST_ASSERT_FALSE(machine.isPlaying());
}

void test_release_all_requires_debounced_stable_levels()
{
    SongSelectionStateMachine machine(0, true);
    machine.update(
        debounced(levels(false, false, true, true)), button1Press(), 10);

    DebouncedButtonState raw_glitch{
        levels(true, true, true, true), false};
    machine.update(raw_glitch, ButtonEvents{}, 20);
    TEST_ASSERT_EQUAL(SelectionState::AwaitingRelease, machine.state());

    machine.update(
        debounced(levels(true, true, true, true)), ButtonEvents{}, 21);
    TEST_ASSERT_EQUAL(
        SelectionState::AwaitingConfirmation, machine.state());
}

void test_all_high_selection_uses_button_one_long_press()
{
    SongSelectionStateMachine machine(0, true);
    const ButtonLevels allHighButton1Held = levels(false, true, true, true);

    machine.update(
        debounced(allHighButton1Held), button1Press(), 100);
    StateChange captured = machine.update(
        debounced(allHighButton1Held), ButtonEvents{},
        100 + kSelectionEntryLongPressMs);
    TEST_ASSERT_TRUE(captured.candidateCaptured);
    TEST_ASSERT_EQUAL_UINT8(7, machine.pendingSong());
}

void test_direct_down_moves_candidate_by_one_row()
{
    DirectSongNavigation navigation(0, true);
    DirectNavigationChange change =
        navigation.update(downPress(), levels(true, true, true, true), 0);

    TEST_ASSERT_TRUE(change.candidateChanged);
    TEST_ASSERT_EQUAL_UINT8(2, navigation.candidateSong());
    TEST_ASSERT_TRUE(navigation.isPlaying());
}

void test_direct_browsing_does_not_change_playback_state()
{
    DirectSongNavigation navigation(0, true);
    navigation.update(downPress(), levels(true, true, true, true), 0);

    TEST_ASSERT_EQUAL_UINT8(2, navigation.candidateSong());
    TEST_ASSERT_TRUE(navigation.isPlaying());
}

void test_direct_short_right_moves_across_without_confirming()
{
    DirectSongNavigation navigation(0, true);
    navigation.update(rightPress(), levels(true, true, true, false), 0);
    DirectNavigationChange change =
        navigation.update(rightRelease(), levels(true, true, true, true), 100);

    TEST_ASSERT_TRUE(change.candidateChanged);
    TEST_ASSERT_FALSE(change.selectionConfirmed);
    TEST_ASSERT_EQUAL_UINT8(1, navigation.candidateSong());
}

void test_direct_long_right_confirms_once()
{
    DirectSongNavigation navigation(2, false);
    navigation.update(rightPress(), levels(true, true, true, false), 0);
    DirectNavigationChange confirmed =
        navigation.update(ButtonEvents{}, levels(true, true, true, false),
                          kRightLongPressMs);
    DirectNavigationChange held =
        navigation.update(ButtonEvents{}, levels(true, true, true, false),
                          kRightLongPressMs + 100);
    DirectNavigationChange released =
        navigation.update(rightRelease(), levels(true, true, true, true),
                          kRightLongPressMs + 200);

    TEST_ASSERT_TRUE(confirmed.selectionConfirmed);
    TEST_ASSERT_FALSE(held.selectionConfirmed);
    TEST_ASSERT_FALSE(released.candidateChanged);
    TEST_ASSERT_EQUAL_UINT8(2, navigation.candidateSong());
    TEST_ASSERT_TRUE(navigation.isPlaying());
}

void test_direct_up_toggles_pause_without_changing_candidate()
{
    DirectSongNavigation navigation(4, true);
    DirectNavigationChange change =
        navigation.update(upPress(), levels(false, true, true, true), 0);

    TEST_ASSERT_TRUE(change.playPauseChanged);
    TEST_ASSERT_FALSE(navigation.isPlaying());
    TEST_ASSERT_EQUAL_UINT8(4, navigation.candidateSong());
}

void test_direct_candidate_wraps_at_boundaries()
{
    DirectSongNavigation navigation(6, true);
    navigation.update(downPress(), levels(true, true, true, true), 0);
    TEST_ASSERT_EQUAL_UINT8(0, navigation.candidateSong());

    navigation.update(rightPress(), levels(true, true, true, false), 10);
    navigation.update(rightRelease(), levels(true, true, true, true), 100);
    TEST_ASSERT_EQUAL_UINT8(1, navigation.candidateSong());

    navigation.update(leftPress(), levels(true, false, true, true), 110);
    TEST_ASSERT_EQUAL_UINT8(0, navigation.candidateSong());
}

void test_playing_effect_stays_within_brightness_bounds()
{
    for (uint16_t phase = 0; phase < 256; phase += 7)
    {
        for (uint8_t index = 0; index < kMatrixEffectLedCount; ++index)
        {
            const MatrixEffectRgb color = led_matrix_color_for(
                LedMatrixEffectMode::Playing, index, phase);
            TEST_ASSERT_TRUE(color.r <= kMatrixPlayingMaximum);
            TEST_ASSERT_TRUE(color.g <= kMatrixPlayingMaximum);
            TEST_ASSERT_TRUE(color.b <= kMatrixPlayingMaximum);
        }
    }
}

void test_paused_effect_remains_dim_and_blue_weighted()
{
    for (uint16_t phase = 0; phase < 256; phase += 11)
    {
        const MatrixEffectRgb color = led_matrix_color_for(
            LedMatrixEffectMode::Paused, 4, phase);
        TEST_ASSERT_TRUE(color.r <= kMatrixPausedMaximum);
        TEST_ASSERT_TRUE(color.g <= kMatrixPausedMaximum);
        TEST_ASSERT_TRUE(color.b <= kMatrixPausedMaximum);
        TEST_ASSERT_TRUE(color.b >= color.r);
        TEST_ASSERT_TRUE(color.b >= color.g);
    }
}

void test_playing_pulse_changes_smoothly()
{
    uint8_t previous = led_matrix_pulse(0);
    for (uint16_t phase = 1; phase < 32; ++phase)
    {
        const uint8_t current = led_matrix_pulse(phase);
        const uint8_t difference = current > previous ?
            current - previous : previous - current;
        TEST_ASSERT_TRUE(difference <= 2);
        previous = current;
    }
    TEST_ASSERT_EQUAL_UINT8(0, led_matrix_pulse(0));
    TEST_ASSERT_EQUAL_UINT8(0, led_matrix_pulse(32));
}

void test_effect_modes_are_distinct()
{
    const MatrixEffectRgb playing = led_matrix_color_for(
        LedMatrixEffectMode::Playing, 0, 8);
    const MatrixEffectRgb paused = led_matrix_color_for(
        LedMatrixEffectMode::Paused, 0, 8);
    const MatrixEffectRgb confirmed = led_matrix_color_for(
        LedMatrixEffectMode::Confirmed, 0, 8);

    TEST_ASSERT_TRUE(
        playing.r != paused.r ||
        playing.g != paused.g ||
        playing.b != paused.b);
    TEST_ASSERT_TRUE(
        confirmed.r != paused.r ||
        confirmed.g != paused.g ||
        confirmed.b != paused.b);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_song_catalog_maps_the_eight_selectable_course_songs);
    RUN_TEST(test_initial_audio_gate_keeps_paused_song_silent);
    RUN_TEST(test_course_song_sequencer_reaches_every_declared_event);
    RUN_TEST(test_reference_timing_and_zero_note_rest);
    RUN_TEST(test_streaming_synth_emits_bounded_stereo_and_releases);
    RUN_TEST(test_song_player_period_conversion_and_assumed_duration);
    RUN_TEST(test_song_player_frequency_encoding_is_configurable);
    RUN_TEST(test_zero_note_is_a_duration_bearing_rest_by_default);
    RUN_TEST(test_null_arrays_are_rejected_for_nonempty_song);
    RUN_TEST(test_invalid_note_beat_and_nonfinite_values_are_rejected);
    RUN_TEST(test_invalid_tempo_and_duration_overflow_are_rejected);
    RUN_TEST(test_frequency_bounds_and_zero_policy_are_configurable);
    RUN_TEST(test_transition_gap_and_connected_mode);
    RUN_TEST(test_end_of_song_is_stable_and_reset_replays_within_bounds);
    RUN_TEST(test_empty_song_ends_without_dereferencing_arrays);
    RUN_TEST(test_pcm_capacity_stereo_alignment_and_exact_sample_counts);
    RUN_TEST(test_pcm_capacity_failure_does_not_overrun_buffer);
    RUN_TEST(test_pcm_rejects_invalid_events_and_invalid_configuration);
    RUN_TEST(test_pcm_44100_hz_rounding_and_tail_capacity_are_exact);
    RUN_TEST(test_pcm_end_event_appends_tail_once_and_rejects_later_events);
    RUN_TEST(test_fur_elise_first_fourteen_events_render_expected_duration);
    RUN_TEST(test_all_eight_raw_binary_combinations);
    RUN_TEST(test_low_low_low_selects_first_song);
    RUN_TEST(test_debouncer_emits_press_and_release_only_after_stability);
    RUN_TEST(test_selection_requires_capture_release_then_confirmation);
    RUN_TEST(test_selection_timeout_preserves_current_song);
    RUN_TEST(test_button_one_toggles_play_pause_without_selection_activity);
    RUN_TEST(test_high_high_high_can_be_captured_after_selection_mode_is_armed);
    RUN_TEST(test_timeout_in_confirmation_state);
    RUN_TEST(test_timeout_survives_counter_wraparound);
    RUN_TEST(test_stray_selection_button_does_not_change_later_play_pause);
    RUN_TEST(test_release_all_requires_debounced_stable_levels);
    RUN_TEST(test_all_high_selection_uses_button_one_long_press);
    RUN_TEST(test_direct_down_moves_candidate_by_one_row);
    RUN_TEST(test_direct_browsing_does_not_change_playback_state);
    RUN_TEST(test_direct_short_right_moves_across_without_confirming);
    RUN_TEST(test_direct_long_right_confirms_once);
    RUN_TEST(test_direct_up_toggles_pause_without_changing_candidate);
    RUN_TEST(test_direct_candidate_wraps_at_boundaries);
    RUN_TEST(test_playing_effect_stays_within_brightness_bounds);
    RUN_TEST(test_paused_effect_remains_dim_and_blue_weighted);
    RUN_TEST(test_playing_pulse_changes_smoothly);
    RUN_TEST(test_effect_modes_are_distinct);
    return UNITY_END();
}
