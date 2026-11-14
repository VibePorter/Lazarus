# 🧬 Importing a Mass Effect 1 Shepard

New Game → *Import ME1 Character* works on the Switch, without the Xbox's separate importer program.

## How to use it

1. Put your ME1 saves in **`sdmc:/switch/me2-deko/ME1/`** (the profiles' `me2_me1_import_dir`):
   - **PC saves:** the `.MassEffectSave` files from `Documents/BioWare/Mass Effect/Save/<character>/`, as they are;
   - **Xbox 360 saves:** a folder per save holding its `player.sav` (and `state.sav`), extracted from the save's STFS
     package (`python tools/stfs_extract.py <package> <folder>`).
2. Make sure `me2_menu_hide_import_me1 = false` (the profiles have it).
3. Main menu → *New Game* → *Import ME1 Character*: the game's load screen lists the saves. Pick one: the character is
   imported and the new game starts right away.

Backing out of the list is fine: *New Game* afterwards starts a normal (canon) game.

## How it works

On the Xbox 360 the menu starts `Unreap.xex` (the "Mass Effect 2 Importer"), which copies the chosen ME1 save into its
own saved game `Char_Prep` and restarts ME2 with `-legacydataready -expectuser<n>` on the command line; the prologue
then runs `importcharacter Char_Prep`. The port cannot start another program or restart itself, so it takes the
**PC version's route**, which is still in the Xbox code:

- `WorldInfo.IsConsoleBuild` answers *false* for the main menu's import event only, so the menu opens the game's own
  load screen in its ME1 mode instead of the importer;
- the port fills that list: every save of the import folder becomes a saved game of the importer's title
  (`4D5308C5`); PC saves are unzipped, their header is stripped, the package offsets are moved and every value is
  put in the Xbox's byte order (`app/me2_src/me2_me1_import.cpp`);
- picking an entry runs the PC's `defer importcharacter <save> <device> forcenewgame`. At that moment the port adds
  `-legacydataready` to the command line, so the prologue's `SFXSeqAct_LegacyDataCallback` applies the imported
  plot instead of the canon one;
- PC saves are read with package version 491 (the PC's), the Xbox's licensee version 92.

`me2_me1_import_no_restart` / `me2_me1_import_load_screen` select the other routes (one save without a list, or the
Xbox's restart with a launch-data file); the load screen is the default and the tested one.

## Legendary Edition saves (`.pcsav`)

Since 2026-11-14 a Mass Effect 1 **Legendary Edition** save (`.pcsav`) in the import folder is converted too
(`app/me2_src/me2_me1_le1.cpp`): its plot state is rebuilt as an ME1 Xbox `state.sav`, and its values are written
into a template ME1 Xbox `player.sav`.

- Male Shepard with the default face only; a female Shepard or a custom head is refused with a reason in the log.
- A Cyrillic first name is written in Latin letters unless the game runs in Russian
  (`me2_me1_import_le1_latin_names`).
- **The template is not in this repository** (it is a real ME1 Xbox save). Generate
  `app/me2_src/me2_me1_le1_template.inc` from an ME1 Xbox 360 `player.sav` of your own: `kTemplateSize` = its size,
  `kTemplateZ` = `zlib.compress(player.sav, 9)` as bytes. Without the file the build works and `.pcsav` saves are
  refused.

## Known limits

- PC → Xbox conversion of ME1 data was checked on two PC saves; unusual saves (other versions, mods) may fail. The log
  has `[me1-import]` lines for every step.
- Saves of the ME1 Switch port (masseffect-nx / Cinnamon) are not picked up from that port's folder yet.
- The fix that keeps a backed-out visit from breaking *New Game* is from 2026-11-04 and still awaits a console test.
