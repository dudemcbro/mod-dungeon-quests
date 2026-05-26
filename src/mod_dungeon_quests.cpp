#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // mapId -> quest IDs whose givers are in that dungeon/raid
    std::unordered_map<uint32, std::vector<uint32>> _dungeonQuestMap;

    struct PlayerState
    {
        uint32 lastInstanceId = 0;
        std::vector<uint32> available; // quest IDs the player can currently accept
    };

    std::unordered_map<ObjectGuid, PlayerState> _playerState;
    std::unordered_set<ObjectGuid> _optedOut;

    void BuildCache()
    {
        _dungeonQuestMap.clear();

        auto process = [](QueryResult result)
        {
            if (!result)
                return;
            do
            {
                Field* f       = result->Fetch();
                uint32 mapId   = f[0].Get<uint32>();
                uint32 questId = f[1].Get<uint32>();
                auto& v = _dungeonQuestMap[mapId];
                if (std::find(v.begin(), v.end(), questId) == v.end())
                    v.push_back(questId);
            }
            while (result->NextRow());
        };

        process(WorldDatabase.Query(
            "SELECT DISTINCT c.map, cqs.quest "
            "FROM creature c "
            "INNER JOIN creature_queststarter cqs ON cqs.id = c.id "
            "INNER JOIN instance_template it ON it.map = c.map"));

        process(WorldDatabase.Query(
            "SELECT DISTINCT go.map, gqs.quest "
            "FROM gameobject go "
            "INNER JOIN gameobject_queststarter gqs ON gqs.id = go.id "
            "INNER JOIN instance_template it ON it.map = go.map"));
    }

    // Returns the earliest unmet prerequisite quest ID in the chain, or 0.
    // Depth-capped at 32 to guard against bad DB data.
    uint32 FirstMissingPrereq(Player* player, Quest const* quest)
    {
        uint32 missing = 0;
        int32 prevId   = quest->GetPrevQuestId();
        int depth      = 0;

        while (prevId > 0 && depth++ < 32)
        {
            uint32 pid = static_cast<uint32>(prevId);
            if (!player->GetQuestRewardStatus(pid))
                missing = pid;
            Quest const* prev = sObjectMgr->GetQuestTemplate(pid);
            if (!prev)
                break;
            prevId = prev->GetPrevQuestId();
        }
        return missing;
    }

    void PrintQuestList(ChatHandler& ch, PlayerState const& state)
    {
        ch.SendSysMessage("|cffFFFF00[Dungeon Quests]|r Type |cffFFD700.dq <number>|r to accept:");
        for (size_t i = 0; i < state.available.size(); ++i)
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(state.available[i]);
            if (q)
                ch.PSendSysMessage("  |cffFFD700%zu.|r %s", i + 1, q->GetTitle().c_str());
        }
    }

    void NotifyOnEntry(Player* player, Map* map)
    {
        uint32 instanceId = map->GetInstanceId();
        ObjectGuid guid   = player->GetGUID();
        auto& state       = _playerState[guid];

        // Only notify once per unique instance to avoid re-entry spam
        if (instanceId != 0 && state.lastInstanceId == instanceId)
            return;

        state.lastInstanceId = instanceId;
        state.available.clear();

        auto mapIt = _dungeonQuestMap.find(map->GetId());
        if (mapIt == _dungeonQuestMap.end())
            return;

        ChatHandler ch(player->GetSession());
        bool anyLocked = false;

        for (uint32 questId : mapIt->second)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE ||
                player->GetQuestRewardStatus(questId))
                continue;

            uint32 missing = FirstMissingPrereq(player, quest);
            if (missing)
            {
                Quest const* mq    = sObjectMgr->GetQuestTemplate(missing);
                std::string mname  = mq ? mq->GetTitle() : std::to_string(missing);
                ch.PSendSysMessage(
                    "|cffFFFF00[Dungeon Quests]|r \"|cffAAAAAA%s|r\" requires "
                    "\"|cffFFD700%s|r\" first.",
                    quest->GetTitle().c_str(), mname.c_str());
                anyLocked = true;
                continue;
            }

            if (!player->CanTakeQuest(quest, false))
                continue;

            state.available.push_back(questId);
        }

        if (!state.available.empty())
            PrintQuestList(ch, state);
        else if (!anyLocked)
            return; // nothing to say, stay silent
    }
}

// ---------------------------------------------------------------------------

