# Story Duel LP (behavior mod for Tag Force Recomp)

Story duels start with the Life Points set in `config.ini` for both duelists (default 8000, the game's own value; lower is faster, higher is longer).

**Install:** copy the `StoryLP` folder (`plugin.dll`, `mod.txt`) into `mods/` and check it in the **Mods** menu.

**How it works.** The duel engine's start function (`09eb14f4`) writes a fixed 8000 into both duelists' LP and tells the duel manager through
callback `09f62d88(13, player, LP, 0)`. The mod wraps the start function: while it runs, the LP in that callback and in the duel state is
replaced. Free Duel sets its own LP from the rules screen after this point, so it is not affected.

`config.ini` in the mod folder: `lp=8000` (the value for both duelists, 100..99999). The file is created on first run. In the launcher's Mods page, turning the mod on opens a box to type the LP, and an **Edit** button on the row reopens it; you can also edit the file by hand.
