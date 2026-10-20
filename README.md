# hypr-xshape

A Hyprland plugin that clips XWayland windows to their X11 SHAPE bounding region, the way GNOME (Mutter) does.

## Why

Some X11 apps make non-rectangular windows with the X11 shape extension. Hyprland ignores the shape, so the part of the window outside it gets drawn anyway. Depending on what the app put in its buffer there, that shows up as a black or grey block.

The most visible case is pie menus in **Foundry Modo** and **SideFX Houdini**:

- Houdini paints transparent pixels outside the pie, but they showed as black.
- Modo fills its full-screen overlay with the UI grey and relies entirely on the shape to hide it, so most of the screen went grey or black.

With this plugin both look the same as on GNOME: just the pie on top of your scene.

## How it works

- The plugin opens its own connection to XWayland when the first X11 window appears.
- It reads each window's bounding shape, on map and whenever the shape changes, and stores it in the window surface's `m_visibleRegion`. That is the region Hyprland's surface renderer already clips to. The same field is used by the `hyprland-surface-v1` protocol.
- All X11 requests are asynchronous, so the compositor never blocks waiting on XWayland.

## What it affects

- **Native Wayland windows:** not touched.
- **X11 windows without a shape:** unchanged.
- **X11 windows with a shape:** clipped to it, which is what X11 intends.
- **Input:** unchanged. Clicks in the clipped-away area go wherever they went before.

## Install

### With hyprpm (recommended)

hyprpm builds the plugin against your exact Hyprland version and rebuilds it after updates.

```sh
hyprpm update
hyprpm add https://github.com/SreckoM/hypr-xshape
hyprpm enable hypr-xshape
```

Then make sure plugins load at login. Add this to `hyprland.conf` if it isn't there already:

```
exec-once = hyprpm reload -n
```

After a Hyprland update, run `hyprpm update`.

### Manual build

Dependencies:

- **Arch / Omarchy:** `hyprland`, `libxcb`, `mesa`. The headers ship with the packages.
- **Debian / Ubuntu:** `hyprland-dev`, `libxcb-shape0-dev`, `libgles-dev`.

Build, then load it into the running Hyprland:

```sh
make
hyprctl plugin load "$PWD/hypr-xshape.so"
```

To load it at login, add this to `hyprland.conf`:

```
plugin = /path/to/hypr-xshape/hypr-xshape.so
```

A manual build must be redone (`make clean all`) after every Hyprland update. Otherwise the plugin refuses to load, because it was built for a different Hyprland version.

## Optional: no blur or animation on the pie menus

Hyprland's close animation draws an unclipped snapshot of the window, which can briefly flash a blurred box. This rule (Hyprland 0.53+ syntax) turns off blur and animation for the Modo and Houdini pie windows only:

```
windowrule = match:class ^(Houdini FX|nexus)$, match:title ^(|Houdini FX|nexus|modo)$, no_blur on, no_anim on
```

## Status

- Tested on Hyprland 0.53.3 (Ubuntu package) with an NVIDIA GPU, using Modo 17.1 and Houdini 22.
- Hyprland 0.54+ changed the plugin event API. The code detects this at build time and has a path for it, written against the 0.56.2 source. That path has not been built or tested yet.
- It relies on Hyprland internals, so a future Hyprland version may need code changes.

## AI disclosure

This plugin was written by an AI assistant (Claude, by Anthropic), working with me on my machine. The assistant diagnosed the problem, wrote the code and tested it. I tested it in daily use with Modo and Houdini. This is a personal workaround, not an official Hyprland feature. It is not affiliated with the Hyprland project, and it has not been submitted upstream.

## License

MIT
