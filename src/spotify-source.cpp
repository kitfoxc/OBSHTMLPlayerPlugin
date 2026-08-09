/*
OBS Now Playing Plugin
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

#include "spotify-source.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/bmem.h>

#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <gdiplus.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <random>
#include <cmath>
#include <memory>
#include <cstring>
#include <fstream>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

namespace {

constexpr int POLL_INTERVAL_MS = 250; // how often we ask SMTC for the current track; cheap local RPC, not a network call
constexpr int DEFAULT_CARD_W = 380;
constexpr int DEFAULT_CARD_H = 100;
constexpr int PAD = 12;
constexpr int MIN_TEXT_W = 20;
constexpr auto SCROLL_END_PAUSE = std::chrono::seconds(2);
constexpr int MIN_ART_SIZE = 10;
constexpr int DEFAULT_BACKGROUND_CORNER_RADIUS = 14;
constexpr int DEFAULT_ALBUM_ART_CORNER_RADIUS = 8;
constexpr int DEFAULT_BG_OPACITY = 70;
constexpr int DEFAULT_SCROLL_SPEED_MS = 500;
constexpr int DEFAULT_VU_UPDATE_MS = 100;
constexpr int DEFAULT_VU_WIDTH = 37;
constexpr int DEFAULT_VU_HEIGHT = 43;
constexpr int DEFAULT_VU_BAR_COUNT = 5;
constexpr int DEFAULT_VU_RANDOMNESS = 30;
constexpr int DEFAULT_TITLE_FONT_SIZE = 22;
constexpr int DEFAULT_ARTIST_FONT_SIZE = 20;
constexpr int DEFAULT_COLOR_WHITE = 0xFFFFFFFF;
constexpr int DEFAULT_COLOR_BLACK = 0x00000000;
constexpr int DEFAULT_COLOR_DARK_GREY = 0xFF5A5A5A;
constexpr int DEFAULT_COLOR_GREEN = 0xFF60D71E;

constexpr int VU_MAX_BAR_COUNT = 50;
constexpr int VU_BAR_GAP = 3;
constexpr int VU_GAP_BEFORE_TEXT = 10;

constexpr int DEFAULT_PROGRESS_BAR_HEIGHT = 6;
constexpr int DEFAULT_PROGRESS_BAR_GAP = 6; // gap between artist text and the bar
constexpr int PROGRESS_UPDATE_MS = 1000;    // how often the bar redraws while a track is loaded

constexpr int TRACK_CHANGE_TRANSITION_MS = 300;

constexpr int AUTOHIDE_FADE_MS = 1000;
constexpr int DEFAULT_AUTOHIDE_AFTER_S = 5;

constexpr int DEFAULT_NOT_PLAYING_AUTOHIDE_AFTER_S = 10;

ULONG_PTR g_gdiplusToken = 0;

std::wstring Utf8ToWide(const std::string &utf8)
{
	if (utf8.empty())
		return std::wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (len <= 0)
		return std::wstring();
	std::wstring out(len - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), len);
	return out;
}

// ---------------------------------------------------------------------
// Native SMTC reading (formerly SpotifyBridge.dll + SpotifyReader.dll)
//
// This block replaces the old C++/CLI bridge and its managed SpotifyReader.dll
// assembly. GlobalSystemMediaTransportControlsSessionManager is a WinRT type,
// and WinRT types are directly callable from plain native C++ via C++/WinRT
// (the <winrt/...> headers) -- no .NET runtime, no AssemblyResolve handler,
// and no separate DLL that can fail to load from the wrong directory.
// ---------------------------------------------------------------------

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackInfo;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionTimelineProperties;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::IRandomAccessStreamWithContentType;

struct NativeMediaInfo {
	bool HasTrack;
	int64_t SongDurationTicks;
	int64_t CurrentPlaybackTimeTicks;
	bool IsPlaying;
	char SongName[256];
	char ArtistName[256];
	char AlbumName[256];
	uint8_t *ImageData;
	int ImageLength;
};

const char *const kPossibleMusicSystems[] = {
	"spotify", "youtube", "ytm", "pear", "applemusic", "cider", "focal", "vlc",
};

// hstring -> UTF-8, into a fixed buffer. Required to decode unicode text
void CopyHstringToUtf8(const winrt::hstring &src, char *dst, int maxLen)
{
	if (maxLen <= 0)
		return;
	if (src.empty()) {
		dst[0] = '\0';
		return;
	}

	int needed = WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 0) {
		dst[0] = '\0';
		return;
	}

	std::vector<char> utf8((size_t)needed); // needed includes the terminating null
	WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, utf8.data(), needed, nullptr, nullptr);

	int copyLen = needed - 1; // exclude the null terminator itself from the length check
	if (copyLen > maxLen - 1)
		copyLen = maxLen - 1; // leave room for the null terminator

	if (copyLen > 0)
		memcpy(dst, utf8.data(), (size_t)copyLen);
	dst[copyLen] = '\0';
}

bool AppUserModelIdMatches(const winrt::hstring &appId, const char *needleUtf8)
{
	std::wstring haystack(appId.c_str());
	std::wstring needle = Utf8ToWide(needleUtf8);
	if (needle.empty())
		return false;

	auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
	return it != haystack.end();
}

void ReadThumbnail(const GlobalSystemMediaTransportControlsSessionMediaProperties &props, NativeMediaInfo *outInfo)
{
	auto thumbRef = props.Thumbnail();
	if (!thumbRef)
		return;

	IRandomAccessStreamWithContentType stream = thumbRef.OpenReadAsync().get();
	if (!stream)
		return;

	uint32_t size = (uint32_t)stream.Size();
	if (size == 0)
		return;

	DataReader reader(stream);
	reader.LoadAsync(size).get();

	std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]);
	reader.ReadBytes(winrt::array_view<uint8_t>(buffer.get(), buffer.get() + size));

	outInfo->ImageData = buffer.release();
	outInfo->ImageLength = (int)size;
}

bool GetCurrentTrackInternal(GlobalSystemMediaTransportControlsSessionManager const &manager, NativeMediaInfo *outInfo)
{
	auto sessions = manager.GetSessions();

	GlobalSystemMediaTransportControlsSession session = nullptr;
	uint32_t sessionCount = sessions.Size();
	for (const char *system : kPossibleMusicSystems) {
		for (uint32_t i = 0; i < sessionCount; i++) {
			GlobalSystemMediaTransportControlsSession s = sessions.GetAt(i);
			if (AppUserModelIdMatches(s.SourceAppUserModelId(), system)) {
				session = s;
				break;
			}
		}
		if (session)
			break;
	}

	if (!session)
		return false;

	GlobalSystemMediaTransportControlsSessionMediaProperties props = session.TryGetMediaPropertiesAsync().get();
	if (!props)
		return false;

	GlobalSystemMediaTransportControlsSessionTimelineProperties timeline = session.GetTimelineProperties();
	GlobalSystemMediaTransportControlsSessionPlaybackInfo playbackInfo = session.GetPlaybackInfo();

	outInfo->HasTrack = true;
	outInfo->SongDurationTicks = (timeline.EndTime() - timeline.StartTime()).count();
	outInfo->CurrentPlaybackTimeTicks = timeline.Position().count();
	outInfo->IsPlaying = playbackInfo.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

	CopyHstringToUtf8(props.Title(), outInfo->SongName, 256);
	CopyHstringToUtf8(props.Artist(), outInfo->ArtistName, 256);
	CopyHstringToUtf8(props.AlbumTitle(), outInfo->AlbumName, 256);

	ReadThumbnail(props, outInfo);

	return true;
}

// `manager` may be null if RequestAsync() hasn't succeeded yet -- poll_loop
// retries creating it every poll until it succeeds.
bool GetCurrentTrackNative(GlobalSystemMediaTransportControlsSessionManager const &manager, NativeMediaInfo *outInfo)
{
	if (outInfo == nullptr)
		return false;

	outInfo->SongDurationTicks = 0;
	outInfo->CurrentPlaybackTimeTicks = 0;
	outInfo->SongName[0] = '\0';
	outInfo->ArtistName[0] = '\0';
	outInfo->AlbumName[0] = '\0';
	outInfo->IsPlaying = false;
	outInfo->HasTrack = false;
	outInfo->ImageData = nullptr;
	outInfo->ImageLength = 0;

	if (!manager) {
		return false;
	}

	try {
		return GetCurrentTrackInternal(manager, outInfo);
	} catch (const winrt::hresult_error &ex) {
		blog(LOG_DEBUG, "[spotify_now_playing] SMTC read failed: %ls", ex.message().c_str());
		outInfo->HasTrack = false;
		return false;
	} catch (const std::exception &ex) {
		blog(LOG_DEBUG, "[spotify_now_playing] SMTC read failed: %s", ex.what());
		outInfo->HasTrack = false;
		return false;
	}
}

void FreeImageBuffer(uint8_t *buffer)
{
	delete[] buffer;
}

void AddRoundedRect(GraphicsPath &path, const Rect &r, int radius)
{
	int d = radius * 2;
	path.Reset();
	path.AddArc(r.X, r.Y, d, d, 180, 90);
	path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
	path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
	path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
	path.CloseFigure();
}

Color ObsColorToGdip(long long packed)
{
	uint32_t v = (uint32_t)packed;
	BYTE r = (BYTE)(v & 0xFF);
	BYTE g = (BYTE)((v >> 8) & 0xFF);
	BYTE b = (BYTE)((v >> 16) & 0xFF);
	BYTE a = (BYTE)((v >> 24) & 0xFF);
	return Color(a, r, g, b);
}

Color ObsColorToGdipWithAlpha(long long packed, int opacityPercent)
{
	uint32_t v = (uint32_t)packed;
	BYTE r = (BYTE)(v & 0xFF);
	BYTE g = (BYTE)((v >> 8) & 0xFF);
	BYTE b = (BYTE)((v >> 16) & 0xFF);
	int clampedPct = std::clamp(opacityPercent, 0, 100);
	BYTE a = (BYTE)((clampedPct * 255 + 50) / 100); // round to nearest
	return Color(a, r, g, b);
}

FontStyle ParseFontStyle(const std::string &style, int flags)
{
	// Prefer explicit OBS font flags when present.
	const int OBS_FONT_BOLD_FLAG = 1 << 0;
	const int OBS_FONT_ITALIC_FLAG = 1 << 1;
	if (flags & OBS_FONT_BOLD_FLAG && flags & OBS_FONT_ITALIC_FLAG)
		return FontStyleBoldItalic;
	if (flags & OBS_FONT_BOLD_FLAG)
		return FontStyleBold;
	if (flags & OBS_FONT_ITALIC_FLAG)
		return FontStyleItalic;

	bool bold = style.find("Bold") != std::string::npos;
	bool italic = style.find("Italic") != std::string::npos;
	if (bold && italic)
		return FontStyleBoldItalic;
	if (bold)
		return FontStyleBold;
	if (italic)
		return FontStyleItalic;
	return FontStyleRegular;
}

void DrawScrollableLine(Graphics &g, const std::wstring &text, Font &font, Brush &brush, const RectF &bounds, double scrollOffsetPx, bool centerWhenStatic, bool *outNeedsScroll, double *outAvgCharPx, double *outMaxOffsetPx)
{
	*outNeedsScroll = false;
	*outMaxOffsetPx = 0.0;
	if (text.empty())
		return;

	std::unique_ptr<StringFormat> sfClone(StringFormat::GenericTypographic()->Clone());
	StringFormat defaultFallback;
	StringFormat &sf = sfClone ? *sfClone : defaultFallback;
	sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);

	RectF measured;
	g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &sf, &measured);
	*outAvgCharPx = std::max(1.0, (double)measured.Width / (double)text.length());

	if (measured.Width <= bounds.Width) {
		if (centerWhenStatic)
			sf.SetAlignment(StringAlignmentCenter);
		sf.SetTrimming(StringTrimmingEllipsisCharacter); // safety net
		g.DrawString(text.c_str(), -1, &font, bounds, &sf, &brush);
		return;
	}

	*outNeedsScroll = true;
	*outMaxOffsetPx = (double)(measured.Width - bounds.Width);

	double offset = std::clamp(scrollOffsetPx, 0.0, *outMaxOffsetPx);

	Region savedClip;
	g.GetClip(&savedClip);
	g.SetClip(bounds);

	RectF r = bounds;
	r.X -= (REAL)offset;
	r.Width = measured.Width + 4.0f; // wide enough for the full text

	g.DrawString(text.c_str(), -1, &font, r, &sf, &brush);

	g.SetClip(&savedClip);
}

struct CachedFont {
	std::unique_ptr<Font> font;
	std::string face;
	std::string style;
	int size = -1;
	int flags = -1;
};

Font *EnsureFont(CachedFont &cache, const std::string &face, const std::string &style, int size, int flags)
{
	if (!cache.font || cache.face != face || cache.style != style || cache.size != size || cache.flags != flags) {
		FontFamily requestedFam(Utf8ToWide(face).c_str());
		const FontFamily *fam = &requestedFam;
		if (requestedFam.GetLastStatus() != Ok)
			fam = FontFamily::GenericSansSerif();

		FontStyle gdiStyle = ParseFontStyle(style, flags);
		cache.font = std::make_unique<Font>(fam, (REAL)size, gdiStyle, UnitPixel);

		cache.face = face;
		cache.style = style;
		cache.size = size;
		cache.flags = flags;
	}
	return cache.font.get();
}

void BlendPixelBuffers(const std::vector<uint8_t> &from, const std::vector<uint8_t> &to, std::vector<uint8_t> &out, double t)
{
	size_t n = std::min(from.size(), to.size());
	out.resize(n);
	int ti = (int)std::lround(std::clamp(t, 0.0, 1.0) * 255.0);
	for (size_t i = 0; i < n; i++) {
		int a = from[i];
		int b = to[i];
		out[i] = (uint8_t)(a + ((b - a) * ti) / 255);
	}
}

void ScaleAlphaChannel(std::vector<uint8_t> &pixels, float alpha)
{
	if (alpha >= 0.999f)
		return;
	int mul = std::clamp((int)std::lround(alpha * 255.0f), 0, 255);
	for (size_t i = 3; i < pixels.size(); i += 4)
		pixels[i] = (uint8_t)(((int)pixels[i] * mul) / 255);
}

} // namespace

struct spotify_source {
	obs_source_t *source = nullptr;

	std::thread poll_thread;
	std::atomic<bool> running{false};
	std::atomic<bool> is_active{false}; // true only while this source's scene is part of the live/program output

	std::mutex settings_mutex;
	long long title_color = DEFAULT_COLOR_WHITE;
	long long artist_color = DEFAULT_COLOR_WHITE;
	long long bg_color = 0;
	int bg_opacity = DEFAULT_BG_OPACITY; // percent, 0-100
	bool use_bg_image = false;
	std::string bg_image_path;
	int background_corner_radius = DEFAULT_BACKGROUND_CORNER_RADIUS;
	int album_art_corner_radius = DEFAULT_ALBUM_ART_CORNER_RADIUS;
	std::string title_font_face = "Segoe UI";
	std::string title_font_style = "Regular";
	int title_font_size = 16;
	int title_font_flags = 0;
	std::string artist_font_face = "Segoe UI";
	std::string artist_font_style = "Regular";
	int artist_font_size = 14;
	int artist_font_flags = 0;
	int card_w = DEFAULT_CARD_W;
	int card_h = DEFAULT_CARD_H;
	int text_offset_y = 0;
	int progress_bar_gap = DEFAULT_PROGRESS_BAR_GAP;
	int progress_bar_height = DEFAULT_PROGRESS_BAR_HEIGHT;
	int scroll_speed_ms = DEFAULT_SCROLL_SPEED_MS; // ms per letter for the marquee scroll
	bool vu_meter_enabled = true;
	long long vu_color = 0xFFFFFFFF;
	int vu_update_ms = 250;
	int vu_randomness = 50;
	int vu_width = 37;
	int vu_height = 43;
	int vu_bar_count = 5;
	bool vu_horizontal = false;
	bool vertical_layout = false;
	bool show_album_name = false;
	bool show_goat_placeholder = true;
	bool show_plugin_attribution = true;
	bool hide_album_art = false;
	bool show_progress_bar = true;
	long long progress_fill_color = DEFAULT_COLOR_WHITE;
	long long progress_bg_color = DEFAULT_COLOR_DARK_GREY;
	bool track_change_animation_enabled = true;
	bool autohide_enabled = false;
	int autohide_after_s = DEFAULT_AUTOHIDE_AFTER_S;
	bool autohide_when_not_playing = false;
	std::atomic<bool> settings_dirty{true};

	std::mutex bitmap_mutex;
	std::vector<uint8_t> pending_pixels;
	uint32_t pending_w = 0, pending_h = 0;
	std::atomic<bool> new_bitmap_ready{false};

	gs_texture_t *texture = nullptr;
	uint32_t tex_w = 0, tex_h = 0;

	std::string last_song;
	std::string last_artist;
	std::unique_ptr<Image> cached_art_image;
	std::vector<uint8_t> last_art_bytes; // raw bytes of the album art we last cached, for change detection
	bool have_track = false;

	bool title_needs_scroll = false;
	bool artist_needs_scroll = false;
	double title_scroll_px = 0.0;
	double artist_scroll_px = 0.0;
	double title_avg_char_px = 8.0;
	double artist_avg_char_px = 7.0;
	double title_scroll_max_px = 0.0;
	double artist_scroll_max_px = 0.0;
	bool title_scroll_paused_at_end = false;
	bool artist_scroll_paused_at_end = false;
	bool title_scroll_paused_at_start = false;
	bool artist_scroll_paused_at_start = false;
	std::chrono::steady_clock::time_point title_pause_start{};
	std::chrono::steady_clock::time_point artist_pause_start{};
	std::chrono::steady_clock::time_point last_scroll_tick{};

	double vu_bar_frac[VU_MAX_BAR_COUNT] = {0.0}; // 0..1, scaled to pixel height/length at draw time
	bool is_playing = false;
	bool vu_was_playing = false;
	std::chrono::steady_clock::time_point last_vu_tick{};
	std::mt19937 vu_rng{std::random_device{}()};

	int64_t song_duration_ticks = 0; // .NET TimeSpan ticks (100ns each)
	int64_t playback_position_ticks = 0;
	std::chrono::steady_clock::time_point position_sample_time{};
	std::chrono::steady_clock::time_point last_progress_tick{};
	int64_t max_displayed_position_ticks = 0;

	std::unique_ptr<Image> goat_image;
	bool goat_image_load_attempted = false;

	std::unique_ptr<Image> cached_bg_image;
	std::string cached_bg_image_path;

	std::unique_ptr<Bitmap> cached_bitmap;
	int cached_bitmap_w = 0;
	int cached_bitmap_h = 0;

	CachedFont title_font_cache;
	CachedFont artist_font_cache;

	bool transition_active = false;
	std::vector<uint8_t> transition_from_pixels;
	std::vector<uint8_t> transition_to_pixels;
	uint32_t transition_w = 0, transition_h = 0;
	std::chrono::steady_clock::time_point transition_start{};

	float autohide_alpha = 1.0f;
	std::chrono::steady_clock::time_point autohide_reference_time{};
	std::chrono::steady_clock::time_point last_autohide_tick{};

	std::chrono::steady_clock::time_point last_playing_time{};
};

struct AppearanceSettings {
	long long title_color;
	long long artist_color;
	long long bg_color;
	int bg_opacity;
	bool use_bg_image;
	std::string bg_image_path;
	int background_corner_radius;
	int album_art_corner_radius;
	std::string title_font_face;
	std::string title_font_style;
	int title_font_size;
	int title_font_flags;
	std::string artist_font_face;
	std::string artist_font_style;
	int artist_font_size;
	int artist_font_flags;
	int card_w;
	int card_h;
	int text_offset_y;
	int progress_bar_gap;
	int progress_bar_height;
	int scroll_speed_ms;
	bool vu_meter_enabled;
	long long vu_color;
	int vu_update_ms;
	int vu_randomness;
	int vu_width;
	int vu_height;
	int vu_bar_count;
	bool vu_horizontal;
	bool vertical_layout;
	bool show_album_name;
	bool show_goat_placeholder;
	bool show_plugin_attribution;
	bool hide_album_art;
	bool show_progress_bar;
	long long progress_fill_color;
	long long progress_bg_color;
	bool track_change_animation_enabled;
	bool autohide_enabled;
	int autohide_after_s;
	bool autohide_when_not_playing;
};

static void DrawVuMeter(Graphics &g, spotify_source *ctx, const AppearanceSettings &s, const Rect &blockRect)
{
	if (!s.vu_meter_enabled || blockRect.Width <= 0 || blockRect.Height <= 0)
		return;

	int barCount = std::clamp(s.vu_bar_count, 1, VU_MAX_BAR_COUNT);
	Color vuColor = ObsColorToGdip(s.vu_color);
	SolidBrush vuBrush(vuColor);

	int totalGap = (barCount - 1) * VU_BAR_GAP;

	if (!s.vu_horizontal) {
		int barThickness = std::max(1, (blockRect.Width - totalGap) / barCount);
		int baselineY = blockRect.Y + blockRect.Height;

		for (int i = 0; i < barCount; i++) {
			double frac = std::clamp(ctx->vu_bar_frac[i], 0.0, 1.0);
			int barH = (int)std::lround(2.0 + frac * (double)(blockRect.Height - 2));
			if (barH < 2)
				barH = 2;
			int barX = blockRect.X + i * (barThickness + VU_BAR_GAP);
			int barY = baselineY - barH;

			Rect barRect(barX, barY, barThickness, barH);
			GraphicsPath barPath;
			AddRoundedRect(barPath, barRect, std::min(2, barThickness / 2));
			g.FillPath(&vuBrush, &barPath);
		}
	} else {
		int barThickness = std::max(1, (blockRect.Height - totalGap) / barCount);

		for (int i = 0; i < barCount; i++) {
			double frac = std::clamp(ctx->vu_bar_frac[i], 0.0, 1.0);
			int barLen = (int)std::lround(2.0 + frac * (double)(blockRect.Width - 2));
			if (barLen < 2)
				barLen = 2;
			int barY = blockRect.Y + i * (barThickness + VU_BAR_GAP);
			int barX = blockRect.X;

			Rect barRect(barX, barY, barLen, barThickness);
			GraphicsPath barPath;
			AddRoundedRect(barPath, barRect, std::min(2, barThickness / 2));
			g.FillPath(&vuBrush, &barPath);
		}
	}
}

static void DrawProgressBar(Graphics &g, spotify_source *ctx, const AppearanceSettings &s, const Rect &barRect)
{
	if (!s.show_progress_bar || barRect.Width <= 0 || barRect.Height <= 0)
		return;

	double frac = 0.0;
	if (ctx->song_duration_ticks > 0) {
		int64_t elapsedTicks = ctx->playback_position_ticks;
		if (ctx->is_playing) {
			double elapsedSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - ctx->position_sample_time).count();
			elapsedTicks += (int64_t)(elapsedSeconds * 1.0e7); // 1 tick = 100ns
		}

		// Never let the displayed position move backward
		if (elapsedTicks < ctx->max_displayed_position_ticks)
			elapsedTicks = ctx->max_displayed_position_ticks;
		else
			ctx->max_displayed_position_ticks = elapsedTicks;

		frac = std::clamp((double)elapsedTicks / (double)ctx->song_duration_ticks, 0.0, 1.0);
	}

	Color bgColor = ObsColorToGdip(s.progress_bg_color);
	SolidBrush bgBrush(bgColor);
	GraphicsPath bgPath;
	AddRoundedRect(bgPath, barRect, barRect.Height / 2);
	g.FillPath(&bgBrush, &bgPath);

	int fillWidth = (int)std::lround(barRect.Width * frac);
	if (fillWidth > 0) {
		Rect fillRect(barRect.X, barRect.Y, fillWidth, barRect.Height);
		Color fillColor = ObsColorToGdip(s.progress_fill_color);
		SolidBrush fillBrush(fillColor);
		GraphicsPath fillPath;
		AddRoundedRect(fillPath, fillRect, barRect.Height / 2);
		g.FillPath(&fillBrush, &fillPath);
	}
}

static Image *GetGoatImage(spotify_source *ctx)
{
	if (ctx->goat_image_load_attempted)
		return ctx->goat_image.get();

	ctx->goat_image_load_attempted = true;

	char *path = obs_module_file("goat.png");
	if (!path)
		return nullptr;

	std::wstring wpath = Utf8ToWide(path);
	bfree(path);

	auto img = std::make_unique<Image>(wpath.c_str());
	if (img->GetLastStatus() != Ok)
		return nullptr;

	ctx->goat_image = std::move(img);
	return ctx->goat_image.get();
}

static Image *EnsureBackgroundImage(spotify_source *ctx, const std::string &path)
{
	if (path.empty()) {
		ctx->cached_bg_image.reset();
		ctx->cached_bg_image_path.clear();
		return nullptr;
	}

	if (ctx->cached_bg_image && ctx->cached_bg_image_path == path)
		return ctx->cached_bg_image.get();

	ctx->cached_bg_image.reset();
	ctx->cached_bg_image_path.clear();

	std::wstring wpath = Utf8ToWide(path);

	std::ifstream file(wpath, std::ios::binary | std::ios::ate);
	if (!file)
		return nullptr;

	std::streamsize fileSize = file.tellg();
	if (fileSize <= 0)
		return nullptr;
	file.seekg(0, std::ios::beg);

	std::vector<uint8_t> bytes((size_t)fileSize);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), fileSize))
		return nullptr;
	file.close();

	IStream *stream = SHCreateMemStream(bytes.data(), (UINT)bytes.size());
	if (!stream)
		return nullptr;

	auto img = std::make_unique<Image>(stream);
	stream->Release();

	if (img->GetLastStatus() != Ok)
		return nullptr;

	// Clone so the cached image no longer depends on the (already-released) stream.
	auto cloned = std::unique_ptr<Image>(img->Clone());
	if (!cloned || cloned->GetLastStatus() != Ok)
		return nullptr;

	ctx->cached_bg_image = std::move(cloned);
	ctx->cached_bg_image_path = path;
	return ctx->cached_bg_image.get();
}

static bool ArtBytesDiffer(const std::vector<uint8_t> &cached, const uint8_t *image_data, int image_len)
{
	if (image_data == nullptr || image_len <= 0)
		return !cached.empty();
	if (cached.size() != (size_t)image_len)
		return true;
	return memcmp(cached.data(), image_data, (size_t)image_len) != 0;
}

static void UpdateCachedArt(spotify_source *ctx, const uint8_t *image_data, int image_len)
{
	ctx->cached_art_image.reset();
	if (image_data == nullptr || image_len <= 0) {
		ctx->last_art_bytes.clear();
		return;
	}

	IStream *stream = SHCreateMemStream(image_data, (UINT)image_len);
	if (!stream)
		return;

	auto img = std::make_unique<Image>(stream);
	stream->Release();

	if (img->GetLastStatus() != Ok)
		return;

	auto cloned = std::unique_ptr<Image>(img->Clone());
	if (!cloned || cloned->GetLastStatus() != Ok)
		return;

	ctx->cached_art_image = std::move(cloned);
	ctx->last_art_bytes.assign(image_data, image_data + image_len);
}

static void compose_bitmap(spotify_source *ctx, const std::string &title, const std::string &artist, const AppearanceSettings &s, bool settings_changed = false)
{
	const int cardW = std::max(s.card_w, 50);
	const int cardH = std::max(s.card_h, 30);

	if (!ctx->cached_bitmap || ctx->cached_bitmap_w != cardW || ctx->cached_bitmap_h != cardH) {
		ctx->cached_bitmap = std::make_unique<Bitmap>(cardW, cardH, PixelFormat32bppARGB);
		ctx->cached_bitmap_w = cardW;
		ctx->cached_bitmap_h = cardH;
	}
	Bitmap &card = *ctx->cached_bitmap;
	Graphics g(&card);

	g.SetSmoothingMode(SmoothingModeHighQuality);
	g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(Color(0, 0, 0, 0));

	// card background
	GraphicsPath bgPath;
	AddRoundedRect(bgPath, Rect(0, 0, cardW, cardH), s.background_corner_radius);

	Image *bgImage = nullptr;
	if (s.use_bg_image) {
		if (!ctx->cached_bg_image || settings_changed)
			bgImage = EnsureBackgroundImage(ctx, s.bg_image_path);
		else
			bgImage = ctx->cached_bg_image.get();
	}
	if (bgImage) {
		Region savedBgClip;
		g.GetClip(&savedBgClip);
		g.SetClip(&bgPath);

		UINT imgW = bgImage->GetWidth();
		UINT imgH = bgImage->GetHeight();
		REAL srcW = (REAL)std::min<UINT>(imgW, (UINT)cardW);
		REAL srcH = (REAL)std::min<UINT>(imgH, (UINT)cardH);

		ImageAttributes bgAttr;
		if (s.bg_opacity < 100) {
			REAL a = std::clamp(s.bg_opacity, 0, 100) / 100.0f;
			Gdiplus::ColorMatrix cm = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, a, 0, 0, 0, 0, 0, 1};
			bgAttr.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
		}

		// Draw the top-left crop of the source image 1:1 (no scaling) into the card
		RectF bgDestRect(0.0f, 0.0f, srcW, srcH);
		g.DrawImage(bgImage, bgDestRect, 0.0f, 0.0f, srcW, srcH, UnitPixel, &bgAttr);

		g.SetClip(&savedBgClip);
	} else {
		SolidBrush bgBrush(ObsColorToGdipWithAlpha(s.bg_color, s.bg_opacity));
		g.FillPath(&bgBrush, &bgPath);
	}

	int titleSize = s.title_font_size > 0 ? s.title_font_size : DEFAULT_TITLE_FONT_SIZE;
	int artistSize = s.artist_font_size > 0 ? s.artist_font_size : DEFAULT_ARTIST_FONT_SIZE;

	Font &titleFont = *EnsureFont(ctx->title_font_cache, s.title_font_face, s.title_font_style, titleSize, s.title_font_flags);
	Font &artistFont = *EnsureFont(ctx->artist_font_cache, s.artist_font_face, s.artist_font_style, artistSize, s.artist_font_flags);

	Color titleColor = ObsColorToGdip(s.title_color);
	Color artistColor = ObsColorToGdip(s.artist_color);
	SolidBrush titleBrush(titleColor);
	SolidBrush artistBrush(artistColor);

	int titleLineH = titleSize + 10;
	int artistLineH = artistSize + 8;
	int progressH = s.show_progress_bar ? (s.progress_bar_gap + s.progress_bar_height) : 0;
	int blockH = titleLineH + artistLineH + progressH;

	int artSize = 0;
	Rect artRect;
	RectF titleRect, artistRect;
	bool centerText = false;
	Rect vuBlockRect(0, 0, 0, 0);
	Rect progressBarRect(0, 0, 0, 0);

	bool showArt = !s.hide_album_art;

	if (s.vertical_layout) {
		constexpr int GAP_ART_TEXT = 14;
		constexpr int GAP_TEXT_VU = VU_GAP_BEFORE_TEXT;

		int textW = cardW - PAD * 2;
		if (textW < MIN_TEXT_W)
			textW = MIN_TEXT_W;
		int textX = PAD;
		int textTop;

		if (showArt) {
			int maxArtByWidth = cardW - PAD * 2;
			if (maxArtByWidth < MIN_ART_SIZE)
				maxArtByWidth = MIN_ART_SIZE;

			int reservedNonArt = PAD * 2 + GAP_ART_TEXT + blockH + (s.vu_meter_enabled ? (GAP_TEXT_VU + s.vu_height) : 0);
			artSize = cardH - reservedNonArt;
			if (artSize > maxArtByWidth)
				artSize = maxArtByWidth;
			if (artSize < MIN_ART_SIZE)
				artSize = MIN_ART_SIZE;

			int artX = (cardW - artSize) / 2;
			int artY = PAD;
			artRect = Rect(artX, artY, artSize, artSize);

			textTop = artY + artSize + GAP_ART_TEXT + s.text_offset_y;
		} else {
			artSize = 0;
			artRect = Rect(0, 0, 0, 0);
			textTop = PAD + s.text_offset_y;
		}

		titleRect = RectF((REAL)textX, (REAL)textTop, (REAL)textW, (REAL)titleLineH);
		artistRect = RectF((REAL)textX, (REAL)(textTop + titleLineH), (REAL)textW, (REAL)artistLineH);

		if (s.show_progress_bar) {
			int progressY = textTop + titleLineH + artistLineH + s.progress_bar_gap;
			progressBarRect = Rect(textX, progressY, textW, s.progress_bar_height);
		}

		if (s.vu_meter_enabled) {
			int vuTop = textTop + blockH + GAP_TEXT_VU;
			int vuLeft = (cardW - s.vu_width) / 2;
			vuBlockRect = Rect(vuLeft, vuTop, s.vu_width, s.vu_height);
		}

		centerText = true;
	} else {
		int textX;

		if (showArt) {
			artSize = cardH - PAD * 2;
			if (artSize < MIN_ART_SIZE)
				artSize = MIN_ART_SIZE;
			int maxArtForWidth = cardW - PAD * 2 - MIN_TEXT_W;
			if (artSize > maxArtForWidth)
				artSize = std::max(MIN_ART_SIZE, maxArtForWidth);

			artRect = Rect(PAD, PAD, artSize, artSize);
			textX = PAD + artSize + 14;
		} else {
			artSize = 0;
			artRect = Rect(0, 0, 0, 0);
			textX = PAD;
		}

		int vuBlockWidthReserved = s.vu_meter_enabled ? (s.vu_width + VU_GAP_BEFORE_TEXT) : 0;
		int textW = cardW - textX - PAD - vuBlockWidthReserved;
		if (textW < MIN_TEXT_W)
			textW = MIN_TEXT_W;

		int topY = (cardH - blockH) / 2 + s.text_offset_y;
		titleRect = RectF((REAL)textX, (REAL)topY, (REAL)textW, (REAL)titleLineH);
		artistRect = RectF((REAL)textX, (REAL)(topY + titleLineH), (REAL)textW, (REAL)artistLineH);

		if (s.show_progress_bar) {
			int progressY = topY + titleLineH + artistLineH + s.progress_bar_gap;
			progressBarRect = Rect(textX, progressY, textW, s.progress_bar_height);
		}

		if (s.vu_meter_enabled) {
			int vuRight = cardW - PAD;
			int vuLeft = vuRight - s.vu_width;
			int vuTop = (cardH - s.vu_height) / 2; // block vertically centered
			vuBlockRect = Rect(vuLeft, vuTop, s.vu_width, s.vu_height);
		}

		centerText = false;
	}

	if (showArt) {
		GraphicsPath artClip;
		AddRoundedRect(artClip, artRect, s.album_art_corner_radius);

		Region savedClip;
		g.GetClip(&savedClip);
		g.SetClip(&artClip);

		bool drewArt = false;
		if (ctx->cached_art_image) {
			g.DrawImage(ctx->cached_art_image.get(), artRect);
			drewArt = true;
		}
		if (!drewArt && s.show_goat_placeholder) {
			Image *goat = GetGoatImage(ctx);
			if (goat) {
				g.DrawImage(goat, artRect);
				drewArt = true;
			}
		}
		if (!drewArt) {
			SolidBrush placeholder(Color(255, 55, 55, 60));
			g.FillRectangle(&placeholder, artRect);
		}
		g.SetClip(&savedClip);
	}

	// text (shared drawing code)
	static const std::string kAttributionTitle = "NowPlayingWidget by lingeriegoat";
	static const std::string kAttributionArtist = "Play some music to get started";
	bool useAttribution = !ctx->have_track && s.show_plugin_attribution;
	const std::string &displayTitle = useAttribution ? kAttributionTitle : title;
	const std::string &displayArtist = useAttribution ? kAttributionArtist : artist;

	std::wstring wtitle = Utf8ToWide(displayTitle);
	std::wstring wartist = Utf8ToWide(displayArtist);

	bool titleScroll = false, artistScroll = false;
	double titleAvgChar = ctx->title_avg_char_px, artistAvgChar = ctx->artist_avg_char_px;
	double titleMaxOffset = ctx->title_scroll_max_px, artistMaxOffset = ctx->artist_scroll_max_px;
	DrawScrollableLine(g, wtitle, titleFont, titleBrush, titleRect, ctx->title_scroll_px, centerText, &titleScroll, &titleAvgChar, &titleMaxOffset);
	DrawScrollableLine(g, wartist, artistFont, artistBrush, artistRect, ctx->artist_scroll_px, centerText, &artistScroll, &artistAvgChar, &artistMaxOffset);
	ctx->title_needs_scroll = titleScroll;
	ctx->artist_needs_scroll = artistScroll;
	ctx->title_avg_char_px = titleAvgChar;
	ctx->artist_avg_char_px = artistAvgChar;
	ctx->title_scroll_max_px = titleMaxOffset;
	ctx->artist_scroll_max_px = artistMaxOffset;

	// VU meter (shared drawing code)
	DrawVuMeter(g, ctx, s, vuBlockRect);

	// Progress bar
	DrawProgressBar(g, ctx, s, progressBarRect);

	BitmapData bd;
	Rect full(0, 0, cardW, cardH);
	if (card.LockBits(&full, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
		return;

	std::vector<uint8_t> buf((size_t)cardW * cardH * 4);
	const uint8_t *src = (const uint8_t *)bd.Scan0;
	for (int y = 0; y < cardH; y++)
		memcpy(buf.data() + (size_t)y * cardW * 4, src + (size_t)y * bd.Stride, (size_t)cardW * 4);
	card.UnlockBits(&bd);

	ScaleAlphaChannel(buf, ctx->autohide_alpha);

	{
		std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
		ctx->pending_pixels = std::move(buf);
		ctx->pending_w = (uint32_t)cardW;
		ctx->pending_h = (uint32_t)cardH;
	}
	ctx->new_bitmap_ready = true;
}

static AppearanceSettings snapshot_settings(spotify_source *ctx)
{
	std::lock_guard<std::mutex> lock(ctx->settings_mutex);
	return AppearanceSettings
	{
		ctx->title_color, 
		ctx->artist_color, 
		ctx->bg_color, 
		ctx->bg_opacity, 
		ctx->use_bg_image, 
		ctx->bg_image_path, 
		ctx->background_corner_radius, 
		ctx->album_art_corner_radius, 
		ctx->title_font_face, 
		ctx->title_font_style, 
		ctx->title_font_size, 
		ctx->title_font_flags, 
		ctx->artist_font_face, 
		ctx->artist_font_style, 
		ctx->artist_font_size, 
		ctx->artist_font_flags, 
		ctx->card_w, 
		ctx->card_h, 
		ctx->text_offset_y, 
		ctx->progress_bar_gap, 
		ctx->progress_bar_height, 
		ctx->scroll_speed_ms, 
		ctx->vu_meter_enabled, 
		ctx->vu_color, 
		ctx->vu_update_ms, 
		ctx->vu_randomness, 
		ctx->vu_width, 
		ctx->vu_height, 
		ctx->vu_bar_count, 
		ctx->vu_horizontal, 
		ctx->vertical_layout, 
		ctx->show_album_name, 
		ctx->show_goat_placeholder, 
		ctx->show_plugin_attribution, 
		ctx->hide_album_art, 
		ctx->show_progress_bar, 
		ctx->progress_fill_color, 
		ctx->progress_bg_color, 
		ctx->track_change_animation_enabled, 
		ctx->autohide_enabled, 
		ctx->autohide_after_s, 
		ctx->autohide_when_not_playing
	};
}

static bool UpdateAutohideAlpha(spotify_source *ctx, const AppearanceSettings &s, std::chrono::steady_clock::time_point now)
{
	double dtMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->last_autohide_tick).count();
	ctx->last_autohide_tick = now;
	if (dtMs <= 0.0 || dtMs > 2000.0) // first call, or a long gap since the last tick (e.g. the card was hidden)
		dtMs = 50.0;

	float target = 1.0f;

	// "Autohide after track change": fades out a fixed, user-configurable delay after the
	// source became active / the last track change, regardless of playback state.
	if (s.autohide_enabled) {
		if (obs_source_active(ctx->source)) {
			double elapsedMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->autohide_reference_time).count();
			double delayMs = (double)std::max(0, s.autohide_after_s) * 1000.0;
			if (elapsedMs >= delayMs)
				target = 0.0f;
		}
	}

	// "Autohide when music is not playing": fades out a fixed 10s after playback was last
	// observed to be playing -- covers both paused/stopped tracks and no track/session at all.
	if (s.autohide_when_not_playing) {
		double notPlayingElapsedMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->last_playing_time).count();
		double notPlayingDelayMs = (double)DEFAULT_NOT_PLAYING_AUTOHIDE_AFTER_S * 1000.0;
		if (notPlayingElapsedMs >= notPlayingDelayMs)
			target = 0.0f;
	}

	if (ctx->autohide_alpha == target)
		return false;

	double step = dtMs / (double)AUTOHIDE_FADE_MS;
	if (target > ctx->autohide_alpha)
		ctx->autohide_alpha = (float)std::min((double)target, (double)ctx->autohide_alpha + step);
	else
		ctx->autohide_alpha = (float)std::max((double)target, (double)ctx->autohide_alpha - step);

	return true;
}

static void poll_loop(spotify_source *ctx)
{
	bool com_initialized = false;
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		com_initialized = true;
	} catch (const winrt::hresult_error &) {
		com_initialized = false;
	}

	//manager must be created inside poll_loop so OBS exits cleanly. Its a com apartment context thing.
	GlobalSystemMediaTransportControlsSessionManager sessionManager = nullptr;

	//Hold the last good bitmap for 2 seconds as some clients drop their session during track skip
	constexpr auto MISSING_SESSION_GRACE = std::chrono::seconds(2);
	bool gap_active = false;
	std::chrono::steady_clock::time_point gap_start{};

	while (ctx->running) {
		if (!ctx->is_active) {
			if (ctx->settings_dirty) {
				compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx));
				ctx->settings_dirty = false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			continue;
		}

		if (!sessionManager && com_initialized) {
			try {
				sessionManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
			} catch (const winrt::hresult_error &) {
				sessionManager = nullptr;
			}
		}

		NativeMediaInfo info{};
		bool has = GetCurrentTrackNative(sessionManager, &info);

		std::string title = has ? std::string(info.SongName) : std::string();
		std::string artist = has ? std::string(info.ArtistName) : std::string();

		bool show_album_name;
		{
			std::lock_guard<std::mutex> lock(ctx->settings_mutex);
			show_album_name = ctx->show_album_name;
		}
		if (show_album_name) {
			std::string albumName = " - " + std::string(info.AlbumName);
			artist.append(albumName);
		}

		bool track_changed = (has != ctx->have_track) || (title != ctx->last_song) || (artist != ctx->last_artist);

		if (has) {
			gap_active = false;
			ctx->is_playing = info.IsPlaying;
			auto now = std::chrono::steady_clock::now();

			if (ctx->is_playing) {
				ctx->last_playing_time = now;
			}

			bool positionChanged = (info.CurrentPlaybackTimeTicks != ctx->playback_position_ticks) || (info.SongDurationTicks != ctx->song_duration_ticks);
			if (track_changed || positionChanged) {
				ctx->song_duration_ticks = info.SongDurationTicks;
				ctx->playback_position_ticks = info.CurrentPlaybackTimeTicks;
				ctx->position_sample_time = now;
			}

			if (track_changed) {
				UpdateCachedArt(ctx, info.ImageData, info.ImageLength);

				ctx->last_song = title;
				ctx->last_artist = artist;
				ctx->have_track = true;
				ctx->max_displayed_position_ticks = 0;
				ctx->autohide_reference_time = now;

				ctx->title_scroll_px = 0.0; // New track -- restart the marquee from the beginning.
				ctx->artist_scroll_px = 0.0;
				ctx->title_scroll_paused_at_end = false;
				ctx->artist_scroll_paused_at_end = false;
				ctx->title_scroll_paused_at_start = true;
				ctx->artist_scroll_paused_at_start = true;
				ctx->title_pause_start = now;
				ctx->artist_pause_start = now;
				ctx->last_scroll_tick = std::chrono::steady_clock::now();

				AppearanceSettings snap = snapshot_settings(ctx);

				std::vector<uint8_t> fromPixels;
				uint32_t fromW = 0, fromH = 0;
				{
					std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
					fromPixels = ctx->pending_pixels;
					fromW = ctx->pending_w;
					fromH = ctx->pending_h;
				}

				compose_bitmap(ctx, title, artist, snap);

				if (snap.track_change_animation_enabled && !fromPixels.empty()) {
					std::vector<uint8_t> toPixels;
					uint32_t toW = 0, toH = 0;
					{
						std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
						toPixels = ctx->pending_pixels;
						toW = ctx->pending_w;
						toH = ctx->pending_h;
					}

					if (fromW == toW && fromH == toH) {
						ctx->transition_from_pixels = std::move(fromPixels);
						ctx->transition_to_pixels = std::move(toPixels);
						ctx->transition_w = toW;
						ctx->transition_h = toH;
						ctx->transition_start = now;
						ctx->transition_active = true;

						std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
						ctx->pending_pixels = ctx->transition_from_pixels;
						ctx->pending_w = fromW;
						ctx->pending_h = fromH;
						ctx->new_bitmap_ready = true;
					}
				}
			} else if (ArtBytesDiffer(ctx->last_art_bytes, info.ImageData, info.ImageLength)) {
				UpdateCachedArt(ctx, info.ImageData, info.ImageLength);
				compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx));
			} else if (ctx->settings_dirty) {
				ctx->title_scroll_px = 0.0;
				ctx->artist_scroll_px = 0.0;
				ctx->title_scroll_paused_at_end = false;
				ctx->artist_scroll_paused_at_end = false;
				ctx->title_scroll_paused_at_start = true;
				ctx->artist_scroll_paused_at_start = true;
				ctx->title_pause_start = now;
				ctx->artist_pause_start = now;
				compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx), /*settings_changed=*/true);
			}
		} else if (ctx->have_track) {
			ctx->is_playing = false;

			if (!gap_active) {
				gap_active = true;
				gap_start = std::chrono::steady_clock::now();
			}

			if (std::chrono::steady_clock::now() - gap_start >= MISSING_SESSION_GRACE) {
				ctx->last_song.clear();
				ctx->last_artist.clear();
				UpdateCachedArt(ctx, nullptr, 0);
				ctx->song_duration_ticks = 0;
				ctx->playback_position_ticks = 0;
				ctx->max_displayed_position_ticks = 0;
				ctx->have_track = false;
				ctx->autohide_reference_time = std::chrono::steady_clock::now();
				gap_active = false;
				ctx->title_scroll_px = 0.0;
				ctx->artist_scroll_px = 0.0;
				ctx->title_scroll_paused_at_end = false;
				ctx->artist_scroll_paused_at_end = false;
				ctx->title_scroll_paused_at_start = true;
				ctx->artist_scroll_paused_at_start = true;
				ctx->title_pause_start = std::chrono::steady_clock::now();
				ctx->artist_pause_start = std::chrono::steady_clock::now();
				compose_bitmap(ctx, "", "", snapshot_settings(ctx));
			}
		} else if (ctx->settings_dirty) {
			compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx), /*settings_changed=*/true);
		}
		ctx->settings_dirty = false;

		if (has && info.ImageData != nullptr)
			FreeImageBuffer(info.ImageData);

		AppearanceSettings s = snapshot_settings(ctx);

		for (int waited = 0; waited < POLL_INTERVAL_MS && ctx->running; waited += 50) {
			if (ctx->settings_dirty)
				break; // let the outer loop apply the appearance change immediately

			if (ctx->have_track || s.show_plugin_attribution || s.autohide_when_not_playing) {
				auto now = std::chrono::steady_clock::now();

				bool autohideChanged = UpdateAutohideAlpha(ctx, s, now);

				if (ctx->transition_active) {
					double elapsedMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->transition_start).count();
					double t = elapsedMs / (double)TRACK_CHANGE_TRANSITION_MS;

					std::vector<uint8_t> blended;
					BlendPixelBuffers(ctx->transition_from_pixels, ctx->transition_to_pixels, blended, t);
					ScaleAlphaChannel(blended, ctx->autohide_alpha);

					{
						std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
						ctx->pending_pixels = std::move(blended);
						ctx->pending_w = ctx->transition_w;
						ctx->pending_h = ctx->transition_h;
					}
					ctx->new_bitmap_ready = true;

					if (t >= 1.0) {
						ctx->transition_active = false;
						ctx->transition_from_pixels.clear();
						ctx->transition_to_pixels.clear();
					}
				} else {
					bool needCompose = autohideChanged;

					if (ctx->title_needs_scroll || ctx->artist_needs_scroll) {
						if (now - ctx->last_scroll_tick >= std::chrono::milliseconds(s.scroll_speed_ms)) {
							ctx->last_scroll_tick = now;

							if (ctx->title_needs_scroll) {
								if (ctx->title_scroll_paused_at_end || ctx->title_scroll_paused_at_start) {
									if (now - ctx->title_pause_start >= SCROLL_END_PAUSE) {
										if (ctx->title_scroll_paused_at_end) {
											ctx->title_scroll_px = 0.0;
											ctx->title_scroll_paused_at_end = false;
											ctx->title_scroll_paused_at_start = true;
											ctx->title_pause_start = now;
											needCompose = true;
										} else {
											ctx->title_scroll_paused_at_start = false;
											needCompose = true;
										}
									}
								} else {
									ctx->title_scroll_px += ctx->title_avg_char_px;
									if (ctx->title_scroll_px >= ctx->title_scroll_max_px) {
										ctx->title_scroll_px = ctx->title_scroll_max_px;
										ctx->title_scroll_paused_at_end = true;
										ctx->title_pause_start = now;
									}
									needCompose = true;
								}
							}

							if (ctx->artist_needs_scroll) {
								if (ctx->artist_scroll_paused_at_end || ctx->artist_scroll_paused_at_start) {
									if (now - ctx->artist_pause_start >= SCROLL_END_PAUSE) {
										if (ctx->artist_scroll_paused_at_end) {
											ctx->artist_scroll_px = 0.0;
											ctx->artist_scroll_paused_at_end = false;
											ctx->artist_scroll_paused_at_start = true;
											ctx->artist_pause_start = now;
											needCompose = true;
										} else {
											ctx->artist_scroll_paused_at_start = false;
											needCompose = true;
										}
									}
								} else {
									ctx->artist_scroll_px += ctx->artist_avg_char_px;
									if (ctx->artist_scroll_px >= ctx->artist_scroll_max_px) {
										ctx->artist_scroll_px = ctx->artist_scroll_max_px;
										ctx->artist_scroll_paused_at_end = true;
										ctx->artist_pause_start = now;
									}
									needCompose = true;
								}
							}
						}
					}

					if (s.vu_meter_enabled && now - ctx->last_vu_tick >= std::chrono::milliseconds(s.vu_update_ms)) {
						ctx->last_vu_tick = now;
						int barCount = std::clamp(s.vu_bar_count, 1, VU_MAX_BAR_COUNT);
						if (ctx->is_playing) {
							std::uniform_real_distribution<double> dist(0.0, 1.0);

							double pull = std::clamp(s.vu_randomness, 0, 100) / 100.0;
							for (int i = 0; i < barCount; i++) {
								double target = dist(ctx->vu_rng);
								ctx->vu_bar_frac[i] += (target - ctx->vu_bar_frac[i]) * pull;
							}
							ctx->vu_was_playing = true;
							needCompose = true;
						} else if (ctx->vu_was_playing) {
							for (int i = 0; i < barCount; i++)
								ctx->vu_bar_frac[i] = 0.0;
							ctx->vu_was_playing = false;
							needCompose = true;
						}
					}

					if (s.show_progress_bar && ctx->have_track && now - ctx->last_progress_tick >= std::chrono::milliseconds(PROGRESS_UPDATE_MS)) {
						ctx->last_progress_tick = now;
						needCompose = true;
					}

					if (needCompose) {
						compose_bitmap(ctx, ctx->last_song, ctx->last_artist, s);
					}
				}
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	// Release explicitly, on this same thread
	sessionManager = nullptr;

	if (com_initialized)
		winrt::uninit_apartment();
}

