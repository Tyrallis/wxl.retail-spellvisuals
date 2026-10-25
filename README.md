# wxl.retail-spellvisuals

Retail spell visuals in the 3.3.5 client, played the way retail plays them.

3.3.5's spell visuals have one kit per moment of a cast, a few fixed model slots per kit, and no
delays, offsets or turns. Retail's have any number of events, each starting at one moment of the
cast and ending at another, on the caster, the target or the ground, each showing any number of
models at any attachment point with an offset, a turn, a scale, a delay and animations. This mod
plays retail's events itself, and the **body glow** retail puts on the caster for spells such as
Avenging Wrath (gold), Bestial Wrath (red) and Combustion (fire).

Spells are made and installed with the [Retroport Workbench](https://github.com/Tyrallis/Retroport/tree/creatures)'s
Spells page: open a retail spell, *Make a new spell*, keep *retail visuals* on, *Install new spells*.

## What it reads

`DBFilesClient\WXLRetailSpellVisuals.txt`, which the Retroport Workbench writes into
`Data\patch-S.MPQ` for each new spell it makes with **retail visuals** on. Tab-separated:

```
spell    id  retail_id  cast  duration  speed  launch
missile  path  attachment  destination  cast_offset(3)  impact_offset(3)  scale  flags  speed
event    start  end  start_min  start_max  end_min  end_max  target  kit_delay_min  kit_delay_max
model    path  attachment  offset(3)  yaw  pitch  roll  scale  delay  start_anim  anim  end_anim  alpha  flags
glow     fade_in  fade_out  color0(3)  color1(3)  color2(3)  alpha_middle  alpha_edge  power  halo(3)  halo_px  halo_alpha
```

Lines belong to the spell above them, model and glow lines to the event above them. Times in ms, angles in
radians, offsets in yards. The spell's own 3.3.5 visual keeps only the caster's animations.

## When events play

| Retail moment | In game |
|---|---|
| 1 precast start | the cast starts (`SpellCastStart`) |
| 2 precast end | the cast goes off, fails or is interrupted |
| 3 cast, 4 travel start | the cast goes off (`SpellCastGo`) |
| 5 travel end, 6 impact | after the flight: distance / missile speed |
| 7 aura start, 8 aura end | the aura is applied / removed; a ground aura with its area |
| 9 area start, 10 area end | a dynamic object of the spell appears / leaves |
| 11 channel start, 12 channel end | the channel starts / stops |
| 13 (as an end) | the model's animation has played once |

Models on the caster or target hang on the unit's model at retail's attachment point (a point the
3.3.5 body lacks falls back to the chest, then the body's origin). Ground models stand at the
cast's ground point, turned with the caster. Missiles fly from the caster's point to the target's.

## Body glows

A glow line is retail's edge-glow kit effect: a colour ramp from the body's middle (`color0`) to its
silhouette (`color2`), brighter toward the edge by `power`, and a soft halo just outside it. It fades in
and out with the event. The mod draws it after the world: it marks the glowing units' pixels with
their own skinned models, then tints them from the units' surface normals. It changes only render
states the client tracks itself and sets no shader constants, so the client's own drawing is left as
it was.

## Needs

Since 0.3.0 the mod needs WarcraftXL ABI 0.1.21 (WarcraftXL main, or the `x64-port` branches below); older clients
refuse it. Build it with Visual Studio 2022: the 2026 Insiders compiler (MSVC 14.50) crashes on the SDK's
headers. The body glows read drawn batches from the `ModelBatchDrawn` event.

- The 64-bit WarcraftXL client, built from the `x64-port` branches of
  [client](https://github.com/Tyrallis/client/tree/x64-port), [core](https://github.com/Tyrallis/core/tree/x64-port)
  and [sdk](https://github.com/Tyrallis/sdk/tree/x64-port): `wxl.m2` world calls (`create_world_model`, `place_model`, `set_model_transform`, `play_animation`,
  `animation_length`, `set_alpha`, `set_emitting`, `object_model`, `attachment_world`) and the
  cast events' ground point (`has_dest`, `dest`).
- `wxl.modern-m2`, for retail models.
- `wxl.equip-plus` 1.4.0 or newer, for the spell models' physics (Avenging Wrath's wings).

## Install

Unpack the release zip into the client's `Mods` folder, so that `Mods\wxl.retail-spellvisuals\mod.json`
and `Mods\wxl.retail-spellvisuals\bin\x64\wxl-retail-spellvisuals.dll` exist.

## Settings

`RetailSpellVisuals.ini` beside `mod.json`:

```ini
[Log]
Verbose=0   ; 1: a line in Logs\WarcraftXL.log for every event played and model shown
```

## Not carried yet

Sounds, scripted model paths (positioners), camera shakes.
