package GPULootSeedFinder;

import GPULootSeedFinder.cubiomes2612.VanillaStructureBiomeOracle;
import GPULootSeedFinder.loot12111.ExactLootTableLibrary;
import GPULootSeedFinder.loot12111.ExactWorldgenLootResolver;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import kaptainwutax.featureutils.loot.ChestContent;
import kaptainwutax.featureutils.loot.ILoot;
import kaptainwutax.featureutils.loot.item.ItemStack;
import kaptainwutax.featureutils.structure.BuriedTreasure;
import kaptainwutax.featureutils.structure.DesertPyramid;
import kaptainwutax.featureutils.structure.RuinedPortal;
import kaptainwutax.featureutils.structure.Shipwreck;
import kaptainwutax.featureutils.structure.Village;
import kaptainwutax.mcutils.rand.ChunkRand;
import kaptainwutax.mcutils.state.Dimension;
import kaptainwutax.mcutils.util.pos.CPos;
import kaptainwutax.mcutils.version.MCVersion;

public class LootValidationServer {
    private static MCVersion resolveVersion(String token) {
        String t = token == null ? "" : token.trim().toLowerCase();
        if ("1.17".equals(t)) {
            return MCVersion.v1_17;
        }
        if ("1.17.1".equals(t)) {
            return MCVersion.v1_17_1;
        }
        return null;
    }

    private static boolean isExact2612(String token) {
        String t = token == null ? "" : token.trim();
        return "26.1.2".equals(t) || "26.1.1".equals(t) || "1.21.11".equals(t) || "4790".equals(t);
    }

    private static String encodeStructure(String structure, String versionToken, long seed, int blockX, int blockZ)
        throws Exception {
        if (!isExact2612(versionToken)) {
            return "OK\tSTRUCTURE\tSKIP";
        }
        VanillaStructureBiomeOracle.ValidationResult result =
            VanillaStructureBiomeOracle.validateStructure(structure, seed, blockX, blockZ, false);
        if (result == null || (result.vanillaGenerationPoint().isEmpty() && !result.message().isEmpty())) {
            return "OK\tSTRUCTURE\tSKIP";
        }
        return "OK\tSTRUCTURE\t" + (result.viable() ? "1" : "0");
    }

    private static class StructureConfig {
        final ILoot loot;

        StructureConfig(ILoot loot) {
            this.loot = loot;
        }
    }

    private record SlotEncoding(String slots, String counts) {
    }

    private record LootRequirement(String item, int count) {
    }

    private record StructureSettingSpec(boolean abandoned, List<String> buildings) {
    }

    private record VillageHouseEntry(String location, int weight) {
    }

    private static final Map<String, List<LootRequirement>> REQUIREMENT_CACHE = new HashMap<>();
    private static final Map<String, List<VillageHouseEntry>> VILLAGE_HOUSE_POOL_CACHE = new HashMap<>();
    private static final Pattern VILLAGE_HOUSE_ENTRY = Pattern.compile(
        "\"location\"\\s*:\\s*\"minecraft:village/([^\"]+)\"[\\s\\S]*?\"weight\"\\s*:\\s*(\\d+)"
    );
    private static final String DECOMPILED_ROOT_PROPERTY = "helixseed.minecraft2612.decompiled";
    private static final String DECOMPILED_ROOT_ENV = "HELIXSEED_MINECRAFT_2612_DECOMPILED";
    private static final String DEFAULT_DECOMPILED_DIR = "Minecraft-Decompiled-26.1.2";

    private static String normalizeItemId(String item) {
        String out = item == null ? "" : item.trim().toLowerCase();
        if (out.startsWith("minecraft:")) {
            out = out.substring("minecraft:".length());
        }
        return out;
    }

    private static List<LootRequirement> parseRequirements(String encoded) {
        List<LootRequirement> out = new ArrayList<>();
        String raw = encoded == null ? "" : encoded.trim();
        if (raw.isEmpty() || "_".equals(raw)) {
            return out;
        }
        String[] entries = raw.split(";");
        for (String entry : entries) {
            String e = entry == null ? "" : entry.trim();
            if (e.isEmpty() || "_".equals(e)) {
                continue;
            }
            int sep = e.lastIndexOf(':');
            if (sep <= 0 || sep + 1 >= e.length()) {
                throw new IllegalArgumentException("Invalid loot requirement: " + e);
            }
            String item = normalizeItemId(e.substring(0, sep));
            int count = Integer.parseInt(e.substring(sep + 1).trim());
            if (!item.isEmpty() && count > 0) {
                out.add(new LootRequirement(item, count));
            }
        }
        return out;
    }

    private static List<LootRequirement> parseRequirementsCached(String encoded) {
        String key = encoded == null ? "" : encoded.trim();
        List<LootRequirement> cached = REQUIREMENT_CACHE.get(key);
        if (cached != null) {
            return cached;
        }
        List<LootRequirement> parsed = List.copyOf(parseRequirements(key));
        REQUIREMENT_CACHE.put(key, parsed);
        return parsed;
    }

