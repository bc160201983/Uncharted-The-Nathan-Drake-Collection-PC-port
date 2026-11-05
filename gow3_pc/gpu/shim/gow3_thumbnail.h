// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: a small picture of the game for the save list. The presenter scales a game frame down
// about once a second while the game plays (its camera hooks run: not in the pause menu or a
// cutscene), before the overlay is drawn; a save then gets the last one as thumb.png.

#pragma once

#include <cstdint>

namespace Gow3Thumbnail {

constexpr uint32_t Width = 456, Height = 256; // twice the save list's 228x128 picture

/// Present thread: whether to capture this frame (a second since the last request, and the
/// game's camera hooks ran in between).
bool Wanted();
/// Present thread: the captured picture, Width x Height RGBA8 rows top to bottom.
void Store(const unsigned char* rgba);
/// The game's save list ("Save game") was shown: the next save written within two minutes gets
/// the picture from before the pause menu (gow3gpu_thumbnail_pause).
void NoteSaveList();

} // namespace Gow3Thumbnail
