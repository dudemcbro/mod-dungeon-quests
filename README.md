# mod-dungeon-quests

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that offers a
dungeon's quests when you enter it.

- On entering a dungeon or raid, lists the quests that start or end there that you can
  take, and `.dq <number>` accepts one without visiting the quest giver.
- Quests you can't take yet (missing prerequisites, reputation or skill) are listed
  separately. In dungeons (not raids), `.dq prereq <number>` completes the missing
  prerequisite quests without their rewards (no XP, gold or reputation).
- `.dq` turns the prompts on or off for your character.

The quest lists are built from the world database at startup and rebuilt on
`.reload config`.

## Installation

Clone into the core's `modules` directory, re-run CMake and rebuild. There is no SQL
and no configuration file.
