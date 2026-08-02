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



#include "spotify-source.h"
#include "SpotifyBridge.h"

#include <obs-module.h>
#include <util/dstr.h>

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

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

namespace {

constexpr int POLL_INTERVAL_MS = 1000;
constexpr int DEFAULT_CARD_W = 400;
constexpr int DEFAULT_CARD_H = 110;
constexpr int PAD = 12;
constexpr int MIN_TEXT_W = 20; // never let the text column collapse to nothing
constexpr int MIN_ART_SIZE = 10;

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

} // namespace

struct spotify_source {
	obs_source_t *source = nullptr;

	std::thread poll_thread;
	std::atomic<bool> running{false};

	std::mutex settings_mutex;
	long long title_color = 0xFFFFFFFF;
	long long artist_color = 0xFFFFFFFF;
	long long bg_color = 0;
	int bg_opacity = 92; // percent, 0-100
	std::string font_face = "Segoe UI";
	std::string font_style = "Regular";
	int font_size = 16;
	int font_flags = 0;
	int artist_font_difference = -2;
	int card_w = DEFAULT_CARD_W;
	int card_h = DEFAULT_CARD_H;
	int text_offset_y = 0;
	std::atomic<bool> settings_dirty{true};

	std::mutex bitmap_mutex;
	std::vector<uint8_t> pending_pixels;
	uint32_t pending_w = 0, pending_h = 0;
	std::atomic<bool> new_bitmap_ready{false};

	gs_texture_t *texture = nullptr;
	uint32_t tex_w = 0, tex_h = 0;

	std::string last_song;
	std::string last_artist;
	std::vector<uint8_t> last_image;
	bool have_track = false;
};


struct AppearanceSettings {
	long long title_color;
	long long artist_color;
	long long bg_color;
	int bg_opacity;
	std::string font_face;
	std::string font_style;
	int font_size;
	int font_flags;
	int artist_font_difference;
	int card_w;
	int card_h;
	int text_offset_y;
};

