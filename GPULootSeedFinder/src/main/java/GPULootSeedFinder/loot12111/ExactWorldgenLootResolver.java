package GPULootSeedFinder.loot12111;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;
import kaptainwutax.featureutils.Feature;
import kaptainwutax.featureutils.GenerationContext;
import kaptainwutax.featureutils.loot.ILoot;
import kaptainwutax.featureutils.structure.BuriedTreasure;
import kaptainwutax.featureutils.structure.DesertPyramid;
import kaptainwutax.featureutils.structure.Shipwreck;
import kaptainwutax.featureutils.structure.generator.Generator;
import kaptainwutax.featureutils.structure.generator.Generators;
import kaptainwutax.mcutils.rand.ChunkRand;
import kaptainwutax.mcutils.util.block.BlockBox;
import kaptainwutax.mcutils.util.block.BlockMirror;
import kaptainwutax.mcutils.util.block.BlockRotation;
import kaptainwutax.mcutils.util.data.Pair;
import kaptainwutax.mcutils.util.pos.BPos;
import kaptainwutax.mcutils.util.pos.CPos;
import kaptainwutax.mcutils.version.MCVersion;
import kaptainwutax.seedutils.rand.JRand;

public final class ExactWorldgenLootResolver {
    // We intentionally keep the 26.1.2 exact path conservative here.
    // The 26.1.2 exact path is intentionally narrow. Buried treasure has a
    // direct decoration-time chest seed. Ruined portal template chests receive
    // LootTableSeed from the structure decoration RNG during template placement.
    private static final MCVersion LEGACY_WORLDGEN_VERSION = MCVersion.v1_17_1;
    private static final long GOLDEN_RATIO_64 = -7046029254386353131L;
    private static final long SILVER_RATIO_64 = 7640891576956012809L;
    private static final long STAFFORD13_MULTIPLIER_1 = -4658895280553007687L;
    private static final long STAFFORD13_MULTIPLIER_2 = -7723592293110705685L;
    private static final int BURIED_TREASURE_DECORATION_STEP_12111 = 3;
    // Runtime registry iteration orders underground structures by id here, so
    // buried_treasure is the first structure in the underground step.
    private static final int BURIED_TREASURE_FEATURE_INDEX_12111 = 0;
    private static final int DESERT_PYRAMID_DECORATION_STEP_12111 = 4;
    private static final int DESERT_PYRAMID_FEATURE_INDEX_12111 = 1;
    private static final int RUINED_PORTAL_DECORATION_STEP_12111 = 4;
    private static final int RUINED_PORTAL_FEATURE_INDEX_12111 = 10;
    private static final int RUINED_PORTAL_DESERT_FEATURE_INDEX_12111 = 11;
    private static final int RUINED_PORTAL_JUNGLE_FEATURE_INDEX_12111 = 12;
    private static final int RUINED_PORTAL_MOUNTAIN_FEATURE_INDEX_12111 = 13;
    private static final int RUINED_PORTAL_NETHER_FEATURE_INDEX_12111 = 14;
    private static final int RUINED_PORTAL_OCEAN_FEATURE_INDEX_12111 = 15;
    private static final int RUINED_PORTAL_SWAMP_FEATURE_INDEX_12111 = 16;
    private static final RuinedPortalTemplate[] RUINED_PORTAL_TEMPLATES = new RuinedPortalTemplate[]{
        new RuinedPortalTemplate(6, 6, 2, 0),
        new RuinedPortalTemplate(9, 9, 8, 6),
        new RuinedPortalTemplate(8, 9, 3, 6),
        new RuinedPortalTemplate(8, 9, 3, 2),
        new RuinedPortalTemplate(10, 7, 4, 2),
        new RuinedPortalTemplate(5, 7, 1, 4),
        new RuinedPortalTemplate(9, 9, 0, 2),
        new RuinedPortalTemplate(14, 9, 4, 2),
        new RuinedPortalTemplate(10, 9, 4, 0),
        new RuinedPortalTemplate(12, 10, 2, 7)
    };
    private static final RuinedPortalTemplate[] RUINED_PORTAL_GIANT_TEMPLATES = new RuinedPortalTemplate[]{
        new RuinedPortalTemplate(11, 16, 4, 3),
        new RuinedPortalTemplate(11, 16, 9, 9),
        new RuinedPortalTemplate(16, 16, 9, 3)
    };
    private static final int SHIPWRECK_DECORATION_STEP_12111 = 4;
    private static final int SHIPWRECK_OCEAN_FEATURE_INDEX_12111 = 17;
    private static final int SHIPWRECK_BEACHED_FEATURE_INDEX_12111 = 18;
    private static final int VILLAGE_DECORATION_STEP_12111 = 4;
    private static final int VILLAGE_PLAINS_FEATURE_INDEX_12111 = 13;
    private static final int VILLAGE_DESERT_FEATURE_INDEX_12111 = 14;
    private static final int VILLAGE_SAVANNA_FEATURE_INDEX_12111 = 15;
    private static final int VILLAGE_SNOWY_FEATURE_INDEX_12111 = 16;
    private static final int VILLAGE_TAIGA_FEATURE_INDEX_12111 = 17;
    private static final int VILLAGE_LOOT_SEED_SEARCH_LIMIT = 1;
    private static final int UNKNOWN_CHEST_Y = -1;
    private static final String[] SHIPWRECK_TYPES_BEACHED = new String[]{
        "with_mast",
        "sideways_full",
        "sideways_fronthalf",
        "sideways_backhalf",
        "rightsideup_full",
        "rightsideup_fronthalf",
        "rightsideup_backhalf",
        "with_mast_degraded",
        "rightsideup_full_degraded",
        "rightsideup_fronthalf_degraded",
        "rightsideup_backhalf_degraded"
    };
    private static final String[] SHIPWRECK_TYPES_OCEAN = new String[]{
        "with_mast",
        "upsidedown_full",
        "upsidedown_fronthalf",
        "upsidedown_backhalf",
        "sideways_full",
        "sideways_fronthalf",
        "sideways_backhalf",
        "rightsideup_full",
        "rightsideup_fronthalf",
        "rightsideup_backhalf",
        "with_mast_degraded",
        "upsidedown_full_degraded",
        "upsidedown_fronthalf_degraded",
        "upsidedown_backhalf_degraded",
        "sideways_full_degraded",
        "sideways_fronthalf_degraded",
        "sideways_backhalf_degraded",
        "rightsideup_full_degraded",
        "rightsideup_fronthalf_degraded",
        "rightsideup_backhalf_degraded"
    };
    private static final BPos SHIPWRECK_PIVOT = new BPos(4, 0, 15);
    private static final Map<String, ShipwreckLayout> SHIPWRECK_LAYOUTS = createShipwreckLayouts();
    private static final Map<String, List<String>> LOOT_SOURCE_TABLES = createLootSourceTables();