    private static String normalizeId(String value) {
        String out = value == null ? "" : value.trim().toLowerCase(Locale.ROOT);
        if (out.startsWith("minecraft:")) {
            out = out.substring("minecraft:".length());
        }
        return out.replace('-', '_').replace(' ', '_');
    }

    private static StructureSettingSpec parseStructureSettings(String encoded) {
        boolean abandoned = false;
        List<String> buildings = new ArrayList<>();
        String raw = encoded == null ? "" : encoded.trim();
        if (raw.isEmpty() || "_".equals(raw)) {
            return new StructureSettingSpec(false, List.of());
        }
        for (String group : raw.split(";")) {
            if (group.isBlank()) {
                continue;
            }
            int sep = group.indexOf('=');
            if (sep <= 0) {
                continue;
            }
            String key = normalizeId(group.substring(0, sep));
            String value = group.substring(sep + 1).trim();
            if ("state".equals(key)) {
                for (String part : value.split(",")) {
                    if ("abandoned".equals(normalizeId(part))) {
                        abandoned = true;
                    }
                }
            } else if ("buildings".equals(key)) {
                for (String part : value.split(",")) {
                    String building = normalizeId(part);
                    if (!building.isEmpty()) {
                        buildings.add(building);
                    }
                }
            }
        }
        return new StructureSettingSpec(abandoned, List.copyOf(buildings));
    }

    private static MCVersion resolveVillageVersion(String token) {
        MCVersion version = resolveVersion(token);
        if (version != null) {
            return version;
        }
        if (isExact2612(token)) {
            return MCVersion.latest();
        }
        return MCVersion.latest();
    }

    private static Path resolveDecompiledRoot() {
        String configured = System.getProperty(DECOMPILED_ROOT_PROPERTY);
        if (configured == null || configured.isBlank()) {
            configured = System.getenv(DECOMPILED_ROOT_ENV);
        }
        if (configured != null && !configured.isBlank()) {
            return Path.of(configured);
        }
        String home = System.getProperty("user.home");
        if (home != null && !home.isBlank()) {
            Path desktopCopy = Path.of(home, "Desktop", DEFAULT_DECOMPILED_DIR);
            if (Files.isDirectory(desktopCopy)) {
                return desktopCopy;
            }
        }
        return Path.of(DEFAULT_DECOMPILED_DIR);
    }

    private static String villageStyleFromStructure(String structure) {
        String s = normalizeId(structure);
        if (s.startsWith("village_")) {
            String tail = s.substring("village_".length());
            if ("plains".equals(tail) || "desert".equals(tail) || "savanna".equals(tail) ||
                "snowy".equals(tail) || "taiga".equals(tail)) {
                return tail;
            }
        }
        return "plains";
    }

    private static List<VillageHouseEntry> loadVillageHousePool(String style, boolean zombie) {
        String normalizedStyle = normalizeId(style);
        String key = normalizedStyle + "|" + (zombie ? "zombie" : "normal");
        List<VillageHouseEntry> cached = VILLAGE_HOUSE_POOL_CACHE.get(key);
        if (cached != null) {
            return cached;
        }
        Path root = resolveDecompiledRoot();
        Path path = root.resolve("resources")
            .resolve("data")
            .resolve("minecraft")
            .resolve("worldgen")
            .resolve("template_pool")
            .resolve("village")
            .resolve(normalizedStyle);
        if (zombie) {
            path = path.resolve("zombie");
        }
        path = path.resolve("houses.json");
        List<VillageHouseEntry> out = new ArrayList<>();
        try {
            if (Files.isRegularFile(path)) {
                String json = Files.readString(path, StandardCharsets.UTF_8);
                Matcher matcher = VILLAGE_HOUSE_ENTRY.matcher(json);
                while (matcher.find()) {
                    String location = normalizeId(matcher.group(1));
                    int weight = Integer.parseInt(matcher.group(2));
                    if (location.contains("/houses/") && weight > 0) {
                        out.add(new VillageHouseEntry(location, weight));
                    }
                }
            }
        } catch (Exception ignored) {
            out.clear();
        }
        List<VillageHouseEntry> frozen = List.copyOf(out);
        VILLAGE_HOUSE_POOL_CACHE.put(key, frozen);
        return frozen;
    }

    private static VillageHouseEntry pickWeightedHouse(List<VillageHouseEntry> pool, ChunkRand rand) {
        int total = 0;
        for (VillageHouseEntry entry : pool) {
            total += Math.max(0, entry.weight());
        }
        if (total <= 0) {
            return null;
        }
        int roll = rand.nextInt(total);
        for (VillageHouseEntry entry : pool) {
            roll -= Math.max(0, entry.weight());
            if (roll < 0) {
                return entry;
            }
        }
        return pool.get(pool.size() - 1);
    }

