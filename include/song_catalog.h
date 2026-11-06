#ifndef SONG_CATALOG_H
#define SONG_CATALOG_H

#include <stdint.h>
#include "song.h"

namespace song_catalog
{
constexpr uint8_t kSelectableSongCount = 8U;
constexpr uint8_t kCourseSongCount = 10U;
Song* get(uint8_t index);
Song* getCourseSong(uint8_t index);
bool allCourseLengthsMatch();
}

#endif
