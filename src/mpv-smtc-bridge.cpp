#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <systemmediatransportcontrolsinterop.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "user32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "propsys")

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Media;
using namespace Windows::Data::Json;

static constexpr wchar_t WINDOW_CLASS_NAME[] = L"mpv_smtc_bridge_hidden_window";
static constexpr wchar_t APP_ID[] = L"mpv.exe";
static constexpr UINT WM_MPV_PROCESS_EXITED = WM_APP + 0x233;
static constexpr wchar_t SMTC_OWNER_MUTEX[] = L"Local\\mpv_smtc_bridge_first_owner";

static void set_window_app_id(HWND hwnd)
{
    winrt::com_ptr<IPropertyStore> store;

    HRESULT hr = SHGetPropertyStoreForWindow(
        hwnd,
        IID_PPV_ARGS(store.put())
    );

    if (FAILED(hr)) return;

    PROPVARIANT pv;
    PropVariantInit(&pv);

    hr = InitPropVariantFromString(APP_ID, &pv);
    if (SUCCEEDED(hr)) {
        store->SetValue(PKEY_AppUserModel_ID, pv);
        store->Commit();
    }

    PropVariantClear(&pv);
}

static std::wstring lower_copy(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

static std::wstring filename_from_path(const std::wstring& path)
{
    auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

static std::wstring trim_copy(std::wstring s)
{
    auto is_space = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };

    while (!s.empty() && is_space(s.front())) {
        s.erase(s.begin());
    }

    while (!s.empty() && is_space(s.back())) {
        s.pop_back();
    }

    return s;
}

static std::wstring strip_extension(std::wstring s)
{
    auto slash = s.find_last_of(L"\\/");
    auto dot = s.find_last_of(L'.');

    if (dot != std::wstring::npos &&
        (slash == std::wstring::npos || dot > slash) &&
        s.size() - dot <= 8) {
        s.erase(dot);
    }

    return s;
}

static void split_artist_title(
    const std::wstring& raw,
    std::wstring& artist,
    std::wstring& title
)
{
    std::wstring s = trim_copy(strip_extension(filename_from_path(raw)));

    const wchar_t* separators[] = {
        L" - ",
        L" – ",
        L" — ",
        L"／",
        L" / "
    };

    for (auto sep : separators) {
        auto pos = s.find(sep);
        if (pos != std::wstring::npos) {
            std::wstring a = trim_copy(s.substr(0, pos));
            std::wstring t = trim_copy(s.substr(pos + wcslen(sep)));

            if (!a.empty() && !t.empty()) {
                artist = a;
                title = t;
                return;
            }
        }
    }

    title = s;
}

static std::wstring number_json(double value)
{
    if (!std::isfinite(value)) value = 0.0;

    std::wostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::fixed << std::setprecision(3) << value;
    return ss.str();
}

static TimeSpan seconds_to_timespan(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;

    using namespace std::chrono;
    return duration_cast<TimeSpan>(duration<double>(seconds));
}

static double timespan_to_seconds(TimeSpan value)
{
    return std::chrono::duration<double>(value).count();
}

static bool json_bool(IJsonValue const& v, bool fallback)
{
    if (!v || v.ValueType() != JsonValueType::Boolean) return fallback;
    return v.GetBoolean();
}

static double json_number(IJsonValue const& v, double fallback)
{
    if (!v || v.ValueType() != JsonValueType::Number) return fallback;
    return v.GetNumber();
}

static std::wstring json_string(IJsonValue const& v, const std::wstring& fallback = L"")
{
    if (!v || v.ValueType() != JsonValueType::String) return fallback;
    return std::wstring(v.GetString().c_str());
}

static std::wstring obj_string(JsonObject const& obj, const wchar_t* key, const std::wstring& fallback = L"")
{
    if (!obj.HasKey(key)) return fallback;
    return json_string(obj.Lookup(key), fallback);
}

static bool obj_bool(JsonObject const& obj, const wchar_t* key, bool fallback)
{
    if (!obj.HasKey(key)) return fallback;
    return json_bool(obj.Lookup(key), fallback);
}

static double obj_number(JsonObject const& obj, const wchar_t* key, double fallback)
{
    if (!obj.HasKey(key)) return fallback;
    return json_number(obj.Lookup(key), fallback);
}

class MpvSmtcBridge
{
public:
    MpvSmtcBridge(
        std::wstring mpv_pipe_name,
        std::wstring bridge_pipe_name,
        DWORD main_thread_id,
        DWORD mpv_pid
    )
        : pipe_name_(std::move(mpv_pipe_name)),
        bridge_pipe_name_(std::move(bridge_pipe_name)),
        main_thread_id_(main_thread_id),
        mpv_pid_(mpv_pid)
    {
    }

    ~MpvSmtcBridge()
    {
        stop();
    }

    void init_smtc(HWND hwnd)
    {
        hwnd_ = hwnd;
        auto interop = winrt::get_activation_factory<
            SystemMediaTransportControls,
            ::ISystemMediaTransportControlsInterop>();

        smtc_ = winrt::capture<SystemMediaTransportControls>(
            interop,
            &::ISystemMediaTransportControlsInterop::GetForWindow,
            hwnd);

        smtc_.IsEnabled(true);
        smtc_.IsPlayEnabled(true);
        smtc_.IsPauseEnabled(true);
        smtc_.IsStopEnabled(true);
        smtc_.IsNextEnabled(true);
        smtc_.IsPreviousEnabled(true);
        smtc_.IsFastForwardEnabled(true);
        smtc_.IsRewindEnabled(true);
        smtc_.PlaybackRate(1.0);
        smtc_.PlaybackStatus(MediaPlaybackStatus::Stopped);

        smtc_.ButtonPressed([this](auto const&, SystemMediaTransportControlsButtonPressedEventArgs const& args) {
            on_button(args.Button());
        });

        smtc_.PlaybackPositionChangeRequested([this](auto const&, PlaybackPositionChangeRequestedEventArgs const& args) {
            double sec = timespan_to_seconds(args.RequestedPlaybackPosition());
            send_command_once(L"{\"command\":[\"set_property\",\"time-pos\"," + number_json(sec) + L"]}");
        });

        smtc_.PlaybackRateChangeRequested([this](auto const&, PlaybackRateChangeRequestedEventArgs const& args) {
            double rate = args.RequestedPlaybackRate();
            if (rate >= 0.25 && rate <= 4.0) {
                send_command_once(L"{\"command\":[\"set_property\",\"speed\"," + number_json(rate) + L"]}");
            }
        }); 

        update_display();
        update_timeline(true);
    }

    bool acquire_smtc_ownership()
    {
        owner_mutex_ = CreateMutexW(
            nullptr,
            TRUE,
            SMTC_OWNER_MUTEX
        );

        if (!owner_mutex_) {
            owns_smtc_ = false;
            return false;
        }

        DWORD err = GetLastError();

        if (err == ERROR_ALREADY_EXISTS) {
            owns_smtc_ = false;
            return false;
        }

        owns_smtc_ = true;
        return true;
    }

    void shutdown_smtc()
    {
        idle_ = true;
        pause_ = true;
        position_ = 0;
        duration_ = 0;

        if (smtc_) {
            smtc_.PlaybackStatus(MediaPlaybackStatus::Stopped);
            smtc_.IsEnabled(false);
        }

        update_timeline(true);
    }

    void start_watchdog()
    {
        if (mpv_pid_ == 0) {
            return;
        }

        watchdog_ = std::thread([this]() {
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, mpv_pid_);
            if (!h) {
                return;
            }

            while (alive_) {
                DWORD r = WaitForSingleObject(h, 500);

                if (r == WAIT_OBJECT_0) {
                    PostThreadMessageW(main_thread_id_, WM_MPV_PROCESS_EXITED, 0, 0);
                    break;
                }

                if (r == WAIT_FAILED) {
                    break;
                }
            }

            CloseHandle(h);
        });
    }

    void start()
    {
        worker_ = std::thread([this]() {
            while (alive_) {
                if (!connect_event_pipe()) {
                    continue;
                }

                read_loop();

                if (event_pipe_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(event_pipe_);
                    event_pipe_ = INVALID_HANDLE_VALUE;
                }
            }
        });
    }

    void stop()
    {
        alive_ = false;

        if (event_pipe_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(event_pipe_, nullptr);
            CloseHandle(event_pipe_);
            event_pipe_ = INVALID_HANDLE_VALUE;
        }

        if (worker_.joinable()) {
            worker_.join();
        }

        if (watchdog_.joinable()) {
            watchdog_.join();
        }

        if (owner_mutex_) {
            if (owns_smtc_) {
                ReleaseMutex(owner_mutex_);
            }

            CloseHandle(owner_mutex_);
            owner_mutex_ = nullptr;
            owns_smtc_ = false;
        }
    }

private:
    HWND hwnd_ = nullptr;
    DWORD mpv_pid_ = 0;
    std::thread watchdog_;
    HANDLE owner_mutex_ = nullptr;
    bool owns_smtc_ = false;
    enum PropertyId
    {
        PROP_PAUSE = 1,
        PROP_IDLE,
        PROP_MEDIA_TITLE,
        PROP_FILENAME,
        PROP_PATH,
        PROP_METADATA,
        PROP_DURATION,
        PROP_TIME_POS,
        PROP_PLAYLIST_POS,
        PROP_PLAYLIST_COUNT,
        PROP_SPEED
    };

    bool open_pipe(HANDLE& h, DWORD access)
    {
        for (int i = 0; i < 150 && alive_; ++i) {
            h = CreateFileW(
                pipe_name_.c_str(),
                access,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (h != INVALID_HANDLE_VALUE) {
                return true;
            }

            DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(pipe_name_.c_str(), 500);
            } else {
                Sleep(200);
            }
        }

        return false;
    }

    bool connect_event_pipe()
    {
        event_pipe_ = CreateNamedPipeW(
            bridge_pipe_name_.c_str(),
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            0,
            nullptr
        );

        if (event_pipe_ == INVALID_HANDLE_VALUE) {
            Sleep(500);
            return false;
        }

        BOOL ok = ConnectNamedPipe(event_pipe_, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!ok) {
            CloseHandle(event_pipe_);
            event_pipe_ = INVALID_HANDLE_VALUE;
            Sleep(500);
            return false;
        }

        return true;
    }

    void write_line_to_handle(HANDLE h, const std::wstring& line)
    {
        if (h == INVALID_HANDLE_VALUE) return;

        std::string utf8 = winrt::to_string(line + L"\n");
        DWORD written = 0;

        WriteFile(
            h,
            utf8.data(),
            static_cast<DWORD>(utf8.size()),
            &written,
            nullptr
        );
    }

    void send_observe_line(const std::wstring& line)
    {
        write_line_to_handle(event_pipe_, line);
    }

    void send_command_once(const std::wstring& line)
    {
        std::lock_guard<std::mutex> lock(command_mutex_);

        HANDLE h = INVALID_HANDLE_VALUE;

        for (int i = 0; i < 20 && alive_; ++i) {
            h = CreateFileW(
                pipe_name_.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (h != INVALID_HANDLE_VALUE) {
                break;
            }

            if (GetLastError() == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(pipe_name_.c_str(), 200);
            } else {
                Sleep(50);
            }
        }

        if (h == INVALID_HANDLE_VALUE) {
            return;
        }

        write_line_to_handle(h, line);
        CloseHandle(h);
    }

    void observe(int id, const wchar_t* property)
    {
        send_observe_line(
            L"{\"command\":[\"observe_property\"," +
            std::to_wstring(id) +
            L",\"" +
            property +
            L"\"]}"
        );
    }

    void request_property(int id, const wchar_t* property)
    {
        send_observe_line(
            L"{\"command\":[\"get_property\",\"" +
            std::wstring(property) +
            L"\"],\"request_id\":" +
            std::to_wstring(id) +
            L"}"
        );
    }

    void request_initial_snapshot()
    {
        request_property(PROP_PAUSE, L"pause");
        request_property(PROP_IDLE, L"idle-active");
        request_property(PROP_MEDIA_TITLE, L"media-title");
        request_property(PROP_FILENAME, L"filename");
        request_property(PROP_PATH, L"path");
        request_property(PROP_METADATA, L"metadata");
        request_property(PROP_DURATION, L"duration");
        request_property(PROP_TIME_POS, L"time-pos");
        request_property(PROP_PLAYLIST_POS, L"playlist-pos");
        request_property(PROP_PLAYLIST_COUNT, L"playlist-count");
        request_property(PROP_SPEED, L"speed");
    }

    void subscribe()
    {
        observe(PROP_PAUSE, L"pause");
        observe(PROP_IDLE, L"idle-active");
        observe(PROP_MEDIA_TITLE, L"media-title");
        observe(PROP_FILENAME, L"filename");
        observe(PROP_PATH, L"path");
        observe(PROP_METADATA, L"metadata");
        observe(PROP_DURATION, L"duration");
        observe(PROP_TIME_POS, L"time-pos");
        observe(PROP_PLAYLIST_POS, L"playlist-pos");
        observe(PROP_PLAYLIST_COUNT, L"playlist-count");
        observe(PROP_SPEED, L"speed");
    }

    void read_loop()
    {
        std::string buffer;
        char chunk[8192];

        while (alive_) {
            DWORD read = 0;

            BOOL ok = ReadFile(
                event_pipe_,
                chunk,
                static_cast<DWORD>(sizeof(chunk)),
                &read,
                nullptr
            );

            if (!ok || read == 0) {
                break;
            }

            buffer.append(chunk, chunk + read);

            for (;;) {
                auto pos = buffer.find('\n');
                if (pos == std::string::npos) break;

                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);

                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (!line.empty()) {
                    handle_json_line(line);
                }
            }
        }
    }

    void handle_json_line(const std::string& line)
    {
        JsonObject obj{ nullptr };

        if (!JsonObject::TryParse(winrt::to_hstring(line), obj)) {
            return;
        }


        std::wstring type = obj_string(obj, L"type");

        if (!owns_smtc_) {
            if (type == L"quit") {
                PostThreadMessageW(main_thread_id_, WM_QUIT, 0, 0);
            }

            return;
        }

        if (type == L"quit") {
            idle_ = true;
            pause_ = true;
            position_ = 0;
            duration_ = 0;

            if (smtc_) {
                smtc_.PlaybackStatus(MediaPlaybackStatus::Stopped);
                smtc_.IsEnabled(false);
            }

            update_timeline(true);
            PostThreadMessageW(main_thread_id_, WM_QUIT, 0, 0);
            return;
        }

        if (type == L"track") {
            bool changed = false;

            if (obj.HasKey(L"title")) {
                std::wstring v = obj_string(obj, L"title");
                if (v != meta_title_) {
                    meta_title_ = v;
                    changed = true;
                }
            }

            if (obj.HasKey(L"artist")) {
                std::wstring v = obj_string(obj, L"artist");
                if (v != meta_artist_) {
                    meta_artist_ = v;
                    changed = true;
                }
            }

            if (obj.HasKey(L"album")) {
                std::wstring v = obj_string(obj, L"album");
                if (v != meta_album_) {
                    meta_album_ = v;
                    changed = true;
                }
            }

            if (changed) {
                update_display();
            }

            return;
        }

        if (type == L"state") {
            if (obj.HasKey(L"paused")) {
                pause_ = obj_bool(obj, L"paused", pause_);
            }

            if (obj.HasKey(L"idle")) {
                idle_ = obj_bool(obj, L"idle", idle_);
            }

            if (obj.HasKey(L"speed")) {
                speed_ = obj_number(obj, L"speed", speed_);
                if (smtc_) {
                    smtc_.PlaybackRate(speed_);
                }
            }

            if (obj.HasKey(L"playlist_pos")) {
                playlist_pos_ = static_cast<int>(
                    obj_number(obj, L"playlist_pos", playlist_pos_)
                );
            }

            if (obj.HasKey(L"playlist_count")) {
                playlist_count_ = static_cast<int>(
                    obj_number(obj, L"playlist_count", playlist_count_)
                );
            }

            update_button_state();
            update_status();
            return;
        }

        if (type == L"timeline") {
            if (obj.HasKey(L"duration")) {
                duration_ = obj_number(obj, L"duration", duration_);
            }

            if (obj.HasKey(L"position")) {
                position_ = obj_number(obj, L"position", position_);
            }

            update_timeline(true);
            return;
        }

        if (type == L"update") {
            bool track_changed = false;

            if (obj.HasKey(L"title")) {
                std::wstring v = obj_string(obj, L"title");
                if (v != meta_title_) {
                    meta_title_ = v;
                    track_changed = true;
                }
            }

            if (obj.HasKey(L"artist")) {
                std::wstring v = obj_string(obj, L"artist");
                if (v != meta_artist_) {
                    meta_artist_ = v;
                    track_changed = true;
                }
            }

            if (obj.HasKey(L"album")) {
                std::wstring v = obj_string(obj, L"album");
                if (v != meta_album_) {
                    meta_album_ = v;
                    track_changed = true;
                }
            }

            if (obj.HasKey(L"duration")) {
                duration_ = obj_number(obj, L"duration", duration_);
            }

            if (obj.HasKey(L"position")) {
                position_ = obj_number(obj, L"position", position_);
            }

            if (obj.HasKey(L"paused")) {
                pause_ = obj_bool(obj, L"paused", pause_);
            }

            if (obj.HasKey(L"idle")) {
                idle_ = obj_bool(obj, L"idle", idle_);
            }

            if (obj.HasKey(L"speed")) {
                speed_ = obj_number(obj, L"speed", speed_);
                if (smtc_) {
                    smtc_.PlaybackRate(speed_);
                }
            }

            if (obj.HasKey(L"playlist_pos")) {
                playlist_pos_ = static_cast<int>(
                    obj_number(obj, L"playlist_pos", playlist_pos_)
                );
            }

            if (obj.HasKey(L"playlist_count")) {
                playlist_count_ = static_cast<int>(
                    obj_number(obj, L"playlist_count", playlist_count_)
                );
            }

            if (track_changed) {
                update_display();
            }

            update_button_state();
            update_status();
            update_timeline(true);
            return;
        }
    }

    void handle_property(int id, IJsonValue const& data)
    {
        bool display_dirty = false;
        bool status_dirty = false;
        bool timeline_dirty = false;
        bool controls_dirty = false;

        switch (id) {
        case PROP_PAUSE:
            pause_ = json_bool(data, pause_);
            status_dirty = true;
            break;

        case PROP_IDLE:
            idle_ = json_bool(data, idle_);
            status_dirty = true;
            break;

        case PROP_MEDIA_TITLE:
            media_title_ = json_string(data);
            display_dirty = true;
            break;

        case PROP_FILENAME:
            filename_ = json_string(data);
            display_dirty = true;
            break;

        case PROP_PATH:
            path_ = json_string(data);
            display_dirty = true;
            break;

        case PROP_METADATA:
            read_metadata(data);
            display_dirty = true;
            break;

        case PROP_DURATION:
            duration_ = json_number(data, -1);
            timeline_dirty = true;
            break;

        case PROP_TIME_POS:
            position_ = json_number(data, 0);
            timeline_dirty = true;
            break;

        case PROP_PLAYLIST_POS:
            playlist_pos_ = static_cast<int>(json_number(data, -1));
            controls_dirty = true;
            break;

        case PROP_PLAYLIST_COUNT:
            playlist_count_ = static_cast<int>(json_number(data, 0));
            controls_dirty = true;
            break;

        case PROP_SPEED:
            speed_ = json_number(data, 1.0);
            if (smtc_) smtc_.PlaybackRate(speed_);
            break;

        default:
            break;
        }

        if (status_dirty) update_status();
        if (controls_dirty) update_button_state();
        if (display_dirty) update_display();
        if (timeline_dirty) update_timeline(false);
    }

    void read_metadata(IJsonValue const& data)
    {
        meta_title_.clear();
        meta_artist_.clear();
        meta_album_.clear();
        meta_album_artist_.clear();

        if (!data || data.ValueType() != JsonValueType::Object) {
            return;
        }

        JsonObject meta = data.GetObject();

        for (auto const& kv : meta) {
            std::wstring key = lower_copy(std::wstring(kv.Key().c_str()));
            IJsonValue val = kv.Value();

            if (!val || val.ValueType() != JsonValueType::String) {
                continue;
            }

            std::wstring s = std::wstring(val.GetString().c_str());
            if (key == L"title") {
                meta_title_ = s;
            } else if (
                key == L"artist" ||
                key == L"artists" ||
                key == L"performer" ||
                key == L"album_artist" ||
                key == L"album artist" ||
                key == L"albumartist"
            ) {
                if (meta_artist_.empty()) {
                    meta_artist_ = s;
                }
            } else if (
                key == L"album"
            ) {
                meta_album_ = s;
            } else if (
                key == L"album_artist" ||
                key == L"album artist" ||
                key == L"albumartist"
            ) {
                meta_album_artist_ = s;
            }
        }
    }

    std::wstring best_title() const
    {
        if (!meta_title_.empty()) return meta_title_;
        if (!media_title_.empty()) return media_title_;
        if (!filename_.empty()) return filename_;
        if (!path_.empty()) return filename_from_path(path_);
        return L"mpv";
    }

    std::wstring best_artist() const
    {
        if (!meta_artist_.empty()) return meta_artist_;
        if (!meta_album_artist_.empty()) return meta_album_artist_;
        return L"";
    }

    void update_status()
    {
        if (!smtc_) return;

        if (idle_) {
            smtc_.PlaybackStatus(MediaPlaybackStatus::Stopped);
        } else if (pause_) {
            smtc_.PlaybackStatus(MediaPlaybackStatus::Paused);
        } else {
            smtc_.PlaybackStatus(MediaPlaybackStatus::Playing);
        }
    }

    void update_button_state()
    {
        if (!smtc_) return;

        bool has_playlist = playlist_count_ > 1;

        smtc_.IsPreviousEnabled(has_playlist);
        smtc_.IsNextEnabled(has_playlist);
    }

    void update_display()
    {
        if (!smtc_) return;

        std::wstring title = trim_copy(meta_title_);
        std::wstring artist = trim_copy(best_artist());

        if (title.empty()) {
            std::wstring parsed_artist;
            std::wstring parsed_title;

            if (!media_title_.empty()) {
                split_artist_title(media_title_, parsed_artist, parsed_title);
            } else if (!filename_.empty()) {
                split_artist_title(filename_, parsed_artist, parsed_title);
            } else if (!path_.empty()) {
                split_artist_title(path_, parsed_artist, parsed_title);
            }

            if (!parsed_title.empty()) {
                title = parsed_title;
            }

            if (artist.empty() && !parsed_artist.empty()) {
                artist = parsed_artist;
            }
        }

        if (title.empty()) {
            title = L"mpv";
        }

        auto updater = smtc_.DisplayUpdater();

        updater.ClearAll();
        updater.Type(MediaPlaybackType::Music);

        auto music = updater.MusicProperties();

        music.Title(title);
        music.Artist(artist);
        music.AlbumTitle(meta_album_);

        if (!meta_album_artist_.empty()) {
            music.AlbumArtist(meta_album_artist_);
        } else {
            music.AlbumArtist(artist);
        }

        updater.Update();

        if (hwnd_) {
            std::wstring window_title = artist.empty()
                ? title
                : artist + L" - " + title;

            SetWindowTextW(hwnd_, window_title.c_str());
        }
    }

    void update_timeline(bool force)
    {
        if (!smtc_) return;

        auto now = std::chrono::steady_clock::now();

        if (!force) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_timeline_update_).count();

            if (!pause_ && elapsed < 4500) {
                return;
            }
        }

        last_timeline_update_ = now;

        if (duration_ <= 0 || !std::isfinite(duration_)) {
            SystemMediaTransportControlsTimelineProperties t;
            t.StartTime(seconds_to_timespan(0));
            t.MinSeekTime(seconds_to_timespan(0));
            t.Position(seconds_to_timespan(0));
            t.MaxSeekTime(seconds_to_timespan(0));
            t.EndTime(seconds_to_timespan(0));
            smtc_.UpdateTimelineProperties(t);
            return;
        }

        double pos = position_;
        if (!std::isfinite(pos)) pos = 0;
        if (pos < 0) pos = 0;
        if (pos > duration_) pos = duration_;

        SystemMediaTransportControlsTimelineProperties t;
        t.StartTime(seconds_to_timespan(0));
        t.MinSeekTime(seconds_to_timespan(0));
        t.Position(seconds_to_timespan(pos));
        t.MaxSeekTime(seconds_to_timespan(duration_));
        t.EndTime(seconds_to_timespan(duration_));

        smtc_.UpdateTimelineProperties(t);
    }

    void on_button(SystemMediaTransportControlsButton button)
    {
        switch (button) {
        case SystemMediaTransportControlsButton::Play:
            send_command_once(L"{\"command\":[\"set_property\",\"pause\",false]}");
            break;

        case SystemMediaTransportControlsButton::Pause:
            send_command_once(L"{\"command\":[\"set_property\",\"pause\",true]}");
            break;

        case SystemMediaTransportControlsButton::Stop:
            send_command_once(L"{\"command\":[\"stop\"]}");
            break;

        case SystemMediaTransportControlsButton::Next:
            send_command_once(L"{\"command\":[\"playlist-next\",\"force\"]}");
            break;

        case SystemMediaTransportControlsButton::Previous:
            send_command_once(L"{\"command\":[\"playlist-prev\",\"force\"]}");
            break;

        case SystemMediaTransportControlsButton::FastForward:
            send_command_once(L"{\"command\":[\"seek\",10,\"relative\"]}");
            break;

        case SystemMediaTransportControlsButton::Rewind:
            send_command_once(L"{\"command\":[\"seek\",-10,\"relative\"]}");
            break;

        default:
            break;
        }
    }