    private static boolean checkStructureSettings(
        String structure,
        String versionToken,
        long seed,
        int blockX,
        int blockZ,
        String encodedSettings
    ) {
        StructureSettingSpec spec = parseStructureSettings(encodedSettings);
        if (spec.buildings().isEmpty() && !spec.abandoned()) {
            return true;
        }
        String family = normalizeId(structure);
        if (!family.equals("village") && !family.startsWith("village_")) {
            return true;
        }
        Path decompiledRoot = resolveDecompiledRoot();
        if (!spec.buildings().isEmpty()) {
            boolean buildingMatch = false;
            List<String> needles = buildingNeedles(spec.buildings());
            for (String styleStructure : candidateVillageSettingStructures(structure, versionToken, seed, blockX, blockZ)) {
                Optional<Boolean> vanillaMatch =
                    VanillaStructureBiomeOracle.jigsawPiecesContainAny(styleStructure, seed, blockX, blockZ, needles);
                if (vanillaMatch.isPresent()) {
                    if (vanillaMatch.get()) {
                        buildingMatch = true;
                        break;
                    }
                    continue;
                }
                if (VillageJigsawBuildingDetector.villageHasAnyBuilding(
                        styleStructure,
                        versionToken,
                        seed,
                        blockX,
                        blockZ,
                        decompiledRoot,
                        spec.buildings())) {
                    buildingMatch = true;
                    break;
                }
                if (estimatedVillageHasBuildings(
                        styleStructure,
                        versionToken,
                        seed,
                        blockX,
                        blockZ,
                        spec.abandoned(),
                        spec.buildings())) {
                    buildingMatch = true;
                    break;
                }
            }
            if (!buildingMatch) {
                return false;
            }
        }
        if (spec.abandoned()) {
            boolean abandoned = VillageJigsawBuildingDetector.villageAbandoned(
                structure,
                versionToken,
                seed,
                blockX,
                blockZ,
                decompiledRoot
            );
            if (!abandoned) {
                return false;
            }
        }
        return true;
    }

    private static boolean estimatedVillageHasBuildings(
        String structure,
        String versionToken,
        long seed,
        int blockX,
        int blockZ,
        boolean zombie,
        List<String> buildings
    ) {
        if (buildings.isEmpty()) {
            return true;
        }
        String style = villageStyleFromStructure(structure);
        List<VillageHouseEntry> pool = loadVillageHousePool(style, zombie);
        if (pool.isEmpty()) {
            return false;
        }

        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        ChunkRand rand = new ChunkRand();
        rand.setCarverSeed(seed, chunkX, chunkZ, resolveVillageVersion(versionToken));
        rand.nextInt(4);
        rand.nextInt(4);
        int houseDraws = 10 + rand.nextInt(12);
        List<String> needles = buildingNeedles(buildings);
        for (int i = 0; i < houseDraws; i++) {
            VillageHouseEntry entry = pickWeightedHouse(pool, rand);
            if (entry == null) {
                break;
            }
            String location = entry.location();
            for (String needle : needles) {
                if (!needle.isEmpty() && location.contains(needle)) {
                    return true;
                }
            }
        }
        return false;
    }

    private static List<String> buildingNeedles(List<String> buildings) {
        List<String> out = new ArrayList<>();
        for (String building : buildings) {
            String b = normalizeId(building);
            if (b.startsWith("village_")) {
                b = b.substring("village_".length());
            }
            switch (b) {
                case "blacksmith":
                case "weaponsmith":
                    out.add("weaponsmith");
                    out.add("weapon_smith");
                    break;
                case "toolsmith":
                case "tool_smith":
                    out.add("tool_smith");
                    break;
                case "tannery":
                case "leatherworker":
                    out.add("tannery");
                    break;
                case "temple":
                case "cleric":
                    out.add("temple");
                    break;
                default:
                    if (!b.isEmpty()) {
                        out.add(b);
                    }
                    break;
            }
        }
        return List.copyOf(out);
    }

    private static List<String> candidateVillageSettingStructures(
        String structure,
        String versionToken,
        long seed,
        int blockX,
        int blockZ
    ) {
        String s = normalizeId(structure);
        if (s.equals("village_plains") || s.equals("village_desert") || s.equals("village_savanna") ||
            s.equals("village_snowy") || s.equals("village_taiga")) {
            return List.of(s);
        }
        String sampled = sampledVillageStyle(seed, blockX, blockZ);
        if (!sampled.isEmpty()) {
            return List.of("village_" + sampled);
        }

        List<String> vanillaViable = new ArrayList<>();
        for (String candidate : List.of("village_plains", "village_desert", "village_savanna", "village_snowy", "village_taiga")) {
            try {
                VanillaStructureBiomeOracle.ValidationResult result =
                    VanillaStructureBiomeOracle.validateStructure(candidate, seed, blockX, blockZ, false);
                if (result != null && result.viable()) {
                    vanillaViable.add(candidate);
                }
            } catch (Throwable ignored) {
            }
        }
        if (!vanillaViable.isEmpty()) {
            return List.copyOf(vanillaViable);
        }

        return List.of("village_plains", "village_desert", "village_savanna", "village_snowy", "village_taiga");
    }