// ---------------------------------------------------------------------
// obs_source_info callbacks
// ---------------------------------------------------------------------

static const char *spotify_source_get_name(void *)
{
	return obs_module_text("NowPlayingWidget");
}

// ---------------------------------------------------------------------
// Settings export / import
//
// These lists are the single source of truth for which settings keys get
// backed up. Add a key here whenever a new setting is introduced -- any
// key NOT listed here is simply never touched by export/import. On import,
// a key that isn't present in the file being imported (e.g. it came from
// an older plugin version) is left untouched, so partial/older backups
// degrade gracefully instead of clobbering the rest of the config.
// ---------------------------------------------------------------------

static const char *const kSettingsIntKeys[] = {
	"title_color", "artist_color", "bg_color", "bg_opacity", "background_corner_radius", "album_art_corner_radius", 
	"card_width", "card_height", "text_offset_y", "progress_bar_gap", "progress_bar_height", "scroll_speed_ms", 
	"vu_color", "vu_update_ms", "vu_randomness", "vu_width", "vu_height", "vu_bar_count", "progress_fill_color", 
	"progress_bg_color", "autohide_after_s",
};

static const char *const kSettingsBoolKeys[] = {
	"use_bg_image", "vu_meter_enabled", "vu_horizontal", "vertical_layout", "show_album_name", "show_goat_placeholder", 
	"show_plugin_attribution", "hide_album_art", "show_progress_bar", "track_change_animation_enabled", "autohide_enabled", 
	"autohide_when_not_playing",
};

