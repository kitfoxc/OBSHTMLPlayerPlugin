#include "html-player-source.h"
#include <obs-module.h>
#include <util/dstr.h>
#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

namespace {
constexpr int kPollMs = 250;
constexpr unsigned short kPort = 38765;

struct State {
    bool has_track = false;
    bool playing = false;
    std::string title, artist, album, source, art_data;
    double duration = 0;
    double position = 0;
};

std::mutex g_state_mutex;
State g_state;
std::atomic<bool> g_running{false};
std::thread g_server_thread;
SOCKET g_server = INVALID_SOCKET;

std::string json_escape(const std::string &s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\b': o << "\\b"; break;
        case '\f': o << "\\f"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:
            if (c < 0x20) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
            else o << c;
        }
    }
    return o.str();
}

std::string base64(const uint8_t *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? table[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? table[v & 63] : '=');
    }
    return out;
}

std::string content_type(const std::string &ct) {
    if (ct == "image/png") return "image/png";
    if (ct == "image/webp") return "image/webp";
    if (ct == "image/gif") return "image/gif";
    return "image/jpeg";
}

void read_smtc() {
    try {
        init_apartment(apartment_type::multi_threaded);
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        while (g_running) {
            State next;
            try {
                auto sessions = manager.GetSessions();
                GlobalSystemMediaTransportControlsSession session = nullptr;
                for (uint32_t i = 0; i < sessions.Size(); ++i) {
                    auto s = sessions.GetAt(i);
                    std::string id = to_string(s.SourceAppUserModelId());
                    if (id.find("spotify") != std::string::npos || id.find("youtube") != std::string::npos ||
                        id.find("ytm") != std::string::npos || id.find("applemusic") != std::string::npos ||
                        id.find("cider") != std::string::npos || id.find("vlc") != std::string::npos ||
                        id.find("chrome") != std::string::npos || id.find("msedge") != std::string::npos ||
                        id.find("firefox") != std::string::npos || id.find("opera") != std::string::npos ||
                        id.find("brave") != std::string::npos) { session = s; break; }
                }
                if (session) {
                    auto props = session.TryGetMediaPropertiesAsync().get();
                    auto timeline = session.GetTimelineProperties();
                    auto playback = session.GetPlaybackInfo();
                    if (props && timeline && playback) {
                        next.has_track = true;
                        next.playing = playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                        next.title = to_string(props.Title());
                        next.artist = to_string(props.Artist());
                        next.album = to_string(props.AlbumTitle());
                        next.source = to_string(session.SourceAppUserModelId());
                        next.duration = std::max(0.0, std::chrono::duration<double>(timeline.EndTime() - timeline.StartTime()).count());
                        next.position = std::max(0.0, std::chrono::duration<double>(timeline.Position()).count());
                        auto thumb = props.Thumbnail();
                        if (thumb) {
                            auto stream = thumb.OpenReadAsync().get();
                            uint32_t size = (uint32_t)stream.Size();
                            if (size && size < 16 * 1024 * 1024) {
                                DataReader reader(stream);
                                reader.LoadAsync(size).get();
                                std::vector<uint8_t> bytes(size);
                                reader.ReadBytes(array_view<uint8_t>(bytes));
                                next.art_data = "data:" + content_type(to_string(stream.ContentType())) + ";base64," + base64(bytes.data(), bytes.size());
                            }
                        }
                    }
                }
            } catch (...) {}
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                if (next.has_track) g_state = std::move(next);
                else if (!g_state.has_track) g_state = std::move(next);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
        uninit_apartment();
    } catch (...) {
        blog(LOG_ERROR, "[obs-html-player] Windows SMTC initialization failed");
    }
}

std::string state_json() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::ostringstream o;
    o << "{\"hasTrack\":" << (g_state.has_track ? "true" : "false")
      << ",\"playing\":" << (g_state.playing ? "true" : "false")
      << ",\"title\":\"" << json_escape(g_state.title)
      << "\",\"artist\":\"" << json_escape(g_state.artist)
      << "\",\"album\":\"" << json_escape(g_state.album)
      << "\",\"source\":\"" << json_escape(g_state.source)
      << "\",\"duration\":" << g_state.duration
      << ",\"position\":" << g_state.position
      << ",\"timestamp\":" << now
      << ",\"albumArt\":\"" << json_escape(g_state.art_data) << "\"}";
    return o.str();
}

void send_http(SOCKET s, const std::string &body, const char *type) {
    std::ostringstream h;
    h << "HTTP/1.1 200 OK\r\nContent-Type: " << type << "\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n\r\n";
    std::string hs = h.str();
    send(s, hs.data(), (int)hs.size(), 0);
    send(s, body.data(), (int)body.size(), 0);
}

void server_loop() {
    while (g_running) {
        sockaddr_in addr{}; int len = sizeof(addr);
        SOCKET c = accept(g_server, (sockaddr *)&addr, &len);
        if (c == INVALID_SOCKET) continue;
        char buf[2048]{}; int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            std::string req(buf, n);
            if (req.rfind("GET /state", 0) == 0) send_http(c, state_json(), "application/json; charset=utf-8");
            else if (req.rfind("GET /ping", 0) == 0) send_http(c, "{\"ok\":true}", "application/json");
            else send_http(c, "Not Found", "text/plain; charset=utf-8");
        }
        closesocket(c);
    }
}

