// SpotifyBridge.h
//
// This must match the NativeMediaInfo struct and exported function
// signatures from your SpotifyBridge C++/CLI project exactly.
// Copy this file (or keep them in sync) between the two projects.

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
