# SS Beam Rail Fix

**Private test build. Not published.**

A small Satisfactory mod for the beam railway in D4rk's Structural Solutions mod. In the current game, that railway's track material fails to load, so the rail shows up grey and doesn't follow its curve.

This mod draws the rail with its own material, `M_SSRail`, instead. The material:
- bends each rail piece along the track's curve on the GPU, using the spline data the game already provides
- takes the normal paint colour, like other buildables

## Requirements

- Structural Solutions by D4rk (1.1.47 or newer). It's listed as an optional dependency, and the fix does nothing without it.
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
| `ssrail.PaintMode` | `0` | Which part of the rail takes the paint colour. `0` paints the whole mesh. `1`-`4` use the R, G, B or A channel of SS's `AOMasks` texture. `5` paints only the parts of the albedo that are already coloured. |
| `ssrail.PaintGain` | `2` | Brightness of the paint colour. |

Changes apply immediately to rail that's already built.

## Building

The repo root is the plugin folder. Put it in an SML starter project under `Mods/SSBeamRailFix`, then:

1. **Only if the material changes:** close the Unreal Editor and rebuild it with `Scripts/make_M_SSRail.py`. Run it through the editor's Python commandlet: `UnrealEditor-Cmd.exe <FactoryGame.uproject> -run=pythonscript -script=<path to make_M_SSRail.py>`. It needs the Python Editor Script Plugin.
2. Package with Alpakit, or with `RunUAT PackagePlugin -DLCName=SSBeamRailFix`.

`tools/rebuild-0.4.1.ps1` writes the current `.uplugin` and C++ source into the mod folder. By default that's the repo root; use `-mod <path>` to point it elsewhere.
