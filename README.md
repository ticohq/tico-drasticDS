<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  

*Part of the Tico ecosystem* — https://www.ticoverse.com

**DraStic** is a fast and widely used emulator for the Nintendo DS, known for its performance, compatibility, and polish.

This port is exclusive to Tico. It loads the original Android ARM64 DraStic core, patches it, and runs it natively on the Nintendo Switch inside a minimal Android-like environment, adapted to work with the Tico frontend and runtime.

----------

## Summary

This port focuses on making DraStic fit naturally within Tico, rather than behaving as a separate application.

It adds:

-   Custom overlay matching Tico design, including time, date, user avatar, and game title
-   Save and load states with a picture of each slot, plus an auto save to continue where you left off
-   Per-game settings, editable in game or from Tico
-   Screen layouts: vertical, horizontal, large screen, screen overlay, hybrid, single screen, and custom, with a hotkey to cycle through them
-   Vulkan and OpenGL rendering, with DraStic's original post-FX filters, FSR 1.0, and custom `.dfx` shaders
-   Action Replay cheats (including Tico's cheat files), microphone input, Slot-2 accessories, and LSFG frame generation
-   A game list when started on its own, reading the same ROM folders as Tico plus folders you add
-   Games on SD, USB drives, and archives (`.zip`, `.7z`, `.rar`)

----------

## Credits

This port is built on top of the DraStic emulator and the work of many others.

- **DraStic developers** — for the emulator core
- **NaGaa95** — for the original DraStic Switch port this is based on
- **fgsfds** — for the Switch so-loader groundwork
- **TheOfficialFloW** — for the original Android so-loader lineage
- **Dantiicu** — for the Switch Vulkan driver
- **PancakeTAS** — for LSFG-VK
- **Slluxx** — for IconGrabber
- **jdgleaver and the original RetroArch shader authors** — for the bundled shader collection; individual source headers retain full attribution

----------

## Legal

This project has no affiliation with Exophase or the DraStic developers. DraStic is proprietary software. No emulator core, BIOS, firmware, database, cheat table, `Lossless.dll`, or game image is distributed in this repository; you must supply legally obtained copies. We do not condone piracy.

Unless noted otherwise, the wrapper source is under the MIT License (see `LICENSE`). The vendored LSFG-VK subset under `third_party/lsfg-vk` is GPL-3.0-or-later.

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.

If you are building something, consider releasing it. Even small contributions can help others move forward.

----------

## Support

If you enjoy NaGaa95's work on the original port and want to support them:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)
