local mp = require "mp"
local msg = require "mp.msg"
local utils = require "mp.utils"
local options = require "mp.options"

local o = {
    bridge = "",
    bridge_pipe = "\\\\.\\pipe\\mpv-smtc-bridge-{pid}",
    ipc_template = "\\\\.\\pipe\\mpv-smtc-{pid}",

    timeline_interval = 5.0,
}
options.read_options(o, "smtc")

local pid = utils.getpid()
local mpv_ipc = o.ipc_template:gsub("{pid}", tostring(pid))
local bridge_pipe = o.bridge_pipe:gsub("{pid}", tostring(pid))

mp.set_property("options/input-ipc-server", mpv_ipc)

local function is_windows()
    return package.config:sub(1, 1) == "\\"
end

local function file_exists(path)
    if not path or path == "" then return false end
    local info = utils.file_info(path)
    return info and info.is_file
end

local function get_script_dir()
    local src = debug.getinfo(1, "S").source
    if src:sub(1, 1) == "@" then
        src = src:sub(2)
    end
    return src:match("^(.*[\\/])") or ""
end

local function find_bridge()
    if o.bridge ~= "" then
        return o.bridge
    end

    local p1 = utils.join_path(get_script_dir(), "mpv-smtc-bridge.exe")
    if file_exists(p1) then
        return p1
    end

    local p2 = mp.find_config_file("scripts/mpv-smtc-bridge.exe")
    if p2 and file_exists(p2) then
        return p2
    end

    return "mpv-smtc-bridge.exe"
end

local bridge_started = false

local function start_bridge()
    if bridge_started or not is_windows() then return end

    local bridge = find_bridge()

    msg.info("starting SMTC bridge: " .. bridge)
    msg.info("mpv ipc: " .. mpv_ipc)
    msg.info("bridge pipe: " .. bridge_pipe)

    utils.subprocess_detached({
        args = { bridge, mpv_ipc, bridge_pipe, tostring(pid) }
    })

    bridge_started = true
end

local function trim(s)
    if not s then return "" end
    s = s:gsub("^%s+", "")
    s = s:gsub("%s+$", "")
    return s
end

local function basename(path)
    if not path then return "" end
    return path:match("([^\\/]+)$") or path
end

local function strip_ext(name)
    if not name then return "" end
    return name:gsub("%.[^%.\\/]+$", "")
end

local function get_meta_value(meta, keys)
    if not meta then return "" end

    for _, key in ipairs(keys) do
        local v = meta[key]
        if v and tostring(v) ~= "" then
            return tostring(v)
        end
    end

    return ""
end

local function split_artist_title(raw)
    raw = trim(strip_ext(basename(raw or "")))

    local seps = {
        " %- ",
        " – ",
        " — ",
        " / ",
        "／",
    }

    for _, sep in ipairs(seps) do
        local a, t = raw:match("^(.-)" .. sep .. "(.+)$")
        if a and t then
            a = trim(a)
            t = trim(t)
            if a ~= "" and t ~= "" then
                return a, t
            end
        end
    end

    return "", raw
end

local function is_url(path)
    return path and path:match("^%a[%w+.-]*://")
end

local function get_local_media_path()
    local path = mp.get_property_native("path") or ""
    if path == "" or is_url(path) then
        return ""
    end

    local ok, normalized = pcall(function()
        return mp.command_native({"normalize-path", path})
    end)

    if ok and normalized and normalized ~= "" and not is_url(normalized) then
        return normalized
    end

    return path
end

local function normalize_local_path(path)
    if not path or path == "" or is_url(path) then
        return ""
    end

    local ok, normalized = pcall(function()
        return mp.command_native({"normalize-path", path})
    end)

    if ok and normalized and normalized ~= "" and not is_url(normalized) then
        return normalized
    end

    return path
end

local function get_cover_info()
    local tracks = mp.get_property_native("track-list") or {}

    local has_embedded_cover = false
    local external_cover_path = ""

    for _, t in ipairs(tracks) do
        if t.type == "video" and t.albumart then
            if t.external and t["external-filename"] and t["external-filename"] ~= "" then
                external_cover_path = normalize_local_path(t["external-filename"])
            else
                has_embedded_cover = true
            end
        end
    end

    return has_embedded_cover, external_cover_path
end

local function get_track_info()
    local meta = mp.get_property_native("metadata") or {}
    local chapter_meta = mp.get_property_native("chapter-metadata") or {}

    local artist = get_meta_value(meta, {
        "artist", "ARTIST", "Artist",
        "album_artist", "ALBUMARTIST", "albumartist",
        "performer", "PERFORMER", "Performer",
        "artists", "ARTISTS", "Artists",
    })

    local title = get_meta_value(meta, {
        "title", "TITLE", "Title",
        "icy-title", "ICY-TITLE",
    })

    local album = get_meta_value(meta, {
        "album", "ALBUM", "Album",
    })

    if artist == "" then
        artist = get_meta_value(chapter_meta, {
            "artist", "ARTIST", "performer", "PERFORMER",
        })
    end

    if title == "" then
        title = get_meta_value(chapter_meta, {
            "title", "TITLE", "Title",
        })
    end

    local media_title = mp.get_property_native("media-title") or ""
    local filename = mp.get_property_native("filename") or ""
    local path = mp.get_property_native("path") or ""

    if title == "" then
        title = media_title
    end

    if title == "" then
        title = filename
    end

    if title == "" then
        title = path
    end

    if artist == "" then
        local parsed_artist, parsed_title = split_artist_title(title)
        if parsed_artist ~= "" and parsed_title ~= "" then
            artist = parsed_artist
            title = parsed_title
        end
    end

    title = trim(strip_ext(basename(title)))
    artist = trim(artist)
    album = trim(album)

    if title == "" then
        title = "mpv"
    end

    return title, artist, album
