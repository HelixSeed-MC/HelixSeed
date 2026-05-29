package GPULootSeedFinder.loot12111;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.io.Reader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Random;
import java.util.Set;

// Uses local 26.1.2 vanilla loot JSONs when available, with baked tables as a fallback.
// 26.1.1 is supported as an alias for 26.1.2 because Mojang did not alter any
// vanilla chest loot tables between those patch releases.
public final class ExactLootTableLibrary {
    public static final String TARGET_VERSION = "26.1.2";
    public static final String COMPATIBLE_VERSION = "26.1.1";
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

    private interface LootFunctionSpec {
        void apply(MutableStack stack, Random random);
    }

    private record SetCountFunction(NumberSpec count, boolean add) implements LootFunctionSpec {
        @Override
        public void apply(MutableStack stack, Random random) {
            int value = Math.max(0, this.count.getInt(random));
            stack.count = this.add ? Math.max(0, stack.count + value) : value;
        }
    }

    private record SetDamageFunction(NumberSpec damage) implements LootFunctionSpec {
        @Override
        public void apply(MutableStack stack, Random random) {
            if (isDamageableItem(stack.itemId)) {
                this.damage.getFloat(random);
            }
        }
    }

    private record SetStewEffectFunction(List<NumberSpec> durations) implements LootFunctionSpec {
        @Override
        public void apply(MutableStack stack, Random random) {
            if (!"minecraft:suspicious_stew".equals(stack.itemId) || this.durations.isEmpty()) {
                return;
            }
            NumberSpec duration = this.durations.get(random.nextInt(this.durations.size()));
            duration.getInt(random);
        }
    }

    private record EnchantRandomlyFunction(String options, boolean onlyCompatible) implements LootFunctionSpec {
        @Override
        public void apply(MutableStack stack, Random random) {
            int optionCount = enchantmentOptionCount(this.options, stack.itemId, this.onlyCompatible);
            if (optionCount <= 0) {
                return;
            }
            random.nextInt(optionCount);
            // Vanilla then chooses a level between the selected enchantment's min/max.
            // We consume one bounded draw for non-trivial enchantment level ranges.
            random.nextInt(5);
            if ("minecraft:book".equals(stack.itemId)) {
                stack.itemId = "minecraft:enchanted_book";
            }
        }
    }

    private record EnchantWithLevelsFunction(NumberSpec levels, String options) implements LootFunctionSpec {
        @Override
        public void apply(MutableStack stack, Random random) {
            int enchantability = enchantability(stack.itemId);
            int level = Math.max(0, this.levels.getInt(random));
            if (enchantability <= 0 || level <= 0) {
                if ("minecraft:book".equals(stack.itemId)) {
                    stack.itemId = "minecraft:enchanted_book";
                }
                return;
            }
            level += 1 + random.nextInt(enchantability / 4 + 1) + random.nextInt(enchantability / 4 + 1);
            random.nextFloat();
            random.nextFloat();
            int optionCount = Math.max(1, enchantmentOptionCount(this.options, stack.itemId, true));
            random.nextInt(optionCount);
            while (random.nextInt(50) <= level) {
                optionCount = Math.max(1, optionCount / 2);
                random.nextInt(optionCount);
                level /= 2;
            }
            if ("minecraft:book".equals(stack.itemId)) {
                stack.itemId = "minecraft:enchanted_book";
            }
        }
    }

    private record NoopFunction() implements LootFunctionSpec {
        @Override
        public void apply(MutableStack stack, Random random) {
        }
    }

