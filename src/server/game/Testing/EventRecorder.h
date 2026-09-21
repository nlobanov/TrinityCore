/*
 * EventRecorder: server-side event stream (JSON Lines) for automated tests. cata-solo fork.
 * Several recordings can run at once (one per test lane), each with its own file and an optional map / instance filter.
 */
#ifndef TRINITY_EVENTRECORDER_H
#define TRINITY_EVENTRECORDER_H

#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Map;
class WorldObject;

class TC_GAME_API EventRecorder
{
public:
    static EventRecorder* instance();

    bool IsEnabled() const { return _enabled.load(std::memory_order_relaxed); }
    // mapFilter < 0: every map; instanceFilter < 0: every instance of that map
    bool Start(std::string const& name, int32 mapFilter, int32 instanceFilter = -1);
    bool Stop(std::string const& name);       // empty name: stop all
    std::string Status() const;
    void Mark(std::string const& text, std::string const& name = "");   // empty name: every recording

    // fields: comma separated JSON members without braces, e.g. "\"spell\":123,\"amount\":5"
    void Emit(WorldObject const* actor, char const* event, std::string const& fields);
    void EmitOnMap(Map const* map, char const* event, std::string const& fields);
    // position sample for an in-combat creature, at most once per second per guid
    void SamplePosition(WorldObject const* actor);

    static std::string Actor(WorldObject const* object);   // {"guid":..,"entry":..,"name":..,"x":..,"y":..,"z":..}
    static std::string Ref(WorldObject const* object);     // short {"guid":..,"entry":..}
    static std::string Esc(std::string_view text);

private:
    struct Recording
    {
        std::string Name;
        std::ofstream File;
        int32 MapFilter = -1;
        int32 InstanceFilter = -1;
        uint32 StartMs = 0;
        uint64 Count = 0;
        bool Accepts(Map const* map) const;
    };

    EventRecorder() = default;
    void Write(Map const* map, char const* event, std::string const& actor, std::string const& fields, std::string const& only = "");

    std::atomic<bool> _enabled{ false };
    mutable std::mutex _lock;
    std::vector<std::unique_ptr<Recording>> _recordings;
    std::unordered_map<ObjectGuid, uint32> _lastSample;
};

#define sEventRecorder EventRecorder::instance()

#endif
