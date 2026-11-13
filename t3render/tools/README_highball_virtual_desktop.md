# Highball virtual desktop experiment

On 2026-11-13 the user verified switching from an 800×600 Wine virtual desktop
containing Notepad to a macOS application and back. This did not launch TDS,
exercise Direct3D, or establish whether the game keeps rendering in the
background. See the [result](../benchmarks/runtime/2026-11-13-virtual-desktop/result.json).

The test did not change a persistent Wine desktop setting. With an existing
Highball engine and bottle, the relevant command was:

```sh
env WINEPREFIX="$T3RENDER_BOTTLE" \
  WINESERVER="$T3RENDER_ENGINE/engine/bin/wineserver" \
  DYLD_FALLBACK_LIBRARY_PATH="$T3RENDER_ENGINE/frameworks:$T3RENDER_ENGINE/frameworks/GStreamer.framework/Versions/1.0/lib" \
  DYLD_FALLBACK_FRAMEWORK_PATH="$T3RENDER_ENGINE/frameworks" WINEDEBUG=-all \
  "$T3RENDER_ENGINE/engine/bin/wine" 'C:\windows\explorer.exe' \
  /desktop=T3RenderTest,800x600 'C:\windows\system32\notepad.exe'
```

For a later TDS test, Steam and its children would need to start in the same
virtual desktop; an already-running Steam may launch the game outside it.
Whether the launcher preserves the desktop, whether the 1024×768 game is
contained in the window, and whether `Present` continues when unfocused have
not been tested. Close or restart Steam only when convenient for the user.

T3SDK also offers `Display.Borderless=1` and `PauseInBackground=0`, but its
loader is not currently installed in this bottle and compatibility with the
T3Render proxy on Highball is unverified. Do not assume either option makes the
game run in the background until a real capture checks it.
