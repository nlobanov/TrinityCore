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

    static bool HandleRecorderStart(ChatHandler* handler, std::string name, Optional<int32> mapId)
    {
        if (!sEventRecorder->Start(name, mapId.value_or(-1)))
        {
            handler->SendSysMessage("recorder: failed to open output file, see Server.log");
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->SendSysMessage(sEventRecorder->Status());
        return true;
    }

    static bool HandleRecorderStop(ChatHandler* handler)
    {
        sEventRecorder->Stop();
        handler->SendSysMessage("recorder: stopped");
        return true;
    }

    static bool HandleRecorderStatus(ChatHandler* handler)
    {
        handler->SendSysMessage(sEventRecorder->Status());
        return true;
    }

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
