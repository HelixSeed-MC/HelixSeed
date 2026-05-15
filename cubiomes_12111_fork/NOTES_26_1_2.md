# Vanilla 26.1.2 Reference Notes

This fork was restored from the tracked HelixSeed `cubiomes_12111_fork` baseline and retargeted to the PrismLauncher vanilla `26.1.2` install (`DataVersion 4790`).

Reference extraction used Vineflower `1.12.0` against a local Minecraft `26.1.2` client jar (from a PrismLauncher install — path varies per machine). Decompiled references live under a local `scratch/` workspace that is intentionally not tracked in git.

Confirmed vanilla facts incorporated in this fork:

- Random spread structure placement still uses `WorldgenRandom.setLargeFeatureWithSalt(seed, regionX, regionZ, salt)`.
- `RandomSpreadType.LINEAR` uses one bounded random draw per axis; `TRIANGULAR` averages two bounded random draws per axis.
- Stronghold ring generation still starts from a random angle, uses distance `32`, count `128`, spread `3`, and searches preferred biomes within a 112 block radius.
- The shipped `structure_set` JSON confirms the HelixSeed constants for villages, temples, shipwrecks, monuments, mansions, ancient cities, trail ruins, and trial chambers.
- `26.1.2` is now the canonical token; the previous HelixSeed token `1.21.11` is kept as an alias.

## Vanilla Smoke Test

Using `scratch/vanilla_test_query.json`, HelixSeed found seed `0` for a village placement within `256` blocks of origin. A scratch harness compiled against the actual PrismLauncher `26.1.2` client jar (`scratch/VanillaSpreadCheck.java`) confirmed Mojang's `RandomSpreadStructurePlacement` returns the same position:

- structure: `village`
- seed: `0`
- region: `0,0`
- chunk: `15,2`
- block: `240,32`

The rebuilt cubiomes fork agrees via `getStructurePos(Village, MC_26_1_2, 0, 0, 0)`.