    private static String sampledVillageStyle(long seed, int blockX, int blockZ) {
        try {
            int y = 64;
            try {
                y = VanillaStructureBiomeOracle.surfaceHeight(seed, blockX, blockZ);
            } catch (Throwable ignored) {
            }
            String biome = normalizeId(VanillaStructureBiomeOracle.sampleBiomeId(seed, blockX, y, blockZ));
            return switch (biome) {
                case "plains", "meadow" -> "plains";
                case "desert" -> "desert";
                case "savanna" -> "savanna";
                case "snowy_plains" -> "snowy";
                case "taiga" -> "taiga";
                default -> "";
            };
        } catch (Throwable ignored) {
            return "";
        }
    }

    private static void addCount(Map<String, Integer> counts, String item, int count) {
        if (count <= 0) {
            return;
        }
        String normalized = normalizeItemId(item);
        if (normalized.isEmpty()) {
            return;
        }
        counts.put(normalized, counts.getOrDefault(normalized, 0) + count);
    }

    private static boolean countsSatisfy(Map<String, Integer> counts, List<LootRequirement> requirements) {
        for (LootRequirement req : requirements) {
            if (counts.getOrDefault(req.item(), 0) < req.count()) {
                return false;
            }
        }
        return true;
    }

    private static Map<String, Integer> aggregateLegacyItems(List<ItemStack> slots) {
        Map<String, Integer> counts = new TreeMap<>();
        for (ItemStack stack : slots) {
            if (stack == null || stack.isEmpty()) {
                continue;
            }
            addCount(counts, stack.getItem().getName(), stack.getCount());
        }
        return counts;
    }

    private static Map<String, Integer> aggregateExactItems(List<ExactLootTableLibrary.ItemStackView> slots) {
        Map<String, Integer> counts = new TreeMap<>();
        for (ExactLootTableLibrary.ItemStackView stack : slots) {
            if (stack == null || stack.isEmpty()) {
                continue;
            }
            addCount(counts, stack.itemId(), stack.count());
        }
        return counts;
    }

    private static StructureConfig resolveStructureConfig(String structure, MCVersion version) {
        if ("ruined_portal".equals(structure)) {
            return new StructureConfig(new RuinedPortal(Dimension.OVERWORLD, version));
        }
        if ("buried_treasure".equals(structure)) {
            return new StructureConfig(new BuriedTreasure(version));
        }
        if ("desert_pyramid".equals(structure)) {
            return new StructureConfig(new DesertPyramid(version));
        }
        if ("shipwreck".equals(structure)) {
            return new StructureConfig(new Shipwreck(version));
        }
        return null;
    }

    private static List<ChestContent> gatherLegacyChests(StructureConfig cfg, long seed, int blockX, int blockZ) {
        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        List<ChestContent> chests = cfg.loot.getLootAtPos(seed, new CPos(chunkX, chunkZ), new ChunkRand(), true);
        List<ChestContent> out = new ArrayList<>();
        if (chests == null || chests.isEmpty()) {
            return out;
        }
        for (ChestContent chest : chests) {
            if (chest != null) {
                out.add(chest);
            }
        }
        return out;
    }

    private static List<ItemStack> flattenLegacyItems(List<ChestContent> chests) {
        List<ItemStack> out = new ArrayList<>();
        for (ChestContent chest : chests) {
            List<ItemStack> chestItems = chest.getItems();
            if (chestItems == null || chestItems.isEmpty()) {
                continue;
            }
            out.addAll(chestItems);
        }
        return out;
    }

    private static SlotEncoding encodeLegacySlots(List<ItemStack> slots) {
        LinkedList<ItemStack> indexedSlots = new LinkedList<>(slots);

        StringBuilder slotBuilder = new StringBuilder();
        Map<String, Integer> counts = new TreeMap<>();
        if (indexedSlots.isEmpty()) {
            slotBuilder.append('_');
        }
        for (int i = 0; i < indexedSlots.size(); i++) {
            if (i > 0) {
                slotBuilder.append(';');
            }
            ItemStack stack = indexedSlots.get(i);
            if (stack == null || stack.isEmpty()) {
                slotBuilder.append('_');
                continue;
            }
            String item = stack.getItem().getName().toLowerCase();
            int count = stack.getCount();
            slotBuilder.append(item).append(':').append(count);
            counts.put(item, counts.getOrDefault(item, 0) + count);
        }

        StringBuilder countBuilder = new StringBuilder();
        boolean first = true;
        for (Map.Entry<String, Integer> e : counts.entrySet()) {
            if (!first) {
                countBuilder.append(';');
            }
            first = false;
            countBuilder.append(e.getKey()).append(':').append(e.getValue());
        }
        if (countBuilder.length() == 0) {
            countBuilder.append('_');
        }
        return new SlotEncoding(slotBuilder.toString(), countBuilder.toString());
    }

