/*
OBS Spotify Plugin
Copyright (C) 2026 lingeriegoat https://github.com/lingeriegoat

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/


#pragma once
#include <cstdint>

extern "C" {

struct NativeMediaInfo {
	int64_t SongDurationTicks;
	int64_t CurrentPlaybackTimeTicks;
	char SongName[256];
	char ArtistName[256];
	char AlbumName[256];
	bool IsPlaying;
	bool HasTrack;
	uint8_t *ImageData; // raw encoded image bytes (png/jpg), owned by caller until freed
	int32_t ImageLength;
};

__declspec(dllimport) bool GetCurrentTrackNative(NativeMediaInfo *outInfo);
__declspec(dllimport) void FreeImageBuffer(uint8_t *buffer);
}
