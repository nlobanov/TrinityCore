#include "EventRecorder.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "Object.h"
#include "StringFormat.h"

EventRecorder* EventRecorder::instance()
{
    static EventRecorder instance;
    return &instance;
}

std::string EventRecorder::Esc(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    for (char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else out += c;
        }
    }
    return out;
}

std::string EventRecorder::Actor(WorldObject const* object)
{
    if (!object)
        return "null";
    return Trinity::StringFormat("{{\"guid\":\"{}\",\"entry\":{},\"name\":\"{}\",\"x\":{:.1f},\"y\":{:.1f},\"z\":{:.1f}}}",
        object->GetGUID().ToString(), object->GetEntry(), Esc(object->GetName()),
        object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
}

std::string EventRecorder::Ref(WorldObject const* object)
{
    if (!object)
        return "null";
    return Trinity::StringFormat("{{\"guid\":\"{}\",\"entry\":{}}}", object->GetGUID().ToString(), object->GetEntry());
}

bool EventRecorder::Start(std::string const& name, int32 mapFilter)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_file.is_open())
        _file.close();
    std::string path = sLog->GetLogsDir() + "recorder-" + name + ".jsonl";
    _file.open(path, std::ios::out | std::ios::trunc);
    if (!_file.is_open())
    {
        TC_LOG_ERROR("misc", "EventRecorder: cannot open {}", path);
        return false;
    }
    _name = name;
    _mapFilter = mapFilter;
    _startMs = GameTime::GetGameTimeMS();
    _count = 0;
    _lastSample.clear();
    _enabled.store(true, std::memory_order_relaxed);
    TC_LOG_INFO("misc", "EventRecorder: started '{}' -> {} (map filter {})", name, path, mapFilter);
    return true;
}

void EventRecorder::Stop()
{
    _enabled.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(_lock);
    if (_file.is_open())
    {
        _file.flush();
        _file.close();
    }
    TC_LOG_INFO("misc", "EventRecorder: stopped '{}' after {} events", _name, _count);
}

std::string EventRecorder::Status() const
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_enabled.load(std::memory_order_relaxed))
        return "recorder: stopped";
    return Trinity::StringFormat("recorder: '{}' running, map filter {}, {} events, {} ms", _name, _mapFilter, _count, GameTime::GetGameTimeMS() - _startMs);
}

void EventRecorder::Write(Map const* map, char const* event, std::string const& actor, std::string const& fields)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_file.is_open())
        return;
    _file << "{\"t\":" << (GameTime::GetGameTimeMS() - _startMs)
          << ",\"map\":" << (map ? int64(map->GetId()) : -1)
          << ",\"inst\":" << (map ? map->GetInstanceId() : 0)
          << ",\"ev\":\"" << event << "\",\"actor\":" << actor;
    if (!fields.empty())
        _file << ',' << fields;
    _file << "}\n";
    if ((++_count & 0xFF) == 0)
        _file.flush();
}

void EventRecorder::Mark(std::string const& text)
{
    if (!IsEnabled())
        return;
    Write(nullptr, "mark", "null", "\"text\":\"" + Esc(text) + "\"");
    std::lock_guard<std::mutex> guard(_lock);
    _file.flush();
}

void EventRecorder::Emit(WorldObject const* actor, char const* event, std::string const& fields)
{
    if (!IsEnabled())
        return;
    Map const* map = actor ? actor->FindMap() : nullptr;
    if (_mapFilter >= 0 && (!map || int32(map->GetId()) != _mapFilter))
        return;
    Write(map, event, Actor(actor), fields);
}

void EventRecorder::EmitOnMap(Map const* map, char const* event, std::string const& fields)
{
    if (!IsEnabled())
        return;
    if (_mapFilter >= 0 && (!map || int32(map->GetId()) != _mapFilter))
        return;
    Write(map, event, "null", fields);
}

void EventRecorder::SamplePosition(WorldObject const* actor)
{
    if (!IsEnabled() || !actor)
        return;
    Map const* map = actor->FindMap();
    if (_mapFilter >= 0 && (!map || int32(map->GetId()) != _mapFilter))
        return;
    uint32 now = GameTime::GetGameTimeMS();
    {
        std::lock_guard<std::mutex> guard(_lock);
        uint32& last = _lastSample[actor->GetGUID()];
        if (now - last < 1000)
            return;
        last = now;
    }
    Write(map, "pos", Actor(actor), "");
}