    private record EntrySpec(
        String itemId,
        String tableRef,
        int weight,
        NumberSpec count,
        boolean empty,
        List<LootFunctionSpec> functions
    ) {
        List<MutableStack> createStacks(Random random) {
            if (this.empty) {
                return List.of();
            }
            if (this.tableRef != null && !this.tableRef.isBlank()) {
                TableSpec child = TABLES.get(normalizeTableId(this.tableRef));
                if (child == null) {
                    return List.of();
                }
                List<MutableStack> stacks = generateRawItems(child, random);
                for (MutableStack stack : stacks) {
                    for (LootFunctionSpec function : this.functions) {
                        function.apply(stack, random);
                    }
                }
                return stacks;
            }
            int stackCount = this.count == null ? 1 : Math.max(0, this.count.getInt(random));
            MutableStack stack = new MutableStack(this.itemId, stackCount);
            for (LootFunctionSpec function : this.functions) {
                function.apply(stack, random);
            }
            return List.of(stack);
        }
    }

    private record PoolSpec(NumberSpec rolls, NumberSpec bonusRolls, List<LootFunctionSpec> functions, EntrySpec[] entries) {
    }

    private record TableSpec(PoolSpec[] pools, List<LootFunctionSpec> functions) {
    }

    private static final class MutableStack {
        private String itemId;
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

