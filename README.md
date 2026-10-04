<p align="center">
  <img src="dist/azahar.png" alt="AzaharX" width="160">
</p>

# AzaharX

AzaharX is a fork of [Azahar](https://github.com/azahar-emu/azahar), the
open-source Nintendo 3DS emulator, focused on one thing: **game playability and
compatibility**. The goal is simple: every game playable, from start to finish.

AzaharX keeps everything Azahar does and adds fixes for games that are
difficult to emulate, plus Skylanders portal and Disney Infinity base
emulation.

AzaharX is a Games Ex Obscura project, built around game preservation and
making every game playable. It is an unofficial fork and is not affiliated
with or endorsed by the Azahar team.

**[Download the latest release](https://github.com/GamesExObscura/AzaharX/releases/latest)** ·
[Report a game that isn't working](https://github.com/GamesExObscura/AzaharX/issues)

---

## What AzaharX adds

- **Per-game fixes.** Each fix switches on only for the games that need it, so
  every other game runs exactly as it does on Azahar. The fixes are listed in
  `src/common/hacks/hack_list.cpp`, with the reason for each one.
- **Skylanders portal emulation.** Use **Tools → Manage Skylanders** to load and
  swap figures while you play.
- **Disney Infinity base emulation.** Use **Tools → Manage Disney Infinity** to
  load and swap figures and play sets.
- **Automatic render-thread delays.** Games that need a render-thread delay to
  run at the right speed get one automatically, so there's no need to set
  "Delay game render thread" yourself. If you do set it, your value is used
  instead.

## Use the OpenGL renderer

AzaharX's fixes were tested with **OpenGL**, and some of them exist only in the
OpenGL renderer. On Vulkan, these games still do not render properly:

To switch: **Emulation → Configure → Graphics → Graphics API → OpenGL**.

## Getting started

1. Download `AzaharX-<version>-win64.zip` from
   [Releases](https://github.com/GamesExObscura/AzaharX/releases) and unzip it
   anywhere.
2. Run `azaharx.exe`. Set **Graphics API** to **OpenGL** (see above).
3. Load your games the same way as in Azahar.

AzaharX doesn't include any games or system files. Use dumps of games and a
console you own.

AzaharX keeps its settings and saves in the same folder as Azahar
(`%APPDATA%\Azahar`), so your existing setup carries over. The two share that
folder, so a setting changed in one shows up in the other.

**Requirements:** 64-bit Windows 10 or 11, and a GPU with OpenGL 4.3. The
Windows build is the only one AzaharX provides.

## Issues welcome

Found a game that doesn't work, or a fix that broke something? Please
[open an issue](https://github.com/GamesExObscura/AzaharX/issues) with:

- the game's name and region
- what happened, and roughly where in the game
- the graphics API you used (OpenGL or Vulkan)
- your log file: **Help → Open Log Folder**, then attach `azahar_log.txt` after
  closing the emulator. The log includes file paths from your PC, so check it
  before posting.

## Building from source

AzaharX builds the same way as Azahar. Follow Azahar's
[building from source](https://github.com/azahar-emu/azahar/wiki/Building-From-Source)
guide; on Windows, build the `citra_meta` target with MSVC to get
`azaharx.exe`. Clone with `--recursive`, since the external libraries are git
submodules.

## Credits

- **The Azahar team**, and the Citra team before them.
- **Joshua de Reeper** ([deReeperJosh](https://github.com/deReeperJosh)) for
  the Skylanders portal emulation.
- **[HubSteven](https://github.com/HubSteven)** for the Disney Infinity base
  emulation.
- **The Dolphin Emulator project**, whose implementation of the Disney Infinity
  figure encryption AzaharX's is based on.

## License

AzaharX is licensed under the **GNU General Public License v2.0 or later**, the
same as Azahar. See [license.txt](license.txt).

Nintendo 3DS is a trademark of Nintendo. AzaharX is not affiliated with or
endorsed by Nintendo or the Azahar team.
