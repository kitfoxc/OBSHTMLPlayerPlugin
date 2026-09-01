#include "html-player-source.h"
#include <obs-module.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cctype>
#pragma comment(lib, "ws2_32.lib")

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;
namespace fs = std::filesystem;

namespace {
constexpr unsigned short PORT = 38765;
constexpr int POLL_MS = 250;
constexpr size_t MAX_FILE = 16 * 1024 * 1024;

struct State {
    bool has = false;
    bool playing = false;
    std::string title, artist, album, source, art;
    double duration = 0;
    double position = 0;
};

struct Inst {
    std::string id;
    fs::path root;
    fs::path entry;
};

std::mutex state_mutex;
std::mutex instance_mutex;
State current_state;
std::map<std::string, std::weak_ptr<Inst>> instances;
std::atomic_bool running = false;
SOCKET server_socket = INVALID_SOCKET;
std::thread server_thread;
std::thread smtc_thread;

const char *SDK = R"JS((()=>{
const m=location.pathname.match(/^\/p\/([^/]+)\//),id=m?m[1]:'',o=location.origin,e=o+'/state',v=id?o+'/p/'+id+'/__version':'';
let S={hasTrack:false,playing:false,paused:false,stopped:true,title:'',artist:'',album:'',source:'',duration:0,position:0,timestamp:0,albumArt:''},V=null,L=new Map;
const emit=(n,x)=>(L.get(n)||[]).forEach(f=>{try{f(x)}catch(_){}});
const upd=x=>{const q=S;S=Object.assign({hasTrack:false,playing:false,paused:false,stopped:true,title:'',artist:'',album:'',source:'',duration:0,position:0,timestamp:0,albumArt:''},x||{});if(q.title!==S.title||q.artist!==S.artist||q.album!==S.album||q.source!==S.source)emit('trackchange',S);if(q.playing!==S.playing)emit(S.playing?'play':'pause',S);if(q.hasTrack!==S.hasTrack)emit(S.hasTrack?'trackchange':'stop',S);if(q.albumArt!==S.albumArt)emit('albumart',S.albumArt);emit('state',S)};
async function refresh(){try{const r=await fetch(e+'?t='+Date.now(),{cache:'no-store'});if(r.ok)upd(await r.json())}catch(_){}}
async function checkVersion(){if(!v)return;try{const r=await fetch(v+'?t='+Date.now(),{cache:'no-store'});if(!r.ok)return;const n=await r.text();if(V!==null&&n!==V)location.reload();V=n}catch(_) {}}
function loop(){refresh().finally(()=>setTimeout(loop,250))} function versionLoop(){checkVersion().finally(()=>setTimeout(versionLoop,1000))}
window.obsPlayer={get state(){return S},get playerId(){return id},get endpoint(){return e},on(n,f){if(!L.has(n))L.set(n,new Set);L.get(n).add(f);return()=>L.get(n)?.delete(f)},once(n,f){const x=this.on(n,y=>{x();f(y)});return x},refresh,getProgress(){return S.duration?Math.max(0,Math.min(1,S.position/S.duration)):0},getPosition(){if(!S.playing||!S.timestamp)return S.position;const now=performance.timeOrigin/1000+performance.now()/1000;return Math.max(0,Math.min(S.duration||Infinity,S.position+now-S.timestamp))}};
window.OBSPlayer=window.obsPlayer;emit('ready',S);loop();versionLoop();
})();)JS";

std::string escape_json(const std::string &s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
            else out << c;
        }
    }
    return out.str();
}

std::string base64(const uint8_t *data, size_t size) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < size) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < size) v |= data[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < size ? table[(v >> 6) & 63] : '=';
        out += i + 2 < size ? table[v & 63] : '=';
    }
    return out;
}

std::string image_mime(const std::string &content_type) {
    if (content_type == "image/png") return "image/png";
    if (content_type == "image/webp") return "image/webp";
    if (content_type == "image/gif") return "image/gif";
    return "image/jpeg";
}