    private ExactWorldgenLootResolver() {
    }

    public static boolean supportsStructure(String structureId) {
        String normalized = normalizeSourceId(structureId);
        return "buried_treasure".equals(normalized)
            || isExactRuinedPortalSource(normalized)
            || isExactVillageLootSource(normalized);
    }

    public static List<ChestRollView> gatherChestRolls(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        structureId = normalizeSourceId(structureId);
        if ("buried_treasure".equals(structureId)) {
            return gatherExactBuriedTreasureChestRolls(worldSeed, blockX, blockZ);
        }
        if (isExactRuinedPortalSource(structureId)) {
            return gatherExactRuinedPortalChestRolls(structureId, worldSeed, blockX, blockZ);
        }
        if (isExactVillageLootSource(structureId)) {
            return gatherExactVillageChestRolls(structureId, worldSeed, blockX, blockZ);
        }

        return List.of();
    }

    public static List<ChestRollView> gatherChestRollsAt(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ,
        int chestX,
        int chestY,
        int chestZ
    ) {
        List<ChestRollView> allChests = gatherChestRolls(structureId, worldSeed, blockX, blockZ);
        if (allChests.isEmpty()) {
            return allChests;
        }
        String normalizedStructureId = normalizeSourceId(structureId);
        List<ChestRollView> out = new ArrayList<>();
        for (ChestRollView chest : allChests) {
            if (chest == null) {
                continue;
            }
            boolean positionMatches = chest.x() == chestX && chest.z() == chestZ;
            if (!positionMatches && "buried_treasure".equals(normalizedStructureId)) {
                positionMatches = Math.abs(chest.x() - chestX) <= 1 && Math.abs(chest.z() - chestZ) <= 1;
            }
            if (positionMatches && (chestY == UNKNOWN_CHEST_Y || chest.y() == UNKNOWN_CHEST_Y || chest.y() == chestY)) {
                out.add(chest);
            }
        }
        return List.copyOf(out);
    }

    public static Map<String, Integer> gatherAggregateCounts(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        structureId = normalizeSourceId(structureId);
        if ("buried_treasure".equals(structureId)) {
            int chunkX = Math.floorDiv(blockX, 16);
            int chunkZ = Math.floorDiv(blockZ, 16);
            return ExactLootTableLibrary.rollNormalizedCountsOnly(
                "chests/buried_treasure",
                exactBuriedTreasureLootSeed12111(worldSeed, chunkX, chunkZ)
            );
        }
        if (isExactRuinedPortalSource(structureId)) {
            int chunkX = Math.floorDiv(blockX, 16);
            int chunkZ = Math.floorDiv(blockZ, 16);
            int featureIndex = ruinedPortalFeatureIndex12111(structureId);
            Xoroshiro128PlusPlus rng = new Xoroshiro128PlusPlus(exactFeatureSeed12111(
                worldSeed,
                chunkX,
                chunkZ,
                featureIndex,
                RUINED_PORTAL_DECORATION_STEP_12111
            ));
            return ExactLootTableLibrary.rollNormalizedCountsOnly("chests/ruined_portal", rng.nextLong());
        }
        if (isExactVillageLootSource(structureId)) {
            return gatherExactVillageAggregateCounts(structureId, worldSeed, blockX, blockZ);
        }
        return Map.of();
    }

    public static List<Map<String, Integer>> gatherAggregateCountAlternatives(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        structureId = normalizeSourceId(structureId);
        if (isExactVillageLootSource(structureId)) {
            return gatherExactVillageAggregateCountAlternatives(structureId, worldSeed, blockX, blockZ);
        }

        Map<String, Integer> counts = gatherAggregateCounts(structureId, worldSeed, blockX, blockZ);
        return counts.isEmpty() ? List.of() : List.of(counts);
    }

    private static BPos remapLegacyChestPos12111(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ,
        int chestIndex,
        BPos fallback
    ) {
        if (!"desert_pyramid".equals(structureId)) {
            return fallback;
        }
        final int[][] localChestPositions = new int[][]{
            {10, -11, 8},
            {12, -11, 10},
            {10, -11, 12}
            ,
            {8, -11, 10}
        };
        if (chestIndex < 0 || chestIndex >= localChestPositions.length) {
            return fallback;
        }

        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        int baseX = chunkX * 16;
        int baseZ = chunkZ * 16;
        Direction2D orientation = exactDesertPyramidOrientation12111(worldSeed, chunkX, chunkZ);
        int[] localPos = localChestPositions[chestIndex];
        WorldPos worldPos = transformDesertPyramidPos(
            baseX,
            baseZ,
            orientation,
            localPos[0],
            localPos[1],
            localPos[2]
        );
        return new BPos(worldPos.x(), fallback.getY(), worldPos.z());
    }

    private static void sortLegacyDesertPyramidChests(List<ChestRollView> chests, int blockX, int blockZ) {
        if (chests.size() <= 1) {
            return;
        }
        int centerX = Math.floorDiv(blockX, 16) * 16 + 10;
        int centerZ = Math.floorDiv(blockZ, 16) * 16 + 10;
        chests.sort((lhs, rhs) -> {
            int lhsKey = desertPyramidChestSortKey(lhs.x(), lhs.z(), centerX, centerZ);
            int rhsKey = desertPyramidChestSortKey(rhs.x(), rhs.z(), centerX, centerZ);
            if (lhsKey != rhsKey) {
                return Integer.compare(lhsKey, rhsKey);
            }
            if (lhs.x() != rhs.x()) {
                return Integer.compare(lhs.x(), rhs.x());
            }
            return Integer.compare(lhs.z(), rhs.z());
        });
    }

    private static int desertPyramidChestSortKey(int x, int z, int centerX, int centerZ) {
        if (x < centerX) {
            return 0;
        }
        if (z < centerZ) {
            return 1;
        }
        if (x > centerX) {
            return 2;
        }
        if (z > centerZ) {
            return 3;
        }
        return 4;
    }

    private static List<ChestRollView> gatherExactBuriedTreasureChestRolls(
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        long lootSeed = exactBuriedTreasureLootSeed12111(worldSeed, chunkX, chunkZ);
        ExactLootTableLibrary.LootRollResult roll =
            ExactLootTableLibrary.rollTable("chests/buried_treasure", lootSeed);

        return List.of(new ChestRollView(
            "chests/buried_treasure",
            chunkX * 16 + 9,
            UNKNOWN_CHEST_Y,
            chunkZ * 16 + 9,
            roll.slots(),
            roll.counts()
        ));
    }