void start_backend() {
    if (g_running.exchange(true)) return;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { g_running = false; return; }
    g_server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_server == INVALID_SOCKET) { g_running = false; WSACleanup(); return; }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(kPort); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(g_server, (sockaddr *)&a, sizeof(a)) != 0 || listen(g_server, 8) != 0) {
        closesocket(g_server); g_server = INVALID_SOCKET; g_running = false; WSACleanup();
        blog(LOG_ERROR, "[obs-html-player] Could not bind local API port %u", kPort);
        return;
    }
    g_server_thread = std::thread(server_loop);
    std::thread(read_smtc).detach();
}

void stop_backend() {
    if (!g_running.exchange(false)) return;
    if (g_server != INVALID_SOCKET) { shutdown(g_server, SD_BOTH); closesocket(g_server); g_server = INVALID_SOCKET; }
    if (g_server_thread.joinable()) g_server_thread.join();
    WSACleanup();
}

struct html_source { obs_source_t *source = nullptr; obs_source_t *browser = nullptr; int width = 800; int height = 250; };

void rebuild_browser(html_source *s, obs_data_t *settings) {
    const char *file = obs_data_get_string(settings, "local_file");
    if (!file || !*file) return;
    obs_data_t *bs = obs_data_create();
    obs_data_set_bool(bs, "is_local_file", true);
    obs_data_set_string(bs, "local_file", file);
    obs_data_set_int(bs, "width", obs_data_get_int(settings, "width"));
    obs_data_set_int(bs, "height", obs_data_get_int(settings, "height"));
    obs_data_set_int(bs, "fps_num", obs_data_get_int(settings, "fps_num"));
    obs_data_set_int(bs, "fps_den", 1);
    obs_data_set_bool(bs, "shutdown", obs_data_get_bool(settings, "shutdown"));
    obs_data_set_bool(bs, "reroute_audio", false);
    obs_data_set_string(bs, "css", "");
    obs_source_t *nb = obs_source_create_private("browser_source", "HTML Player Browser", bs);
    obs_data_release(bs);
    if (!nb) { blog(LOG_ERROR, "[obs-html-player] browser_source unavailable; OBS Browser Source must be enabled."); return; }
    if (s->browser) { obs_source_remove_active_child(s->source, s->browser); obs_source_release(s->browser); }
    s->browser = nb;
    obs_source_add_active_child(s->source, s->browser);
}

void *create(obs_data_t *settings, obs_source_t *source) {
    auto *s = new html_source; s->source = source;
    s->width = (int)obs_data_get_int(settings, "width"); s->height = (int)obs_data_get_int(settings, "height");
    start_backend(); rebuild_browser(s, settings); return s;
}
void destroy(void *data) {
    auto *s = (html_source *)data;
    if (s->browser) { obs_source_remove_active_child(s->source, s->browser); obs_source_release(s->browser); }
    delete s;
}
void update(void *data, obs_data_t *settings) {
    auto *s = (html_source *)data; s->width = (int)obs_data_get_int(settings, "width"); s->height = (int)obs_data_get_int(settings, "height"); rebuild_browser(s, settings);
}
uint32_t get_width(void *data) { return (uint32_t)((html_source *)data)->width; }
uint32_t get_height(void *data) { return (uint32_t)((html_source *)data)->height; }
void render(void *data, gs_effect_t *) { auto *s = (html_source *)data; if (s->browser) obs_source_video_render(s->browser); }
obs_properties_t *get_properties(void *) {
    auto *p = obs_properties_create();
    obs_properties_add_path(p, "local_file", "HTML 文件", OBS_PATH_FILE, "HTML (*.html;*.htm)", nullptr);
    obs_properties_add_int(p, "width", "宽度", 1, 7680, 1);
    obs_properties_add_int(p, "height", "高度", 1, 7680, 1);
    obs_properties_add_int(p, "fps_num", "FPS", 1, 120, 1);
    obs_properties_add_bool(p, "shutdown", "隐藏时关闭页面");
    return p;
}
void get_defaults(obs_data_t *s) { obs_data_set_default_int(s, "width", 800); obs_data_set_default_int(s, "height", 250); obs_data_set_default_int(s, "fps_num", 60); obs_data_set_default_bool(s, "shutdown", false); }
const char *get_name(void *) { return "HTML Now Playing"; }
obs_source_info info{};
}

extern "C" void html_source_register() {
    info.id = "html_now_playing_source"; info.type = OBS_SOURCE_TYPE_INPUT; info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = get_name; info.create = create; info.destroy = destroy; info.update = update; info.get_width = get_width; info.get_height = get_height;
    info.get_properties = get_properties; info.get_defaults = get_defaults; info.video_render = render;
    obs_register_source(&info);
}
extern "C" void html_source_unregister() { stop_backend(); }