void smtc_loop() {
    try {
        init_apartment(apartment_type::multi_threaded);
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();

        while (running) {
            State next;
            try {
                auto sessions = manager.GetSessions();
                GlobalSystemMediaTransportControlsSession selected = nullptr;

                const uint32_t count = sessions.Size();
                for (uint32_t i = 0; i < count; ++i) {
                    auto session = sessions.GetAt(i);
                    const std::string app = to_string(session.SourceAppUserModelId());
                    if (app.find("spotify") != std::string::npos ||
                        app.find("youtube") != std::string::npos ||
                        app.find("ytm") != std::string::npos ||
                        app.find("applemusic") != std::string::npos ||
                        app.find("cider") != std::string::npos ||
                        app.find("vlc") != std::string::npos ||
                        app.find("chrome") != std::string::npos ||
                        app.find("msedge") != std::string::npos ||
                        app.find("firefox") != std::string::npos ||
                        app.find("opera") != std::string::npos ||
                        app.find("brave") != std::string::npos) {
                        selected = session;
                        break;
                    }
                }

                if (selected) {
                    auto properties = selected.TryGetMediaPropertiesAsync().get();
                    auto timeline = selected.GetTimelineProperties();
                    auto playback = selected.GetPlaybackInfo();

                    if (properties && playback) {
                        next.has = true;
                        next.playing = playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                        next.title = to_string(properties.Title());
                        next.artist = to_string(properties.Artist());
                        next.album = to_string(properties.AlbumTitle());
                        next.source = to_string(selected.SourceAppUserModelId());
                        next.duration = std::max(0.0, std::chrono::duration<double>(timeline.EndTime() - timeline.StartTime()).count());
                        next.position = std::max(0.0, std::chrono::duration<double>(timeline.Position()).count());

                        auto thumbnail = properties.Thumbnail();
                        if (thumbnail) {
                            auto stream = thumbnail.OpenReadAsync().get();
                            const uint32_t size = static_cast<uint32_t>(stream.Size());
                            if (size > 0 && size <= MAX_FILE) {
                                DataReader reader(stream);
                                reader.LoadAsync(size).get();
                                std::vector<uint8_t> bytes(size);
                                reader.ReadBytes(array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
                                next.art = "data:" + image_mime(to_string(stream.ContentType())) + ";base64," + base64(bytes.data(), bytes.size());
                            }
                        }
                    }
                }
            } catch (...) {
                // Keep polling even when a media session disappears between calls.
            }

            {
                std::lock_guard<std::mutex> lock(state_mutex);
                current_state = std::move(next);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        }
        uninit_apartment();
    } catch (...) {
        blog(LOG_ERROR, "[obs-html-player] SMTC initialization failed");
    }
}

std::string state_json() {
    std::lock_guard<std::mutex> lock(state_mutex);
    const double timestamp = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::ostringstream out;
    out << "{\"hasTrack\":" << (current_state.has ? "true" : "false")
        << ",\"playing\":" << (current_state.playing ? "true" : "false")
        << ",\"paused\":" << (!current_state.playing && current_state.has ? "true" : "false")
        << ",\"stopped\":" << (!current_state.has ? "true" : "false")
        << ",\"title\":\"" << escape_json(current_state.title)
        << "\",\"artist\":\"" << escape_json(current_state.artist)
        << "\",\"album\":\"" << escape_json(current_state.album)
        << "\",\"source\":\"" << escape_json(current_state.source)
        << "\",\"duration\":" << current_state.duration
        << ",\"position\":" << current_state.position
        << ",\"timestamp\":" << timestamp
        << ",\"albumArt\":\"" << escape_json(current_state.art) << "\"}";
    return out.str();
}

std::string url_decode(const std::string &input) {
    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int a = hex(input[i + 1]);
            const int b = hex(input[i + 2]);
            if (a >= 0 && b >= 0) {
                out.push_back(static_cast<char>(a * 16 + b));
                i += 2;
                continue;
            }
        }
        out.push_back(input[i]);
    }
    return out;
}

bool safe_path(const fs::path &root, const fs::path &relative, fs::path &output) {
    try {
        const auto base = fs::weakly_canonical(root);
        const auto candidate = fs::weakly_canonical(root / relative);
        const auto a = base.native();
        const auto b = candidate.native();
        if (b.size() < a.size() || b.compare(0, a.size(), a) != 0)
            return false;
        if (b.size() > a.size() && b[a.size()] != fs::path::preferred_separator)
            return false;
        output = candidate;
        return true;
    } catch (...) {
        return false;
    }
}

std::string mime_type(const fs::path &path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".gif") return "image/gif";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".otf") return "font/otf";
    return "application/octet-stream";
}

void send_response(SOCKET client, int code, const std::string &body, const char *content_type) {
    const char *reason = code == 200 ? "OK" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Bad Request";
    std::ostringstream header;
    header << "HTTP/1.1 " << code << " " << reason << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "Cache-Control: no-store\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n";
    const std::string h = header.str();
    ::send(client, h.data(), static_cast<int>(h.size()), 0);
    if (!body.empty()) ::send(client, body.data(), static_cast<int>(body.size()), 0);
}