    private static List<ChestRollView> gatherExactDesertPyramidChestRolls(
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        long featureSeed = exactFeatureSeed12111(
            worldSeed,
            chunkX,
            chunkZ,
            DESERT_PYRAMID_FEATURE_INDEX_12111,
            DESERT_PYRAMID_DECORATION_STEP_12111
        );
        Xoroshiro128PlusPlus rng = new Xoroshiro128PlusPlus(featureSeed);

        Direction2D orientation = exactDesertPyramidOrientation12111(worldSeed, chunkX, chunkZ);
        rng.nextInt(3);

        int baseX = chunkX * 16;
        int baseZ = chunkZ * 16;
        int[][] localChestPositions = new int[][]{
            {10, -11, 8},
            {12, -11, 10},
            {10, -11, 12},
            {8, -11, 10}
        };

        List<ChestRollView> out = new ArrayList<>(localChestPositions.length);
        for (int[] localPos : localChestPositions) {
            WorldPos worldPos = transformDesertPyramidPos(baseX, baseZ, orientation, localPos[0], localPos[1], localPos[2]);
            ExactLootTableLibrary.LootRollResult roll =
                ExactLootTableLibrary.rollTable("chests/desert_pyramid", rng.nextLong());
            out.add(new ChestRollView(
                "chests/desert_pyramid",
                worldPos.x(),
                worldPos.y(),
                worldPos.z(),
                roll.slots(),
                roll.counts()
            ));
        }
        return List.copyOf(out);
    }

    private static long exactBuriedTreasureLootSeed12111(long worldSeed, int chunkX, int chunkZ) {
        Xoroshiro128PlusPlus rng = new Xoroshiro128PlusPlus(exactFeatureSeed12111(
            worldSeed,
            chunkX,
            chunkZ,
            BURIED_TREASURE_FEATURE_INDEX_12111,
            BURIED_TREASURE_DECORATION_STEP_12111
        ));
        return rng.nextLong();
    }

    private static List<ChestRollView> gatherExactRuinedPortalChestRolls(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        int featureIndex = ruinedPortalFeatureIndex12111(structureId);
        Xoroshiro128PlusPlus rng = new Xoroshiro128PlusPlus(exactFeatureSeed12111(
            worldSeed,
            chunkX,
            chunkZ,
            featureIndex,
            RUINED_PORTAL_DECORATION_STEP_12111
        ));
        ExactLootTableLibrary.LootRollResult roll =
            ExactLootTableLibrary.rollTable("chests/ruined_portal", rng.nextLong());
        WorldPos chestPos = exactRuinedPortalChestPos12111(structureId, worldSeed, chunkX, chunkZ);

        return List.of(new ChestRollView(
            "chests/ruined_portal",
            chestPos.x(),
            UNKNOWN_CHEST_Y,
            chestPos.z(),
            roll.slots(),
            roll.counts()
        ));
    }

    private static long exactFeatureSeed12111(long worldSeed, int chunkX, int chunkZ, int featureIndex, int decorationStep) {
        long decorationSeed = setDecorationSeed12111(worldSeed, chunkX * 16, chunkZ * 16);
        return decorationSeed + (long) featureIndex + (long) decorationStep * 10000L;
    }

    private static boolean isExactRuinedPortalSource(String structureId) {
        return switch (structureId) {
            case "ruined_portal",
                 "ruined_portal_desert",
                 "ruined_portal_jungle",
                 "ruined_portal_mountain",
                 "ruined_portal_nether",
                 "ruined_portal_ocean",
                 "ruined_portal_swamp" -> true;
            default -> false;
        };
    }

    private static boolean isExactVillageLootSource(String structureId) {
        return switch (normalizeSourceId(structureId)) {
            case "village_weaponsmith" -> true;
            default -> false;
        };
    }

    private static String villageLootTableId(String structureId) {
        return switch (normalizeSourceId(structureId)) {
            case "village_weaponsmith" -> "chests/village/village_weaponsmith";
            default -> null;
        };
    }

    private static int preferredVillageFeatureIndex12111(String structureId) {
        return switch (normalizeSourceId(structureId)) {
            case "village_desert" -> VILLAGE_DESERT_FEATURE_INDEX_12111;
            case "village_savanna" -> VILLAGE_SAVANNA_FEATURE_INDEX_12111;
            case "village_snowy" -> VILLAGE_SNOWY_FEATURE_INDEX_12111;
            case "village_taiga" -> VILLAGE_TAIGA_FEATURE_INDEX_12111;
            default -> VILLAGE_PLAINS_FEATURE_INDEX_12111;
        };
    }

    private static int[] villageFeatureIndexSearchOrder12111(String structureId) {
        int preferred = preferredVillageFeatureIndex12111(structureId);
        int[] all = new int[]{
            VILLAGE_PLAINS_FEATURE_INDEX_12111,
            VILLAGE_DESERT_FEATURE_INDEX_12111,
            VILLAGE_SAVANNA_FEATURE_INDEX_12111,
            VILLAGE_SNOWY_FEATURE_INDEX_12111,
            VILLAGE_TAIGA_FEATURE_INDEX_12111
        };
        int[] out = new int[all.length];
        out[0] = preferred;
        int write = 1;
        for (int value : all) {
            if (value != preferred) {
                out[write++] = value;
            }
        }
        return out;
    }

    private static List<ChestRollView> gatherExactVillageChestRolls(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        String tableId = villageLootTableId(structureId);
        if (tableId == null) {
            return List.of();
        }

        WorldPos chestPos = exactVillageRepresentativeChestPos12111(blockX, blockZ);
        List<ChestRollView> out = new ArrayList<>();
        for (long lootSeed : exactVillageLootSeedCandidates12111(structureId, worldSeed, chestPos.x(), chestPos.z())) {
            ExactLootTableLibrary.LootRollResult roll = ExactLootTableLibrary.rollTable(tableId, lootSeed);
            out.add(new ChestRollView(
                tableId,
                chestPos.x(),
                UNKNOWN_CHEST_Y,
                chestPos.z(),
                roll.slots(),
                roll.counts()
            ));
        }
        return List.copyOf(out);
    }

    private static Map<String, Integer> gatherExactVillageAggregateCounts(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        String tableId = villageLootTableId(structureId);
        if (tableId == null) {
            return Map.of();
        }

        WorldPos chestPos = exactVillageRepresentativeChestPos12111(blockX, blockZ);
        Map<String, Integer> out = new HashMap<>();
        for (long lootSeed : exactVillageLootSeedCandidates12111(structureId, worldSeed, chestPos.x(), chestPos.z())) {
            Map<String, Integer> counts = ExactLootTableLibrary.rollNormalizedCountsOnly(tableId, lootSeed);
            for (Map.Entry<String, Integer> entry : counts.entrySet()) {
                out.merge(entry.getKey(), entry.getValue(), Math::max);
            }
        }
        return Map.copyOf(out);
    }

