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
#include "SpotifyBridge.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/bmem.h>

#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <gdiplus.h>

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

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

namespace {

constexpr int POLL_INTERVAL_MS = 1000;
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

// VU meter geometry
constexpr int VU_MAX_BAR_COUNT = 50;
constexpr int VU_BAR_GAP = 3;
constexpr int VU_GAP_BEFORE_TEXT = 10;

// Progress bar geometry (not exposed as settings -- these are fixed layout
// details, same idea as VU_BAR_GAP/GAP_ART_TEXT elsewhere in this file)
constexpr int PROGRESS_BAR_HEIGHT = 4;
constexpr int PROGRESS_BAR_GAP = 6;      // gap between artist text and the bar
constexpr int PROGRESS_UPDATE_MS = 1000; // how often the bar redraws while a track is loaded

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

void DrawScrollableLine(Graphics &g, const std::wstring &text, Font &font, Brush &brush, const RectF &bounds, double scrollOffsetPx, bool centerWhenStatic, bool *outNeedsScroll,
			double *outAvgCharPx, double *outMaxOffsetPx)
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

// Caches a built Font alongside the settings it was built from, so repeated
// calls with unchanged font settings (the common case -- font settings
// change far less often than compose_bitmap runs) skip the FontFamily
// lookup and Font construction entirely.
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

} // namespace

struct spotify_source {
	obs_source_t *source = nullptr;

	std::thread poll_thread;
	std::atomic<bool> running{false};

	std::mutex settings_mutex;
	long long title_color = 0xFFFFFFFF;
	long long artist_color = 0xFFFFFFFF;
	long long bg_color = 0;
	int bg_opacity = DEFAULT_BG_OPACITY; // percent, 0-100
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
	long long progress_fill_color = 0xFFFFFFFF; // white
	long long progress_bg_color = 0xFF5A5A5A;   // grey (packed R|G<<8|B<<16|A<<24)
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

	// Playback position tracking for the progress bar. The bridge only
	// reports position once per poll (~1s), so we interpolate between
	// polls using how much wall-clock time has passed since the last
	// sample, rather than only updating the bar in visible 1s jumps.
	int64_t song_duration_ticks = 0;     // .NET TimeSpan ticks (100ns each)
	int64_t playback_position_ticks = 0; // position as of position_sample_time
	std::chrono::steady_clock::time_point position_sample_time{};
	std::chrono::steady_clock::time_point last_progress_tick{};
	// Highest position value actually displayed so far for the current
	// track. A fresh sample can occasionally read a touch behind where
	// we'd already extrapolated to (ordinary timing drift over the ~5s
	// gap between real SMTC updates) -- rather than ever visibly stepping
	// backward, we hold at this high point until the real value catches
	// back up. Reset to 0 whenever the track changes.
	int64_t max_displayed_position_ticks = 0;

	std::unique_ptr<Image> goat_image;
	bool goat_image_load_attempted = false;

	std::unique_ptr<Bitmap> cached_bitmap;
	int cached_bitmap_w = 0;
	int cached_bitmap_h = 0;

	CachedFont title_font_cache;
	CachedFont artist_font_cache;
};

struct AppearanceSettings {
	long long title_color;
	long long artist_color;
	long long bg_color;
	int bg_opacity;
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

// Draws the two-layer progress bar: a full-width background "track" (always
// drawn when enabled, even at 0% or with unknown duration, so the reserved
// layout space never just looks like an empty gap) plus a growing fill on
// top. Position is interpolated using how much wall-clock time has passed
// since the last bridge poll, so the fill advances smoothly rather than
// only jumping once a second.
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

		// Never let the displayed position move backward -- if a fresh
		// sample happens to read a touch behind where we'd already
		// extrapolated to, just hold at the previous high point instead
		// of visibly snapping back; the real value will catch up on its
		// own within a poll or two.
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

// Decodes image_data (raw PNG/JPEG bytes from the bridge) exactly once and
// caches the result on ctx. Called only when the track actually changes
// (see poll_loop) -- compose_bitmap just draws whatever's cached here
// without ever touching the raw bytes or re-decoding, since image decoding
// is the single most expensive thing GDI+ does in this whole pipeline and
// there's no reason to pay that cost on every scroll/VU tick when the art
// itself hasn't changed.
static void UpdateCachedArt(spotify_source *ctx, const uint8_t *image_data, int image_len)
{
	ctx->cached_art_image.reset();
	if (image_data == nullptr || image_len <= 0)
		return;

	IStream *stream = SHCreateMemStream(image_data, (UINT)image_len);
	if (!stream)
		return;

	auto img = std::make_unique<Image>(stream);
	stream->Release();

	if (img->GetLastStatus() == Ok)
		ctx->cached_art_image = std::move(img);
}

static void compose_bitmap(spotify_source *ctx, const std::string &title, const std::string &artist, const AppearanceSettings &s)
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
	SolidBrush bgBrush(ObsColorToGdipWithAlpha(s.bg_color, s.bg_opacity));
	GraphicsPath bgPath;
	AddRoundedRect(bgPath, Rect(0, 0, cardW, cardH), s.background_corner_radius);
	g.FillPath(&bgBrush, &bgPath);

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
	int progressH = s.show_progress_bar ? (PROGRESS_BAR_GAP + PROGRESS_BAR_HEIGHT) : 0;
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
			int progressY = textTop + titleLineH + artistLineH + PROGRESS_BAR_GAP;
			progressBarRect = Rect(textX, progressY, textW, PROGRESS_BAR_HEIGHT);
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
			int progressY = topY + titleLineH + artistLineH + PROGRESS_BAR_GAP;
			progressBarRect = Rect(textX, progressY, textW, PROGRESS_BAR_HEIGHT);
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
	return AppearanceSettings{ctx->title_color,
				  ctx->artist_color,
				  ctx->bg_color,
				  ctx->bg_opacity,
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
				  ctx->progress_bg_color};
}

