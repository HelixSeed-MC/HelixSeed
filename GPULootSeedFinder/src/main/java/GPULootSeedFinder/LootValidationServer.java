package GPULootSeedFinder;

import GPULootSeedFinder.cubiomes2612.VanillaWorldgen2612;
import GPULootSeedFinder.loot12111.ExactLootTableLibrary;
import GPULootSeedFinder.loot12111.ExactWorldgenLootResolver;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import kaptainwutax.featureutils.loot.ChestContent;
import kaptainwutax.featureutils.loot.ILoot;
import kaptainwutax.featureutils.loot.item.ItemStack;
import kaptainwutax.featureutils.structure.BuriedTreasure;
import kaptainwutax.featureutils.structure.DesertPyramid;
import kaptainwutax.featureutils.structure.RuinedPortal;
import kaptainwutax.featureutils.structure.Shipwreck;
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
        return "26.1.2".equals(t) || "1.21.11".equals(t) || "4790".equals(t);
    }

    private static String encodeStructure(String structure, String versionToken, long seed, int blockX, int blockZ)
        throws Exception {
        if (!isExact2612(versionToken)) {
            return "OK\tSTRUCTURE\tSKIP";
        }
        Boolean result = VanillaWorldgen2612.hasStructure(structure, seed, blockX, blockZ);
        if (result == null) {
            return "OK\tSTRUCTURE\tSKIP";
        }
        return "OK\tSTRUCTURE\t" + (result ? "1" : "0");
    }

    private static class StructureConfig {
        final ILoot loot;

        StructureConfig(ILoot loot) {
            this.loot = loot;
        }
    }

    private record SlotEncoding(String slots, String counts) {
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
            String[] parts = line.split("\t");
            if (parts.length != 6 || (
                !"ROLL".equals(parts[0]) && !"ROLL_DETAIL".equals(parts[0]) && !"STRUCTURE".equals(parts[0])
            )) {
                out.println(
                    "ERR\tInvalid command. Expected: ROLL<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z or " +
                    "ROLL_DETAIL<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z or " +
                    "STRUCTURE<TAB>structure<TAB>version<TAB>seed<TAB>x<TAB>z"
                );
                continue;
            }
            try {
                String command = parts[0].trim();
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
                        out.println("ERR\tUnsupported exact 26.1.2 world-seed loot validation: " + structure);
                        continue;
                    }
                    long seed = Long.parseLong(parts[3].trim());
                    int blockX = Integer.parseInt(parts[4].trim());
                    int blockZ = Integer.parseInt(parts[5].trim());
                    if ("ROLL_DETAIL".equals(command)) {
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
                long seed = Long.parseLong(parts[3].trim());
                int blockX = Integer.parseInt(parts[4].trim());
                int blockZ = Integer.parseInt(parts[5].trim());

                StructureConfig cfg = resolveStructureConfig(structure, version);
                if (cfg == null) {
                    out.println("ERR\tUnsupported structure for loot validation: " + structure);
                    continue;
                }
                if ("ROLL_DETAIL".equals(command)) {
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