    private static SlotEncoding encodeExactSlots(List<ExactLootTableLibrary.ItemStackView> indexedSlots) {
        StringBuilder slotBuilder = new StringBuilder();
        Map<String, Integer> counts = new TreeMap<>();
        if (indexedSlots.isEmpty()) {
            slotBuilder.append('_');
        }
        for (int i = 0; i < indexedSlots.size(); i++) {
            if (i > 0) {
                slotBuilder.append(';');
            }
            ExactLootTableLibrary.ItemStackView stack = indexedSlots.get(i);
            if (stack == null || stack.isEmpty()) {
                slotBuilder.append('_');
                continue;
            }
            String item = stack.itemId().toLowerCase();
            int count = stack.count();
            slotBuilder.append(item).append(':').append(count);
            counts.put(item, counts.getOrDefault(item, 0) + count);
        }

        StringBuilder countBuilder = new StringBuilder();
        boolean first = true;
        for (Map.Entry<String, Integer> e : counts.entrySet()) {
            if (!first) {
                countBuilder.append(';');
            }
            first = false;
            countBuilder.append(e.getKey()).append(':').append(e.getValue());
        }
        if (countBuilder.length() == 0) {
            countBuilder.append('_');
        }
        return new SlotEncoding(slotBuilder.toString(), countBuilder.toString());
    }

    private static String resolveLegacyTableId(String structure, ChestContent chest) {
        String lootType = String.valueOf(chest.getLootType()).toLowerCase();
        if ("buried_treasure".equals(structure)) {
            return "chests/buried_treasure";
        }
        if ("desert_pyramid".equals(structure)) {
            return "chests/desert_pyramid";
        }
        if ("ruined_portal".equals(structure)) {
            return "chests/ruined_portal";
        }
        if ("shipwreck".equals(structure)) {
            if (lootType.contains("supply")) {
                return "chests/shipwreck_supply";
            }
            if (lootType.contains("map")) {
                return "chests/shipwreck_map";
            }
            if (lootType.contains("treasure")) {
                return "chests/shipwreck_treasure";
            }
            return "chests/shipwreck";
        }
        return structure;
    }

    private static String encodeLegacyChestDetails(String structure, List<ChestContent> chests) {
        if (chests.isEmpty()) {
            return "_";
        }
        StringBuilder out = new StringBuilder();
        boolean firstChest = true;
        for (ChestContent chest : chests) {
            if (chest == null) {
                continue;
            }
            if (!firstChest) {
                out.append('|');
            }
            firstChest = false;
            String tableId = resolveLegacyTableId(structure, chest);
            out.append(tableId).append('@');
            if (chest.getPos() == null) {
                out.append("0,0,0");
            } else {
                out.append(chest.getPos().getX()).append(',').append(chest.getPos().getY()).append(',').append(chest.getPos().getZ());
            }
            SlotEncoding encoding = encodeLegacySlots(chest.getItems() == null ? List.of() : chest.getItems());
            out.append('@').append(encoding.counts());
        }
        return out.length() == 0 ? "_" : out.toString();
    }

    private static String encodeExactChestDetails(List<ExactWorldgenLootResolver.ChestRollView> chests) {
        if (chests.isEmpty()) {
            return "_";
        }
        StringBuilder out = new StringBuilder();
        boolean firstChest = true;
        for (ExactWorldgenLootResolver.ChestRollView chest : chests) {
            if (chest == null) {
                continue;
            }
            if (!firstChest) {
                out.append('|');
            }
            firstChest = false;
            out.append(chest.tableId()).append('@')
               .append(chest.x()).append(',').append(chest.y()).append(',').append(chest.z());
            SlotEncoding encoding = encodeExactSlots(chest.slots() == null ? List.of() : chest.slots());
            out.append('@').append(encoding.counts());
        }
        return out.length() == 0 ? "_" : out.toString();
    }

    private static String encodeRoll(StructureConfig cfg, MCVersion version, long seed, int blockX, int blockZ) {
        List<ChestContent> chests = gatherLegacyChests(cfg, seed, blockX, blockZ);
        SlotEncoding encoding = encodeLegacySlots(flattenLegacyItems(chests));
        return "OK\tROLL\t" + encoding.slots() + "\t" + encoding.counts();
    }