static const char *const kSettingsStringKeys[] = {
	"bg_image_path",
};

static const char *const kSettingsObjKeys[] = {
	"title_font",
	"artist_font",
};

static void export_known_settings(obs_data_t *settings, obs_data_t *out)
{
	for (const char *key : kSettingsIntKeys)
		obs_data_set_int(out, key, obs_data_get_int(settings, key));
	for (const char *key : kSettingsBoolKeys)
		obs_data_set_bool(out, key, obs_data_get_bool(settings, key));
	for (const char *key : kSettingsStringKeys)
		obs_data_set_string(out, key, obs_data_get_string(settings, key));
	for (const char *key : kSettingsObjKeys) {
		obs_data_t *obj = obs_data_get_obj(settings, key);
		if (obj) {
			obs_data_set_obj(out, key, obj);
			obs_data_release(obj);
		}
	}
}

static void import_known_settings(obs_data_t *imported, obs_data_t *settings)
{
	for (const char *key : kSettingsIntKeys)
		if (obs_data_has_user_value(imported, key))
			obs_data_set_int(settings, key, obs_data_get_int(imported, key));
	for (const char *key : kSettingsBoolKeys)
		if (obs_data_has_user_value(imported, key))
			obs_data_set_bool(settings, key, obs_data_get_bool(imported, key));
	for (const char *key : kSettingsStringKeys)
		if (obs_data_has_user_value(imported, key))
			obs_data_set_string(settings, key, obs_data_get_string(imported, key));
	for (const char *key : kSettingsObjKeys) {
		if (obs_data_has_user_value(imported, key)) {
			obs_data_t *obj = obs_data_get_obj(imported, key);
			if (obj) {
				obs_data_set_obj(settings, key, obj);
				obs_data_release(obj);
			}
		}
	}
}

