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

bool EventRecorder::Recording::Accepts(Map const* map) const
{
    if (MapFilter < 0)
        return true;
    if (!map || int32(map->GetId()) != MapFilter)
        return false;
    return InstanceFilter < 0 || int32(map->GetInstanceId()) == InstanceFilter;
}

bool EventRecorder::Start(std::string const& name, int32 mapFilter, int32 instanceFilter /*= -1*/)
{
    std::lock_guard<std::mutex> guard(_lock);
    for (auto itr = _recordings.begin(); itr != _recordings.end(); ++itr)
        if ((*itr)->Name == name)
        {
            (*itr)->File.close();
            _recordings.erase(itr);
            break;
        }
    std::string path = sLog->GetLogsDir() + "recorder-" + name + ".jsonl";
    auto rec = std::make_unique<Recording>();
    rec->File.open(path, std::ios::out | std::ios::trunc);
    if (!rec->File.is_open())
    {
        TC_LOG_ERROR("misc", "EventRecorder: cannot open {}", path);
        return false;
    }
    rec->Name = name;
    rec->MapFilter = mapFilter;
    rec->InstanceFilter = instanceFilter;
    rec->StartMs = GameTime::GetGameTimeMS();
    _recordings.push_back(std::move(rec));
    _enabled.store(true, std::memory_order_relaxed);
    TC_LOG_INFO("misc", "EventRecorder: started '{}' -> {} (map filter {}, instance filter {})", name, path, mapFilter, instanceFilter);
    return true;
}

bool EventRecorder::Stop(std::string const& name)
{
    std::lock_guard<std::mutex> guard(_lock);
    bool found = false;
    for (auto itr = _recordings.begin(); itr != _recordings.end();)
    {
        if (name.empty() || (*itr)->Name == name)
        {
            (*itr)->File.flush();
            (*itr)->File.close();
            TC_LOG_INFO("misc", "EventRecorder: stopped '{}' after {} events", (*itr)->Name, (*itr)->Count);
            itr = _recordings.erase(itr);
            found = true;
        }
        else
            ++itr;
    }
    if (_recordings.empty())
    {
        _enabled.store(false, std::memory_order_relaxed);
        _lastSample.clear();
    }
    return found;
}

std::string EventRecorder::Status() const
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_recordings.empty())
        return "recorder: stopped";
    std::string out;
    for (auto const& rec : _recordings)
        out += Trinity::StringFormat("recorder: '{}' running, map filter {}, instance filter {}, {} events, {} ms\n", rec->Name, rec->MapFilter, rec->InstanceFilter, rec->Count, GameTime::GetGameTimeMS() - rec->StartMs);
    return out;
}

void EventRecorder::Write(Map const* map, char const* event, std::string const& actor, std::string const& fields, std::string const& only /*= ""*/)
{
    std::lock_guard<std::mutex> guard(_lock);
    for (auto const& rec : _recordings)
    {
        if (!rec->File.is_open() || (only.empty() ? !rec->Accepts(map) : rec->Name != only))
            continue;
        rec->File << "{\"t\":" << (GameTime::GetGameTimeMS() - rec->StartMs)
                  << ",\"map\":" << (map ? int64(map->GetId()) : -1)
                  << ",\"inst\":" << (map ? map->GetInstanceId() : 0)
                  << ",\"ev\":\"" << event << "\",\"actor\":" << actor;
        if (!fields.empty())
            rec->File << ',' << fields;
        rec->File << "}\n";
        ++rec->Count;
        rec->File.flush();                                  // every event: the record must survive a crash right after it
    }
}

void EventRecorder::Mark(std::string const& text, std::string const& name /*= ""*/)
{
    if (!IsEnabled())
        return;
    if (name.empty())
    {
        // a mark goes to every recording regardless of its map filter
        std::lock_guard<std::mutex> guard(_lock);
        for (auto const& rec : _recordings)
            if (rec->File.is_open())
            {
                rec->File << "{\"t\":" << (GameTime::GetGameTimeMS() - rec->StartMs) << ",\"map\":-1,\"inst\":0,\"ev\":\"mark\",\"actor\":null,\"text\":\"" << Esc(text) << "\"}\n";
                ++rec->Count;
                rec->File.flush();
            }
        return;
    }
    Write(nullptr, "mark", "null", "\"text\":\"" + Esc(text) + "\"", name);
}

void EventRecorder::Emit(WorldObject const* actor, char const* event, std::string const& fields)
{
    if (!IsEnabled())
        return;
    Map const* map = actor ? actor->FindMap() : nullptr;
    Write(map, event, Actor(actor), fields);
}

void EventRecorder::EmitOnMap(Map const* map, char const* event, std::string const& fields)
{
    if (!IsEnabled())
        return;
    Write(map, event, "null", fields);
}

void EventRecorder::SamplePosition(WorldObject const* actor)
{
    if (!IsEnabled() || !actor)
        return;
    Map const* map = actor->FindMap();
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
