# Quake on iPod Classic

Put the original Quake `pak0.pak` in `/.rockbox/quake/id1/`. The shareware
`pak0.pak` works. For a registered copy, also put `pak1.pak` beside it.
Stop music playback, then open **Plugins > Games > quake**. Quake writes
its `config.cfg` and saves in the same folder.

When `pak0.pak` is missing, Quake shows **Game data not found** with the
folder and returns to Games. A truncated or damaged pak shows **Invalid or
truncated pak file** instead. When music is playing, Quake shows **Quake
needs 12 MiB of free memory** and returns to Games. Each notice returns
after three seconds, or at once when you press Menu.

The buttons are those of Rockbox's own Quake.

| Button | In a level | In the Quake menu |
| --- | --- | --- |
| Menu | Move forward | Up |
| Play | Change weapon and jump | Down |
| Left / Right | Turn | Change a setting |
| Select | Fire | Confirm |
| Wheel | Strafe | Nothing |
| Hold on | Open the menu | Back |

Buttons do nothing while Hold is on, so turn Hold on and then off again.
During the opening demos, any button opens the menu.

To leave Quake, open the menu, choose **Quit** and press Select. Hold on
at the quit question returns to the menu instead.

Remote volume keys change the player's volume. Quake plays its sound
effects. CD music and multiplayer are not available.

## Device acceptance

| Target | Setup | Exercise | Pass condition |
| --- | --- | --- | --- |
| ipod6g | Build with `--rock-compat`. Put the shareware `pak0.pak` in `/.rockbox/quake/id1/`. Stop playback. | Launch. Turn Hold on and off. Use Menu, Play and Select to start a New Game. Hold Menu, tap Play, Select, Left, Right and turn the wheel. Turn Hold on and off, choose Quit and press Select. Launch again and connect USB; test Charge Only and Disk Mode. Disconnect and play a track. | The game draws at 320x240 and plays effects. Hold opens the menu. Menu moves forward, Play changes weapon, Select fires, Left and Right turn, the wheel strafes. Quit returns to Games. Charge Only returns to Quake. Disk Mode ends Quake and returns to AstraPod after disconnect. The track plays. |
| ipod6g | Remove `pak0.pak`. | Launch quake. Launch again and press Menu on the notice. | The plugin shows **Game data not found. Put pak0.pak in** and `/.rockbox/quake/id1/`, then returns to Games without **Plugin error**. Menu returns at once. |