class DungeonQuestsPlayer : public PlayerScript
{
public:
    DungeonQuestsPlayer() : PlayerScript("DungeonQuestsPlayer") {}

    void OnPlayerMapChanged(Player* player) override
    {
        Map* map = player->GetMap();
        if (!map || !map->IsDungeon())
            return;
        if (_optedOut.count(player->GetGUID()))
            return;
        NotifyOnEntry(player, map);
    }

    void OnPlayerLogout(Player* player) override
    {
        _playerState.erase(player->GetGUID());
    }
};

// ---------------------------------------------------------------------------

class DungeonQuestsCommand : public CommandScript
{
public:
    DungeonQuestsCommand() : CommandScript("DungeonQuestsCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table = {
            { "dq", HandleDQCommand, SEC_PLAYER, Console::No },
        };
        return table;
    }

    static bool HandleDQCommand(ChatHandler* handler, Tail args)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        ObjectGuid guid = player->GetGUID();

        std::string_view arg = args;
        while (!arg.empty() && arg.front() == ' ') arg.remove_prefix(1);
        while (!arg.empty() && arg.back()  == ' ') arg.remove_suffix(1);

        if (arg == "off")
        {
            _optedOut.insert(guid);
            handler->SendSysMessage(
                "|cffFFFF00[Dungeon Quests]|r Disabled. Type |cffFFD700.dq on|r to re-enable.");
            return true;
        }

        if (arg == "on")
        {
            _optedOut.erase(guid);
            handler->SendSysMessage("|cffFFFF00[Dungeon Quests]|r Enabled.");
            return true;
        }

        if (arg.empty())
        {
            auto it = _playerState.find(guid);
            if (it == _playerState.end() || it->second.available.empty())
            {
                handler->SendSysMessage(
                    "|cffFFFF00[Dungeon Quests]|r No quests queued. Enter a dungeon first.");
                return true;
            }
            PrintQuestList(*handler, it->second);
            return true;
        }

        // Parse numeric index
        char* end;
        long idx = std::strtol(std::string(arg).c_str(), &end, 10);
        if (*end != '\0' || idx < 1)
        {
            handler->SendSysMessage(
                "|cffFF4444[Dungeon Quests]|r Usage: .dq [number | on | off]");
            return true;
        }

        auto it = _playerState.find(guid);
        if (it == _playerState.end() || it->second.available.empty())
        {
            handler->SendSysMessage(
                "|cffFFFF00[Dungeon Quests]|r No quests queued. Enter a dungeon first.");
            return true;
        }

        auto& available = it->second.available;
        size_t index    = static_cast<size_t>(idx) - 1;

        if (index >= available.size())
        {
            handler->PSendSysMessage(
                "|cffFF4444[Dungeon Quests]|r Pick a number between 1 and %zu.", available.size());
            return true;
        }

        uint32 questId    = available[index];
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
        {
            handler->SendSysMessage("|cffFF4444[Dungeon Quests]|r Quest no longer valid.");
            available.erase(available.begin() + index);
            return true;
        }

        if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE ||
            player->GetQuestRewardStatus(questId))
        {
            handler->PSendSysMessage(
                "|cffFFFF00[Dungeon Quests]|r \"%s\" is already in your log.",
                quest->GetTitle().c_str());
            available.erase(available.begin() + index);
            return true;
        }

        if (!player->CanTakeQuest(quest, false))
        {
            handler->PSendSysMessage(
                "|cffFF4444[Dungeon Quests]|r Cannot accept \"%s\" right now.",
                quest->GetTitle().c_str());
            return true;
        }

        if (player->FindQuestSlot(0) >= MAX_QUEST_LOG_SIZE)
        {
            handler->SendSysMessage("|cffFF4444[Dungeon Quests]|r Quest log is full.");
            return true;
        }

        player->AddQuest(quest, nullptr);
        handler->PSendSysMessage(
            "|cff44FF44[Dungeon Quests]|r Accepted: \"%s\".", quest->GetTitle().c_str());
        available.erase(available.begin() + index);
        return true;
    }
};

// ---------------------------------------------------------------------------

class DungeonQuestsWorld : public WorldScript
{
public:
    DungeonQuestsWorld() : WorldScript("DungeonQuestsWorld") {}
    void OnStartup() override { BuildCache(); }
};

void Addmod_dungeon_questsScripts()
{
    new DungeonQuestsPlayer();
    new DungeonQuestsCommand();
    new DungeonQuestsWorld();
}
