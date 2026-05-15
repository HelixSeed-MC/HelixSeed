package GPULootSeedFinder.loot12111;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;

// Generated from the local 26.1.2 jar loot tables on 2026-03-15.
public final class ExactLootTableLibrary {
    public static final String TARGET_VERSION = "26.1.2";
    public static final int CHEST_SLOT_COUNT = 27;

    public record ItemStackView(String itemId, int count) {
        public static ItemStackView empty() {
            return new ItemStackView("", 0);
        }

        public boolean isEmpty() {
            return itemId == null || itemId.isBlank() || count <= 0;
        }
    }

    public record LootRollResult(List<ItemStackView> slots, Map<String, Integer> counts) {
    }

    private interface NumberSpec {
        float getFloat(Random random);

        default int getInt(Random random) {
            return Math.round(this.getFloat(random));
        }
    }

    private record ConstantNumber(float value) implements NumberSpec {
        @Override
        public float getFloat(Random random) {
            return value;
        }
    }

    private record UniformNumber(NumberSpec min, NumberSpec max) implements NumberSpec {
        @Override
        public float getFloat(Random random) {
            float lo = this.min.getFloat(random);
            float hi = this.max.getFloat(random);
            if (lo >= hi) {
                return lo;
            }
            return random.nextFloat() * (hi - lo) + lo;
        }

        @Override
        public int getInt(Random random) {
            int lo = this.min.getInt(random);
            int hi = this.max.getInt(random);
            if (lo >= hi) {
                return lo;
            }
            return random.nextInt(hi - lo + 1) + lo;
        }
    }

    private record EntrySpec(String itemId, int weight, NumberSpec count, boolean empty) {
        int count(Random random) {
            return this.count == null ? 1 : Math.max(0, this.count.getInt(random));
        }
    }

    private record PoolSpec(NumberSpec rolls, EntrySpec[] entries) {
    }

    private record TableSpec(PoolSpec[] pools) {
    }

    private static final class MutableStack {
        private final String itemId;
        private int count;

        private MutableStack(String itemId, int count) {
            this.itemId = itemId;
            this.count = count;
        }

        private MutableStack split(int amount) {
            int taken = Math.min(amount, this.count);
            this.count -= taken;
            return new MutableStack(this.itemId, taken);
        }

        private ItemStackView freeze() {
            return new ItemStackView(this.itemId, this.count);
        }
    }

    private static final Map<String, TableSpec> TABLES = createTables();

    private ExactLootTableLibrary() {
    }

    public static boolean supports(String tableId) {
        return TABLES.containsKey(normalizeTableId(tableId));
    }

    public static LootRollResult rollTable(String tableId, long lootSeed) {
        return rollTable(tableId, lootSeed, CHEST_SLOT_COUNT);
    }

    public static LootRollResult rollTable(String tableId, long lootSeed, int slotCount) {
        String normalized = normalizeTableId(tableId);
        TableSpec table = TABLES.get(normalized);
        if (table == null) {
            throw new IllegalArgumentException("Unsupported exact 26.1.2 loot table: " + tableId);
        }
        Random random = new Random(lootSeed);
        List<MutableStack> rawItems = generateRawItems(table, random);
        return fillContainer(rawItems, slotCount, random);
    }

    private static LootRollResult fillContainer(List<MutableStack> rawItems, int slotCount, Random random) {
        List<MutableStack> items = new ArrayList<>();
        for (MutableStack rawItem : rawItems) {
            if (rawItem.count <= 0) {
                continue;
            }
            int remaining = rawItem.count;
            while (remaining > 64) {
                items.add(new MutableStack(rawItem.itemId, 64));
                remaining -= 64;
            }
            if (remaining > 0) {
                items.add(new MutableStack(rawItem.itemId, remaining));
            }
        }

        List<Integer> availableSlots = new ArrayList<>();
        for (int i = 0; i < slotCount; i++) {
            availableSlots.add(i);
        }
        shuffle(availableSlots, random);
        shuffleAndSplitItems(items, availableSlots.size(), random);

        List<ItemStackView> slots = new ArrayList<>(Collections.nCopies(slotCount, ItemStackView.empty()));
        for (MutableStack item : items) {
            if (availableSlots.isEmpty()) {
                break;
            }
            int slot = availableSlots.remove(availableSlots.size() - 1);
            slots.set(slot, item.freeze());
        }

        Map<String, Integer> counts = new LinkedHashMap<>();
        for (ItemStackView slot : slots) {
            if (slot.isEmpty()) {
                continue;
            }
            counts.put(slot.itemId(), counts.getOrDefault(slot.itemId(), 0) + slot.count());
        }
        return new LootRollResult(List.copyOf(slots), Collections.unmodifiableMap(counts));
    }

