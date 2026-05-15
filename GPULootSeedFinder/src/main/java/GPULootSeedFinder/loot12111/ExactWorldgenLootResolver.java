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
    // Buried treasure and desert pyramid currently use the older FeatureUtils
    // worldgen bridge because it tracks observed 26.1.2 results more closely
    // than the partial decoration-seed replay. Shipwreck still has a dedicated
    // exact path, but is not exposed as supported here until that route is
    // trustworthy again. Ruined portal remains excluded because its terrain
    // path is still not modeled exactly enough here.
    private static final MCVersion LEGACY_WORLDGEN_VERSION = MCVersion.v1_17_1;
    private static final long GOLDEN_RATIO_64 = -7046029254386353131L;
    private static final long SILVER_RATIO_64 = 7640891576956012809L;
    private static final long STAFFORD13_MULTIPLIER_1 = -4658895280553007687L;
    private static final long STAFFORD13_MULTIPLIER_2 = -7723592293110705685L;
    private static final int BURIED_TREASURE_DECORATION_STEP_12111 = 3;
    private static final int BURIED_TREASURE_FEATURE_INDEX_12111 = 2;
    private static final int DESERT_PYRAMID_DECORATION_STEP_12111 = 4;
    private static final int DESERT_PYRAMID_FEATURE_INDEX_12111 = 3;
    private static final int UNKNOWN_CHEST_Y = -1;
    private static final int SHIPWRECK_DECORATION_SALT = 40006;
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

    private ExactWorldgenLootResolver() {
    }

    public static boolean supportsStructure(String structureId) {
        return "buried_treasure".equals(structureId)
            || "desert_pyramid".equals(structureId)
            || false;
    }

    public static List<ChestRollView> gatherChestRolls(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        if (isExactShipwreckVariant(structureId)) {
            return gatherExactShipwreckChestRolls(structureId, worldSeed, blockX, blockZ);
        }

        Feature<?, ?> feature = resolveFeature(structureId);
        if (!(feature instanceof ILoot)) {
            return List.of();
        }
        ILoot loot = (ILoot) feature;

        Generator.GeneratorFactory<?> factory = Generators.get(feature.getClass());
        if (factory == null) {
            return List.of();
        }
        Generator generator = factory.create(LEGACY_WORLDGEN_VERSION);
        if (generator == null) {
            return List.of();
        }

        GenerationContext.Context context = feature.getContext(worldSeed);
        if (context == null) {
            return List.of();
        }

        CPos structurePos = new CPos(Math.floorDiv(blockX, 16), Math.floorDiv(blockZ, 16));
        if (!generator.generate(context.getGenerator(), structurePos)) {
            return List.of();
        }

        List<Pair<Generator.ILootType, BPos>> lootPositions = generator.getLootPos();
        Map<CPos, List<Pair<Generator.ILootType, BPos>>> chunkBuckets = new HashMap<>();
        for (Pair<Generator.ILootType, BPos> lootPos : lootPositions) {
            chunkBuckets.computeIfAbsent(lootPos.getSecond().toChunkPos(), key -> new ArrayList<>()).add(lootPos);
        }

        ChunkRand rand = new ChunkRand();
        List<ChestRollView> out = new ArrayList<>();
        String requestedTableId = requestedTableId(structureId);
        for (Pair<Generator.ILootType, BPos> lootPos : lootPositions) {
            CPos chunkPos = lootPos.getSecond().toChunkPos();
            List<Pair<Generator.ILootType, BPos>> chunkLoot = chunkBuckets.get(chunkPos);
            if (chunkLoot == null || chunkLoot.isEmpty()) {
                continue;
            }

            int indexInChunk = indexOf(chunkLoot, lootPos);
            rand.setDecoratorSeed(
                worldSeed,
                chunkPos.getX() * 16,
                chunkPos.getZ() * 16,
                loot.getDecorationSalt(),
                LEGACY_WORLDGEN_VERSION
            );
            ILoot.SpecificCalls calls = loot.getSpecificCalls();
            if (calls != null) {
                calls.run(generator, rand);
            }
            if (loot.shouldAdvanceInChunks()) {
                rand.advance((long) chunkLoot.size() * 2L);
            }
            rand.advance((long) indexInChunk * 2L);

            String tableId = resolveTableId(structureId, lootPos.getFirst());
            if (requestedTableId != null && !requestedTableId.equals(tableId)) {
                continue;
            }
            ExactLootTableLibrary.LootRollResult roll = ExactLootTableLibrary.rollTable(tableId, rand.nextLong());
            BPos pos = remapLegacyChestPos12111(
                structureId,
                worldSeed,
                blockX,
                blockZ,
                indexInChunk,
                lootPos.getSecond()
            );
            out.add(new ChestRollView(
                tableId,
                pos.getX(),
                pos.getY(),
                pos.getZ(),
                roll.slots(),
                roll.counts()
            ));
        }
        if ("desert_pyramid".equals(structureId)) {
            sortLegacyDesertPyramidChests(out, blockX, blockZ);
        }
        return List.copyOf(out);
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

    private static long exactFeatureSeed12111(long worldSeed, int chunkX, int chunkZ, int featureIndex, int decorationStep) {
        long decorationSeed = setDecorationSeed12111(worldSeed, chunkX * 16, chunkZ * 16);
        return decorationSeed + (long) featureIndex + (long) decorationStep * 10000L;
    }

    private static long setDecorationSeed12111(long worldSeed, int blockX, int blockZ) {
        Xoroshiro128PlusPlus rng = new Xoroshiro128PlusPlus(worldSeed);
        long xMultiplier = rng.nextLong() | 1L;
        long zMultiplier = rng.nextLong() | 1L;
        return ((long) blockX * xMultiplier + (long) blockZ * zMultiplier) ^ worldSeed;
    }

    private static Direction2D exactDesertPyramidOrientation12111(long worldSeed, int chunkX, int chunkZ) {
        Random random = new Random(worldSeed);
        long xMultiplier = random.nextLong();
        long zMultiplier = random.nextLong();
        long largeFeatureSeed = (long) chunkX * xMultiplier ^ (long) chunkZ * zMultiplier ^ worldSeed;
        random.setSeed(largeFeatureSeed);
        return Direction2D.HORIZONTAL[random.nextInt(Direction2D.HORIZONTAL.length)];
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

        private long nextLong() {
            long lo = seedLo;
            long hi = seedHi;
            long result = Long.rotateLeft(lo + hi, 17) + lo;
            hi ^= lo;
            seedLo = Long.rotateLeft(lo, 49) ^ hi ^ (hi << 21);
            seedHi = Long.rotateLeft(hi, 28);
            return result;
        }

        private int nextInt(int bound) {
            if (bound <= 0) {
                throw new IllegalArgumentException("Bound must be positive");
            }
            long value = Integer.toUnsignedLong((int) nextLong());
            long product = value * (long) bound;
            long low = product & 0xFFFFFFFFL;
            if (low < (long) bound) {
                int threshold = Integer.remainderUnsigned(~bound + 1, bound);
                while (low < (long) threshold) {
                    value = Integer.toUnsignedLong((int) nextLong());
                    product = value * (long) bound;
                    low = product & 0xFFFFFFFFL;
                }
            }
            return (int) (product >>> 32);
        }
    }

    private enum Direction2D {
        NORTH,
        EAST,
        SOUTH,
        WEST;

        private static final Direction2D[] HORIZONTAL = values();
    }

    private record WorldPos(int x, int y, int z) {
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
            case "shipwreck_supply" -> "chests/shipwreck_supply";
            case "shipwreck_map" -> "chests/shipwreck_map";
            case "shipwreck_treasure" -> "chests/shipwreck_treasure";
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
        throw new IllegalArgumentException("Unsupported exact 26.1.2 loot type: " + structureId + " / " + lootType);
    }

    private static boolean isExactShipwreckVariant(String structureId) {
        return structureId.endsWith("_beached") || structureId.endsWith("_ocean");
    }

    private static List<ChestRollView> gatherExactShipwreckChestRolls(
        String structureId,
        long worldSeed,
        int blockX,
        int blockZ
    ) {
        ShipwreckRequest request = ShipwreckRequest.parse(structureId);
        if (request == null) {
            return List.of();
        }

        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        ChunkRand rand = new ChunkRand();
        rand.setCarverSeed(worldSeed, chunkX, chunkZ, LEGACY_WORLDGEN_VERSION);
        BlockRotation rotation = BlockRotation.getRandom((JRand) rand);
        String[] templatePool = request.isBeached() ? SHIPWRECK_TYPES_BEACHED : SHIPWRECK_TYPES_OCEAN;
        String type = templatePool[rand.nextInt(templatePool.length)];
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

            rand.setDecoratorSeed(
                worldSeed,
                chunkPos.getX() * 16,
                chunkPos.getZ() * 16,
                SHIPWRECK_DECORATION_SALT,
                LEGACY_WORLDGEN_VERSION
            );
            if (request.isBeached()) {
                rand.nextInt(3);
            }
            rand.advance((long) chunkLoot.size() * 2L);
            rand.advance((long) indexOfShipwreckMarker(chunkLoot, marker) * 2L);

            ExactLootTableLibrary.LootRollResult roll = ExactLootTableLibrary.rollTable(marker.tableId(), rand.nextLong());
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
        private static ShipwreckRequest parse(String structureId) {
            if (structureId == null) {
                return null;
            }
            boolean isBeached = false;
            String base = structureId;
            if (structureId.endsWith("_beached")) {
                isBeached = true;
                base = structureId.substring(0, structureId.length() - "_beached".length());
            } else if (structureId.endsWith("_ocean")) {
                isBeached = false;
                base = structureId.substring(0, structureId.length() - "_ocean".length());
            } else {
                return null;
            }
            String requestedTableId = switch (base) {
                case "shipwreck" -> null;
                case "shipwreck_supply" -> "chests/shipwreck_supply";
                case "shipwreck_map" -> "chests/shipwreck_map";
                case "shipwreck_treasure" -> "chests/shipwreck_treasure";
                default -> null;
            };
            if (!"shipwreck".equals(base)
                && !"shipwreck_supply".equals(base)
                && !"shipwreck_map".equals(base)
                && !"shipwreck_treasure".equals(base)) {
                return null;
            }
            return new ShipwreckRequest(isBeached, requestedTableId);
        }
    }
}
