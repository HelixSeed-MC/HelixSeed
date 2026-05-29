package GPULootSeedFinder.cubiomes2612;

import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class VanillaWorldgen2612 {
    public enum State {
        PASS,
        FAIL,
        SKIP
    }

    public record Result(State state, String detail) {
        public static Result pass(String detail) {
            return new Result(State.PASS, detail);
        }

        public static Result fail(String detail) {
            return new Result(State.FAIL, detail);
        }

        public static Result skip(String detail) {
            return new Result(State.SKIP, detail);
        }
    }

    private record StructureData(
        String id,
        String type,
        String biomeRef,
        Set<String> biomes,
        boolean projectStartToHeightmap,
        String heightmap,
        int startHeight
    ) {
    }

    private record SamplePoint(int x, int y, int z, String reason) {
    }

    private record SeedContext(long seed, Object randomState, Object sampler) {
    }

    private static final Pattern STRING_FIELD = Pattern.compile("\"%s\"\\s*:\\s*\"([^\"]+)\"");
    private static final Pattern ABSOLUTE_HEIGHT = Pattern.compile(
        "\"start_height\"\\s*:\\s*\\{[^}]*\"absolute\"\\s*:\\s*(-?\\d+)",
        Pattern.DOTALL
    );
    private static final Pattern JSON_STRING = Pattern.compile("\"((?:\\\\.|[^\"\\\\])*)\"");
    private static final String DECOMPILED_ROOT_PROPERTY = "helixseed.minecraft2612.decompiled";
    private static final String DECOMPILED_ROOT_ENV = "HELIXSEED_MINECRAFT_2612_DECOMPILED";
    private static final String DEFAULT_DECOMPILED_DIR = "Minecraft-Decompiled-26.1.2";

    private static volatile VanillaWorldgen2612 instance;

    private final Path dataRoot;
    private final Map<String, Optional<StructureData>> structureCache = new HashMap<>();
    private final Map<String, Set<String>> tagCache = new HashMap<>();
    private final Map<Long, Object> randomStateCache = new HashMap<>();

    private final Object lookup;
    private final Object generator;
    private final Object biomeSource;
    private final Object heightAccessor;
    private final Object overworldNoiseSettings;
    private final Object worldSurfaceWg;
    private final Object oceanFloorWg;
    private final Method randomStateCreate;
    private final Method randomStateSampler;
    private final Method generatorGetFirstOccupiedHeight;
    private final Method biomeSourceGetNoiseBiome;
    private final Method holderUnwrapKey;
    private final Method resourceKeyIdentifier;

    private VanillaWorldgen2612() throws ReflectiveOperationException {
        this.dataRoot = resolveDataRoot();

        Class.forName("net.minecraft.SharedConstants").getMethod("tryDetectVersion").invoke(null);
        Class.forName("net.minecraft.server.Bootstrap").getMethod("bootStrap").invoke(null);

        Class<?> holderLookupProviderClass = Class.forName("net.minecraft.core.HolderLookup$Provider");
        Class<?> holderGetterProviderClass = Class.forName("net.minecraft.core.HolderGetter$Provider");
        Class<?> resourceKeyClass = Class.forName("net.minecraft.resources.ResourceKey");
        Class<?> randomStateClass = Class.forName("net.minecraft.world.level.levelgen.RandomState");
        Class<?> climateSamplerClass = Class.forName("net.minecraft.world.level.biome.Climate$Sampler");
        Class<?> heightmapTypesClass = Class.forName("net.minecraft.world.level.levelgen.Heightmap$Types");
        Class<?> levelHeightAccessorClass = Class.forName("net.minecraft.world.level.LevelHeightAccessor");

        this.lookup = Class.forName("net.minecraft.data.registries.VanillaRegistries")
            .getMethod("createLookup")
            .invoke(null);
        Object dimensions = Class.forName("net.minecraft.world.level.levelgen.presets.WorldPresets")
            .getMethod("createNormalWorldDimensions", holderLookupProviderClass)
            .invoke(null, lookup);
        this.generator = dimensions.getClass().getMethod("overworld").invoke(dimensions);
        this.biomeSource = generator.getClass().getMethod("getBiomeSource").invoke(generator);
        this.heightAccessor = levelHeightAccessorClass.getMethod("create", int.class, int.class).invoke(null, -64, 384);
        this.overworldNoiseSettings = Class.forName("net.minecraft.world.level.levelgen.NoiseGeneratorSettings")
            .getField("OVERWORLD")
            .get(null);
        this.worldSurfaceWg = heightmapTypesClass.getField("WORLD_SURFACE_WG").get(null);
        this.oceanFloorWg = heightmapTypesClass.getField("OCEAN_FLOOR_WG").get(null);

        this.randomStateCreate = randomStateClass.getMethod("create", holderGetterProviderClass, resourceKeyClass, long.class);
        this.randomStateSampler = randomStateClass.getMethod("sampler");
        this.generatorGetFirstOccupiedHeight = generator.getClass()
            .getMethod(
                "getFirstOccupiedHeight",
                int.class,
                int.class,
                heightmapTypesClass,
                levelHeightAccessorClass,
                randomStateClass
            );
        Class<?> biomeSourceClass = Class.forName("net.minecraft.world.level.biome.BiomeSource");
        this.biomeSourceGetNoiseBiome = biomeSourceClass.getMethod(
            "getNoiseBiome",
            int.class,
            int.class,
            int.class,
            climateSamplerClass
        );
        this.holderUnwrapKey = Class.forName("net.minecraft.core.Holder").getMethod("unwrapKey");
        this.resourceKeyIdentifier = resourceKeyClass.getMethod("identifier");
    }

    public static Result validateStructure(String structure, long seed, int blockX, int blockZ) {
        try {
            return get().validate(structure, seed, blockX, blockZ);
        } catch (ClassNotFoundException ex) {
            return Result.skip("Minecraft 26.1.1 / 26.1.2 classes are not on the Java runtime classpath.");
        } catch (Throwable ex) {
            Throwable root = ex instanceof InvocationTargetException && ex.getCause() != null ? ex.getCause() : ex;
            return Result.skip(root.getClass().getSimpleName() + ": " + String.valueOf(root.getMessage()));
        }
    }

    public static Boolean hasStructure(String structure, long seed, int blockX, int blockZ) {
        Result result = validateStructure(structure, seed, blockX, blockZ);
        return switch (result.state()) {
            case PASS -> Boolean.TRUE;
            case FAIL -> Boolean.FALSE;
            case SKIP -> null;
        };
    }

    private static VanillaWorldgen2612 get() throws ReflectiveOperationException {
        VanillaWorldgen2612 local = instance;
        if (local == null) {
            synchronized (VanillaWorldgen2612.class) {
                local = instance;
                if (local == null) {
                    local = new VanillaWorldgen2612();
                    instance = local;
                }
            }
        }
        return local;
    }

    private Result validate(String structure, long seed, int blockX, int blockZ) throws ReflectiveOperationException, IOException {
        List<String> ids = mappedStructureIds(structure);
        if (ids.isEmpty()) {
            return Result.skip("Unsupported structure id: " + structure);
        }

        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        int minX = chunkX * 16;
        int minZ = chunkZ * 16;
        SeedContext ctx = seedContext(seed);
        List<String> missing = new ArrayList<>();
        for (String id : ids) {
            StructureData data = loadStructure(id);
            if (data == null) {
                missing.add(id);
                continue;
            }
            if (data.biomes().isEmpty()) {
                continue;
            }
            if ("monument".equals(id) && !monumentSurroundingBiomesPass(ctx, minX, minZ)) {
                continue;
            }
            for (SamplePoint sample : samplePoints(data, ctx, minX, minZ)) {
                String biome = biomeId(ctx, sample.x(), sample.y(), sample.z());
                if (data.biomes().contains(biome)) {
                    return Result.pass(id + " biome=" + biome + " sample=" + sample.reason());
                }
            }
        }
        if (missing.size() == ids.size()) {
            return Result.skip("No vanilla structure JSON found for " + structure + ": " + missing);
        }
        return Result.fail("No vanilla biome-valid sample matched for " + normalizeId(structure));
    }

    private List<SamplePoint> samplePoints(StructureData data, SeedContext ctx, int minX, int minZ)
        throws ReflectiveOperationException {
        List<SamplePoint> out = new ArrayList<>();
        int centerX = minX + 8;
        int centerZ = minZ + 8;
        String type = data.type();

        if ("mineshaft".equals(data.id()) || "mineshaft_mesa".equals(data.id())) {
            out.add(new SamplePoint(centerX, 50, minZ, "mineshaft_start"));
            return out;
        }

        if ("mansion".equals(data.id())) {
            addSurface(out, ctx, centerX, centerZ, worldSurfaceWg, "mansion_center_surface");
            addSurface(out, ctx, minX + 7, minZ + 7, worldSurfaceWg, "mansion_offset_surface");
            out.add(new SamplePoint(centerX, 80, centerZ, "mansion_fallback"));
            return out;
        }

        if (type.contains("jigsaw")) {
            int y = data.startHeight();
            if (data.projectStartToHeightmap()) {
                y = surfaceHeight(ctx, minX, minZ, heightmapFor(data.heightmap()));
            }
            out.add(new SamplePoint(minX, y, minZ, "jigsaw_start"));
            out.add(new SamplePoint(centerX, y, centerZ, "jigsaw_center"));
            out.add(new SamplePoint(minX + 15, y, minZ + 15, "jigsaw_far_corner"));
            out.add(new SamplePoint(centerX, 64, centerZ, "jigsaw_y64"));
            out.add(new SamplePoint(centerX, 319, centerZ, "jigsaw_high"));
            return out;
        }

        Object heightmap = worldSurfaceWg;
        if (data.id().startsWith("shipwreck") && !data.id().endsWith("beached")) {
            heightmap = oceanFloorWg;
        } else if (data.id().startsWith("ocean_ruin") || "buried_treasure".equals(data.id()) || "monument".equals(data.id())) {
            heightmap = oceanFloorWg;
        }

        addSurface(out, ctx, centerX, centerZ, heightmap, "chunk_center_heightmap");
        out.add(new SamplePoint(centerX, 64, centerZ, "chunk_center_y64"));
        out.add(new SamplePoint(centerX, 319, centerZ, "chunk_center_high"));
        out.add(new SamplePoint(minX + 9, 64, minZ + 9, "block_9_y64"));
        return out;
    }

    private void addSurface(List<SamplePoint> out, SeedContext ctx, int x, int z, Object heightmap, String reason)
        throws ReflectiveOperationException {
        out.add(new SamplePoint(x, surfaceHeight(ctx, x, z, heightmap), z, reason));
    }

    private boolean monumentSurroundingBiomesPass(SeedContext ctx, int minX, int minZ) throws ReflectiveOperationException, IOException {
        Set<String> surrounding = resolveBiomeReference("#minecraft:required_ocean_monument_surrounding");
        if (surrounding.isEmpty()) {
            return true;
        }
        int centerX = minX + 9;
        int centerZ = minZ + 9;
        for (int dx = -28; dx <= 28; dx += 4) {
            for (int dz = -28; dz <= 28; dz += 4) {
                if ((dx * dx) + (dz * dz) > 29 * 29) {
                    continue;
                }
                String biome = biomeId(ctx, centerX + dx, 63, centerZ + dz);
                if (!surrounding.contains(biome)) {
                    return false;
                }
            }
        }
        return true;
    }

    private int surfaceHeight(SeedContext ctx, int x, int z, Object heightmap) throws ReflectiveOperationException {
        return (Integer) generatorGetFirstOccupiedHeight.invoke(generator, x, z, heightmap, heightAccessor, ctx.randomState());
    }

    private Object heightmapFor(String name) {
        return "OCEAN_FLOOR_WG".equals(name) ? oceanFloorWg : worldSurfaceWg;
    }

    private String biomeId(SeedContext ctx, int blockX, int blockY, int blockZ) throws ReflectiveOperationException {
        Object holder = biomeSourceGetNoiseBiome.invoke(
            biomeSource,
            Math.floorDiv(blockX, 4),
            Math.floorDiv(blockY, 4),
            Math.floorDiv(blockZ, 4),
            ctx.sampler()
        );
        @SuppressWarnings("unchecked")
        Optional<Object> key = (Optional<Object>) holderUnwrapKey.invoke(holder);
        if (key.isEmpty()) {
            return "<inline>";
        }
        return resourceKeyIdentifier.invoke(key.get()).toString();
    }

    private SeedContext seedContext(long seed) throws ReflectiveOperationException {
        Object state = randomState(seed);
        return new SeedContext(seed, state, randomStateSampler.invoke(state));
    }

    private Object randomState(long seed) throws ReflectiveOperationException {
        synchronized (randomStateCache) {
            Object state = randomStateCache.get(seed);
            if (state == null) {
                state = randomStateCreate.invoke(null, lookup, overworldNoiseSettings, seed);
                if (randomStateCache.size() > 64) {
                    randomStateCache.clear();
                }
                randomStateCache.put(seed, state);
            }
            return state;
        }
    }

    private StructureData loadStructure(String id) throws IOException {
        Optional<StructureData> cached = structureCache.get(id);
        if (cached != null) {
            return cached.orElse(null);
        }

        Path path = dataRoot.resolve("worldgen").resolve("structure").resolve(id + ".json");
        if (!Files.isRegularFile(path)) {
            structureCache.put(id, Optional.empty());
            return null;
        }
        String json = Files.readString(path, StandardCharsets.UTF_8);
        String type = field(json, "type");
        String biomeRef = field(json, "biomes");
        if (biomeRef == null || biomeRef.isEmpty()) {
            biomeRef = "#minecraft:is_overworld";
        }
        String heightmap = field(json, "project_start_to_heightmap");
        Matcher heightMatcher = ABSOLUTE_HEIGHT.matcher(json);
        int startHeight = heightMatcher.find() ? Integer.parseInt(heightMatcher.group(1)) : 64;
        StructureData data = new StructureData(
            id,
            type == null ? "" : type,
            biomeRef,
            resolveBiomeReference(biomeRef),
            heightmap != null,
            heightmap == null ? "" : heightmap,
            startHeight
        );
        structureCache.put(id, Optional.of(data));
        return data;
    }

    private Set<String> resolveBiomeReference(String ref) throws IOException {
        if (ref == null || ref.isBlank()) {
            return Set.of();
        }
        String normalized = ref.trim();
        if (!normalized.startsWith("#")) {
            return Set.of(withMinecraftNamespace(normalized));
        }
        return resolveTag(normalized.substring(1), new HashSet<>());
    }

    private Set<String> resolveTag(String tagId, Set<String> visiting) throws IOException {
        tagId = withMinecraftNamespace(tagId);
        Set<String> cached = tagCache.get(tagId);
        if (cached != null) {
            return cached;
        }
        if (!visiting.add(tagId)) {
            return Set.of();
        }
        int colon = tagId.indexOf(':');
        String namespace = colon >= 0 ? tagId.substring(0, colon) : "minecraft";
        String pathPart = colon >= 0 ? tagId.substring(colon + 1) : tagId;
        Path path = dataRoot.getParent()
            .resolve(namespace)
            .resolve("tags")
            .resolve("worldgen")
            .resolve("biome")
            .resolve(pathPart + ".json");
        if (!Files.isRegularFile(path)) {
            visiting.remove(tagId);
            tagCache.put(tagId, Set.of());
            return Set.of();
        }
        String json = Files.readString(path, StandardCharsets.UTF_8);
        Set<String> out = new HashSet<>();
        Matcher matcher = JSON_STRING.matcher(json);
        while (matcher.find()) {
            String value = unescapeJsonString(matcher.group(1));
            if ("values".equals(value) || "replace".equals(value)) {
                continue;
            }
            if (value.startsWith("#")) {
                out.addAll(resolveTag(value.substring(1), visiting));
            } else if (value.contains(":")) {
                out.add(withMinecraftNamespace(value));
            }
        }
        Set<String> frozen = Set.copyOf(out);
        tagCache.put(tagId, frozen);
        visiting.remove(tagId);
        return frozen;
    }

    private static String field(String json, String name) {
        Matcher matcher = Pattern.compile(String.format(STRING_FIELD.pattern(), Pattern.quote(name))).matcher(json);
        return matcher.find() ? matcher.group(1) : null;
    }

    private static String unescapeJsonString(String value) {
        return value.replace("\\\"", "\"").replace("\\\\", "\\");
    }

    private static Path resolveDataRoot() {
        String configured = System.getProperty(DECOMPILED_ROOT_PROPERTY);
        if (configured == null || configured.isBlank()) {
            configured = System.getenv(DECOMPILED_ROOT_ENV);
        }
        Path root = configured == null || configured.isBlank() ? defaultDecompiledRoot() : Path.of(configured);
        Path data = root.resolve("data").resolve("minecraft");
        if (Files.isDirectory(data)) {
            return data;
        }
        if (Files.isDirectory(root.resolve("worldgen"))) {
            return root;
        }
        return defaultDecompiledRoot().resolve("resources").resolve("data").resolve("minecraft");
    }

    private static Path defaultDecompiledRoot() {
        String home = System.getProperty("user.home");
        if (home != null && !home.isBlank()) {
            Path desktopCopy = Path.of(home, "Desktop", DEFAULT_DECOMPILED_DIR);
            if (Files.isDirectory(desktopCopy)) {
                return desktopCopy;
            }
        }
        return Path.of(DEFAULT_DECOMPILED_DIR);
    }

    private static List<String> mappedStructureIds(String structure) {
        String s = normalizeId(structure);
        return switch (s) {
            case "village", "villages" -> List.of("village_plains", "village_desert", "village_savanna", "village_snowy", "village_taiga");
            case "jungle_temple", "jungle_pyramid" -> List.of("jungle_pyramid");
            case "desert_temple", "desert_pyramid" -> List.of("desert_pyramid");
            case "witch_hut", "swamp_hut" -> List.of("swamp_hut");
            case "ocean_monument", "monument" -> List.of("monument");
            case "woodland_mansion", "mansion" -> List.of("mansion");
            case "ocean_ruin" -> List.of("ocean_ruin_cold", "ocean_ruin_warm");
            case "ruined_portal" -> List.of(
                "ruined_portal",
                "ruined_portal_desert",
                "ruined_portal_jungle",
                "ruined_portal_mountain",
                "ruined_portal_ocean",
                "ruined_portal_swamp"
            );
            case "shipwreck" -> List.of("shipwreck", "shipwreck_beached");
            case "trail_ruin", "trail_ruins" -> List.of("trail_ruins");
            case "trial_chamber", "trial_chambers" -> List.of("trial_chambers");
            case "mineshaft" -> List.of("mineshaft", "mineshaft_mesa");
            case "outpost", "pillager_outpost" -> List.of("pillager_outpost");
            case "bastion", "bastion_remnant" -> List.of("bastion_remnant");
            case "fortress", "nether_fortress" -> List.of("fortress");
            default -> List.of(s);
        };
    }

    private static String normalizeId(String structure) {
        String s = structure == null ? "" : structure.trim().toLowerCase(Locale.ROOT);
        if (s.startsWith("minecraft:")) {
            s = s.substring("minecraft:".length());
        }
        return s.replace('-', '_').replace(' ', '_');
    }

    private static String withMinecraftNamespace(String value) {
        String s = value.trim();
        return s.contains(":") ? s : "minecraft:" + s;
    }
}