    private static String encodeCheck(
        StructureConfig cfg,
        long seed,
        int blockX,
        int blockZ,
        List<LootRequirement> requirements
    ) {
        return "OK\tCHECK\t" + (legacyCheckPassed(cfg, seed, blockX, blockZ, requirements) ? "1" : "0");
    }

    private static boolean legacyCheckPassed(
        StructureConfig cfg,
        long seed,
        int blockX,
        int blockZ,
        List<LootRequirement> requirements
    ) {
        List<ChestContent> chests = gatherLegacyChests(cfg, seed, blockX, blockZ);
        return countsSatisfy(aggregateLegacyItems(flattenLegacyItems(chests)), requirements);
    }

    private static String encodeRollDetail(StructureConfig cfg, String structure, long seed, int blockX, int blockZ) {
        List<ChestContent> chests = gatherLegacyChests(cfg, seed, blockX, blockZ);
        SlotEncoding encoding = encodeLegacySlots(flattenLegacyItems(chests));
        return "OK\tROLL_DETAIL\t" + encoding.counts() + "\t" + encodeLegacyChestDetails(structure, chests);
    }

    private static String encodeExactRoll(String structure, long seed, int blockX, int blockZ) {
        List<ExactLootTableLibrary.ItemStackView> indexedSlots =
            ExactWorldgenLootResolver.gatherIndexedSlots(structure, seed, blockX, blockZ);
        SlotEncoding encoding = encodeExactSlots(indexedSlots);
        return "OK\tROLL\t" + encoding.slots() + "\t" + encoding.counts();
    }

    private static String encodeExactCheck(
        String structure,
        long seed,
        int blockX,
        int blockZ,
        List<LootRequirement> requirements
    ) {
        return "OK\tCHECK\t" + (exactCheckPassed(structure, seed, blockX, blockZ, requirements) ? "1" : "0");
    }

    private static boolean exactCheckPassed(
        String structure,
        long seed,
        int blockX,
        int blockZ,
        List<LootRequirement> requirements
    ) {
        List<Map<String, Integer>> alternatives =
            ExactWorldgenLootResolver.gatherAggregateCountAlternatives(structure, seed, blockX, blockZ);
        for (Map<String, Integer> counts : alternatives) {
            if (countsSatisfy(counts, requirements)) {
                return true;
            }
        }
        return false;
    }

    private static Map<String, Integer> normalizeCountKeys(Map<String, Integer> counts) {
        Map<String, Integer> out = new TreeMap<>();
        for (Map.Entry<String, Integer> entry : counts.entrySet()) {
            addCount(out, entry.getKey(), entry.getValue() == null ? 0 : entry.getValue());
        }
        return out;
    }

    private static String encodeExactRollAt(
        String structure,
        long seed,
        int blockX,
        int blockZ,
        int chestX,
        int chestY,
        int chestZ
    ) {
        List<ExactLootTableLibrary.ItemStackView> indexedSlots =
            ExactWorldgenLootResolver.gatherIndexedSlotsAt(structure, seed, blockX, blockZ, chestX, chestY, chestZ);
        SlotEncoding encoding = encodeExactSlots(indexedSlots);
        return "OK\tROLL\t" + encoding.slots() + "\t" + encoding.counts();
    }

    private static String encodeExactRollDetail(String structure, long seed, int blockX, int blockZ) {
        List<ExactWorldgenLootResolver.ChestRollView> chests =
            ExactWorldgenLootResolver.gatherChestRolls(structure, seed, blockX, blockZ);
        List<ExactWorldgenLootResolver.ChestRollView> displayChests = orderExactChestsForDisplay(
            structure,
            chests,
            blockX,
            blockZ
        );
        List<ExactLootTableLibrary.ItemStackView> allSlots = new ArrayList<>();
        for (ExactWorldgenLootResolver.ChestRollView chest : displayChests) {
            if (chest != null && chest.slots() != null) {
                allSlots.addAll(chest.slots());
            }
        }
        SlotEncoding encoding = encodeExactSlots(allSlots);
        return "OK\tROLL_DETAIL\t" + encoding.counts() + "\t" + encodeExactChestDetails(displayChests);
    }

    private static String encodeExactRollDetailAt(
        String structure,
        long seed,
        int blockX,
        int blockZ,
        int chestX,
        int chestY,
        int chestZ
    ) {
        List<ExactWorldgenLootResolver.ChestRollView> chests =
            ExactWorldgenLootResolver.gatherChestRollsAt(structure, seed, blockX, blockZ, chestX, chestY, chestZ);
        List<ExactWorldgenLootResolver.ChestRollView> displayChests = orderExactChestsForDisplay(
            structure,
            chests,
            blockX,
            blockZ
        );
        List<ExactLootTableLibrary.ItemStackView> allSlots = new ArrayList<>();
        for (ExactWorldgenLootResolver.ChestRollView chest : displayChests) {
            if (chest != null && chest.slots() != null) {
                allSlots.addAll(chest.slots());
            }
        }
        SlotEncoding encoding = encodeExactSlots(allSlots);
        return "OK\tROLL_DETAIL\t" + encoding.counts() + "\t" + encodeExactChestDetails(displayChests);
    }