static bool export_settings_modified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const char *path = obs_data_get_string(settings, "export_settings_path");
	if (path && path[0]) {
		struct dstr fixed_path = {0};
		dstr_copy(&fixed_path, path);

		const char *ext = os_get_path_extension(fixed_path.array);
		if (!ext || astrcmpi(ext, ".json") != 0) {
			if (ext && *ext) {
				dstr_resize(&fixed_path, ext - fixed_path.array);
			}
			dstr_cat(&fixed_path, ".json");
		}

		obs_data_t *out = obs_data_create();
		export_known_settings(settings, out);
		if (!obs_data_save_json_pretty_safe(out, fixed_path.array, "tmp", "bak")) {
			blog(LOG_WARNING, "[spotify_now_playing] Failed to export settings to %s", fixed_path.array);
		}
		obs_data_release(out);

		dstr_free(&fixed_path);

		obs_data_set_string(settings, "export_settings_path", "");
	}
	return true;
}

static bool import_settings_modified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const char *path = obs_data_get_string(settings, "import_settings_path");
	if (path && path[0]) {
		obs_data_t *imported = obs_data_create_from_json_file(path);
		if (imported) {
			import_known_settings(imported, settings);
			obs_data_release(imported);
		} else {
			blog(LOG_WARNING, "[spotify_now_playing] Failed to import settings from %s", path);
		}

		obs_data_set_string(settings, "import_settings_path", "");
	}
	return true;
}

