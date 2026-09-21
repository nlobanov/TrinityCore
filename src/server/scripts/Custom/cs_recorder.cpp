/* .recorder commands for EventRecorder (cata-solo fork). */
#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "EventRecorder.h"
#include "RBAC.h"

using namespace Trinity::ChatCommands;

class recorder_commandscript : public CommandScript
{
public:
    recorder_commandscript() : CommandScript("recorder_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable recorderCommandTable =
        {
            { "start",  HandleRecorderStart,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "stop",   HandleRecorderStop,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "status", HandleRecorderStatus, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "mark",   HandleRecorderMark,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "recorder", recorderCommandTable },
        };
        return commandTable;
    }

    // .recorder start <name> [mapId] [instanceId]: several recordings may run at once, one file each
    static bool HandleRecorderStart(ChatHandler* handler, std::string name, Optional<int32> mapId, Optional<int32> instanceId)
    {
        if (!sEventRecorder->Start(name, mapId.value_or(-1), instanceId.value_or(-1)))
        {
            handler->SendSysMessage("recorder: failed to open output file, see Server.log");
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->SendSysMessage(sEventRecorder->Status());
        return true;
    }

    // .recorder stop [name]: without a name every recording stops
    static bool HandleRecorderStop(ChatHandler* handler, Optional<std::string> name)
    {
        bool found = sEventRecorder->Stop(name.value_or(""));
        handler->PSendSysMessage("recorder: %s", found ? "stopped" : "nothing to stop");
        return true;
    }

    static bool HandleRecorderStatus(ChatHandler* handler)
    {
        handler->SendSysMessage(sEventRecorder->Status());
        return true;
    }

    // .recorder mark <text>: written to every running recording
    static bool HandleRecorderMark(ChatHandler* handler, Tail text)
    {
        sEventRecorder->Mark(std::string(text));
        handler->SendSysMessage("recorder: mark written");
        return true;
    }
};

void AddSC_recorder_commandscript()
{
    new recorder_commandscript();
}
