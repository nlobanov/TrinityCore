/*
 * EventRecorder: server-side event stream (JSON Lines) for automated tests. cata-solo fork.
 */
#ifndef TRINITY_EVENTRECORDER_H
#define TRINITY_EVENTRECORDER_H

#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

class Map;
class WorldObject;

class TC_GAME_API EventRecorder
{
public:
    static EventRecorder* instance();

    bool IsEnabled() const { return _enabled.load(std::memory_order_relaxed); }
    bool Start(std::string const& name, int32 mapFilter);
    void Stop();
    std::string Status() const;
    void Mark(std::string const& text);

    // fields: comma separated JSON members without braces, e.g. "\"spell\":123,\"amount\":5"
    void Emit(WorldObject const* actor, char const* event, std::string const& fields);
    void EmitOnMap(Map const* map, char const* event, std::string const& fields);
    // position sample for an in-combat creature, at most once per second per guid
    void SamplePosition(WorldObject const* actor);

    static std::string Actor(WorldObject const* object);   // {"guid":..,"entry":..,"name":..,"x":..,"y":..,"z":..}
    static std::string Ref(WorldObject const* object);     // short {"guid":..,"entry":..}
    static std::string Esc(std::string_view text);

private:
    EventRecorder() = default;
    void Write(Map const* map, char const* event, std::string const& actor, std::string const& fields);

    std::atomic<bool> _enabled{ false };
    mutable std::mutex _lock;
    std::ofstream _file;
    std::string _name;
    int32 _mapFilter = -1;
    uint32 _startMs = 0;
    uint64 _count = 0;
    std::unordered_map<ObjectGuid, uint32> _lastSample;
};

#define sEventRecorder EventRecorder::instance()

#endif