static void apply_settings(spotify_source *ctx, obs_data_t *settings)
{
	std::lock_guard<std::mutex> lock(ctx->settings_mutex);
	ctx->title_color = obs_data_get_int(settings, "title_color");
	ctx->artist_color = obs_data_get_int(settings, "artist_color");
	ctx->bg_color = obs_data_get_int(settings, "bg_color");

	ctx->bg_opacity = (int)obs_data_get_int(settings, "bg_opacity");
	ctx->bg_opacity = std::clamp(ctx->bg_opacity, 0, 100);

	ctx->use_bg_image = obs_data_get_bool(settings, "use_bg_image");
	const char *bg_image_path = obs_data_get_string(settings, "bg_image_path");
	ctx->bg_image_path = bg_image_path ? bg_image_path : "";

	ctx->background_corner_radius = (int)obs_data_get_int(settings, "background_corner_radius");
	ctx->background_corner_radius = std::clamp(ctx->background_corner_radius, 0, 100);

	ctx->album_art_corner_radius = (int)obs_data_get_int(settings, "album_art_corner_radius");
	ctx->album_art_corner_radius = std::clamp(ctx->album_art_corner_radius, 0, 100);

	ctx->card_w = (int)obs_data_get_int(settings, "card_width");
	ctx->card_w = std::clamp(ctx->card_w, 50, 4000);

	ctx->card_h = (int)obs_data_get_int(settings, "card_height");
	ctx->card_h = std::clamp(ctx->card_h, 30, 2000);

	ctx->text_offset_y = (int)obs_data_get_int(settings, "text_offset_y");
	ctx->text_offset_y = std::clamp(ctx->text_offset_y, -1000, 1000);

	ctx->progress_bar_gap = (int)obs_data_get_int(settings, "progress_bar_gap");
	ctx->progress_bar_gap = std::clamp(ctx->progress_bar_gap, -1000, 1000);

	ctx->progress_bar_height = (int)obs_data_get_int(settings, "progress_bar_height");
	ctx->progress_bar_height = std::clamp(ctx->progress_bar_height, 2, 1000);

	ctx->scroll_speed_ms = (int)obs_data_get_int(settings, "scroll_speed_ms");
	ctx->scroll_speed_ms = std::clamp(ctx->scroll_speed_ms, 20, 5000);

	ctx->vu_meter_enabled = obs_data_get_bool(settings, "vu_meter_enabled");
	ctx->vu_color = obs_data_get_int(settings, "vu_color");

	ctx->vu_update_ms = (int)obs_data_get_int(settings, "vu_update_ms");
	ctx->vu_update_ms = std::clamp(ctx->vu_update_ms, 50, 2000);

	ctx->vu_randomness = (int)obs_data_get_int(settings, "vu_randomness");
	ctx->vu_randomness = std::clamp(ctx->vu_randomness, 0, 100);

	ctx->vu_width = (int)obs_data_get_int(settings, "vu_width");
	ctx->vu_width = std::clamp(ctx->vu_width, 4, 2000);

	ctx->vu_height = (int)obs_data_get_int(settings, "vu_height");
	ctx->vu_height = std::clamp(ctx->vu_height, 4, 2000);

	ctx->vu_bar_count = (int)obs_data_get_int(settings, "vu_bar_count");
	ctx->vu_bar_count = std::clamp(ctx->vu_bar_count, 1, VU_MAX_BAR_COUNT);

	ctx->vu_horizontal = obs_data_get_bool(settings, "vu_horizontal");
	ctx->vertical_layout = obs_data_get_bool(settings, "vertical_layout");
	ctx->show_album_name = obs_data_get_bool(settings, "show_album_name");
	ctx->show_goat_placeholder = obs_data_get_bool(settings, "show_goat_placeholder");
	ctx->show_plugin_attribution = obs_data_get_bool(settings, "show_plugin_attribution");
	ctx->hide_album_art = obs_data_get_bool(settings, "hide_album_art");

	ctx->show_progress_bar = obs_data_get_bool(settings, "show_progress_bar");
	ctx->progress_fill_color = obs_data_get_int(settings, "progress_fill_color");
	ctx->progress_bg_color = obs_data_get_int(settings, "progress_bg_color");

	ctx->track_change_animation_enabled = obs_data_get_bool(settings, "track_change_animation_enabled");

	ctx->autohide_enabled = obs_data_get_bool(settings, "autohide_enabled");
	ctx->autohide_after_s = (int)obs_data_get_int(settings, "autohide_after_s");
	ctx->autohide_after_s = std::clamp(ctx->autohide_after_s, 0, 3600);

	ctx->autohide_when_not_playing = obs_data_get_bool(settings, "autohide_when_not_playing");

	obs_data_t *title_font_obj = obs_data_get_obj(settings, "title_font");
	if (title_font_obj) {
		const char *face = obs_data_get_string(title_font_obj, "face");
		const char *style = obs_data_get_string(title_font_obj, "style");
		ctx->title_font_face = (face && face[0]) ? face : "Segoe UI";
		ctx->title_font_style = style ? style : "Regular";
		ctx->title_font_size = (int)obs_data_get_int(title_font_obj, "size");
		ctx->title_font_flags = (int)obs_data_get_int(title_font_obj, "flags");
		obs_data_release(title_font_obj);
	}
	if (ctx->title_font_size <= 0) {
		ctx->title_font_size = DEFAULT_TITLE_FONT_SIZE;
	}

	obs_data_t *artist_font_obj = obs_data_get_obj(settings, "artist_font");
	if (artist_font_obj) {
		const char *face = obs_data_get_string(artist_font_obj, "face");
		const char *style = obs_data_get_string(artist_font_obj, "style");
		ctx->artist_font_face = (face && face[0]) ? face : "Segoe UI";
		ctx->artist_font_style = style ? style : "Regular";
		ctx->artist_font_size = (int)obs_data_get_int(artist_font_obj, "size");
		ctx->artist_font_flags = (int)obs_data_get_int(artist_font_obj, "flags");
		obs_data_release(artist_font_obj);
	}
	if (ctx->artist_font_size <= 0) {
		ctx->artist_font_size = DEFAULT_ARTIST_FONT_SIZE;
	}

	ctx->settings_dirty = true;
}