    private static void shuffleAndSplitItems(List<MutableStack> items, int availableSlots, Random random) {
        List<MutableStack> oversized = new ArrayList<>();
        for (int i = items.size() - 1; i >= 0; i--) {
            MutableStack item = items.get(i);
            if (item.count <= 1) {
                continue;
            }
            oversized.add(item);
            items.remove(i);
        }

        while (availableSlots - items.size() - oversized.size() > 0 && !oversized.isEmpty()) {
            MutableStack source = oversized.remove(random.nextInt(oversized.size()));
            int splitCount = 1 + random.nextInt(Math.max(1, source.count / 2));
            MutableStack split = source.split(splitCount);
            if (source.count > 1 && random.nextBoolean()) {
                oversized.add(source);
            } else if (source.count > 0) {
                items.add(source);
            }
            if (split.count > 1 && random.nextBoolean()) {
                oversized.add(split);
            } else if (split.count > 0) {
                items.add(split);
            }
        }

        items.addAll(oversized);
        shuffle(items, random);
    }

    private static List<MutableStack> generateRawItems(TableSpec table, Random random) {
        List<MutableStack> out = new ArrayList<>();
        for (PoolSpec pool : table.pools()) {
            int rolls = pool.rolls().getInt(random);
            for (int i = 0; i < rolls; i++) {
                EntrySpec entry = pickEntry(pool.entries(), random);
                if (entry == null || entry.empty()) {
                    continue;
                }
                int count = entry.count(random);
                if (count <= 0) {
                    continue;
                }
                out.add(new MutableStack(entry.itemId(), count));
            }
        }
        return out;
    }

    private static EntrySpec pickEntry(EntrySpec[] entries, Random random) {
        int totalWeight = 0;
        for (EntrySpec entry : entries) {
            totalWeight += Math.max(0, entry.weight());
        }
        if (totalWeight <= 0) {
            return null;
        }
        int value = random.nextInt(totalWeight);
        for (EntrySpec entry : entries) {
            value -= Math.max(0, entry.weight());
            if (value < 0) {
                return entry;
            }
        }
        return null;
    }

    private static <T> void shuffle(List<T> list, Random random) {
        for (int i = list.size(); i > 1; i--) {
            int j = random.nextInt(i);
            T temp = list.get(i - 1);
            list.set(i - 1, list.get(j));
            list.set(j, temp);
        }
    }

    private static String normalizeTableId(String tableId) {
        String normalized = tableId == null ? "" : tableId.trim();
        if (normalized.startsWith("minecraft:")) {
            normalized = normalized.substring("minecraft:".length());
        }
        if (normalized.startsWith("data/minecraft/loot_table/")) {
            normalized = normalized.substring("data/minecraft/loot_table/".length());
        }
        if (normalized.endsWith(".json")) {
            normalized = normalized.substring(0, normalized.length() - 5);
        }
        return normalized;
    }

