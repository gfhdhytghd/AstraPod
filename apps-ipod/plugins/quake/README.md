# Quake on iPod Classic

Put the original Quake `pak0.pak` in `/.rockbox/quake/id1/`. The shareware
`pak0.pak` works. For a registered copy, also put `pak1.pak` beside it.
Stop music playback, then open **Plugins > Games > quake**. Quake writes
its `config.cfg` and saves in the same folder.

When `pak0.pak` is missing, Quake shows **Game data not found** with the
folder and returns to Games. A truncated or damaged pak shows **Invalid or
truncated pak file** instead. When music is playing, Quake shows **Quake
needs 12 MiB of free memory** and returns to Games.

| Button | In a level | In the Quake menu |
| --- | --- | --- |
| Play | Move forward | Up |
| Left / Right | Turn | Left / Right |
| Select | Fire | Confirm |
| Wheel | Strafe while turning | Move the cursor |
| Menu | Open the menu | Back |
| Hold Menu | Return to Games | Return to Games |

Remote volume keys change the player's volume. Jump and weapon change
have no button; weapons still switch on pickup. Quake plays its sound
effects. CD music and multiplayer are not available.

## Device acceptance

| Target | Setup | Exercise | Pass condition |
| --- | --- | --- | --- |
| ipod6g | Build with `--rock-compat`. Put the shareware `pak0.pak` in `/.rockbox/quake/id1/`. Stop playback. | Launch, start a New Game, move, strafe and fire. Hold Menu. Launch again and connect USB; test Charge Only and Disk Mode. Disconnect and play a track. | The game draws at 320x240 and plays effects. Hold Menu returns to Games. Charge Only returns to Quake. Disk Mode ends Quake and returns to AstraPod after disconnect. The track plays. |
| ipod6g | Remove `pak0.pak`. | Launch quake. | The plugin shows **Game data not found. Put pak0.pak in** and `/.rockbox/quake/id1/`, then returns to Games without **Plugin error**. |