static void spotify_source_update(void *data, obs_data_t *settings)
{
	auto *ctx = (spotify_source *)data;
	apply_settings(ctx, settings);
}

static void spotify_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "title_color", DEFAULT_COLOR_WHITE);
	obs_data_set_default_int(settings, "artist_color", DEFAULT_COLOR_WHITE);

	obs_data_set_default_int(settings, "bg_color", DEFAULT_COLOR_BLACK);
	obs_data_set_default_int(settings, "bg_opacity", DEFAULT_BG_OPACITY);
	obs_data_set_default_string(settings, "export_settings_path", "");
	obs_data_set_default_string(settings, "import_settings_path", "");

	obs_data_set_default_bool(settings, "use_bg_image", false);
	obs_data_set_default_string(settings, "bg_image_path", "");
	obs_data_set_default_int(settings, "background_corner_radius", DEFAULT_BACKGROUND_CORNER_RADIUS);
	obs_data_set_default_int(settings, "album_art_corner_radius", DEFAULT_ALBUM_ART_CORNER_RADIUS);

	obs_data_set_default_int(settings, "card_width", DEFAULT_CARD_W);
	obs_data_set_default_int(settings, "card_height", DEFAULT_CARD_H);
	obs_data_set_default_int(settings, "text_offset_y", 0);
	obs_data_set_default_int(settings, "progress_bar_gap", DEFAULT_PROGRESS_BAR_GAP);
	obs_data_set_default_int(settings, "progress_bar_height", DEFAULT_PROGRESS_BAR_HEIGHT);

	obs_data_set_default_int(settings, "scroll_speed_ms", DEFAULT_SCROLL_SPEED_MS);

	obs_data_set_default_bool(settings, "vu_meter_enabled", true);
	obs_data_set_default_int(settings, "vu_color", DEFAULT_COLOR_GREEN);
	obs_data_set_default_int(settings, "vu_update_ms", DEFAULT_VU_UPDATE_MS);
	obs_data_set_default_int(settings, "vu_randomness", DEFAULT_VU_RANDOMNESS);

	obs_data_set_default_int(settings, "vu_width", DEFAULT_VU_WIDTH);
	obs_data_set_default_int(settings, "vu_height", DEFAULT_VU_HEIGHT);
	obs_data_set_default_int(settings, "vu_bar_count", DEFAULT_VU_BAR_COUNT);
	obs_data_set_default_bool(settings, "vu_horizontal", false);

	obs_data_set_default_bool(settings, "vertical_layout", false);
	obs_data_set_default_bool(settings, "show_goat_placeholder", true);
	obs_data_set_default_bool(settings, "show_plugin_attribution", true);
	obs_data_set_default_bool(settings, "hide_album_art", false);
	obs_data_set_default_bool(settings, "show_album_name", false);

	obs_data_set_default_bool(settings, "show_progress_bar", true);
	obs_data_set_default_int(settings, "progress_fill_color", DEFAULT_COLOR_WHITE);
	obs_data_set_default_int(settings, "progress_bg_color", DEFAULT_COLOR_DARK_GREY);

	obs_data_set_default_bool(settings, "track_change_animation_enabled", true);

	obs_data_set_default_bool(settings, "autohide_enabled", false);
	obs_data_set_default_int(settings, "autohide_after_s", DEFAULT_AUTOHIDE_AFTER_S);

	obs_data_set_default_bool(settings, "autohide_when_not_playing", false);

	obs_data_t *title_font_obj = obs_data_create();
	obs_data_set_default_string(title_font_obj, "face", "Segoe UI");
	obs_data_set_default_string(title_font_obj, "style", "Bold");
	obs_data_set_default_int(title_font_obj, "size", DEFAULT_TITLE_FONT_SIZE);
	obs_data_set_default_obj(settings, "title_font", title_font_obj);
	obs_data_release(title_font_obj);

	obs_data_t *artist_font_obj = obs_data_create();
	obs_data_set_default_string(artist_font_obj, "face", "Segoe UI");
	obs_data_set_default_string(artist_font_obj, "style", "Regular");
	obs_data_set_default_int(artist_font_obj, "size", DEFAULT_ARTIST_FONT_SIZE);
	obs_data_set_default_obj(settings, "artist_font", artist_font_obj);
	obs_data_release(artist_font_obj);
}

