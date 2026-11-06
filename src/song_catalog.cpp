#include "song_catalog.h"
#include "song_def.h"

static_assert(sizeof(Fur_Elise_note) / sizeof(Fur_Elise_note[0]) == 72U &&
              sizeof(Fur_Elise_beat) / sizeof(Fur_Elise_beat[0]) == 72U,
              "Fur Elise array length mismatch");
static_assert(sizeof(Canon_In_D_note) / sizeof(Canon_In_D_note[0]) == 88U &&
              sizeof(Canon_In_D_beat) / sizeof(Canon_In_D_beat[0]) == 88U,
              "Canon array length mismatch");
static_assert(sizeof(Minuet_In_G_major_note) / sizeof(Minuet_In_G_major_note[0]) == 90U &&
              sizeof(Minuet_In_G_major_beat) / sizeof(Minuet_In_G_major_beat[0]) == 90U,
              "Minuet array length mismatch");
static_assert(sizeof(Turkish_March_note) / sizeof(Turkish_March_note[0]) == 176U &&
              sizeof(Turkish_March_beat) / sizeof(Turkish_March_beat[0]) == 176U,
              "Turkish March array length mismatch");
static_assert(sizeof(Nocturne_in_E_flat_note) / sizeof(Nocturne_in_E_flat_note[0]) == 116U &&
              sizeof(Nocturne_in_E_flat_beat) / sizeof(Nocturne_in_E_flat_beat[0]) == 116U,
              "Nocturne E-flat array length mismatch");
static_assert(sizeof(Waltz_No2_note) / sizeof(Waltz_No2_note[0]) == 135U &&
              sizeof(Waltz_No2_beat) / sizeof(Waltz_No2_beat[0]) == 135U,
              "Waltz array length mismatch");
static_assert(sizeof(Nocturne_in_C_sharp_minor_note) / sizeof(Nocturne_in_C_sharp_minor_note[0]) == 64U &&
              sizeof(Nocturne_in_C_sharp_minor_beat) / sizeof(Nocturne_in_C_sharp_minor_beat[0]) == 64U,
              "Nocturne C-sharp array length mismatch");
static_assert(sizeof(Symphony_No40_note) / sizeof(Symphony_No40_note[0]) == 168U &&
              sizeof(Symphony_No40_beat) / sizeof(Symphony_No40_beat[0]) == 168U,
              "Symphony 40 array length mismatch");
static_assert(sizeof(Symphony_No5_note) / sizeof(Symphony_No5_note[0]) == 116U &&
              sizeof(Symphony_No5_beat) / sizeof(Symphony_No5_beat[0]) == 116U,
              "Symphony 5 array length mismatch");
static_assert(sizeof(Eine_Kleine_Nachtamusik_note) / sizeof(Eine_Kleine_Nachtamusik_note[0]) == 173U &&
              sizeof(Eine_Kleine_Nachtamusik_beat) / sizeof(Eine_Kleine_Nachtamusik_beat[0]) == 173U,
              "Eine Kleine array length mismatch");

namespace song_catalog
{
namespace
{
Song* const kSongs[kSelectableSongCount] = {
    &FUR_ELISE, &CANNON_IN_D, &MINUET_IN_G_MAJOR, &TURKISH_MARCH,
    &NOCTRUNE_IN_E_FLAT, &WALTZ_NO2, &NOCTRUNE_IN_C_SHARP_MAJOR,
    &SYMPHONY_NO40};
Song* const kCourseSongs[kCourseSongCount] = {
    &FUR_ELISE, &CANNON_IN_D, &MINUET_IN_G_MAJOR, &TURKISH_MARCH,
    &NOCTRUNE_IN_E_FLAT, &WALTZ_NO2, &NOCTRUNE_IN_C_SHARP_MAJOR,
    &SYMPHONY_NO40, &SYMPHONY_NO5, &EINE_KLEINE_NACHTAMUSIK};
}

Song* get(uint8_t index)
{
    return index < kSelectableSongCount ? kSongs[index] : nullptr;
}

Song* getCourseSong(uint8_t index)
{
    return index < kCourseSongCount ? kCourseSongs[index] : nullptr;
}

bool allCourseLengthsMatch()
{
    static const int expected[kCourseSongCount] = {
        72, 88, 90, 176, 116, 135, 64, 168, 116, 173};
    for (uint8_t index = 0; index < kCourseSongCount; ++index)
    {
        if (kCourseSongs[index] == nullptr ||
            kCourseSongs[index]->length != expected[index])
        {
            return false;
        }
    }
    return true;
}
} // namespace song_catalog