    private static final String LOOT_ROOT_PROPERTY = "helixseed.mc2612.lootRoot";
    private static final String DECOMPILED_ROOT_PROPERTY = "helixseed.mc2612.decompiledRoot";
    private static final String DATA_ROOT_PROPERTY = "helixseed.mc2612.dataRoot";
    private static final String[] VANILLA_TABLE_IDS = {
        "chests/abandoned_mineshaft",
        "chests/ancient_city",
        "chests/ancient_city_ice_box",
        "chests/bastion_bridge",
        "chests/bastion_hoglin_stable",
        "chests/bastion_other",
        "chests/bastion_treasure",
        "chests/buried_treasure",
        "chests/desert_pyramid",
        "chests/end_city_treasure",
        "chests/igloo_chest",
        "chests/jungle_temple",
        "chests/jungle_temple_dispenser",
        "chests/nether_bridge",
        "chests/pillager_outpost",
        "chests/ruined_portal",
        "chests/shipwreck_map",
        "chests/shipwreck_supply",
        "chests/shipwreck_treasure",
        "chests/simple_dungeon",
        "chests/spawn_bonus_chest",
        "chests/stronghold_corridor",
        "chests/stronghold_crossing",
        "chests/stronghold_library",
        "chests/trial_chambers/corridor",
        "chests/trial_chambers/entrance",
        "chests/trial_chambers/intersection",
        "chests/trial_chambers/intersection_barrel",
        "chests/trial_chambers/reward",
        "chests/trial_chambers/reward_common",
        "chests/trial_chambers/reward_ominous",
        "chests/trial_chambers/reward_ominous_common",
        "chests/trial_chambers/reward_ominous_rare",
        "chests/trial_chambers/reward_ominous_unique",
        "chests/trial_chambers/reward_rare",
        "chests/trial_chambers/reward_unique",
        "chests/trial_chambers/supply",
        "chests/underwater_ruin_big",
        "chests/underwater_ruin_small",
        "chests/village/village_armorer",
        "chests/village/village_butcher",
        "chests/village/village_cartographer",
        "chests/village/village_desert_house",
        "chests/village/village_fisher",
        "chests/village/village_fletcher",
        "chests/village/village_mason",
        "chests/village/village_plains_house",
        "chests/village/village_savanna_house",
        "chests/village/village_shepherd",
        "chests/village/village_snowy_house",
        "chests/village/village_taiga_house",
        "chests/village/village_tannery",
        "chests/village/village_temple",
        "chests/village/village_toolsmith",
        "chests/village/village_weaponsmith",
        "chests/woodland_mansion"
    };
    private static Path vanillaDataRoot;
    private static final Map<String, Set<String>> TAG_CACHE = new HashMap<>();

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
            throw new IllegalArgumentException("Unsupported exact 26.1.1/26.1.2 loot table: " + tableId);
        }
        Random random = new Random(lootSeed);
        List<MutableStack> rawItems = generateRawItems(table, random);
        return fillContainer(rawItems, slotCount, random);
    }

    public static Map<String, Integer> rollCountsOnly(String tableId, long lootSeed) {
        return rollCountsOnly(tableId, lootSeed, false);
    }

    public static Map<String, Integer> rollNormalizedCountsOnly(String tableId, long lootSeed) {
        return rollCountsOnly(tableId, lootSeed, true);
    }

    private static Map<String, Integer> rollCountsOnly(String tableId, long lootSeed, boolean normalizeIds) {
        String normalized = normalizeTableId(tableId);
        TableSpec table = TABLES.get(normalized);
        if (table == null) {
            throw new IllegalArgumentException("Unsupported exact 26.1.1/26.1.2 loot table: " + tableId);
        }
        Random random = new Random(lootSeed);
        Map<String, Integer> counts = new LinkedHashMap<>();
        for (MutableStack stack : generateRawItems(table, random)) {
            if (stack == null || stack.count <= 0 || stack.itemId == null || stack.itemId.isBlank()) {
                continue;
            }
            String itemId = normalizeIds ? normalizeResourceId(stack.itemId) : stack.itemId;
            counts.put(itemId, counts.getOrDefault(itemId, 0) + stack.count);
        }
        return Collections.unmodifiableMap(counts);
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
            int rolls = pool.rolls().getInt(random) + (int) Math.floor(pool.bonusRolls().getFloat(random) * 0.0f);
            for (int i = 0; i < rolls; i++) {
                EntrySpec entry = pickEntry(pool.entries(), random);
                if (entry == null || entry.empty()) {
                    continue;
                }
                for (MutableStack stack : entry.createStacks(random)) {
                    if (stack.count <= 0) {
                        continue;
                    }
                    for (LootFunctionSpec function : pool.functions()) {
                        function.apply(stack, random);
                    }
                    for (LootFunctionSpec function : table.functions()) {
                        function.apply(stack, random);
                    }
                    if (stack.count > 0) {
                        out.add(stack);
                    }
                }
            }
        }
        return out;
    }

    private static EntrySpec pickEntry(EntrySpec[] entries, Random random) {
        int totalWeight = 0;
        int validEntryCount = 0;
        EntrySpec onlyValidEntry = null;
        for (EntrySpec entry : entries) {
            int weight = Math.max(0, entry.weight());
            if (weight > 0) {
                validEntryCount++;
                onlyValidEntry = entry;
                totalWeight += weight;
            }
        }
        if (totalWeight <= 0) {
            return null;
        }
        if (validEntryCount == 1) {
            return onlyValidEntry;
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

    private static String normalizeResourceId(String id) {
        String normalized = id == null ? "" : id.trim();
        if (normalized.startsWith("minecraft:")) {
            normalized = normalized.substring("minecraft:".length());
        }
        return normalized;
    }

    private static Map<String, TableSpec> loadVanillaJsonTables() {
        Optional<Path> root = resolveVanillaLootTableRoot();
        if (root.isEmpty()) {
            return Map.of();
        }

        Map<String, TableSpec> tables = new HashMap<>();
        for (String tableId : VANILLA_TABLE_IDS) {
            Path file = root.get().resolve(tableId + ".json");
            if (!Files.isRegularFile(file)) {
                return Map.of();
            }
            try (Reader reader = Files.newBufferedReader(file, StandardCharsets.UTF_8)) {
                JsonObject json = JsonParser.parseReader(reader).getAsJsonObject();
                tables.put(tableId, parseTable(json));
            } catch (Exception ex) {
                return Map.of();
            }
        }
        return tables;
    }

    private static Optional<Path> resolveVanillaLootTableRoot() {
        List<Path> candidates = new ArrayList<>();
        addConfiguredPath(candidates, System.getProperty(LOOT_ROOT_PROPERTY), true);
        addConfiguredPath(candidates, System.getProperty(DATA_ROOT_PROPERTY), false);
        addConfiguredPath(candidates, System.getProperty(DECOMPILED_ROOT_PROPERTY), false);
        addConfiguredPath(candidates, Path.of(System.getProperty("user.home"), "Desktop", "Minecraft-Decompiled-26.1.2").toString(), false);
        addConfiguredPath(candidates, Path.of(System.getProperty("user.home"), "Desktop", "Minecraft-Decompiled").toString(), false);

        for (Path candidate : candidates) {
            Path lootRoot = normalizeLootRootCandidate(candidate);
            if (Files.isRegularFile(lootRoot.resolve("chests/buried_treasure.json"))) {
                vanillaDataRoot = lootRoot.getParent();
                return Optional.of(lootRoot);
            }
        }
        return Optional.empty();
    }

    private static void addConfiguredPath(List<Path> out, String raw, boolean directLootRoot) {
        if (raw == null || raw.isBlank()) {
            return;
        }
        Path path = Path.of(raw.trim());
        out.add(directLootRoot ? path : normalizeLootRootCandidate(path));
    }

    private static Path normalizeLootRootCandidate(Path path) {
        if (path.endsWith(Path.of("loot_table"))) {
            return path;
        }
        if (path.endsWith(Path.of("minecraft"))) {
            return path.resolve("loot_table");
        }
        if (path.endsWith(Path.of("data"))) {
            return path.resolve("minecraft").resolve("loot_table");
        }
        Path resourcesData = path.resolve("resources").resolve("data").resolve("minecraft").resolve("loot_table");
        if (Files.isDirectory(resourcesData)) {
            return resourcesData;
        }
        Path srcData = path.resolve("src").resolve("data").resolve("minecraft").resolve("loot_table");
        if (Files.isDirectory(srcData)) {
            return srcData;
        }
        return path.resolve("data").resolve("minecraft").resolve("loot_table");
    }

    private static TableSpec parseTable(JsonObject json) {
        List<PoolSpec> pools = new ArrayList<>();
        JsonArray poolArray = json.has("pools") && json.get("pools").isJsonArray()
            ? json.getAsJsonArray("pools")
            : new JsonArray();
        for (JsonElement element : poolArray) {
            if (element.isJsonObject()) {
                pools.add(parsePool(element.getAsJsonObject()));
            }
        }
        return new TableSpec(pools.toArray(new PoolSpec[0]), parseFunctions(json));
    }

    private static PoolSpec parsePool(JsonObject json) {
        NumberSpec rolls = json.has("rolls") ? parseNumber(json.get("rolls")) : n(1);
        NumberSpec bonusRolls = json.has("bonus_rolls") ? parseNumber(json.get("bonus_rolls")) : n(0);
        List<EntrySpec> entries = new ArrayList<>();
        JsonArray entryArray = json.has("entries") && json.get("entries").isJsonArray()
            ? json.getAsJsonArray("entries")
            : new JsonArray();
        for (JsonElement element : entryArray) {
            if (element.isJsonObject()) {
                EntrySpec entry = parseEntry(element.getAsJsonObject());
                if (entry != null) {
                    entries.add(entry);
                }
            }
        }
        return new PoolSpec(rolls, bonusRolls, parseFunctions(json), entries.toArray(new EntrySpec[0]));
    }

    private static EntrySpec parseEntry(JsonObject json) {
        String type = normalizeResourceId(getString(json, "type", "minecraft:item"));
        int weight = getInt(json, "weight", 1);
        if ("empty".equals(type)) {
            return new EntrySpec("", "", weight, null, true, List.of());
        }
        if ("loot_table".equals(type)) {
            String tableId = getString(json, "value", "");
            if (tableId.isBlank()) {
                return null;
            }
            return new EntrySpec("", tableId, weight, null, false, parseFunctions(json));
        }
        if (!"item".equals(type)) {
            return null;
        }
        String itemId = getString(json, "name", "");
        if (itemId.isBlank()) {
            return null;
        }
        return new EntrySpec(itemId, "", weight, null, false, parseFunctions(json));
    }

    private static List<LootFunctionSpec> parseFunctions(JsonObject json) {
        if (!json.has("functions") || !json.get("functions").isJsonArray()) {
            return List.of();
        }
        List<LootFunctionSpec> functions = new ArrayList<>();
        for (JsonElement element : json.getAsJsonArray("functions")) {
            if (element.isJsonObject()) {
                functions.add(parseFunction(element.getAsJsonObject()));
            }
        }
        return List.copyOf(functions);
    }

    private static LootFunctionSpec parseFunction(JsonObject json) {
        String type = normalizeResourceId(getString(json, "function", ""));
        return switch (type) {
            case "set_count" -> new SetCountFunction(
                json.has("count") ? parseNumber(json.get("count")) : n(1),
                getBoolean(json, "add", false)
            );
            case "set_damage" -> new SetDamageFunction(json.has("damage") ? parseNumber(json.get("damage")) : n(0));
            case "set_stew_effect" -> new SetStewEffectFunction(parseStewDurations(json));
            case "enchant_randomly" -> new EnchantRandomlyFunction(getString(json, "options", ""), getBoolean(json, "only_compatible", true));
            case "enchant_with_levels" -> new EnchantWithLevelsFunction(
                json.has("levels") ? parseNumber(json.get("levels")) : n(1),
                getString(json, "options", "")
            );
            default -> new NoopFunction();
        };
    }

    private static List<NumberSpec> parseStewDurations(JsonObject json) {
        if (!json.has("effects") || !json.get("effects").isJsonArray()) {
            return List.of();
        }
        List<NumberSpec> durations = new ArrayList<>();
        for (JsonElement element : json.getAsJsonArray("effects")) {
            if (!element.isJsonObject()) {
                continue;
            }
            JsonObject effect = element.getAsJsonObject();
            durations.add(effect.has("duration") ? parseNumber(effect.get("duration")) : n(0));
        }
        return List.copyOf(durations);
    }

    private static NumberSpec parseNumber(JsonElement element) {
        if (element == null || element.isJsonNull()) {
            return n(0);
        }
        if (element.isJsonPrimitive() && element.getAsJsonPrimitive().isNumber()) {
            return new ConstantNumber(element.getAsFloat());
        }
        if (!element.isJsonObject()) {
            return n(0);
        }
        JsonObject json = element.getAsJsonObject();
        String type = normalizeResourceId(getString(json, "type", "minecraft:constant"));
        if ("uniform".equals(type)) {
            return new UniformNumber(
                json.has("min") ? parseNumber(json.get("min")) : n(0),
                json.has("max") ? parseNumber(json.get("max")) : n(0)
            );
        }
        if (json.has("value")) {
            return parseNumber(json.get("value"));
        }
        return n(0);
    }

    private static String getString(JsonObject json, String key, String fallback) {
        return json.has(key) && json.get(key).isJsonPrimitive()
            ? json.get(key).getAsString()
            : fallback;
    }

    private static int getInt(JsonObject json, String key, int fallback) {
        return json.has(key) && json.get(key).isJsonPrimitive()
            ? json.get(key).getAsInt()
            : fallback;
    }

    private static boolean getBoolean(JsonObject json, String key, boolean fallback) {
        return json.has(key) && json.get(key).isJsonPrimitive()
            ? json.get(key).getAsBoolean()
            : fallback;
    }

    private static int enchantmentOptionCount(String optionId, String itemId, boolean onlyCompatible) {
        Set<String> values = resolveEnchantmentOptions(optionId);
        if (values.isEmpty()) {
            values = resolveEnchantmentOptions("#minecraft:on_random_loot");
        }
        if (!onlyCompatible || "minecraft:book".equals(itemId)) {
            return Math.max(1, values.size());
        }
        return Math.max(1, values.size() / 2);
    }

    private static Set<String> resolveEnchantmentOptions(String optionId) {
        String normalized = optionId == null || optionId.isBlank() ? "#minecraft:on_random_loot" : optionId.trim();
        if (normalized.startsWith("#")) {
            String tagId = normalizeResourceId(normalized.substring(1));
            return resolveTag("enchantment", tagId, new HashSet<>());
        }
        return Set.of(normalized);
    }

    private static Set<String> resolveTag(String type, String tagId, Set<String> visiting) {
        String normalized = normalizeResourceId(tagId);
        String cacheKey = type + "/" + normalized;
        if (TAG_CACHE.containsKey(cacheKey)) {
            return TAG_CACHE.get(cacheKey);
        }
        if (vanillaDataRoot == null || !visiting.add(cacheKey)) {
            return Set.of();
        }
        Path file = vanillaDataRoot.resolve("tags").resolve(type).resolve(normalized + ".json");
        if (!Files.isRegularFile(file)) {
            return Set.of();
        }
        try (Reader reader = Files.newBufferedReader(file, StandardCharsets.UTF_8)) {
            JsonObject json = JsonParser.parseReader(reader).getAsJsonObject();
            Set<String> out = new HashSet<>();
            if (json.has("values") && json.get("values").isJsonArray()) {
                for (JsonElement element : json.getAsJsonArray("values")) {
                    if (!element.isJsonPrimitive()) {
                        continue;
                    }
                    String value = element.getAsString();
                    if (value.startsWith("#")) {
                        out.addAll(resolveTag(type, value.substring(1), visiting));
                    } else {
                        out.add(value);
                    }
                }
            }
            Set<String> frozen = Set.copyOf(out);
            TAG_CACHE.put(cacheKey, frozen);
            return frozen;
        } catch (Exception ex) {
            return Set.of();
        } finally {
            visiting.remove(cacheKey);
        }
    }

    private static boolean isDamageableItem(String itemId) {
        String item = normalizeResourceId(itemId);
        return item.endsWith("_sword")
            || item.endsWith("_axe")
            || item.endsWith("_pickaxe")
            || item.endsWith("_shovel")
            || item.endsWith("_hoe")
            || item.endsWith("_helmet")
            || item.endsWith("_chestplate")
            || item.endsWith("_leggings")
            || item.endsWith("_boots")
            || item.equals("bow")
            || item.equals("crossbow")
            || item.equals("trident")
            || item.equals("shield")
            || item.equals("fishing_rod")
            || item.equals("flint_and_steel");
    }

    private static int enchantability(String itemId) {
        String item = normalizeResourceId(itemId);
        if (item.equals("book")) {
            return 1;
        }
        if (item.startsWith("golden_")) {
            return 22;
        }
        if (item.startsWith("diamond_")) {
            return 10;
        }
        if (item.startsWith("iron_")) {
            return 14;
        }
        if (item.startsWith("leather_")) {
            return 15;
        }
        if (item.startsWith("copper_")) {
            return 14;
        }
        if (item.equals("bow") || item.equals("crossbow")) {
            return 1;
        }
        return isDamageableItem(itemId) ? 10 : 0;
    }

    private static Map<String, TableSpec> createTables() {
        Map<String, TableSpec> vanillaTables = loadVanillaJsonTables();
        if (!vanillaTables.isEmpty()) {
            return Collections.unmodifiableMap(vanillaTables);
        }

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
        return new TableSpec(pools, List.of());
    }

    private static PoolSpec p(NumberSpec rolls, EntrySpec... entries) {
        return new PoolSpec(rolls, n(0), List.of(), entries);
    }

    private static ConstantNumber n(int value) {
        return new ConstantNumber(value);
    }

    private static UniformNumber u(NumberSpec min, NumberSpec max) {
        return new UniformNumber(min, max);
    }

    private static EntrySpec i(String itemId, int weight) {
        return new EntrySpec(itemId, "", weight, null, false, List.of());
    }

    private static EntrySpec i(String itemId, int weight, NumberSpec count) {
        return new EntrySpec(itemId, "", weight, count, false, List.of());
    }

    private static EntrySpec e(int weight) {
        return new EntrySpec("", "", weight, null, true, List.of());
    }
}
