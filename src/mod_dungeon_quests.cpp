#include "Chat.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace
{
    // mapId -> quest IDs whose givers are physically in that dungeon/raid map
    std::unordered_map<uint32, std::vector<uint32>> _dungeonQuestMap;

    void BuildCache()
    {
        _dungeonQuestMap.clear();

        auto process = [](QueryResult result)
        {
            if (!result)
                return;
            do
            {
                Field* f      = result->Fetch();
                uint32 mapId  = f[0].Get<uint32>();
                uint32 questId = f[1].Get<uint32>();

                auto& v = _dungeonQuestMap[mapId];
                if (std::find(v.begin(), v.end(), questId) == v.end())
                    v.push_back(questId);
            }
            while (result->NextRow());
        };

        // Quests given by creatures in dungeon/raid maps
        process(WorldDatabase.Query(
            "SELECT DISTINCT c.map, cqs.quest "
            "FROM creature c "
            "INNER JOIN creature_queststarter cqs ON cqs.id = c.id "
            "INNER JOIN instance_template it ON it.map = c.map"));

        // Quests given by gameobjects in dungeon/raid maps
        process(WorldDatabase.Query(
            "SELECT DISTINCT go.map, gqs.quest "
            "FROM gameobject go "
            "INNER JOIN gameobject_queststarter gqs ON gqs.id = go.id "
            "INNER JOIN instance_template it ON it.map = go.map"));
    }

    // Returns the earliest quest in the prerequisite chain the player hasn't completed, or 0.
    uint32 FirstMissingPrereq(Player* player, Quest const* quest)
    {
        uint32 missing = 0;
        int32 prevId   = quest->GetPrevQuestId();

        while (prevId > 0)
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
}

class DungeonQuestsPlayer : public PlayerScript
{
public:
    DungeonQuestsPlayer() : PlayerScript("DungeonQuestsPlayer") {}

    void OnPlayerMapChanged(Player* player) override
    {
        Map* map = player->GetMap();
        if (!map || !map->IsDungeon())
            return;

        auto it = _dungeonQuestMap.find(map->GetId());
        if (it == _dungeonQuestMap.end())
            return;

        ChatHandler ch(player->GetSession());

        for (uint32 questId : it->second)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // Skip already active, completed, or rewarded quests
            if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE ||
                player->GetQuestRewardStatus(questId))
                continue;

            uint32 missing = FirstMissingPrereq(player, quest);
            if (missing)
            {
                Quest const* mq = sObjectMgr->GetQuestTemplate(missing);
                std::string missingName = mq ? mq->GetTitle() : std::to_string(missing);
                ch.PSendSysMessage(
                    "|cffFFFF00[Dungeon Quests]|r \"%s\" requires completing \"|cffFFD700%s|r\" first.",
                    quest->GetTitle().c_str(), missingName.c_str());
                continue;
            }

            if (!player->CanTakeQuest(quest, false))
                continue;

            if (player->FindQuestSlot(0) >= MAX_QUEST_LOG_SIZE)
            {
                ch.PSendSysMessage(
                    "|cffFF4444[Dungeon Quests]|r Quest log full - could not add \"|cffFFD700%s|r\".",
                    quest->GetTitle().c_str());
                break;
            }

            player->AddQuest(quest, nullptr);
            ch.PSendSysMessage(
                "|cff44FF44[Dungeon Quests]|r Added: \"|cffFFD700%s|r\".",
                quest->GetTitle().c_str());
        }
    }
};

class DungeonQuestsWorld : public WorldScript
{
public:
    DungeonQuestsWorld() : WorldScript("DungeonQuestsWorld") {}

    void OnStartup() override { BuildCache(); }
};

void Addmod_dungeon_questsScripts()
{
    new DungeonQuestsPlayer();
    new DungeonQuestsWorld();
}
