# SS Beam Rail Fix

**Version 1.0.0**

A small Satisfactory mod for the Train Rail I-Beam in D4rk's Structural Solutions mod. In the current game, that railway's track material fails to load, so the rail shows up grey and doesn't follow its curve.

This mod draws the rail with its own material, `M_SSRail`, instead. The material:
- bends each rail piece along the track's curve on the GPU, using the spline data the game already provides
- takes the normal paint colour and paint finish (such as copper), like other buildables

On top of each beam piece, the mod also places the game's own track piece (its mesh and material, loaded from the game at runtime). That way the steel rail and deck look exactly like vanilla track. These extra pieces follow their beam piece: they start hidden or shown like the beam piece, appear when it does, and take the same build-state updates (for example during the build animation). They keep the game's own steel look, so paint and finishes only change the I-beam. They're removed when the beam piece is dismantled. When you point the dismantle tool at beam railway, the outline is bent along the curve too.

While you place beam railway, its build hologram uses the game's own track material instead of SS's broken one, so the preview isn't grey either.

## Requirements

- Structural Solutions by D4rk, 1.1.47 or newer (required).
- SML 3.12.1 or newer.

## What's in it, and what isn't

None of Structural Solutions' files or assets are shipped here or in the built mod. At runtime the mod looks up SS's own textures by path and hands them to `M_SSRail`. Everything in this repo is new:
- the material
- the C++ module
- the Python script that builds the material

Credit for the rail model, textures and the railway itself goes to D4rk.

## Console variables

| Variable | Default | Effect |
|---|---|---|
| `ssrail.PaintMode` | `6` | Which part of the rail takes the paint colour. `6` paints only the I-beam body, meaning everything below `ssrail.RailZ`. `0` paints the whole mesh. `1`-`4` use the R, G, B or A channel of SS's `AOMasks` texture. `5` paints only the parts of the albedo that are already coloured. `7` is a split view for comparing. |
| `ssrail.RailZ` | `0` | The local height where the steel rail starts (used by mode `6`). The rail mesh spans roughly -400 to 135. |
| `ssrail.PaintGain` | `2` | Brightness of the paint colour. |
| `ssrail.PaintDetail` | `0.25` | How much of the texture detail shows through the paint, from `0` (flat colour) to `1`. |
| `ssrail.Finish` | `1` | `1` applies paint finishes (metallic and roughness, for example copper) to the I-beam. `0` ignores them. |
| `ssrail.Roughness` | `0.6` | Base roughness of the I-beam. The paint finish's roughness is multiplied with it. |
| `ssrail.ClipRail` | `1` | `1` hides the rail part of `M_SSRail` and shows the game's own track on top, so the steel rail and deck look exactly like vanilla track. `0` shows the mod's own rail instead. |
| `ssrail.RailBright` | `0.45` | Brightness of the mod's own steel rail, from `0` to `1`. Only seen with `ssrail.ClipRail 0` and in the dismantle outline. |
| `ssrail.RailSat` | `0.3` | Colour saturation of the mod's own steel rail, from `0` to `1`. |
| `ssrail.BendHighlight` | `1` | `1` bends the dismantle outline along the rail's curve. `0` leaves SS's straight outline. |

Changes apply immediately to rail that's already built.

## Building

The repo root is the plugin folder. Put it in an SML starter project under `Mods/SSBeamRailFix`, then:

1. Structural Solutions is a required dependency, so the build has to find a plugin called `SS_Mod`. Put a descriptor-only stub at `Mods/SS_Mod/SS_Mod.uplugin` (name `SS_Mod`, version 1.1.47, `CanContainContent` false, no modules). It's only there for the build and isn't packaged.
2. **Only if the material changes:** close the Unreal Editor and rebuild it with `Scripts/make_M_SSRail.py`. Run it through the editor's Python commandlet: `UnrealEditor-Cmd.exe <FactoryGame.uproject> -run=pythonscript -script=<path to make_M_SSRail.py>`. It needs the Python Editor Script Plugin. If `M_SSRail` already exists, the script loads it and rebuilds its graph in place.
3. Package with Alpakit, or with `RunUAT PackagePlugin -DLCName=SSBeamRailFix`.

`tools/rebuild-1.0.0.ps1` writes the current `.uplugin`, C++ source and material script into the mod folder. By default that's the repo root; use `-mod <path>` to point it elsewhere.