static void compose_bitmap(spotify_source *ctx, const std::string &title, const std::string &artist,
			   const uint8_t *image_data, int image_len, const AppearanceSettings &s)
{
	const int cardW = std::max(s.card_w, 50);
	const int cardH = std::max(s.card_h, 30);

	Bitmap card(cardW, cardH, PixelFormat32bppARGB);
	Graphics g(&card);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(Color(0, 0, 0, 0));

	// card background
	SolidBrush bgBrush(ObsColorToGdipWithAlpha(s.bg_color, s.bg_opacity));
	GraphicsPath bgPath;
	AddRoundedRect(bgPath, Rect(0, 0, cardW, cardH), 14);
	g.FillPath(&bgBrush, &bgPath);

	// album art
	int artSize = cardH - PAD * 2;
	if (artSize < MIN_ART_SIZE)
		artSize = MIN_ART_SIZE;
	int maxArtForWidth = cardW - PAD * 2 - MIN_TEXT_W;
	if (artSize > maxArtForWidth)
		artSize = std::max(MIN_ART_SIZE, maxArtForWidth);

	Rect artRect(PAD, PAD, artSize, artSize);
	GraphicsPath artClip;
	AddRoundedRect(artClip, artRect, 8);

	Region savedClip;
	g.GetClip(&savedClip);
	g.SetClip(&artClip);

	bool drewArt = false;
	if (image_data != nullptr && image_len > 0) {
		IStream *stream = SHCreateMemStream(image_data, (UINT)image_len);
		if (stream) {
			Image img(stream);
			if (img.GetLastStatus() == Ok) {
				g.DrawImage(&img, artRect);
				drewArt = true;
			}
			stream->Release();
		}
	}
	if (!drewArt) {
		SolidBrush placeholder(Color(255, 55, 55, 60));
		g.FillRectangle(&placeholder, artRect);
	}
	g.SetClip(&savedClip);

	// text
	FontFamily requestedFam(Utf8ToWide(s.font_face).c_str());
	const FontFamily *fam = &requestedFam;
	if (requestedFam.GetLastStatus() != Ok)
		fam = FontFamily::GenericSansSerif();

	FontStyle style = ParseFontStyle(s.font_style, s.font_flags);
	int titleSize = s.font_size > 0 ? s.font_size : 16;
	int artistSize = titleSize > 12 ? titleSize + s.artist_font_difference : titleSize;

	Font titleFont(fam, (REAL)titleSize, style, UnitPixel);
	Font artistFont(fam, (REAL)artistSize, FontStyleRegular, UnitPixel);

	Color titleColor = ObsColorToGdip(s.title_color);
	Color artistColor = ObsColorToGdip(s.artist_color); 
	SolidBrush titleBrush(titleColor);
	SolidBrush artistBrush(artistColor);

	int textX = PAD + artSize + 14;
	int textW = cardW - textX - PAD;
	if (textW < MIN_TEXT_W)
		textW = MIN_TEXT_W;

	StringFormat sf;
	sf.SetTrimming(StringTrimmingEllipsisCharacter);
	sf.SetFormatFlags(StringFormatFlagsNoWrap);

	std::wstring wtitle = Utf8ToWide(title);
	std::wstring wartist = Utf8ToWide(artist);

	int titleLineH = titleSize + 10;
	int artistLineH = artistSize + 8;
	int blockH = titleLineH + artistLineH;

	int topY = (cardH - blockH) / 2 + s.text_offset_y;

	RectF titleRect((REAL)textX, (REAL)topY, (REAL)textW, (REAL)titleLineH);
	g.DrawString(wtitle.c_str(), -1, &titleFont, titleRect, &sf, &titleBrush);

	RectF artistRect((REAL)textX, (REAL)(topY + titleLineH), (REAL)textW, (REAL)artistLineH);
	g.DrawString(wartist.c_str(), -1, &artistFont, artistRect, &sf, &artistBrush);

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
	return AppearanceSettings{ctx->title_color, ctx->artist_color, ctx->bg_color,     ctx->bg_opacity, ctx->font_face,
				  ctx->font_style, ctx->font_size,    ctx->font_flags, ctx->artist_font_difference, ctx->card_w,
				  ctx->card_h,     ctx->text_offset_y};
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
		bool track_changed = (has != ctx->have_track) || (title != ctx->last_song) ||
				     (artist != ctx->last_artist);

		if (has) {
			gap_active = false;

			if (track_changed) {
				if (info.ImageData != nullptr && info.ImageLength > 0)
					ctx->last_image.assign(info.ImageData, info.ImageData + info.ImageLength);
				else
					ctx->last_image.clear();
				ctx->last_song = title;
				ctx->last_artist = artist;
				ctx->have_track = true;
				compose_bitmap(ctx, title, artist,
					       ctx->last_image.empty() ? nullptr : ctx->last_image.data(),
					       (int)ctx->last_image.size(), snapshot_settings(ctx));
			} else if (ctx->settings_dirty) {
				compose_bitmap(ctx, ctx->last_song, ctx->last_artist,
					       ctx->last_image.empty() ? nullptr : ctx->last_image.data(),
					       (int)ctx->last_image.size(), snapshot_settings(ctx));
			}
		} else if (ctx->have_track) {			
			if (!gap_active) {
				gap_active = true;
				gap_start = std::chrono::steady_clock::now();
			}

			if (std::chrono::steady_clock::now() - gap_start >= MISSING_SESSION_GRACE) {
				ctx->last_song.clear();
				ctx->last_artist.clear();
				ctx->last_image.clear();
				ctx->have_track = false;
				gap_active = false;
				compose_bitmap(ctx, "", "", nullptr, 0, snapshot_settings(ctx));
			}
		} else if (ctx->settings_dirty) {
			compose_bitmap(ctx, ctx->last_song, ctx->last_artist,
				       ctx->last_image.empty() ? nullptr : ctx->last_image.data(),
				       (int)ctx->last_image.size(), snapshot_settings(ctx));
		}
		ctx->settings_dirty = false;

		if (has && info.ImageData != nullptr)
			FreeImageBuffer(info.ImageData);

		for (int waited = 0; waited < POLL_INTERVAL_MS && ctx->running; waited += 50) {
			if (ctx->settings_dirty)
				break;
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
	return obs_module_text("SpotifyNowPlaying");
}

static void apply_settings(spotify_source *ctx, obs_data_t *settings)
{
	std::lock_guard<std::mutex> lock(ctx->settings_mutex);
	ctx->title_color = obs_data_get_int(settings, "title_color");
	ctx->artist_color = obs_data_get_int(settings, "artist_color");
	ctx->bg_color = obs_data_get_int(settings, "bg_color");

	ctx->bg_opacity = (int)obs_data_get_int(settings, "bg_opacity");
	ctx->bg_opacity = std::clamp(ctx->bg_opacity, 0, 100);

	ctx->card_w = (int)obs_data_get_int(settings, "card_width");
	ctx->card_w = std::clamp(ctx->card_w, 50, 4000);

	ctx->card_h = (int)obs_data_get_int(settings, "card_height");
	ctx->card_h = std::clamp(ctx->card_h, 30, 2000);

	ctx->text_offset_y = (int)obs_data_get_int(settings, "text_offset_y");
	ctx->text_offset_y = std::clamp(ctx->text_offset_y, -1000, 1000);

	ctx->artist_font_difference = (int)obs_data_get_int(settings, "artist_font_difference");
	ctx->artist_font_difference = std::clamp(ctx->artist_font_difference, -50, 50);

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		const char *face = obs_data_get_string(font_obj, "face");
		const char *style = obs_data_get_string(font_obj, "style");
		ctx->font_face = (face && face[0]) ? face : "Segoe UI";
		ctx->font_style = style ? style : "Regular";
		ctx->font_size = (int)obs_data_get_int(font_obj, "size");
		ctx->font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}
	if (ctx->font_size <= 0)
		ctx->font_size = 16;

	ctx->settings_dirty = true;
}

