# mod-dungeon-quests

<img src="icon.png" alt="" width="64" align="right">

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that offers a
dungeon's quests when you enter it.

- **Author:** [dudemcbro](https://github.com/dudemcbro)
- **License:** [GNU AGPL v3.0](LICENSE)
- **Works with:** AzerothCore WotLK (3.3.5a). The module only uses standard core APIs,
  with no SQL, no configuration file and no core patches.

## Features

- **Quests on entry.** On entering a dungeon or raid, lists the quests that start or end
  there that you can take, and `.dq <number>` accepts one without visiting the quest
  giver.
- **Missing prerequisites.** Quests you can't take yet (missing prerequisites,
  reputation or skill) are listed separately. In dungeons (not raids),
  `.dq prereq <number>` completes the missing prerequisite quests without their rewards:
  no XP, gold or reputation, and only the reward items a later quest in the chain needs.
- **Per-character toggle.** `.dq` turns the prompts on or off for your character until
  the server restarts.

The quest lists are built from the world database at startup and rebuilt on
`.reload config`.

## Quick start

1. Clone the module into the core's `modules` folder:
   ```sh
   cd <azerothcore>/modules
   git clone https://github.com/dudemcbro/mod-dungeon-quests.git
   ```
2. Re-run CMake and rebuild the core, for example with `./acore.sh compiler build`.
   AzerothCore compiles everything under `src/` and calls
   `Addmod_dungeon_questsScripts()`, so no `CMakeLists.txt` is needed.
3. Restart `worldserver`.

## Updating

```sh
cd <azerothcore>/modules/mod-dungeon-quests
git pull
```

Then re-run CMake, rebuild and restart `worldserver`.

## Removing the module

Delete its folder, re-run CMake and rebuild. Quests already accepted or completed
through it stay with the characters.

## Contributing

Bug reports and pull requests are welcome on
[GitHub](https://github.com/dudemcbro/mod-dungeon-quests). For larger changes, please
open an issue first, and describe how you tested your change.

## License

Licensed under the GNU Affero General Public License v3.0. See [LICENSE](LICENSE).