static bool autohide_enabled_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "autohide_enabled");
	obs_property_t *after_prop = obs_properties_get(props, "autohide_after_s");
	obs_property_set_enabled(after_prop, enabled);
	return true;
}

static bool use_bg_image_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "use_bg_image");
	obs_property_t *path_prop = obs_properties_get(props, "bg_image_path");
	obs_property_t *bg_color_prop = obs_properties_get(props, "bg_color");
	obs_property_set_enabled(path_prop, enabled);
	obs_property_set_enabled(bg_color_prop, enabled);
	return true;
}

static obs_properties_t *spotify_source_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(props, "vertical_layout", obs_module_text("VerticalLayout"));
	obs_properties_add_bool(props, "hide_album_art", obs_module_text("HideAlbumArt"));
	obs_properties_add_bool(props, "track_change_animation_enabled", obs_module_text("TrackChangeAnimation"));
	obs_properties_add_bool(props, "autohide_when_not_playing", obs_module_text("AutohideWhenNotPlaying"));
	obs_property_t *autohide_prop = obs_properties_add_bool(props, "autohide_enabled", obs_module_text("AutohideEnabled"));
	obs_properties_add_int(props, "autohide_after_s", obs_module_text("AutohideAfterSeconds"), 1, 3600, 1);
	obs_property_set_modified_callback(autohide_prop, autohide_enabled_modified);
	obs_properties_add_color_alpha(props, "title_color", obs_module_text("TitleColor"));
	obs_properties_add_color_alpha(props, "artist_color", obs_module_text("ArtistColor"));
	obs_properties_add_bool(props, "show_album_name", obs_module_text("ShowAlbumName"));
	obs_property_t *use_bg_image_prop = obs_properties_add_bool(props, "use_bg_image", obs_module_text("UseImageAsBackground"));
	obs_properties_add_path(props, "bg_image_path", obs_module_text("BackgroundImagePath"), OBS_PATH_FILE, "Image Files (*.jpg *.jpeg *.png);;All Files (*.*)", nullptr);
	obs_property_set_modified_callback(use_bg_image_prop, use_bg_image_modified);
	obs_properties_add_color(props, "bg_color", obs_module_text("BackgroundColor"));
	obs_properties_add_int(props, "bg_opacity", obs_module_text("BackgroundOpacity"), 0, 100, 1);
	obs_properties_add_int(props, "background_corner_radius", obs_module_text("BackgroundCornerRadius"), 1, 100, 1);
	obs_properties_add_int(props, "album_art_corner_radius", obs_module_text("AlbumArtCornerRadius"), 1, 100, 1);
	obs_properties_add_font(props, "title_font", obs_module_text("TitleFont"));
	obs_properties_add_font(props, "artist_font", obs_module_text("ArtistFont"));
	obs_properties_add_int(props, "card_width", obs_module_text("CardWidth"), 50, 4000, 10);
	obs_properties_add_int(props, "card_height", obs_module_text("CardHeight"), 30, 2000, 10);
	obs_properties_add_int(props, "text_offset_y", obs_module_text("TextVerticalOffset"), -1000, 1000, 1);
	obs_properties_add_int(props, "scroll_speed_ms", obs_module_text("ScrollSpeed"), 50, 5000, 10);
	obs_properties_add_bool(props, "show_progress_bar", obs_module_text("ShowProgressBar"));
	obs_properties_add_color_alpha(props, "progress_fill_color", obs_module_text("ProgressFillColor"));
	obs_properties_add_color_alpha(props, "progress_bg_color", obs_module_text("ProgressBackgroundColor"));
	obs_properties_add_int(props, "progress_bar_height", obs_module_text("ProgressBarHeight"), 2, 1000, 1);
	obs_properties_add_int(props, "progress_bar_gap", obs_module_text("ProgressBarGap"), -1000, 1000, 1);
	obs_properties_add_bool(props, "vu_meter_enabled", obs_module_text("ShowVUMeter"));
	obs_properties_add_bool(props, "vu_horizontal", obs_module_text("VUMeterHorizontalOrientation"));
	obs_properties_add_color_alpha(props, "vu_color", obs_module_text("VUMeterColor"));
	obs_properties_add_int(props, "vu_update_ms", obs_module_text("VUUpdateSpeed"), 50, 2000, 10);
	obs_properties_add_int(props, "vu_randomness", obs_module_text("VURandomness"), 0, 100, 5);
	obs_properties_add_int(props, "vu_width", obs_module_text("VUMeterWidth"), 4, 2000, 1);
	obs_properties_add_int(props, "vu_height", obs_module_text("VUMeterHeight"), 4, 2000, 1);
	obs_properties_add_int(props, "vu_bar_count", obs_module_text("VUBarCount"), 1, VU_MAX_BAR_COUNT, 1);
	obs_properties_add_bool(props, "show_goat_placeholder", obs_module_text("ShowGoatWhenNoAlbumArt"));
	obs_properties_add_bool(props, "show_plugin_attribution", obs_module_text("ShowPluginAttribution"));

	obs_property_t *export_settings_prop = obs_properties_add_path(props, "export_settings_path", obs_module_text("ExportSettings"), OBS_PATH_FILE_SAVE, "JSON (*.json)", nullptr);
	obs_property_set_modified_callback(export_settings_prop, export_settings_modified);

	obs_property_t *import_settings_prop = obs_properties_add_path(props, "import_settings_path", obs_module_text("ImportSettings"), OBS_PATH_FILE, "JSON (*.json)", nullptr);
	obs_property_set_modified_callback(import_settings_prop, import_settings_modified);

	return props;
}