static void poll_loop(spotify_source *ctx)
{
	HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool com_initialized = SUCCEEDED(com_hr);

	//Hold the last good bitmap for 2 seconds as some clients drop their session during track skip
	constexpr auto MISSING_SESSION_GRACE = std::chrono::seconds(2);
	bool gap_active = false;
	std::chrono::steady_clock::time_point gap_start{};

	while (ctx->running) {
		NativeMediaInfo info{};
		bool has = GetCurrentTrackNative(&info);

		std::string title = has ? std::string(info.SongName) : std::string();
		std::string artist = has ? std::string(info.ArtistName) : std::string();

		if (ctx->show_album_name)
		{
			std::string albumName = " - " + std::string(info.AlbumName);
			artist.append(albumName);
		}

		bool track_changed = (has != ctx->have_track) || (title != ctx->last_song) || (artist != ctx->last_artist);

		if (has) {
			gap_active = false;
			ctx->is_playing = info.IsPlaying;
			auto now = std::chrono::steady_clock::now();

			// Windows' media session (SMTC) only refreshes position roughly
			// every 5s even though we poll every 1s -- re-stamping the
			// sample time on every poll, even when the reported value
			// hasn't actually moved, causes the interpolated bar to snap
			// backward to the stale value each time we resample it. Only
			// take a fresh sample when the value genuinely changed, or the
			// track itself changed (which always needs a fresh baseline).
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
				ctx->max_displayed_position_ticks = 0; // fresh timeline for the new track

				ctx->title_scroll_px = 0.0; // New track -- restart the marquee from the beginning.
				ctx->artist_scroll_px = 0.0;
				ctx->title_scroll_paused_at_end = false;
				ctx->artist_scroll_paused_at_end = false;
				ctx->title_scroll_paused_at_start = true;
				ctx->artist_scroll_paused_at_start = true;
				ctx->title_pause_start = now;
				ctx->artist_pause_start = now;
				ctx->last_scroll_tick = std::chrono::steady_clock::now();

				compose_bitmap(ctx, title, artist, snapshot_settings(ctx));
			} else if (ctx->settings_dirty) {
				ctx->title_scroll_px = 0.0;
				ctx->artist_scroll_px = 0.0;
				ctx->title_scroll_paused_at_end = false;
				ctx->artist_scroll_paused_at_end = false;
				ctx->title_scroll_paused_at_start = true;
				ctx->artist_scroll_paused_at_start = true;
				ctx->title_pause_start = now;
				ctx->artist_pause_start = now;
				compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx));
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
			compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx));
		}
		ctx->settings_dirty = false;

		if (has && info.ImageData != nullptr)
			FreeImageBuffer(info.ImageData);

		// Settings only change via apply_settings() (which sets
		// settings_dirty), and the check below still runs every 50ms on a
		// plain atomic bool -- the moment a setting changes, this loop
		// still exits within 50ms and the outer loop immediately
		// recomposes with a fresh snapshot_settings() call. So it's safe
		// to snapshot once per ~1s outer pass here rather than on every
		// 50ms tick: this only skips the redundant mutex-lock-and-copy
		// during the (common) stretches where nothing has actually
		// changed, without adding any delay to how quickly a real
		// settings change gets picked up.
		AppearanceSettings s = snapshot_settings(ctx);

		for (int waited = 0; waited < POLL_INTERVAL_MS && ctx->running; waited += 50) {
			if (ctx->settings_dirty)
				break; // let the outer loop apply the appearance change immediately

			if (ctx->have_track || s.show_plugin_attribution) {
				auto now = std::chrono::steady_clock::now();
				bool needCompose = false;

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

				// Progress bar redraw tick. Fires whenever a track is
				// loaded, not just while playing -- this also catches a
				// seek made while paused, since the outer poll loop only
				// recomposes on track_changed/settings_dirty and would
				// otherwise leave a stale bar position on screen for up
				// to a second.
				if (s.show_progress_bar && ctx->have_track && now - ctx->last_progress_tick >= std::chrono::milliseconds(PROGRESS_UPDATE_MS)) {
					ctx->last_progress_tick = now;
					needCompose = true;
				}

				if (needCompose) {
					compose_bitmap(ctx, ctx->last_song, ctx->last_artist, s);
				}
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	if (com_initialized)
		CoUninitialize();
}

// ---------------------------------------------------------------------
// obs_source_info callbacks
// ---------------------------------------------------------------------

static const char *spotify_source_get_name(void *)
{
	return obs_module_text("NowPlayingWidget");
}

static void apply_settings(spotify_source *ctx, obs_data_t *settings)
{
	std::lock_guard<std::mutex> lock(ctx->settings_mutex);
	ctx->title_color = obs_data_get_int(settings, "title_color");
	ctx->artist_color = obs_data_get_int(settings, "artist_color");
	ctx->bg_color = obs_data_get_int(settings, "bg_color");

	ctx->bg_opacity = (int)obs_data_get_int(settings, "bg_opacity");
	ctx->bg_opacity = std::clamp(ctx->bg_opacity, 0, 100);

	ctx->background_corner_radius = (int)obs_data_get_int(settings, "background_corner_radius");
	ctx->background_corner_radius = std::clamp(ctx->background_corner_radius, 0, 100);

	ctx->album_art_corner_radius= (int)obs_data_get_int(settings, "album_art_corner_radius");
	ctx->album_art_corner_radius = std::clamp(ctx->album_art_corner_radius, 0, 100);

	ctx->card_w = (int)obs_data_get_int(settings, "card_width");
	ctx->card_w = std::clamp(ctx->card_w, 50, 4000);

	ctx->card_h = (int)obs_data_get_int(settings, "card_height");
	ctx->card_h = std::clamp(ctx->card_h, 30, 2000);

	ctx->text_offset_y = (int)obs_data_get_int(settings, "text_offset_y");
	ctx->text_offset_y = std::clamp(ctx->text_offset_y, -1000, 1000);

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
	obs_data_set_default_int(settings, "title_color", DEFAULT_COLOR_WHITE); // opaque white
	obs_data_set_default_int(settings, "artist_color", DEFAULT_COLOR_WHITE);

	obs_data_set_default_int(settings, "bg_color", DEFAULT_COLOR_BLACK); //(long long)(((uint32_t)24 << 16) | ((uint32_t)20 << 8) | 20));
	obs_data_set_default_int(settings, "bg_opacity", DEFAULT_BG_OPACITY);
	obs_data_set_default_int(settings, "background_corner_radius", DEFAULT_BACKGROUND_CORNER_RADIUS);
	obs_data_set_default_int(settings, "album_art_corner_radius", DEFAULT_ALBUM_ART_CORNER_RADIUS);

	obs_data_set_default_int(settings, "card_width", DEFAULT_CARD_W);
	obs_data_set_default_int(settings, "card_height", DEFAULT_CARD_H);
	obs_data_set_default_int(settings, "text_offset_y", 0);

	obs_data_set_default_int(settings, "scroll_speed_ms", DEFAULT_SCROLL_SPEED_MS);

	obs_data_set_default_bool(settings, "vu_meter_enabled", true);
	// Packed R | (G<<8) | (B<<16) | (A<<24) -- a Spotify-green-ish default (#1ED760)
	obs_data_set_default_int(settings, "vu_color", DEFAULT_COLOR_GREEN);//(long long)(((uint32_t)0xFF << 24) | ((uint32_t)0x60 << 16) | ((uint32_t)0xD7 << 8) | 0x1E));
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
	obs_data_set_default_int(settings, "progress_fill_color", DEFAULT_COLOR_WHITE); // white	
	obs_data_set_default_int(settings, "progress_bg_color", DEFAULT_COLOR_DARK_GREY); //(long long)(((uint32_t)0xFF << 24) | ((uint32_t)0x5A << 16) | ((uint32_t)0x5A << 8) | 0x5A));

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

static obs_properties_t *spotify_source_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(props, "vertical_layout", obs_module_text("VerticalLayout"));
	obs_properties_add_bool(props, "hide_album_art", obs_module_text("HideAlbumArt"));
	obs_properties_add_bool(props, "show_progress_bar", obs_module_text("ShowProgressBar"));
	obs_properties_add_color_alpha(props, "progress_fill_color", obs_module_text("ProgressFillColor"));
	obs_properties_add_color_alpha(props, "progress_bg_color", obs_module_text("ProgressBackgroundColor"));
	obs_properties_add_color_alpha(props, "title_color", obs_module_text("TitleColor"));
	obs_properties_add_color_alpha(props, "artist_color", obs_module_text("ArtistColor"));
	obs_properties_add_bool(props, "show_album_name", obs_module_text("ShowAlbumName"));
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

	return props;
}

static void *spotify_source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new spotify_source();
	ctx->source = source;
	apply_settings(ctx, settings);
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

	obs_register_source(&info);
}

void spotify_source_unregister(void)
{
	if (g_gdiplusToken) {
		GdiplusShutdown(g_gdiplusToken);
		g_gdiplusToken = 0;
	}
}