    private static Map<String, TableSpec> createTables() {
        Map<String, TableSpec> tables = new HashMap<>();

        tables.put("chests/ruined_portal", t(
            p(u(n(4), n(8)), i("minecraft:obsidian", 40, u(n(1), n(2))), i("minecraft:flint", 40, u(n(1), n(4))), i("minecraft:iron_nugget", 40, u(n(9), n(18))), i("minecraft:flint_and_steel", 40), i("minecraft:fire_charge", 40), i("minecraft:golden_apple", 15), i("minecraft:gold_nugget", 15, u(n(4), n(24))), i("minecraft:golden_sword", 15), i("minecraft:golden_axe", 15), i("minecraft:golden_hoe", 15), i("minecraft:golden_shovel", 15), i("minecraft:golden_pickaxe", 15), i("minecraft:golden_boots", 15), i("minecraft:golden_chestplate", 15), i("minecraft:golden_helmet", 15), i("minecraft:golden_leggings", 15), i("minecraft:glistering_melon_slice", 5, u(n(4), n(12))), i("minecraft:golden_horse_armor", 5), i("minecraft:light_weighted_pressure_plate", 5), i("minecraft:golden_carrot", 5, u(n(4), n(12))), i("minecraft:clock", 5), i("minecraft:gold_ingot", 5, u(n(2), n(8))), i("minecraft:bell", 1), i("minecraft:enchanted_golden_apple", 1), i("minecraft:gold_block", 1, u(n(1), n(2)))),
            p(n(1), e(1), i("minecraft:lodestone", 2, u(n(1), n(2))))
        ));

        tables.put("chests/shipwreck_supply", t(
            p(u(n(3), n(10)), i("minecraft:paper", 8, u(n(1), n(12))), i("minecraft:potato", 7, u(n(2), n(6))), i("minecraft:moss_block", 7, u(n(1), n(4))), i("minecraft:poisonous_potato", 7, u(n(2), n(6))), i("minecraft:carrot", 7, u(n(4), n(8))), i("minecraft:wheat", 7, u(n(8), n(21))), i("minecraft:suspicious_stew", 10), i("minecraft:coal", 6, u(n(2), n(8))), i("minecraft:rotten_flesh", 5, u(n(5), n(24))), i("minecraft:pumpkin", 2, u(n(1), n(3))), i("minecraft:bamboo", 2, u(n(1), n(3))), i("minecraft:gunpowder", 3, u(n(1), n(5))), i("minecraft:tnt", 1, u(n(1), n(2))), i("minecraft:leather_helmet", 3), i("minecraft:leather_chestplate", 3), i("minecraft:leather_leggings", 3), i("minecraft:leather_boots", 3)),
            p(n(1), e(5), i("minecraft:coast_armor_trim_smithing_template", 1, n(2))),
            p(n(1), e(148), i("minecraft:copper_nautilus_armor", 20, n(1)), i("minecraft:iron_nautilus_armor", 10, n(1)), i("minecraft:golden_nautilus_armor", 5, n(1)), i("minecraft:diamond_nautilus_armor", 2, n(1)))
        ));

        tables.put("chests/shipwreck_treasure", t(
            p(u(n(3), n(6)), i("minecraft:iron_ingot", 90, u(n(1), n(5))), i("minecraft:gold_ingot", 10, u(n(1), n(5))), i("minecraft:emerald", 40, u(n(1), n(5))), i("minecraft:diamond", 5), i("minecraft:experience_bottle", 5)),
            p(u(n(2), n(5)), i("minecraft:iron_nugget", 50, u(n(1), n(10))), i("minecraft:gold_nugget", 10, u(n(1), n(10))), i("minecraft:lapis_lazuli", 20, u(n(1), n(10)))),
            p(n(1), e(5), i("minecraft:coast_armor_trim_smithing_template", 1, n(2))),
            p(n(1), e(148), i("minecraft:copper_nautilus_armor", 20, n(1)), i("minecraft:iron_nautilus_armor", 10, n(1)), i("minecraft:golden_nautilus_armor", 5, n(1)), i("minecraft:diamond_nautilus_armor", 2, n(1)))
        ));

        tables.put("chests/shipwreck_map", t(
            p(n(1), i("minecraft:map", 1)),
            p(n(3), i("minecraft:compass", 1), i("minecraft:map", 1), i("minecraft:clock", 1), i("minecraft:paper", 20, u(n(1), n(10))), i("minecraft:feather", 10, u(n(1), n(5))), i("minecraft:book", 5, u(n(1), n(5)))),
            p(n(1), e(5), i("minecraft:coast_armor_trim_smithing_template", 1, n(2))),
            p(n(1), e(148), i("minecraft:copper_nautilus_armor", 20, n(1)), i("minecraft:iron_nautilus_armor", 10, n(1)), i("minecraft:golden_nautilus_armor", 5, n(1)), i("minecraft:diamond_nautilus_armor", 2, n(1)))
        ));

        tables.put("chests/buried_treasure", t(
            p(n(1), i("minecraft:heart_of_the_sea", 1)),
            p(u(n(5), n(8)), i("minecraft:iron_ingot", 20, u(n(1), n(4))), i("minecraft:gold_ingot", 10, u(n(1), n(4))), i("minecraft:tnt", 5, u(n(1), n(2)))),
            p(u(n(1), n(3)), i("minecraft:emerald", 5, u(n(4), n(8))), i("minecraft:diamond", 5, u(n(1), n(2))), i("minecraft:prismarine_crystals", 5, u(n(1), n(5)))),
            p(u(n(0), n(1)), i("minecraft:leather_chestplate", 1), i("minecraft:iron_sword", 1), i("minecraft:iron_spear", 1)),
            p(n(2), i("minecraft:cooked_cod", 1, u(n(2), n(4))), i("minecraft:cooked_salmon", 1, u(n(2), n(4)))),
            p(u(n(0), n(2)), i("minecraft:potion", 1)),
            p(n(1), e(148), i("minecraft:copper_nautilus_armor", 20, n(1)), i("minecraft:iron_nautilus_armor", 10, n(1)), i("minecraft:golden_nautilus_armor", 5, n(1)), i("minecraft:diamond_nautilus_armor", 2, n(1)))
        ));

        tables.put("chests/desert_pyramid", t(
            p(u(n(2), n(4)), i("minecraft:diamond", 5, u(n(1), n(3))), i("minecraft:iron_ingot", 15, u(n(1), n(5))), i("minecraft:gold_ingot", 15, u(n(2), n(7))), i("minecraft:emerald", 15, u(n(1), n(3))), i("minecraft:bone", 25, u(n(4), n(6))), i("minecraft:spider_eye", 25, u(n(1), n(3))), i("minecraft:rotten_flesh", 25, u(n(3), n(7))), i("minecraft:leather", 20, u(n(1), n(5))), i("minecraft:copper_horse_armor", 15), i("minecraft:iron_horse_armor", 15), i("minecraft:golden_horse_armor", 10), i("minecraft:diamond_horse_armor", 5), i("minecraft:book", 20), i("minecraft:golden_apple", 20), i("minecraft:enchanted_golden_apple", 2), e(15)),
            p(n(4), i("minecraft:bone", 10, u(n(1), n(8))), i("minecraft:gunpowder", 10, u(n(1), n(8))), i("minecraft:rotten_flesh", 10, u(n(1), n(8))), i("minecraft:string", 10, u(n(1), n(8))), i("minecraft:sand", 10, u(n(1), n(8)))),
            p(n(1), e(6), i("minecraft:dune_armor_trim_smithing_template", 1, n(2)))
        ));

        tables.put("chests/jungle_temple", t(
            p(u(n(2), n(6)), i("minecraft:diamond", 3, u(n(1), n(3))), i("minecraft:iron_ingot", 10, u(n(1), n(5))), i("minecraft:gold_ingot", 15, u(n(2), n(7))), i("minecraft:bamboo", 15, u(n(1), n(3))), i("minecraft:emerald", 2, u(n(1), n(3))), i("minecraft:bone", 20, u(n(4), n(6))), i("minecraft:rotten_flesh", 16, u(n(3), n(7))), i("minecraft:leather", 3, u(n(1), n(5))), i("minecraft:copper_horse_armor", 1), i("minecraft:iron_horse_armor", 1), i("minecraft:golden_horse_armor", 1), i("minecraft:diamond_horse_armor", 1), i("minecraft:book", 1)),
            p(n(1), e(2), i("minecraft:wild_armor_trim_smithing_template", 1, n(2)))
        ));

        return Collections.unmodifiableMap(tables);
    }

    private static TableSpec t(PoolSpec... pools) {
        return new TableSpec(pools);
    }

    private static PoolSpec p(NumberSpec rolls, EntrySpec... entries) {
        return new PoolSpec(rolls, entries);
    }

    private static ConstantNumber n(int value) {
        return new ConstantNumber(value);
    }

    private static UniformNumber u(NumberSpec min, NumberSpec max) {
        return new UniformNumber(min, max);
    }

    private static EntrySpec i(String itemId, int weight) {
        return new EntrySpec(itemId, weight, null, false);
    }

    private static EntrySpec i(String itemId, int weight, NumberSpec count) {
        return new EntrySpec(itemId, weight, count, false);
    }

    private static EntrySpec e(int weight) {
        return new EntrySpec("", weight, null, true);
    }
}