static void spotify_source_update(void *data, obs_data_t *settings)
{
	auto *ctx = (spotify_source *)data;
	apply_settings(ctx, settings);
}

static void spotify_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "title_color", 0xFFFFFFFF); // opaque white
	obs_data_set_default_int(settings, "artist_color", 0xFFFFFFFF);

	obs_data_set_default_int(settings, "bg_color", (long long)(((uint32_t)24 << 16) | ((uint32_t)20 << 8) | 20));
	obs_data_set_default_int(settings, "bg_opacity", 92);

	obs_data_set_default_int(settings, "card_width", DEFAULT_CARD_W);
	obs_data_set_default_int(settings, "card_height", DEFAULT_CARD_H);
	obs_data_set_default_int(settings, "text_offset_y", 0);

	obs_data_set_default_int(settings, "artist_font_difference", -2);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_default_string(font_obj, "face", "Segoe UI");
	obs_data_set_default_string(font_obj, "style", "Bold");
	obs_data_set_default_int(font_obj, "size", 16);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);
}

static obs_properties_t *spotify_source_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_color_alpha(props, "title_color", obs_module_text("Title Color"));
	obs_properties_add_color_alpha(props, "artist_color", obs_module_text("Artist Color"));
	obs_properties_add_color(props, "bg_color", obs_module_text("Background Color"));
	obs_properties_add_int(props, "bg_opacity", obs_module_text("Background Opacity"), 0, 100, 1);
	obs_properties_add_font(props, "font", obs_module_text("Font"));
	obs_properties_add_int(props, "artist_font_difference", obs_module_text("Artist Font Size Difference"), -50, 50, 1);
	obs_properties_add_int(props, "card_width", obs_module_text("Card Width"), 50, 4000, 10);
	obs_properties_add_int(props, "card_height", obs_module_text("Card Height"), 30, 2000, 10);
	obs_properties_add_int(props, "text_offset_y", obs_module_text("Text Vertical Offset"), -1000, 1000, 1);

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
