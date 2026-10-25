# wxl.retail-spellvisuals

Retail spell visuals in the 3.3.5 client, played the way retail plays them.

3.3.5's spell visuals have one kit per moment of a cast, a few fixed model slots per kit, and no
delays, offsets or turns. Retail's have any number of events, each starting at one moment of the
cast and ending at another, on the caster, the target or the ground, each showing any number of
models at any attachment point with an offset, a turn, a scale, a delay and animations. This mod
plays retail's events itself.

## What it reads

`DBFilesClient\WXLRetailSpellVisuals.txt`, which the Retroport Workbench writes into
`Data\patch-S.MPQ` for each new spell it makes with **retail visuals** on. Tab-separated:

```
spell    id  retail_id  cast  duration  speed  launch
missile  path  attachment  destination  cast_offset(3)  impact_offset(3)  scale  flags  speed
event    start  end  start_min  start_max  end_min  end_max  target  kit_delay_min  kit_delay_max
model    path  attachment  offset(3)  yaw  pitch  roll  scale  delay  start_anim  anim  end_anim  alpha  flags
```

Lines belong to the spell above them, model lines to the event above them. Times in ms, angles in
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

## Needs

- WarcraftXL with `wxl.m2` world calls (core and client built from the `mod-support` work that
  adds `create_world_model`, `place_model`, `set_model_transform`, `play_animation`,
  `animation_length`, `set_alpha`, `set_emitting`, `object_model`, `attachment_world`) and the
  cast events' ground point (`has_dest`, `dest`).
- `wxl.modern-m2`, for retail models.

## Settings

`RetailSpellVisuals.ini` beside `mod.json`:

```ini
[Log]
Verbose=1   ; a line in Logs\WarcraftXL.log for every event played and model shown
```

## Not carried yet

Sounds, scripted model paths (positioners), camera shakes.