end

local function send_to_bridge(tbl)
    tbl.pid = pid
    tbl.ipc = mpv_ipc

    local line = utils.format_json(tbl) .. "\n"

    local ok, pipe = pcall(io.open, bridge_pipe, "w")
    if not ok or not pipe then
        return false
    end

    pcall(pipe.write, pipe, line)
    pcall(pipe.flush, pipe)
    pcall(pipe.close, pipe)

    return true
end

local last_track_key = nil
local last_state_key = nil
local last_timeline_sent = 0
local playback_ended = false

local function make_track_key(title, artist, album, media_path)
    return table.concat({
        title or "",
        artist or "",
        album or "",
        media_path or "",
    }, "\31")
end

local function send_track(force)
    start_bridge()

    local title, artist, album = get_track_info()
    local media_path = get_local_media_path()
    local has_embedded_cover, external_cover_path = get_cover_info()

    local key = make_track_key(title, artist, album, media_path)
        .. "\31" .. tostring(has_embedded_cover)
        .. "\31" .. (external_cover_path or "")

    if not force and key == last_track_key then
        return
    end

    last_track_key = key

    send_to_bridge({
        type = "track",
        title = title,
        artist = artist,
        album = album,
        media_path = media_path,
        has_embedded_cover = has_embedded_cover,
        external_cover_path = external_cover_path,
    })
end

local function send_state(force)
    start_bridge()

    local pause = mp.get_property_native("pause")
    local idle = mp.get_property_native("idle-active")
    local speed = mp.get_property_number("speed", 1.0) or 1.0
    local playlist_pos = mp.get_property_number("playlist-pos", -1) or -1
    local playlist_count = mp.get_property_number("playlist-count", 0) or 0

    local key = table.concat({
        tostring(pause and true or false),
        tostring(idle and true or false),
        tostring(playback_ended and true or false),
        tostring(speed),
        tostring(playlist_pos),
        tostring(playlist_count),
    }, "\31")

    if not force and key == last_state_key then
        return
    end

    last_state_key = key

    send_to_bridge({
        type = "state",
        paused = pause and true or false,
        idle = idle and true or false,
        ended = playback_ended and true or false,
        speed = speed,
        playlist_pos = playlist_pos,
        playlist_count = playlist_count,
    })
end

local function send_timeline(force, seeking)
    start_bridge()

    local now = mp.get_time()
    local interval = tonumber(o.timeline_interval) or 5.0

    if not force and now - last_timeline_sent < interval then
        return
    end

    last_timeline_sent = now

    local duration = mp.get_property_number("duration/full", 0)
        or mp.get_property_number("duration", 0)
        or 0

    local position = mp.get_property_number("time-pos/full", 0)
        or mp.get_property_number("time-pos", 0)
        or 0

    send_to_bridge({
        type = "timeline",
        duration = duration,
        position = position,
        seeking = seeking and true or false,
    })
end

local function send_all(force_track)
    send_track(force_track)
    send_state(true)
    send_timeline(true)
end

local function send_quit()
    send_to_bridge({
        type = "quit",
    })
end

start_bridge()

mp.add_timeout(0.5, function()
    send_all(true)
end)

mp.add_periodic_timer(1.0, function()
    send_timeline(false, false)
end)

mp.register_event("file-loaded", function()
    playback_ended = false
    last_track_key = nil
    last_state_key = nil

    mp.add_timeout(0.2, function()
        send_all(true)
    end)
end)

mp.register_event("seek", function()
    playback_ended = false
    send_state(true)
    send_timeline(true, true)
end)

mp.register_event("playback-restart", function()
    playback_ended = false
    send_state(true)
    send_timeline(true)
end)

mp.register_event("end-file", function()
    playback_ended = true
    send_state(true)
    send_timeline(true)
end)

mp.register_event("shutdown", send_quit)

mp.observe_property("pause", "native", function()
    send_state(false)
end)

mp.observe_property("idle-active", "native", function()
    send_state(false)
end)

mp.observe_property("speed", "native", function()
    send_state(false)
end)

mp.observe_property("playlist-pos", "native", function()
    send_state(false)
end)

mp.observe_property("playlist-count", "native", function()
    send_state(false)
end)

mp.observe_property("duration", "native", function()
    send_timeline(true)
end)

mp.observe_property("media-title", "native", function()
    send_track(false)
end)

mp.observe_property("filename", "native", function()
    send_track(false)
end)

mp.observe_property("path", "native", function()
    send_track(false)
end)

mp.observe_property("metadata", "native", function()
    send_track(false)
end)

mp.observe_property("chapter-metadata", "native", function()
    send_track(false)
end)