void send_file(SOCKET client, const fs::path &path, bool html) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { send_response(client, 404, "Not Found", "text/plain"); return; }
    const auto size = fs::file_size(path);
    if (size > MAX_FILE) { send_response(client, 403, "File too large", "text/plain"); return; }
    std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (html) {
        const std::string injection = "<script src=\"/obs-player.js\"></script><style>html,body{margin:0;background:transparent}</style>";
        const size_t head = body.find("</head>");
        if (head == std::string::npos) body = injection + body;
        else body.insert(head, injection);
    }
    send_response(client, 200, body, mime_type(path).c_str());
}

std::shared_ptr<Inst> find_instance(const std::string &id) {
    std::lock_guard<std::mutex> lock(instance_mutex);
    const auto it = instances.find(id);
    if (it == instances.end()) return {};
    auto value = it->second.lock();
    if (!value) instances.erase(it);
    return value;
}

void server_loop() {
    while (running) {
        sockaddr_in address{};
        int address_length = sizeof(address);
        SOCKET client = accept(server_socket, reinterpret_cast<sockaddr *>(&address), &address_length);
        if (client == INVALID_SOCKET) continue;

        char buffer[8192]{};
        const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received > 0) {
            const std::string request(buffer, received);
            const std::string line = request.substr(0, request.find("\r\n"));
            if (line.rfind("GET ", 0) != 0) {
                send_response(client, 400, "Bad Request", "text/plain");
            } else {
                const size_t space = line.find(' ', 4);
                const std::string url = line.substr(4, space - 4);
                const std::string path = url.substr(0, url.find('?'));

                if (path == "/state") {
                    send_response(client, 200, state_json(), "application/json; charset=utf-8");
                } else if (path == "/obs-player.js") {
                    send_response(client, 200, SDK, "application/javascript; charset=utf-8");
                } else if (path == "/ping") {
                    send_response(client, 200, "{\"ok\":true}", "application/json");
                } else if (path.rfind("/p/", 0) == 0) {
                    const size_t slash = path.find('/', 3);
                    if (slash == std::string::npos) {
                        send_response(client, 404, "Not Found", "text/plain");
                    } else {
                        auto instance = find_instance(path.substr(3, slash - 3));
                        if (!instance) {
                            send_response(client, 404, "Not Found", "text/plain");
                        } else {
                            std::string relative = url_decode(path.substr(slash + 1));
                            if (relative == "__version") {
                                try {
                                    send_response(client, 200, std::to_string(fs::last_write_time(instance->entry).time_since_epoch().count()), "text/plain");
                                } catch (...) {
                                    send_response(client, 404, "Not Found", "text/plain");
                                }
                            } else {
                                if (relative.empty()) relative = instance->entry.filename().string();
                                fs::path output;
                                if (!safe_path(instance->root, relative, output) || !fs::is_regular_file(output)) {
                                    send_response(client, 404, "Not Found", "text/plain");
                                } else {
                                    std::string ext = output.extension().string();
                                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                                    send_file(client, output, ext == ".html" || ext == ".htm");
                                }
                            }
                        }
                    }
                } else {
                    send_response(client, 404, "Not Found", "text/plain");
                }
            }
        }
        closesocket(client);
    }
}