static void *spotify_source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new spotify_source();
	ctx->source = source;
	apply_settings(ctx, settings);

	auto now = std::chrono::steady_clock::now();
	ctx->autohide_reference_time = now;
	ctx->last_autohide_tick = now;
	ctx->last_playing_time = now;

	ctx->running = true;
	ctx->poll_thread = std::thread(poll_loop, ctx);
	return ctx;
}

static void spotify_source_destroy(void *data)
{
	auto *ctx = (spotify_source *)data;

	ctx->running = false;
	if (ctx->poll_thread.joinable())
		ctx->poll_thread.join();

	if (ctx->texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->texture);
		obs_leave_graphics();
		ctx->texture = nullptr;
	}

	delete ctx;
}

static void spotify_source_activate(void *data)
{
	// Called when this source becomes part of the live/program output (its scene went live,
	// or a hidden scene item was shown). Resume the SMTC poll.
	auto *ctx = (spotify_source *)data;
	ctx->is_active = true;
}

static void spotify_source_deactivate(void *data)
{
	// Called when this source stops being part of the live/program output. Pause the SMTC
	// poll until it's needed again.
	auto *ctx = (spotify_source *)data;
	ctx->is_active = false;
}

static uint32_t spotify_source_get_width(void *data)
{
	auto *ctx = (spotify_source *)data;
	return ctx->tex_w ? ctx->tex_w : DEFAULT_CARD_W;
}

static uint32_t spotify_source_get_height(void *data)
{
	auto *ctx = (spotify_source *)data;
	return ctx->tex_h ? ctx->tex_h : DEFAULT_CARD_H;
}

static void spotify_source_tick(void *data, float)
{
	auto *ctx = (spotify_source *)data;

	if (!ctx->new_bitmap_ready)
		return;

	std::vector<uint8_t> pixels;
	uint32_t w = 0, h = 0;
	{
		std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
		pixels = ctx->pending_pixels;
		w = ctx->pending_w;
		h = ctx->pending_h;
	}
	ctx->new_bitmap_ready = false;

	if (w == 0 || h == 0)
		return;

	obs_enter_graphics();
	if (ctx->texture) {
		gs_texture_destroy(ctx->texture);
		ctx->texture = nullptr;
	}
	const uint8_t *data_ptr = pixels.data();
	ctx->texture = gs_texture_create(w, h, GS_BGRA, 1, &data_ptr, 0);
	obs_leave_graphics();

	ctx->tex_w = w;
	ctx->tex_h = h;
}

static void spotify_source_render(void *data, gs_effect_t *)
{
	auto *ctx = (spotify_source *)data;
	if (!ctx->texture)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, ctx->texture);

	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(ctx->texture, 0, ctx->tex_w, ctx->tex_h);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------

void spotify_source_register(void)
{
	GdiplusStartupInput gdiInput;
	GdiplusStartup(&g_gdiplusToken, &gdiInput, nullptr);

	obs_source_info info = {};
	info.id = "spotify_now_playing_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = spotify_source_get_name;
	info.create = spotify_source_create;
	info.destroy = spotify_source_destroy;
	info.get_width = spotify_source_get_width;
	info.get_height = spotify_source_get_height;
	info.video_tick = spotify_source_tick;
	info.video_render = spotify_source_render;
	info.get_properties = spotify_source_properties;
	info.get_defaults = spotify_source_defaults;
	info.update = spotify_source_update;
	info.activate = spotify_source_activate;
	info.deactivate = spotify_source_deactivate;

	obs_register_source(&info);
}

void spotify_source_unregister(void)
{
	if (g_gdiplusToken) {
		GdiplusShutdown(g_gdiplusToken);
		g_gdiplusToken = 0;
	}
}