    private static List<Map<String, Integer>> gatherExactVillageAggregateCountAlternatives(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        String tableId = villageLootTableId(structureId);
        if (tableId == null) {
            return List.of();
        }

        WorldPos chestPos = exactVillageRepresentativeChestPos12111(blockX, blockZ);
        List<Map<String, Integer>> out = new ArrayList<>();
        for (long lootSeed : exactVillageLootSeedCandidates12111(structureId, worldSeed, chestPos.x(), chestPos.z())) {
            Map<String, Integer> counts = ExactLootTableLibrary.rollNormalizedCountsOnly(tableId, lootSeed);
            if (!counts.isEmpty()) {
                out.add(Map.copyOf(counts));
            }
        }
        return List.copyOf(out);
    }

    private static WorldPos exactVillageRepresentativeChestPos12111(int blockX, int blockZ) {
        return new WorldPos(blockX + 3, UNKNOWN_CHEST_Y, blockZ + 32);
    }

    private static List<Long> exactVillageLootSeedCandidates12111(
        String structureId,
        long worldSeed,
        int chestX,
        int chestZ
    ) {
        int chunkX = Math.floorDiv(chestX, 16);
        int chunkZ = Math.floorDiv(chestZ, 16);
        List<Long> out = new ArrayList<>();
        for (int featureIndex : villageFeatureIndexSearchOrder12111(structureId)) {
            Xoroshiro128PlusPlus rng = new Xoroshiro128PlusPlus(exactFeatureSeed12111(
                worldSeed,
                chunkX,
                chunkZ,
                featureIndex,
                VILLAGE_DECORATION_STEP_12111
            ));
            for (int i = 0; i < VILLAGE_LOOT_SEED_SEARCH_LIMIT; i++) {
                out.add(rng.nextLong());
            }
        }
        return List.copyOf(out);
    }

    private static int ruinedPortalFeatureIndex12111(String structureId) {
        return switch (structureId) {
            case "ruined_portal_desert" -> RUINED_PORTAL_DESERT_FEATURE_INDEX_12111;
            case "ruined_portal_jungle" -> RUINED_PORTAL_JUNGLE_FEATURE_INDEX_12111;
            case "ruined_portal_mountain" -> RUINED_PORTAL_MOUNTAIN_FEATURE_INDEX_12111;
            case "ruined_portal_nether" -> RUINED_PORTAL_NETHER_FEATURE_INDEX_12111;
            case "ruined_portal_ocean" -> RUINED_PORTAL_OCEAN_FEATURE_INDEX_12111;
            case "ruined_portal_swamp" -> RUINED_PORTAL_SWAMP_FEATURE_INDEX_12111;
            default -> RUINED_PORTAL_FEATURE_INDEX_12111;
        };
    }

    private static WorldPos exactRuinedPortalChestPos12111(
        String structureId,
        long worldSeed,
        int chunkX,
        int chunkZ
    ) {
        Random random = exactLargeFeatureRandom12111(worldSeed, chunkX, chunkZ);
        advanceRuinedPortalSetupRandom12111(structureId, random);

        RuinedPortalTemplate[] templates;
        if (random.nextFloat() < 0.05F) {
            templates = RUINED_PORTAL_GIANT_TEMPLATES;
        } else {
            templates = RUINED_PORTAL_TEMPLATES;
        }
        RuinedPortalTemplate template = templates[random.nextInt(templates.length)];
        TemplateRotation rotation = TemplateRotation.VALUES[random.nextInt(TemplateRotation.VALUES.length)];
        boolean mirrorFrontBack = random.nextFloat() >= 0.5F;
        return transformRuinedPortalChestPos12111(
            chunkX * 16,
            chunkZ * 16,
            template,
            rotation,
            mirrorFrontBack
        );
    }

    private static void advanceRuinedPortalSetupRandom12111(String structureId, Random random) {
        float airPocketProbability = switch (structureId) {
            case "ruined_portal", "ruined_portal_mountain" -> random.nextFloat() < 0.5F ? 1.0F : 0.5F;
            case "ruined_portal_jungle", "ruined_portal_nether" -> 0.5F;
            default -> 0.0F;
        };
        if (airPocketProbability != 0.0F && airPocketProbability != 1.0F) {
            random.nextFloat();
        }
    }

    private static WorldPos transformRuinedPortalChestPos12111(
        int baseX,
        int baseZ,
        RuinedPortalTemplate template,
        TemplateRotation rotation,
        boolean mirrorFrontBack
    ) {
        int x = template.chestX();
        int z = template.chestZ();
        if (mirrorFrontBack) {
            x = -x;
        }

        int pivotX = template.sizeX() / 2;
        int pivotZ = template.sizeZ() / 2;
        return switch (rotation) {
            case COUNTERCLOCKWISE_90 -> new WorldPos(baseX + pivotX - pivotZ + z, UNKNOWN_CHEST_Y, baseZ + pivotX + pivotZ - x);
            case CLOCKWISE_90 -> new WorldPos(baseX + pivotX + pivotZ - z, UNKNOWN_CHEST_Y, baseZ + pivotZ - pivotX + x);
            case CLOCKWISE_180 -> new WorldPos(baseX + pivotX + pivotX - x, UNKNOWN_CHEST_Y, baseZ + pivotZ + pivotZ - z);
            case NONE -> new WorldPos(baseX + x, UNKNOWN_CHEST_Y, baseZ + z);
        };
    }

    private static long setDecorationSeed12111(long worldSeed, int blockX, int blockZ) {
        Xoroshiro128PlusPlus rng = new Xoroshiro128PlusPlus(worldSeed);
        long xMultiplier = rng.nextLong() | 1L;
        long zMultiplier = rng.nextLong() | 1L;
        return ((long) blockX * xMultiplier + (long) blockZ * zMultiplier) ^ worldSeed;
    }

    private static Direction2D exactDesertPyramidOrientation12111(long worldSeed, int chunkX, int chunkZ) {
        Random random = exactLargeFeatureRandom12111(worldSeed, chunkX, chunkZ);
        return Direction2D.HORIZONTAL[random.nextInt(Direction2D.HORIZONTAL.length)];
    }

    private static Random exactLargeFeatureRandom12111(long worldSeed, int chunkX, int chunkZ) {
        Random random = new Random(worldSeed);
        long xMultiplier = random.nextLong();
        long zMultiplier = random.nextLong();
        long largeFeatureSeed = (long) chunkX * xMultiplier ^ (long) chunkZ * zMultiplier ^ worldSeed;
        random.setSeed(largeFeatureSeed);
        return random;
    }