private:
    std::wstring pipe_name_;
    std::wstring bridge_pipe_name_;
    DWORD main_thread_id_ = 0;

    HANDLE event_pipe_ = INVALID_HANDLE_VALUE;
    std::mutex command_mutex_;
    std::thread worker_;
    std::atomic<bool> alive_{ true };

    SystemMediaTransportControls smtc_{ nullptr };

    bool pause_ = true;
    bool idle_ = true;

    double duration_ = -1;
    double position_ = 0;
    double speed_ = 1.0;

    int playlist_pos_ = -1;
    int playlist_count_ = 0;

    std::wstring media_title_;
    std::wstring filename_;
    std::wstring path_;

    std::wstring meta_title_;
    std::wstring meta_artist_;
    std::wstring meta_album_;
    std::wstring meta_album_artist_;

    std::chrono::steady_clock::time_point last_timeline_update_{};
};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND create_hidden_window()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = WINDOW_CLASS_NAME;

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        WINDOW_CLASS_NAME,
        L"mpv",
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1,
        1,
        nullptr,
        nullptr,
        inst,
        nullptr);

    return hwnd;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::wcerr << L"usage: mpv-smtc-bridge.exe \\\\.\\pipe\\mpv-smtc\n";
        return 2;
    }
    DWORD mpv_pid = 0;

    if (argc >= 4) {
        mpv_pid = static_cast<DWORD>(wcstoul(argv[3], nullptr, 10));
    }

    SetCurrentProcessExplicitAppUserModelID(APP_ID);

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    HWND hwnd = create_hidden_window();
    if (!hwnd) {
        std::wcerr << L"CreateWindow failed\n";
        return 3;
    }

    set_window_app_id(hwnd);

    DWORD main_thread_id = GetCurrentThreadId();

    std::wstring mpv_pipe = argv[1];
    std::wstring bridge_pipe = argc >= 3
        ? argv[2]
        : L"\\\\.\\pipe\\mpv-smtc-bridge";

    MpvSmtcBridge bridge(mpv_pipe, bridge_pipe, main_thread_id, mpv_pid);

    bool is_owner = bridge.acquire_smtc_ownership();

    if (is_owner) {
        bridge.init_smtc(hwnd);
    }

    bridge.start();
    bridge.start_watchdog();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_MPV_PROCESS_EXITED) {
            bridge.shutdown_smtc();
            PostQuitMessage(0);
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    bridge.stop();
    DestroyWindow(hwnd);

    return 0;
}