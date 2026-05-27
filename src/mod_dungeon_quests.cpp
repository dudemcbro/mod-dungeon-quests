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
    std::unordered_map<uint32, std::vector<uint32>> _dungeonQuestMap;

    struct PlayerState
    {
        uint32 lastInstanceId = 0;
        bool   isDungeon      = false;
        std::vector<uint32> available;
        std::vector<uint32> locked;
    };

    std::unordered_map<ObjectGuid, PlayerState> _playerState;
    std::unordered_set<ObjectGuid> _optedOut;

    // Players currently inside a silent prereq reward - hooks suppress XP/gold/rep.
    std::unordered_set<ObjectGuid> _silentReward;

    // -----------------------------------------------------------------------
    // Cache build
    // -----------------------------------------------------------------------

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

    // -----------------------------------------------------------------------
    // Quest chain helpers
    // -----------------------------------------------------------------------

    // All missing prereq IDs in chain order (earliest first), depth-capped at 32.
    std::vector<uint32> GetMissingPrereqs(Player* player, Quest const* quest)
    {
        std::vector<uint32> chain;
        int32 prevId = quest->GetPrevQuestId();
        int depth    = 0;

        while (prevId > 0 && depth++ < 32)
        {
            uint32 pid = static_cast<uint32>(prevId);
            chain.push_back(pid);
            Quest const* prev = sObjectMgr->GetQuestTemplate(pid);
            if (!prev)
                break;
            prevId = prev->GetPrevQuestId();
        }

        std::reverse(chain.begin(), chain.end()); // earliest first

        std::vector<uint32> missing;
        for (uint32 qid : chain)
            if (!player->GetQuestRewardStatus(qid))
                missing.push_back(qid);

        return missing;
    }

    // Item IDs required as objectives by a set of downstream quests.
    std::unordered_set<uint32> BuildNeededItems(std::vector<uint32> const& downstream)
    {
        std::unordered_set<uint32> needed;
        for (uint32 qid : downstream)
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(qid);
            if (!q)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (q->RequiredItemId[i])
                    needed.insert(q->RequiredItemId[i]);
        }
        return needed;
    }

    // -----------------------------------------------------------------------
    // Silent prereq completion (no XP / gold / rep / unneeded items)
    // -----------------------------------------------------------------------

    void SilentCompletePrereq(Player* player, Quest const* prereq,
                               std::unordered_set<uint32> const& neededItems,
                               ChatHandler* ch)
    {
        uint32 prereqId = prereq->GetQuestId();

        if (player->GetQuestStatus(prereqId) == QUEST_STATUS_NONE)
            player->AddQuest(prereq, nullptr);

        player->CompleteQuest(prereqId);

        // Suppress XP, gold, reputation via PlayerScript hooks during RewardQuest.
        _silentReward.insert(player->GetGUID());
        // Pass player as questGiver to avoid null-deref on spell/mail reward paths.
        player->RewardQuest(prereq, 0, player, false);
        _silentReward.erase(player->GetGUID());

        // Remove any reward items not needed by downstream quests.
        for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
        {
            uint32 itemId = prereq->RewardItemId[i];
            if (!itemId || neededItems.count(itemId))
                continue;
            player->DestroyItemCount(itemId, prereq->RewardItemIdCount[i], true);
        }
        // Choice item: RewardQuest with reward=0 picks index 0.
        uint32 choiceId = prereq->RewardChoiceItemId[0];
        if (choiceId && !neededItems.count(choiceId))
            player->DestroyItemCount(choiceId, prereq->RewardChoiceItemCount[0], true);

        ch->PSendSysMessage("|cff44FF44[Dungeon Quests]|r Completed prerequisite: \"|cffFFD700%s|r\".",
            prereq->GetTitle().c_str());
    }

    // -----------------------------------------------------------------------
    // Complete all missing prereqs then accept the dungeon quest.
    // -----------------------------------------------------------------------

    bool CompletePrereqsAndAccept(ChatHandler* handler, Player* player, uint32 questId)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
        {
            handler->SendSysMessage("|cffFF4444[Dungeon Quests]|r Quest no longer valid.");
            return false;
        }

        // Need at least one free log slot; RewardQuest frees it after each step.
        if (player->FindQuestSlot(0) >= MAX_QUEST_LOG_SIZE)
        {
            handler->SendSysMessage("|cffFF4444[Dungeon Quests]|r Quest log is full.");
            return false;
        }

        std::vector<uint32> missing = GetMissingPrereqs(player, quest);

        for (size_t i = 0; i < missing.size(); ++i)
        {
            Quest const* prereq = sObjectMgr->GetQuestTemplate(missing[i]);
            if (!prereq)
                continue;

            // Downstream = every quest after this one in the missing chain + the dungeon quest.
            std::vector<uint32> downstream(missing.begin() + i + 1, missing.end());
            downstream.push_back(questId);

            SilentCompletePrereq(player, prereq, BuildNeededItems(downstream), handler);
        }

        // Accept the dungeon quest itself.
        if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE ||
            player->GetQuestRewardStatus(questId))
        {
            handler->PSendSysMessage("|cffFFFF00[Dungeon Quests]|r \"%s\" is already in your log.",
                quest->GetTitle().c_str());
            return true;
        }

        if (!player->CanTakeQuest(quest, false))
        {
            handler->PSendSysMessage(
                "|cffFF4444[Dungeon Quests]|r Cannot accept \"%s\" after completing prerequisites.",
                quest->GetTitle().c_str());
            return false;
        }

        if (player->FindQuestSlot(0) >= MAX_QUEST_LOG_SIZE)
        {
            handler->SendSysMessage("|cffFF4444[Dungeon Quests]|r Quest log is full.");
            return false;
        }

        player->AddQuest(quest, nullptr);
        handler->PSendSysMessage("|cff44FF44[Dungeon Quests]|r Accepted: \"|cffFFD700%s|r\".",
            quest->GetTitle().c_str());
        return true;
    }

    // -----------------------------------------------------------------------
    // Notification helpers
    // -----------------------------------------------------------------------

    void PrintQuestList(ChatHandler& ch, PlayerState const& state)
    {
        if (!state.available.empty())
        {
            ch.SendSysMessage("|cffFFFF00[Dungeon Quests]|r Type |cffFFD700.dq <number>|r to accept:");
            for (size_t i = 0; i < state.available.size(); ++i)
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(state.available[i]);
                if (q)
                    ch.PSendSysMessage("  |cffFFD700%zu.|r %s", i + 1, q->GetTitle().c_str());
            }
        }

        if (!state.locked.empty() && state.isDungeon)
        {
            ch.SendSysMessage("|cffFFFF00[Dungeon Quests]|r Type |cffFFD700.dq prereq <number>|r to auto-complete requirements:");
            for (size_t i = 0; i < state.locked.size(); ++i)
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(state.locked[i]);
                if (q)
                    ch.PSendSysMessage("  |cffAAAAAA%zu.|r %s", i + 1, q->GetTitle().c_str());
            }
        }
    }

    void NotifyOnEntry(Player* player, Map* map)
    {
        uint32 instanceId = map->GetInstanceId();
        ObjectGuid guid   = player->GetGUID();
        auto& state       = _playerState[guid];

        if (instanceId != 0 && state.lastInstanceId == instanceId)
            return;

        state.lastInstanceId = instanceId;
        state.isDungeon      = !map->IsRaid();
        state.available.clear();
        state.locked.clear();

        auto mapIt = _dungeonQuestMap.find(map->GetId());
        if (mapIt == _dungeonQuestMap.end())
            return;

        ChatHandler ch(player->GetSession());

        for (uint32 questId : mapIt->second)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE ||
                player->GetQuestRewardStatus(questId))
                continue;

            std::vector<uint32> missing = GetMissingPrereqs(player, quest);
            if (!missing.empty())
            {
                if (state.isDungeon)
                {
                    state.locked.push_back(questId);
                }
                else
                {
                    Quest const* mq   = sObjectMgr->GetQuestTemplate(missing.front());
                    std::string mname = mq ? mq->GetTitle() : std::to_string(missing.front());
                    ch.PSendSysMessage(
                        "|cffFFFF00[Dungeon Quests]|r \"|cffAAAAAA%s|r\" requires "
                        "\"|cffFFD700%s|r\" first.",
                        quest->GetTitle().c_str(), mname.c_str());
                }
                continue;
            }

            if (!player->CanTakeQuest(quest, false))
                continue;

            state.available.push_back(questId);
        }

        if (!state.available.empty() || !state.locked.empty())
            PrintQuestList(ch, state);
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
        ObjectGuid guid = player->GetGUID();
        _playerState.erase(guid);
        _silentReward.erase(guid);
    }

    // Suppress XP gain during silent prereq completion.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (_silentReward.count(player->GetGUID()))
            amount = 0;
    }

    // Suppress gold gain during silent prereq completion.
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (amount > 0 && _silentReward.count(player->GetGUID()))
            amount = 0;
    }

    // Suppress reputation gain during silent prereq completion.
    bool OnPlayerReputationChange(Player* player, uint32 /*factionID*/, int32& /*standing*/, bool /*incremental*/) override
    {
        if (_silentReward.count(player->GetGUID()))
            return false;
        return true;
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
            if (it == _playerState.end() ||
                (it->second.available.empty() && it->second.locked.empty()))
            {
                handler->SendSysMessage(
                    "|cffFFFF00[Dungeon Quests]|r No quests queued. Enter a dungeon first.");
                return true;
            }
            PrintQuestList(*handler, it->second);
            return true;
        }

        // .dq prereq <n>
        if (arg.size() > 7 && arg.substr(0, 7) == "prereq ")
        {
            std::string_view rest = arg.substr(7);
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);

            char* end;
            long idx = std::strtol(std::string(rest).c_str(), &end, 10);
            if (*end != '\0' || idx < 1)
            {
                handler->SendSysMessage("|cffFF4444[Dungeon Quests]|r Usage: .dq prereq <number>");
                return true;
            }

            auto it = _playerState.find(guid);
            if (it == _playerState.end() || it->second.locked.empty())
            {
                handler->SendSysMessage("|cffFFFF00[Dungeon Quests]|r No locked quests queued.");
                return true;
            }

            if (!it->second.isDungeon)
            {
                handler->SendSysMessage(
                    "|cffFF4444[Dungeon Quests]|r Prerequisite auto-complete is only available in dungeons, not raids.");
                return true;
            }

            auto& locked = it->second.locked;
            size_t index = static_cast<size_t>(idx) - 1;
            if (index >= locked.size())
            {
                handler->PSendSysMessage(
                    "|cffFF4444[Dungeon Quests]|r Pick a number between 1 and %zu.", locked.size());
                return true;
            }

            uint32 questId = locked[index];
            if (CompletePrereqsAndAccept(handler, player, questId))
                locked.erase(locked.begin() + index);

            return true;
        }

        // .dq <n>
        char* end;
        long idx = std::strtol(std::string(arg).c_str(), &end, 10);
        if (*end != '\0' || idx < 1)
        {
            handler->SendSysMessage(
                "|cffFF4444[Dungeon Quests]|r Usage: .dq [number | prereq <number> | on | off]");
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
        handler->PSendSysMessage("|cff44FF44[Dungeon Quests]|r Accepted: \"|cffFFD700%s|r\".",
            quest->GetTitle().c_str());
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