void start_server() {
    if (running.exchange(true)) return;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { running = false; return; }

    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (server_socket == INVALID_SOCKET || bind(server_socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(server_socket, 16) != 0) {
        if (server_socket != INVALID_SOCKET) closesocket(server_socket);
        server_socket = INVALID_SOCKET;
        running = false;
        WSACleanup();
        blog(LOG_ERROR, "[obs-html-player] local server bind failed");
        return;
    }

    server_thread = std::thread(server_loop);
    smtc_thread = std::thread(smtc_loop);
}

void stop_server() {
    if (!running.exchange(false)) return;
    if (server_socket != INVALID_SOCKET) {
        shutdown(server_socket, SD_BOTH);
        closesocket(server_socket);
        server_socket = INVALID_SOCKET;
    }
    if (server_thread.joinable()) server_thread.join();
    if (smtc_thread.joinable()) smtc_thread.join();
    { std::lock_guard<std::mutex> lock(instance_mutex); instances.clear(); }
    WSACleanup();
}

struct Source {
    obs_source_t *source = nullptr;
    obs_source_t *browser = nullptr;
    std::shared_ptr<Inst> instance;
    int width = 800;
    int height = 250;
};

std::string source_id(obs_source_t *source) {
    std::ostringstream out;
    out << std::hex << reinterpret_cast<uintptr_t>(source);
    return out.str();
}

void rebuild_browser(Source *source, obs_data_t *settings) {
    const char *file_name = obs_data_get_string(settings, "local_file");
    if (!file_name || !*file_name) return;

    fs::path entry;
    try {
        entry = fs::weakly_canonical(fs::path(file_name));
        if (!fs::is_regular_file(entry)) return;
    } catch (...) { return; }

    auto instance = std::make_shared<Inst>();
    instance->id = source_id(source->source);
    instance->entry = entry;
    instance->root = entry.parent_path();
    { std::lock_guard<std::mutex> lock(instance_mutex); instances[instance->id] = instance; }
    source->instance = instance;

    if (source->browser) {
        obs_source_remove_active_child(source->source, source->browser);
        obs_source_release(source->browser);
        source->browser = nullptr;
    }

    obs_data_t *browser_settings = obs_data_create();
    const std::string url = "http://127.0.0.1:" + std::to_string(PORT) + "/p/" + instance->id + "/";
    obs_data_set_bool(browser_settings, "is_local_file", false);
    obs_data_set_string(browser_settings, "url", url.c_str());
    obs_data_set_int(browser_settings, "width", source->width);
    obs_data_set_int(browser_settings, "height", source->height);
    obs_data_set_bool(browser_settings, "fps_custom", true);
    obs_data_set_int(browser_settings, "fps", 60);
    obs_data_set_bool(browser_settings, "shutdown", false);
    obs_data_set_bool(browser_settings, "restart_when_active", false);
    obs_data_set_string(browser_settings, "css", "html,body{margin:0;background:transparent;overflow:hidden}");

    source->browser = obs_source_create_private("browser_source", "HTML Player Browser", browser_settings);
    obs_data_release(browser_settings);

    if (source->browser) {
        obs_source_add_active_child(source->source, source->browser);
    } else {
        blog(LOG_ERROR, "[obs-html-player] obs-browser source is unavailable");
    }
}

const char *source_get_name(void *) { return "HTML Now Playing"; }

void *source_create(obs_data_t *settings, obs_source_t *source) {
    auto *data = new Source();
    data->source = source;
    data->width = (int)obs_data_get_int(settings, "width");
    data->height = (int)obs_data_get_int(settings, "height");
    if (data->width <= 0) data->width = 800;
    if (data->height <= 0) data->height = 250;
    rebuild_browser(data, settings);
    return data;
}

void source_destroy(void *ptr) {
    auto *data = static_cast<Source *>(ptr);
    if (!data) return;
    if (data->browser) {
        obs_source_remove_active_child(data->source, data->browser);
        obs_source_release(data->browser);
    }
    delete data;
}

void source_update(void *ptr, obs_data_t *settings) {
    auto *data = static_cast<Source *>(ptr);
    if (!data) return;
    const int width = (int)obs_data_get_int(settings, "width");
    const int height = (int)obs_data_get_int(settings, "height");
    const bool changed = width != data->width || height != data->height ||
                         std::string(obs_data_get_string(settings, "local_file")) != (data->instance ? data->instance->entry.string() : "");
    data->width = width > 0 ? width : 800;
    data->height = height > 0 ? height : 250;
    if (changed) rebuild_browser(data, settings);
}

uint32_t source_width(void *ptr) { return static_cast<uint32_t>(static_cast<Source *>(ptr)->width); }
uint32_t source_height(void *ptr) { return static_cast<uint32_t>(static_cast<Source *>(ptr)->height); }

void source_video_render(void *ptr, gs_effect_t *) {
    auto *data = static_cast<Source *>(ptr);
    if (data && data->browser) obs_source_video_render(data->browser);
}

obs_properties_t *source_properties(void *) {
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_path(props, "local_file", "HTML file", OBS_PATH_FILE, "HTML files (*.html *.htm);;All files (*.*)", nullptr);
    obs_properties_add_int(props, "width", "Width", 1, 7680, 1);
    obs_properties_add_int(props, "height", "Height", 1, 4320, 1);
    return props;
}

obs_source_info source_info = {};

} // namespace

void html_source_register(void) {
    start_server();
    source_info.id = "html_now_playing_source";
    source_info.type = OBS_SOURCE_TYPE_INPUT;
    source_info.output_flags = OBS_SOURCE_VIDEO;
    source_info.get_name = source_get_name;
    source_info.create = source_create;
    source_info.destroy = source_destroy;
    source_info.update = source_update;
    source_info.get_width = source_width;
    source_info.get_height = source_height;
    source_info.video_render = source_video_render;
    source_info.get_properties = source_properties;
    obs_register_source(&source_info);
}

void html_source_unregister(void) {
    stop_server();
}