    private static WorldPos transformDesertPyramidPos(
        int baseX,
        int baseZ,
        Direction2D orientation,
        int localX,
        int localY,
        int localZ
    ) {
        return switch (orientation) {
            case NORTH -> new WorldPos(baseX + localX, UNKNOWN_CHEST_Y, baseZ + 20 - localZ);
            case EAST -> new WorldPos(baseX + localZ, UNKNOWN_CHEST_Y, baseZ + localX);
            case SOUTH -> new WorldPos(baseX + localX, UNKNOWN_CHEST_Y, baseZ + localZ);
            case WEST -> new WorldPos(baseX + 20 - localZ, UNKNOWN_CHEST_Y, baseZ + localX);
        };
    }

    private static long mixStafford13(long value) {
        value = (value ^ (value >>> 30)) * STAFFORD13_MULTIPLIER_1;
        value = (value ^ (value >>> 27)) * STAFFORD13_MULTIPLIER_2;
        return value ^ (value >>> 31);
    }

    private static final class Xoroshiro128PlusPlus {
        private long seedLo;
        private long seedHi;

        private Xoroshiro128PlusPlus(long seed) {
            setSeed(seed);
        }

        private void setSeed(long seed) {
            long unmixedLo = seed ^ SILVER_RATIO_64;
            long unmixedHi = unmixedLo + GOLDEN_RATIO_64;
            seedLo = mixStafford13(unmixedLo);
            seedHi = mixStafford13(unmixedHi);
            if ((seedLo | seedHi) == 0L) {
                seedLo = GOLDEN_RATIO_64;
                seedHi = SILVER_RATIO_64;
            }
        }

        private long nextRawLong() {
            long lo = seedLo;
            long hi = seedHi;
            long result = Long.rotateLeft(lo + hi, 17) + lo;
            hi ^= lo;
            seedLo = Long.rotateLeft(lo, 49) ^ hi ^ (hi << 21);
            seedHi = Long.rotateLeft(hi, 28);
            return result;
        }

        private int nextBits(int bits) {
            return (int) (nextRawLong() >>> (64 - bits));
        }

        private long nextLong() {
            int upper = nextBits(32);
            int lower = nextBits(32);
            return ((long) upper << 32) + (long) lower;
        }

        private int nextInt(int bound) {
            if (bound <= 0) {
                throw new IllegalArgumentException("Bound must be positive");
            }
            if ((bound & (bound - 1)) == 0) {
                return (int) ((long) bound * (long) nextBits(31) >> 31);
            }
            int sample;
            int modulo;
            do {
                sample = nextBits(31);
                modulo = sample % bound;
            } while (sample - modulo + (bound - 1) < 0);
            return modulo;
        }
    }

    private enum Direction2D {
        NORTH,
        EAST,
        SOUTH,
        WEST;

        private static final Direction2D[] HORIZONTAL = values();
    }

    private enum TemplateRotation {
        NONE,
        CLOCKWISE_90,
        CLOCKWISE_180,
        COUNTERCLOCKWISE_90;

        private static final TemplateRotation[] VALUES = values();
    }

    private record WorldPos(int x, int y, int z) {
    }

    private record RuinedPortalTemplate(int sizeX, int sizeZ, int chestX, int chestZ) {
    }

    public static List<ExactLootTableLibrary.ItemStackView> gatherIndexedSlots(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        List<ExactLootTableLibrary.ItemStackView> out = new ArrayList<>();
        for (ChestRollView chest : gatherChestRolls(structureId, worldSeed, blockX, blockZ)) {
            out.addAll(chest.slots());
        }
        return List.copyOf(out);
    }

    public static List<ExactLootTableLibrary.ItemStackView> gatherIndexedSlotsAt(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ,
        int chestX,
        int chestY,
        int chestZ
    ) {
        List<ExactLootTableLibrary.ItemStackView> out = new ArrayList<>();
        for (ChestRollView chest : gatherChestRollsAt(structureId, worldSeed, blockX, blockZ, chestX, chestY, chestZ)) {
            out.addAll(chest.slots());
        }
        return List.copyOf(out);
    }

    private static int indexOf(List<Pair<Generator.ILootType, BPos>> chunkLoot, Pair<Generator.ILootType, BPos> needle) {
        for (int i = 0; i < chunkLoot.size(); i++) {
            if (chunkLoot.get(i).equals(needle)) {
                return i;
            }
        }
        return 0;
    }

    private static Feature<?, ?> resolveFeature(String structureId) {
        return switch (structureId) {
            case "buried_treasure" -> new BuriedTreasure(LEGACY_WORLDGEN_VERSION);
            case "desert_pyramid" -> new DesertPyramid(LEGACY_WORLDGEN_VERSION);
            case "shipwreck", "shipwreck_supply", "shipwreck_map", "shipwreck_treasure" ->
                new Shipwreck(LEGACY_WORLDGEN_VERSION);
            default -> null;
        };
    }

    private static String requestedTableId(String structureId) {
        return switch (structureId) {
            case "abandoned_mineshaft" -> "chests/abandoned_mineshaft";
            case "ancient_city" -> "chests/ancient_city";
            case "ancient_city_ice_box" -> "chests/ancient_city_ice_box";
            case "bastion_bridge" -> "chests/bastion_bridge";
            case "bastion_hoglin_stable" -> "chests/bastion_hoglin_stable";
            case "bastion_other" -> "chests/bastion_other";
            case "bastion_treasure" -> "chests/bastion_treasure";
            case "end_city_treasure" -> "chests/end_city_treasure";
            case "igloo_chest" -> "chests/igloo_chest";
            case "jungle_temple_dispenser" -> "chests/jungle_temple_dispenser";
            case "nether_bridge" -> "chests/nether_bridge";
            case "pillager_outpost" -> "chests/pillager_outpost";
            case "ruined_portal" -> "chests/ruined_portal";
            case "shipwreck_supply" -> "chests/shipwreck_supply";
            case "shipwreck_map" -> "chests/shipwreck_map";
            case "shipwreck_treasure" -> "chests/shipwreck_treasure";
            case "stronghold_corridor" -> "chests/stronghold_corridor";
            case "stronghold_crossing" -> "chests/stronghold_crossing";
            case "stronghold_library" -> "chests/stronghold_library";
            case "underwater_ruin_big" -> "chests/underwater_ruin_big";
            case "underwater_ruin_small" -> "chests/underwater_ruin_small";
            case "woodland_mansion" -> "chests/woodland_mansion";
            default -> null;
        };
    }