    private static List<ExactWorldgenLootResolver.ChestRollView> orderExactChestsForDisplay(
        String structure,
        List<ExactWorldgenLootResolver.ChestRollView> chests,
        int blockX,
        int blockZ
    ) {
        if (!"desert_pyramid".equals(structure) || chests.size() <= 1) {
            return chests;
        }
        List<ExactWorldgenLootResolver.ChestRollView> ordered = new ArrayList<>(chests);
        int centerX = Math.floorDiv(blockX, 16) * 16 + 10;
        int centerZ = Math.floorDiv(blockZ, 16) * 16 + 10;
        ordered.sort((lhs, rhs) -> {
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
        return ordered;
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

    public static void main(String[] args) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
        PrintWriter out = new PrintWriter(System.out, true);
        out.println("READY");
        String line;
        while ((line = in.readLine()) != null) {
            String trimmed = line.trim();
            if (trimmed.isEmpty()) {
                continue;
            }
            if ("PING".equals(trimmed)) {
                out.println("PONG");
                continue;
            }
            if ("QUIT".equals(trimmed)) {
                out.println("BYE");
                break;
            }
            String[] parts = line.split("\t", -1);
            String command = parts.length == 0 ? "" : parts[0].trim();
            boolean basicCommand = "ROLL".equals(command) || "ROLL_DETAIL".equals(command) || "STRUCTURE".equals(command);
            boolean chestCommand = "ROLL_AT".equals(command) || "ROLL_DETAIL_AT".equals(command);
            boolean checkCommand = "CHECK".equals(command);
            boolean checkBatchCommand = "CHECK_BATCH".equals(command);
            boolean settingsBatchCommand = "SETTINGS_BATCH".equals(command);
            if ((!basicCommand || parts.length != 6) && (!chestCommand || parts.length != 9) &&
                (!checkCommand || parts.length != 7) &&
                (!checkBatchCommand || parts.length < 3 || ((parts.length - 3) % 5) != 0) &&
                (!settingsBatchCommand || parts.length < 3 || ((parts.length - 3) % 5) != 0)) {
                out.println(
                    "ERR\tInvalid command. Expected: ROLL<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z or " +
                    "ROLL_DETAIL<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z or " +
                    "ROLL_AT<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z<TAB>chestX<TAB>chestY<TAB>chestZ or " +
                    "ROLL_DETAIL_AT<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z<TAB>chestX<TAB>chestY<TAB>chestZ or " +
                    "CHECK<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z<TAB>item:count;... or " +
                    "CHECK_BATCH<TAB>version<TAB>count<TAB>(structure<TAB>seed<TAB>x<TAB>z<TAB>item:count;...)* or " +
                    "SETTINGS_BATCH<TAB>version<TAB>count<TAB>(structure<TAB>seed<TAB>x<TAB>z<TAB>settings)* or " +
                    "STRUCTURE<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z"
                );
                continue;
            }
            try {
                if ("SETTINGS_BATCH".equals(command)) {
                    String versionToken = parts[1].trim();
                    int count = Integer.parseInt(parts[2].trim());
                    if (count < 0 || parts.length != 3 + count * 5) {
                        out.println("ERR\tInvalid SETTINGS_BATCH item count");
                        continue;
                    }
                    StringBuilder bits = new StringBuilder(count);
                    for (int i = 0; i < count; i++) {
                        int offset = 3 + i * 5;
                        String structure = parts[offset].trim().toLowerCase(Locale.ROOT);
                        long seed = Long.parseLong(parts[offset + 1].trim());
                        int blockX = Integer.parseInt(parts[offset + 2].trim());
                        int blockZ = Integer.parseInt(parts[offset + 3].trim());
                        String settings = parts[offset + 4].trim();
                        bits.append(checkStructureSettings(structure, versionToken, seed, blockX, blockZ, settings) ? '1' : '0');
                    }
                    out.println("OK\tSETTINGS_BATCH\t" + bits);
                    continue;
                }
                if ("CHECK_BATCH".equals(command)) {
                    String versionToken = parts[1].trim();
                    int count = Integer.parseInt(parts[2].trim());
                    if (count < 0 || parts.length != 3 + count * 5) {
                        out.println("ERR\tInvalid CHECK_BATCH item count");
                        continue;
                    }
                    StringBuilder bits = new StringBuilder(count);
                    for (int i = 0; i < count; i++) {
                        int offset = 3 + i * 5;
                        String structure = parts[offset].trim().toLowerCase();
                        long seed = Long.parseLong(parts[offset + 1].trim());
                        int blockX = Integer.parseInt(parts[offset + 2].trim());
                        int blockZ = Integer.parseInt(parts[offset + 3].trim());
                        List<LootRequirement> requirements = parseRequirementsCached(parts[offset + 4]);
                        boolean passed;
                        if (isExact2612(versionToken)) {
                            if (!ExactWorldgenLootResolver.supportsStructure(structure)) {
                                out.println("ERR\tUnsupported exact 26.1.1/26.1.2 world-seed loot validation: " + structure);
                                bits = null;
                                break;
                            }
                            passed = exactCheckPassed(structure, seed, blockX, blockZ, requirements);
                        } else {
                            MCVersion version = resolveVersion(versionToken);
                            if (version == null) {
                                out.println("ERR\tUnsupported mc-version for loot validation. Use 1.17 or 1.17.1.");
                                bits = null;
                                break;
                            }
                            StructureConfig cfg = resolveStructureConfig(structure, version);
                            if (cfg == null) {
                                out.println("ERR\tUnsupported structure for loot validation: " + structure);
                                bits = null;
                                break;
                            }
                            passed = legacyCheckPassed(cfg, seed, blockX, blockZ, requirements);
                        }
                        bits.append(passed ? '1' : '0');
                    }
                    if (bits != null) {
                        out.println("OK\tCHECK_BATCH\t" + bits);
                    }
                    continue;
                }
                String structure = parts[1].trim().toLowerCase();
                String versionToken = parts[2].trim();
                if ("STRUCTURE".equals(command)) {
                    long seed = Long.parseLong(parts[3].trim());
                    int blockX = Integer.parseInt(parts[4].trim());
                    int blockZ = Integer.parseInt(parts[5].trim());
                    out.println(encodeStructure(structure, versionToken, seed, blockX, blockZ));
                    continue;
                }
                if (isExact2612(versionToken)) {
                    if (!ExactWorldgenLootResolver.supportsStructure(structure)) {
                        out.println("ERR\tUnsupported exact 26.1.1/26.1.2 world-seed loot validation: " + structure);
                        continue;
                    }
                    long seed = Long.parseLong(parts[3].trim());
                    int blockX = Integer.parseInt(parts[4].trim());
                    int blockZ = Integer.parseInt(parts[5].trim());
                    if ("CHECK".equals(command)) {
                        out.println(encodeExactCheck(
                            structure,
                            seed,
                            blockX,
                            blockZ,
                            parseRequirementsCached(parts[6])
                        ));
                    } else if ("ROLL_AT".equals(command) || "ROLL_DETAIL_AT".equals(command)) {
                        int chestX = Integer.parseInt(parts[6].trim());
                        int chestY = Integer.parseInt(parts[7].trim());
                        int chestZ = Integer.parseInt(parts[8].trim());
                        if ("ROLL_DETAIL_AT".equals(command)) {
                            out.println(encodeExactRollDetailAt(structure, seed, blockX, blockZ, chestX, chestY, chestZ));
                        } else {
                            out.println(encodeExactRollAt(structure, seed, blockX, blockZ, chestX, chestY, chestZ));
                        }
                    } else if ("ROLL_DETAIL".equals(command)) {
                        out.println(encodeExactRollDetail(structure, seed, blockX, blockZ));
                    } else {
                        out.println(encodeExactRoll(structure, seed, blockX, blockZ));
                    }
                    continue;
                }

                MCVersion version = resolveVersion(versionToken);
                if (version == null) {
                    out.println("ERR\tUnsupported mc-version for loot validation. Use 1.17 or 1.17.1.");
                    continue;
                }
                if ("ROLL_AT".equals(command) || "ROLL_DETAIL_AT".equals(command)) {
                    out.println("ERR\tChest-specific loot validation is only supported by the exact 26.1.1/26.1.2 path.");
                    continue;
                }
                long seed = Long.parseLong(parts[3].trim());
                int blockX = Integer.parseInt(parts[4].trim());
                int blockZ = Integer.parseInt(parts[5].trim());

                StructureConfig cfg = resolveStructureConfig(structure, version);
                if (cfg == null) {
                    out.println("ERR\tUnsupported structure for loot validation: " + structure);
                    continue;
                }
                if ("CHECK".equals(command)) {
                    out.println(encodeCheck(cfg, seed, blockX, blockZ, parseRequirementsCached(parts[6])));
                } else if ("ROLL_DETAIL".equals(command)) {
                    out.println(encodeRollDetail(cfg, structure, seed, blockX, blockZ));
                } else {
                    out.println(encodeRoll(cfg, version, seed, blockX, blockZ));
                }
            } catch (Exception ex) {
                out.println("ERR\t" + ex.getClass().getSimpleName() + ": " + ex.getMessage());
            }
        }
    }
}