    private static String resolveTableId(String structureId, Generator.ILootType lootType) {
        if ("buried_treasure".equals(structureId)) {
            return "chests/buried_treasure";
        }
        if ("desert_pyramid".equals(structureId)) {
            return "chests/desert_pyramid";
        }
        if ("shipwreck".equals(structureId)
            || "shipwreck_supply".equals(structureId)
            || "shipwreck_map".equals(structureId)
            || "shipwreck_treasure".equals(structureId)) {
            String name = String.valueOf(lootType);
            if ("SUPPLY_CHEST".equals(name)) {
                return "chests/shipwreck_supply";
            }
            if ("MAP_CHEST".equals(name)) {
                return "chests/shipwreck_map";
            }
            if ("TREASURE_CHEST".equals(name)) {
                return "chests/shipwreck_treasure";
            }
        }
        throw new IllegalArgumentException("Unsupported exact 26.1.1/26.1.2 loot type: " + structureId + " / " + lootType);
    }

    private static String normalizeSourceId(String structureId) {
        if (structureId == null) {
            return "";
        }
        String normalized = structureId.trim().toLowerCase();
        if (normalized.startsWith("minecraft:")) {
            normalized = normalized.substring("minecraft:".length());
        }
        normalized = normalized.replace('\\', '/');
        if (normalized.startsWith("data/minecraft/loot_table/")) {
            normalized = normalized.substring("data/minecraft/loot_table/".length());
        }
        if (normalized.startsWith("loot_table/")) {
            normalized = normalized.substring("loot_table/".length());
        }
        if (normalized.startsWith("chests/")) {
            normalized = normalized.substring("chests/".length());
        }
        if (normalized.endsWith(".json")) {
            normalized = normalized.substring(0, normalized.length() - 5);
        }
        normalized = normalized.replace('/', '_').replace('-', '_').replace(' ', '_');
        return switch (normalized) {
            case "desert_temple" -> "desert_pyramid";
            case "jungle_pyramid" -> "jungle_temple";
            case "mansion" -> "woodland_mansion";
            case "bastion" -> "bastion_remnant";
            case "nether_fortress" -> "fortress";
            case "underwater_ruin" -> "ocean_ruin";
            default -> normalized;
        };
    }

    private static List<String> tableIdsForSource(String structureId) {
        return LOOT_SOURCE_TABLES.getOrDefault(normalizeSourceId(structureId), List.of());
    }

    private static Map<String, List<String>> createLootSourceTables() {
        Map<String, List<String>> tables = new HashMap<>();
        addTables(tables, "abandoned_mineshaft", "chests/abandoned_mineshaft");
        addTables(tables, "mineshaft", "chests/abandoned_mineshaft");
        addTables(tables, "ancient_city", "chests/ancient_city", "chests/ancient_city_ice_box");
        addTables(tables, "ancient_city_ice_box", "chests/ancient_city_ice_box");
        addTables(tables, "bastion_remnant", "chests/bastion_bridge", "chests/bastion_hoglin_stable", "chests/bastion_other", "chests/bastion_treasure");
        addTables(tables, "bastion_bridge", "chests/bastion_bridge");
        addTables(tables, "bastion_hoglin_stable", "chests/bastion_hoglin_stable");
        addTables(tables, "bastion_other", "chests/bastion_other");
        addTables(tables, "bastion_treasure", "chests/bastion_treasure");
        addTables(tables, "buried_treasure", "chests/buried_treasure");
        addTables(tables, "desert_pyramid", "chests/desert_pyramid");
        addTables(tables, "end_city", "chests/end_city_treasure");
        addTables(tables, "end_city_treasure", "chests/end_city_treasure");
        addTables(tables, "fortress", "chests/nether_bridge");
        addTables(tables, "nether_bridge", "chests/nether_bridge");
        addTables(tables, "igloo", "chests/igloo_chest");
        addTables(tables, "igloo_chest", "chests/igloo_chest");
        addTables(tables, "jungle_temple", "chests/jungle_temple", "chests/jungle_temple_dispenser");
        addTables(tables, "jungle_temple_dispenser", "chests/jungle_temple_dispenser");
        addTables(tables, "ocean_ruin", "chests/underwater_ruin_big", "chests/underwater_ruin_small");
        addTables(tables, "underwater_ruin_big", "chests/underwater_ruin_big");
        addTables(tables, "underwater_ruin_small", "chests/underwater_ruin_small");
        addTables(tables, "pillager_outpost", "chests/pillager_outpost");
        addTables(tables, "ruined_portal", "chests/ruined_portal");
        addTables(tables, "ruined_portal_nether", "chests/ruined_portal");
        addTables(tables, "shipwreck", "chests/shipwreck_supply", "chests/shipwreck_map", "chests/shipwreck_treasure");
        addTables(tables, "shipwreck_supply", "chests/shipwreck_supply");
        addTables(tables, "shipwreck_map", "chests/shipwreck_map");
        addTables(tables, "shipwreck_treasure", "chests/shipwreck_treasure");
        addTables(tables, "stronghold", "chests/stronghold_corridor", "chests/stronghold_crossing", "chests/stronghold_library");
        addTables(tables, "stronghold_corridor", "chests/stronghold_corridor");
        addTables(tables, "stronghold_crossing", "chests/stronghold_crossing");
        addTables(tables, "stronghold_library", "chests/stronghold_library");
        addTables(tables, "trial_chambers", "chests/trial_chambers/corridor", "chests/trial_chambers/entrance", "chests/trial_chambers/intersection", "chests/trial_chambers/intersection_barrel", "chests/trial_chambers/reward", "chests/trial_chambers/supply");
        addTables(tables, "trial_chambers_corridor", "chests/trial_chambers/corridor");
        addTables(tables, "trial_chambers_entrance", "chests/trial_chambers/entrance");
        addTables(tables, "trial_chambers_intersection", "chests/trial_chambers/intersection");
        addTables(tables, "trial_chambers_intersection_barrel", "chests/trial_chambers/intersection_barrel");
        addTables(tables, "trial_chambers_reward", "chests/trial_chambers/reward");
        addTables(tables, "trial_chambers_reward_common", "chests/trial_chambers/reward_common");
        addTables(tables, "trial_chambers_reward_rare", "chests/trial_chambers/reward_rare");
        addTables(tables, "trial_chambers_reward_unique", "chests/trial_chambers/reward_unique");
        addTables(tables, "trial_chambers_reward_ominous", "chests/trial_chambers/reward_ominous");
        addTables(tables, "trial_chambers_reward_ominous_common", "chests/trial_chambers/reward_ominous_common");
        addTables(tables, "trial_chambers_reward_ominous_rare", "chests/trial_chambers/reward_ominous_rare");
        addTables(tables, "trial_chambers_reward_ominous_unique", "chests/trial_chambers/reward_ominous_unique");
        addTables(tables, "trial_chambers_supply", "chests/trial_chambers/supply");
        addTables(tables, "village", "chests/village/village_armorer", "chests/village/village_butcher", "chests/village/village_cartographer", "chests/village/village_desert_house", "chests/village/village_fisher", "chests/village/village_fletcher", "chests/village/village_mason", "chests/village/village_plains_house", "chests/village/village_savanna_house", "chests/village/village_shepherd", "chests/village/village_snowy_house", "chests/village/village_taiga_house", "chests/village/village_tannery", "chests/village/village_temple", "chests/village/village_toolsmith", "chests/village/village_weaponsmith");
        addTables(tables, "village_armorer", "chests/village/village_armorer");
        addTables(tables, "village_butcher", "chests/village/village_butcher");
        addTables(tables, "village_cartographer", "chests/village/village_cartographer");
        addTables(tables, "village_desert_house", "chests/village/village_desert_house");
        addTables(tables, "village_fisher", "chests/village/village_fisher");
        addTables(tables, "village_fletcher", "chests/village/village_fletcher");
        addTables(tables, "village_mason", "chests/village/village_mason");
        addTables(tables, "village_plains_house", "chests/village/village_plains_house");
        addTables(tables, "village_savanna_house", "chests/village/village_savanna_house");
        addTables(tables, "village_shepherd", "chests/village/village_shepherd");
        addTables(tables, "village_snowy_house", "chests/village/village_snowy_house");
        addTables(tables, "village_taiga_house", "chests/village/village_taiga_house");
        addTables(tables, "village_tannery", "chests/village/village_tannery");
        addTables(tables, "village_temple", "chests/village/village_temple");
        addTables(tables, "village_toolsmith", "chests/village/village_toolsmith");
        addTables(tables, "village_weaponsmith", "chests/village/village_weaponsmith");
        addTables(tables, "woodland_mansion", "chests/woodland_mansion");
        return Map.copyOf(tables);
    }

    private static void addTables(Map<String, List<String>> out, String source, String... tableIds) {
        out.put(source, List.of(tableIds));
    }

    private static boolean isExactShipwreckSource(String structureId) {
        return ShipwreckRequest.baseSource(structureId) != null;
    }

    private static List<ChestRollView> gatherExactShipwreckChestRolls(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        List<ShipwreckRequest> requests = ShipwreckRequest.parseAll(structureId);
        if (requests.isEmpty()) {
            return List.of();
        }
        List<ChestRollView> out = new ArrayList<>();
        for (ShipwreckRequest request : requests) {
            out.addAll(gatherExactShipwreckChestRolls(request, worldSeed, blockX, blockZ));
        }
        return List.copyOf(out);
    }

    private static List<ChestRollView> gatherExactShipwreckChestRolls(
        ShipwreckRequest request,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        Random structureRandom = exactLargeFeatureRandom12111(worldSeed, chunkX, chunkZ);
        BlockRotation[] rotations = BlockRotation.values();
        BlockRotation rotation = rotations[structureRandom.nextInt(rotations.length)];
        String[] templatePool = request.isBeached() ? SHIPWRECK_TYPES_BEACHED : SHIPWRECK_TYPES_OCEAN;
        String type = templatePool[structureRandom.nextInt(templatePool.length)];
        ShipwreckLayout layout = SHIPWRECK_LAYOUTS.get(type);
        if (layout == null) {
            return List.of();
        }

        BPos anchor = new CPos(chunkX, chunkZ).toBlockPos(90);
        BlockBox piece =
            BlockBox.getBoundingBox(anchor, rotation, SHIPWRECK_PIVOT, BlockMirror.NONE, layout.size()).getRotated(rotation);

        List<ShipwreckMarkerInstance> markers = new ArrayList<>();
        Map<CPos, List<ShipwreckMarkerInstance>> chunkBuckets = new HashMap<>();
        for (ShipwreckMarker marker : layout.markers()) {
            BPos chestPos = piece.getInside(marker.offset(), rotation);
            ShipwreckMarkerInstance instance = new ShipwreckMarkerInstance(marker.tableId(), chestPos);
            markers.add(instance);
            chunkBuckets.computeIfAbsent(chestPos.toChunkPos(), key -> new ArrayList<>()).add(instance);
        }

        int featureIndex = request.isBeached()
            ? SHIPWRECK_BEACHED_FEATURE_INDEX_12111
            : SHIPWRECK_OCEAN_FEATURE_INDEX_12111;
        List<ChestRollView> out = new ArrayList<>();
        for (ShipwreckMarkerInstance marker : markers) {
            if (request.requestedTableId() != null && !request.requestedTableId().equals(marker.tableId())) {
                continue;
            }
            CPos chunkPos = marker.pos().toChunkPos();
            List<ShipwreckMarkerInstance> chunkLoot = chunkBuckets.get(chunkPos);
            if (chunkLoot == null || chunkLoot.isEmpty()) {
                continue;
            }

            Xoroshiro128PlusPlus rng = new Xoroshiro128PlusPlus(exactFeatureSeed12111(
                worldSeed,
                chunkPos.getX(),
                chunkPos.getZ(),
                featureIndex,
                SHIPWRECK_DECORATION_STEP_12111
            ));
            if (request.isBeached()) {
                rng.nextInt(3);
            }
            int markerIndex = indexOfShipwreckMarker(chunkLoot, marker);
            for (int i = 0; i < markerIndex; i++) {
                rng.nextLong();
            }

            ExactLootTableLibrary.LootRollResult roll = ExactLootTableLibrary.rollTable(marker.tableId(), rng.nextLong());
            out.add(new ChestRollView(
                marker.tableId(),
                marker.pos().getX(),
                marker.pos().getY(),
                marker.pos().getZ(),
                roll.slots(),
                roll.counts()
            ));
        }
        return List.copyOf(out);
    }

    private static int indexOfShipwreckMarker(List<ShipwreckMarkerInstance> chunkLoot, ShipwreckMarkerInstance needle) {
        for (int i = 0; i < chunkLoot.size(); i++) {
            if (chunkLoot.get(i).equals(needle)) {
                return i;
            }
        }
        return 0;
    }

    private static Map<String, ShipwreckLayout> createShipwreckLayouts() {
        Map<String, ShipwreckLayout> layouts = new HashMap<>();

        layouts.put(
            "rightsideup_backhalf",
            new ShipwreckLayout(new BPos(9, 9, 16), List.of(
                new ShipwreckMarker("chests/shipwreck_map", new BPos(5, 3, 6)),
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(6, 5, 12))
            ))
        );
        layouts.put(
            "rightsideup_backhalf_degraded",
            new ShipwreckLayout(new BPos(9, 9, 16), List.of(
                new ShipwreckMarker("chests/shipwreck_map", new BPos(5, 3, 6)),
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(6, 5, 12))
            ))
        );
        layouts.put(
            "rightsideup_fronthalf",
            new ShipwreckLayout(new BPos(9, 9, 24), List.of(
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 3, 8))
            ))
        );
        layouts.put(
            "rightsideup_fronthalf_degraded",
            new ShipwreckLayout(new BPos(9, 9, 24), List.of(
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 3, 8))
            ))
        );
        layouts.put(
            "rightsideup_full",
            new ShipwreckLayout(new BPos(9, 9, 28), List.of(
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 3, 8)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(5, 3, 18)),
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(6, 5, 24))
            ))
        );
        layouts.put(
            "rightsideup_full_degraded",
            new ShipwreckLayout(new BPos(9, 9, 28), List.of(
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 3, 8)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(5, 3, 18)),
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(6, 5, 24))
            ))
        );
        layouts.put(
            "sideways_backhalf",
            new ShipwreckLayout(new BPos(9, 9, 17), List.of(
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(3, 3, 13)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(6, 4, 8))
            ))
        );
        layouts.put(
            "sideways_backhalf_degraded",
            new ShipwreckLayout(new BPos(9, 9, 17), List.of(
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(3, 3, 13)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(6, 4, 8))
            ))
        );
        layouts.put(
            "sideways_fronthalf",
            new ShipwreckLayout(new BPos(9, 9, 24), List.of(
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(5, 4, 8))
            ))
        );
        layouts.put(
            "sideways_fronthalf_degraded",
            new ShipwreckLayout(new BPos(9, 9, 24), List.of(
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(5, 4, 8))
            ))
        );
        layouts.put(
            "sideways_full",
            new ShipwreckLayout(new BPos(9, 9, 28), List.of(
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(3, 3, 24)),
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(5, 4, 8)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(6, 4, 19))
            ))
        );
        layouts.put(
            "sideways_full_degraded",
            new ShipwreckLayout(new BPos(9, 9, 28), List.of(
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(3, 3, 24)),
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(5, 4, 8)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(6, 4, 19))
            ))
        );
        layouts.put(
            "upsidedown_backhalf",
            new ShipwreckLayout(new BPos(9, 9, 16), List.of(
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(2, 3, 12)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(3, 6, 5))
            ))
        );
        layouts.put(
            "upsidedown_backhalf_degraded",
            new ShipwreckLayout(new BPos(9, 9, 16), List.of(
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(2, 3, 12)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(3, 6, 5))
            ))
        );
        layouts.put(
            "upsidedown_fronthalf",
            new ShipwreckLayout(new BPos(9, 9, 22), List.of(
                new ShipwreckMarker("chests/shipwreck_map", new BPos(3, 6, 17)),
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 6, 8))
            ))
        );
        layouts.put(
            "upsidedown_fronthalf_degraded",
            new ShipwreckLayout(new BPos(9, 9, 22), List.of(
                new ShipwreckMarker("chests/shipwreck_map", new BPos(3, 6, 17)),
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 6, 8))
            ))
        );
        layouts.put(
            "upsidedown_full",
            new ShipwreckLayout(new BPos(9, 9, 28), List.of(
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(2, 3, 24)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(3, 6, 17)),
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 6, 8))
            ))
        );
        layouts.put(
            "upsidedown_full_degraded",
            new ShipwreckLayout(new BPos(9, 9, 28), List.of(
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(2, 3, 24)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(3, 6, 17)),
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 6, 8))
            ))
        );
        layouts.put(
            "with_mast",
            new ShipwreckLayout(new BPos(9, 21, 28), List.of(
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 3, 9)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(5, 3, 18)),
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(6, 5, 24))
            ))
        );
        layouts.put(
            "with_mast_degraded",
            new ShipwreckLayout(new BPos(9, 21, 28), List.of(
                new ShipwreckMarker("chests/shipwreck_supply", new BPos(4, 3, 9)),
                new ShipwreckMarker("chests/shipwreck_map", new BPos(5, 3, 18)),
                new ShipwreckMarker("chests/shipwreck_treasure", new BPos(6, 5, 24))
            ))
        );

        return Map.copyOf(layouts);
    }

    private record ShipwreckLayout(BPos size, List<ShipwreckMarker> markers) {
    }

    public record ChestRollView(
        String tableId,
        int x,
        int y,
        int z,
        List<ExactLootTableLibrary.ItemStackView> slots,
        Map<String, Integer> counts
    ) {
    }

    private record ShipwreckMarker(String tableId, BPos offset) {
    }

    private record ShipwreckMarkerInstance(String tableId, BPos pos) {
    }

    private record ShipwreckRequest(boolean isBeached, String requestedTableId) {
        private static List<ShipwreckRequest> parseAll(String structureId) {
            if (structureId == null) {
                return List.of();
            }
            boolean explicitVariant = false;
            boolean isBeached = false;
            String base = structureId;
            if (structureId.endsWith("_beached")) {
                explicitVariant = true;
                isBeached = true;
                base = structureId.substring(0, structureId.length() - "_beached".length());
            } else if (structureId.endsWith("_ocean")) {
                explicitVariant = true;
                isBeached = false;
                base = structureId.substring(0, structureId.length() - "_ocean".length());
            }
            String requestedTableId = requestedTableIdForBase(base);
            if (requestedTableId == null && !"shipwreck".equals(base)) {
                return List.of();
            }
            if (explicitVariant) {
                return List.of(new ShipwreckRequest(isBeached, requestedTableId));
            }
            return List.of(
                new ShipwreckRequest(false, requestedTableId),
                new ShipwreckRequest(true, requestedTableId)
            );
        }

        private static String baseSource(String structureId) {
            if (structureId == null) {
                return null;
            }
            String base = structureId;
            if (structureId.endsWith("_beached")) {
                base = structureId.substring(0, structureId.length() - "_beached".length());
            } else if (structureId.endsWith("_ocean")) {
                base = structureId.substring(0, structureId.length() - "_ocean".length());
            }
            if ("shipwreck".equals(base) || requestedTableIdForBase(base) != null) {
                return base;
            }
            return null;
        }

        private static String requestedTableIdForBase(String base) {
            String requestedTableId = switch (base) {
                case "shipwreck" -> null;
                case "shipwreck_supply" -> "chests/shipwreck_supply";
                case "shipwreck_map" -> "chests/shipwreck_map";
                case "shipwreck_treasure" -> "chests/shipwreck_treasure";
                default -> null;
            };
            return requestedTableId;
        }
    }
}
