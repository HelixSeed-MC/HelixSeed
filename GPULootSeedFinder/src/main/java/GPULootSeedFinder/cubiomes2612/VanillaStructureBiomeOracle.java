package GPULootSeedFinder.cubiomes2612;

import java.io.IOException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.net.MalformedURLException;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Reflection-only bridge to Minecraft 26.1.2 vanilla worldgen biome checks.
 *
 * <p>No net.minecraft type is referenced at compile time. At runtime the class
 * expects the named 26.1.2 Minecraft client jar and its libraries on either the
 * process classpath, the thread context classloader, or a classpath supplied by
 * {@code helixseed.mc2612.classpath}. If no explicit classpath is supplied it
 * will also try Prism Launcher's default 26.1.2 library location.</p>
 */
public final class VanillaStructureBiomeOracle {
    private static final Path DEFAULT_DATA_ROOT = Path.of(
        "C:",
        "Users",
        "David",
        "Downloads",
        "minecraft-26.1.2-decompiled-nicest",
        "data",
        "minecraft"
    );
    private static final String DATA_ROOT_PROPERTY = "helixseed.mc2612.dataRoot";
    private static final String CLASSPATH_PROPERTY = "helixseed.mc2612.classpath";
    private static final String CLIENT_JAR_PROPERTY = "helixseed.mc2612.clientJar";

    private static volatile Path dataRoot = defaultDataRoot();
    private static volatile DataRepository dataRepository;
    private static volatile VanillaRuntime runtime;

    private VanillaStructureBiomeOracle() {
    }

    public static ValidationResult validateStructure(String structure, long seed, int blockX, int blockZ) {
        return validateStructure(structure, seed, blockX, blockZ, true);
    }

    public static ValidationResult validateStructure(
        String structure,
        long seed,
        int blockX,
        int blockZ,
        boolean includeSurfaceHeight
    ) {
        String requested = normalizeStructureId(structure);
        if (requested.isEmpty()) {
            return ValidationResult.error(structure, "", "", seed, blockX, blockZ, "Empty structure id");
        }

        DataRepository repository;
        try {
            repository = data();
        } catch (Exception ex) {
            return ValidationResult.error(requested, "", "", seed, blockX, blockZ, "Unable to load vanilla data: " + brief(ex));
        }

        List<String> candidates = repository.resolveStructureCandidates(requested);
        if (candidates.isEmpty()) {
            return ValidationResult.error(requested, "", "", seed, blockX, blockZ, "Unsupported structure alias");
        }

        VanillaRuntime mc = null;
        String runtimeError = null;
        try {
            mc = minecraft();
        } catch (Exception ex) {
            runtimeError = "Minecraft runtime unavailable: " + brief(ex);
        }

        Integer surfaceY = null;
        int sampleY = 64;
        if (includeSurfaceHeight && mc != null) {
            try {
                surfaceY = mc.world(seed).surfaceHeight(blockX, blockZ);
                sampleY = surfaceY;
            } catch (Exception ex) {
                runtimeError = "Surface height unavailable: " + brief(ex);
            }
        }

        String sampledBiome = "";
        if (mc != null) {
            try {
                sampledBiome = mc.world(seed).sampleBiomeId(blockX, sampleY, blockZ);
            } catch (Exception ex) {
                runtimeError = "Biome sample unavailable: " + brief(ex);
            }
        }

        ValidationResult best = null;
        for (String candidate : candidates) {
            Set<String> requiredBiomes;
            try {
                requiredBiomes = repository.requiredBiomes(candidate);
            } catch (Exception ex) {
                best = ValidationResult.error(requested, candidate, sampledBiome, seed, blockX, blockZ, brief(ex));
                continue;
            }

            boolean biomeViable = !sampledBiome.isEmpty() && requiredBiomes.contains(sampledBiome);
            VanillaGenerationCheck vanillaCheck = VanillaGenerationCheck.notRun(runtimeError);
            if (mc != null) {
                try {
                    vanillaCheck = mc.world(seed).validateGenerationPoint(candidate, blockX, blockZ);
                } catch (Exception ex) {
                    vanillaCheck = VanillaGenerationCheck.notRun(brief(ex));
                }
            }

            boolean viable = vanillaCheck.validGenerationPoint.orElse(biomeViable);
            ValidationResult result = new ValidationResult(
                requested,
                candidate,
                seed,
                blockX,
                blockZ,
                sampledBiome,
                requiredBiomes,
                surfaceY,
                vanillaCheck.placementCandidate,
                vanillaCheck.validGenerationPoint,
                vanillaCheck.generationPoint,
                viable,
                nullToEmpty(vanillaCheck.message)
            );
            if (result.viable()) {
                return result;
            }
            if (best == null || (best.biomeId().isEmpty() && !result.biomeId().isEmpty())) {
                best = result;
            }
        }

        return best == null
            ? ValidationResult.error(requested, "", sampledBiome, seed, blockX, blockZ, nullToEmpty(runtimeError))
            : best;
    }

    public static String sampleBiomeId(long seed, int blockX, int blockY, int blockZ) {
        try {
            return minecraft().world(seed).sampleBiomeId(blockX, blockY, blockZ);
        } catch (ReflectiveOperationException | RuntimeException ex) {
            throw new IllegalStateException("Unable to sample vanilla biome: " + brief(ex), ex);
        }
    }

    public static int surfaceHeight(long seed, int blockX, int blockZ) {
        try {
            return minecraft().world(seed).surfaceHeight(blockX, blockZ);
        } catch (ReflectiveOperationException | RuntimeException ex) {
            throw new IllegalStateException("Unable to sample vanilla surface height: " + brief(ex), ex);
        }
    }

    public static void configureDataRoot(Path newDataRoot) {
        if (newDataRoot == null) {
            throw new IllegalArgumentException("newDataRoot");
        }
        dataRoot = newDataRoot;
        dataRepository = null;
    }

    public static Path dataRoot() {
        return dataRoot;
    }

    public static final class ValidationResult {
        private final String requestedStructure;
        private final String matchedStructure;
        private final long seed;
        private final int blockX;
        private final int blockZ;
        private final String biomeId;
        private final Set<String> requiredBiomes;
        private final Integer surfaceY;
        private final boolean placementCandidate;
        private final Optional<Boolean> vanillaGenerationPoint;
        private final String generationPoint;
        private final boolean viable;
        private final String message;

        private ValidationResult(
            String requestedStructure,
            String matchedStructure,
            long seed,
            int blockX,
            int blockZ,
            String biomeId,
            Set<String> requiredBiomes,
            Integer surfaceY,
            boolean placementCandidate,
            Optional<Boolean> vanillaGenerationPoint,
            String generationPoint,
            boolean viable,
            String message
        ) {
            this.requestedStructure = nullToEmpty(requestedStructure);
            this.matchedStructure = nullToEmpty(matchedStructure);
            this.seed = seed;
            this.blockX = blockX;
            this.blockZ = blockZ;
            this.biomeId = nullToEmpty(biomeId);
            this.requiredBiomes = Collections.unmodifiableSet(new LinkedHashSet<>(requiredBiomes));
            this.surfaceY = surfaceY;
            this.placementCandidate = placementCandidate;
            this.vanillaGenerationPoint = vanillaGenerationPoint == null ? Optional.empty() : vanillaGenerationPoint;
            this.generationPoint = nullToEmpty(generationPoint);
            this.viable = viable;
            this.message = nullToEmpty(message);
        }

        private static ValidationResult error(String requested, String matched, String biome, long seed, int x, int z, String message) {
            return new ValidationResult(
                requested,
                matched,
                seed,
                x,
                z,
                biome,
                Set.of(),
                null,
                false,
                Optional.empty(),
                "",
                false,
                message
            );
        }

        public String requestedStructure() {
            return requestedStructure;
        }

        public String matchedStructure() {
            return matchedStructure;
        }

        public long seed() {
            return seed;
        }

        public int blockX() {
            return blockX;
        }

        public int blockZ() {
            return blockZ;
        }

        public String biomeId() {
            return biomeId;
        }

        public Set<String> requiredBiomes() {
            return requiredBiomes;
        }

        public Integer surfaceY() {
            return surfaceY;
        }

        public boolean placementCandidate() {
            return placementCandidate;
        }

        public Optional<Boolean> vanillaGenerationPoint() {
            return vanillaGenerationPoint;
        }

        public String generationPoint() {
            return generationPoint;
        }

        public boolean viable() {
            return viable;
        }

        public String message() {
            return message;
        }

        @Override
        public String toString() {
            return "ValidationResult{" +
                "requestedStructure='" + requestedStructure + '\'' +
                ", matchedStructure='" + matchedStructure + '\'' +
                ", seed=" + seed +
                ", blockX=" + blockX +
                ", blockZ=" + blockZ +
                ", biomeId='" + biomeId + '\'' +
                ", surfaceY=" + surfaceY +
                ", placementCandidate=" + placementCandidate +
                ", vanillaGenerationPoint=" + vanillaGenerationPoint +
                ", generationPoint='" + generationPoint + '\'' +
                ", viable=" + viable +
                ", message='" + message + '\'' +
                '}';
        }
    }

    private static VanillaRuntime minecraft() throws ReflectiveOperationException {
        VanillaRuntime local = runtime;
        if (local == null) {
            synchronized (VanillaStructureBiomeOracle.class) {
                local = runtime;
                if (local == null) {
                    local = new VanillaRuntime(resolveMinecraftClassLoader());
                    runtime = local;
                }
            }
        }
        return local;
    }

    private static DataRepository data() throws IOException {
        DataRepository local = dataRepository;
        Path current = dataRoot();
        if (local == null || !local.root.equals(current)) {
            synchronized (VanillaStructureBiomeOracle.class) {
                local = dataRepository;
                if (local == null || !local.root.equals(current)) {
                    local = new DataRepository(current);
                    dataRepository = local;
                }
            }
        }
        return local;
    }

    private static Path defaultDataRoot() {
        String override = System.getProperty(DATA_ROOT_PROPERTY);
        return override == null || override.isBlank() ? DEFAULT_DATA_ROOT : Path.of(override);
    }

    private static ClassLoader resolveMinecraftClassLoader() {
        ClassLoader context = Thread.currentThread().getContextClassLoader();
        if (canLoadMinecraft(context)) {
            return context;
        }
        ClassLoader own = VanillaStructureBiomeOracle.class.getClassLoader();
        if (canLoadMinecraft(own)) {
            return own;
        }

        List<URL> urls = new ArrayList<>();
        appendClasspathProperty(urls);
        appendClientJarProperty(urls);
        if (urls.isEmpty()) {
            appendDefaultPrismClasspath(urls);
        }
        if (urls.isEmpty()) {
            throw new IllegalStateException("Minecraft 26.1.2 classes are not on the runtime classpath");
        }
        URLClassLoader loader = new URLClassLoader(urls.toArray(new URL[0]), own);
        if (!canLoadMinecraft(loader)) {
            throw new IllegalStateException("Configured Minecraft classpath does not contain named net.minecraft classes");
        }
        return loader;
    }

    private static boolean canLoadMinecraft(ClassLoader loader) {
        if (loader == null) {
            return false;
        }
        try {
            Class.forName("net.minecraft.SharedConstants", false, loader);
            Class.forName("net.minecraft.world.level.biome.BiomeSource", false, loader);
            return true;
        } catch (ClassNotFoundException ex) {
            return false;
        }
    }

    private static void appendClasspathProperty(List<URL> urls) {
        String configured = System.getProperty(CLASSPATH_PROPERTY);
        if (configured == null || configured.isBlank()) {
            return;
        }
        String[] parts = configured.split(java.io.File.pathSeparator);
        for (String part : parts) {
            appendJarOrDirectory(urls, Path.of(part));
        }
    }

    private static void appendClientJarProperty(List<URL> urls) {
        String configured = System.getProperty(CLIENT_JAR_PROPERTY);
        if (configured == null || configured.isBlank()) {
            return;
        }
        appendJarOrDirectory(urls, Path.of(configured));
        Path prismLibraries = Path.of(System.getProperty("user.home"), "AppData", "Roaming", "PrismLauncher", "libraries");
        appendAllJars(urls, prismLibraries);
    }

    private static void appendDefaultPrismClasspath(List<URL> urls) {
        Path prismLibraries = Path.of(System.getProperty("user.home"), "AppData", "Roaming", "PrismLauncher", "libraries");
        Path clientJar = prismLibraries.resolve(Path.of("com", "mojang", "minecraft", "26.1.2", "minecraft-26.1.2-client.jar"));
        appendJarOrDirectory(urls, clientJar);
        appendAllJars(urls, prismLibraries);
    }

    private static void appendAllJars(List<URL> urls, Path root) {
        if (!Files.isDirectory(root)) {
            return;
        }
        try {
            Files.walk(root)
                .filter(path -> Files.isRegularFile(path) && path.getFileName().toString().endsWith(".jar"))
                .forEach(path -> appendJarOrDirectory(urls, path));
        } catch (IOException ignored) {
            // Caller will report a missing Minecraft runtime if the URL set is unusable.
        }
    }

    private static void appendJarOrDirectory(List<URL> urls, Path path) {
        if (!Files.exists(path)) {
            return;
        }
        try {
            urls.add(path.toUri().toURL());
        } catch (MalformedURLException ex) {
            throw new IllegalArgumentException("Bad Minecraft classpath entry: " + path, ex);
        }
    }

    private static final class VanillaRuntime {
        private final ClassLoader loader;
        private final Object lookup;
        private final Object generator;
        private final Object structureRegistryLookup;
        private final Object structureSetRegistryLookup;
        private final Object registryAccess;
        private final Object overworldNoiseSettingsKey;
        private final Object structureRegistryKey;
        private final Map<Long, VanillaWorld> worlds = new ConcurrentHashMap<>();

        VanillaRuntime(ClassLoader loader) throws ReflectiveOperationException {
            this.loader = loader;
            callStatic("net.minecraft.SharedConstants", "tryDetectVersion");
            callStatic("net.minecraft.server.Bootstrap", "bootStrap");
            this.lookup = callStatic("net.minecraft.data.registries.VanillaRegistries", "createLookup");
            this.generator = call(callStatic("net.minecraft.world.level.levelgen.presets.WorldPresets", "createNormalWorldDimensions", lookup), "overworld");
            Class<?> registries = load("net.minecraft.core.registries.Registries");
            this.structureRegistryKey = getStaticField(registries, "STRUCTURE");
            Object structureSetRegistryKey = getStaticField(registries, "STRUCTURE_SET");
            this.structureRegistryLookup = call(lookup, "lookupOrThrow", structureRegistryKey);
            this.structureSetRegistryLookup = call(lookup, "lookupOrThrow", structureSetRegistryKey);
            this.overworldNoiseSettingsKey = getStaticField(load("net.minecraft.world.level.levelgen.NoiseGeneratorSettings"), "OVERWORLD");
            Object registryOfRegistries = getStaticField(load("net.minecraft.core.registries.BuiltInRegistries"), "REGISTRY");
            this.registryAccess = callStatic("net.minecraft.core.RegistryAccess", "fromRegistryOfRegistries", registryOfRegistries);
        }

        VanillaWorld world(long seed) {
            return worlds.computeIfAbsent(seed, this::newWorld);
        }

        private VanillaWorld newWorld(long seed) {
            try {
                Object randomState = callStatic("net.minecraft.world.level.levelgen.RandomState", "create", lookup, overworldNoiseSettingsKey, seed);
                Object structureState = call(generator, "createState", structureSetRegistryLookup, randomState, seed);
                return new VanillaWorld(this, seed, randomState, structureState);
            } catch (ReflectiveOperationException ex) {
                throw new IllegalStateException("Unable to create vanilla worldgen state: " + brief(ex), ex);
            }
        }

        Class<?> load(String className) throws ClassNotFoundException {
            return Class.forName(className, false, loader);
        }

        Object identifier(String id) throws ReflectiveOperationException {
            return callStatic("net.minecraft.resources.Identifier", "withDefaultNamespace", stripMinecraftNamespace(id));
        }

        Object structureKey(String id) throws ReflectiveOperationException {
            return callStatic("net.minecraft.resources.ResourceKey", "create", structureRegistryKey, identifier(id));
        }

        Object structureHolder(String id) throws ReflectiveOperationException {
            return call(structureRegistryLookup, "getOrThrow", structureKey(id));
        }

        Object structureValue(String id) throws ReflectiveOperationException {
            return call(structureHolder(id), "value");
        }

        private Object callStatic(String className, String name, Object... args) throws ReflectiveOperationException {
            return VanillaStructureBiomeOracle.callStatic(load(className), name, args);
        }
    }

    private static final class VanillaWorld {
        private final VanillaRuntime runtime;
        private final long seed;
        private final Object randomState;
        private final Object structureState;
        private final Object biomeSource;
        private final Object heightAccessor;
        private final Object worldSurfaceHeightmapType;

        VanillaWorld(VanillaRuntime runtime, long seed, Object randomState, Object structureState) throws ReflectiveOperationException {
            this.runtime = runtime;
            this.seed = seed;
            this.randomState = randomState;
            this.structureState = structureState;
            this.biomeSource = call(runtime.generator, "getBiomeSource");
            this.heightAccessor = callStatic(runtime.load("net.minecraft.world.level.LevelHeightAccessor"), "create", -64, 384);
            @SuppressWarnings({"rawtypes", "unchecked"})
            Object heightmapType = Enum.valueOf((Class<Enum>) runtime.load("net.minecraft.world.level.levelgen.Heightmap$Types"), "WORLD_SURFACE_WG");
            this.worldSurfaceHeightmapType = heightmapType;
        }

        String sampleBiomeId(int blockX, int blockY, int blockZ) throws ReflectiveOperationException {
            Object sampler = call(randomState, "sampler");
            Object holder = call(
                biomeSource,
                "getNoiseBiome",
                Math.floorDiv(blockX, 4),
                Math.floorDiv(blockY, 4),
                Math.floorDiv(blockZ, 4),
                sampler
            );
            return holderId(holder);
        }

        int surfaceHeight(int blockX, int blockZ) throws ReflectiveOperationException {
            Object height = call(runtime.generator, "getFirstOccupiedHeight", blockX, blockZ, worldSurfaceHeightmapType, heightAccessor, randomState);
            return ((Number) height).intValue();
        }

        VanillaGenerationCheck validateGenerationPoint(String structureId, int blockX, int blockZ) throws ReflectiveOperationException {
            Object holder = runtime.structureHolder(structureId);
            Object structure = call(holder, "value");
            int chunkX = Math.floorDiv(blockX, 16);
            int chunkZ = Math.floorDiv(blockZ, 16);
            boolean placementCandidate = false;
            Object placements = call(structureState, "getPlacementsForStructure", holder);
            if (placements instanceof Iterable<?>) {
                for (Object placement : (Iterable<?>) placements) {
                    Object candidate = call(placement, "isStructureChunk", structureState, chunkX, chunkZ);
                    if (Boolean.TRUE.equals(candidate)) {
                        placementCandidate = true;
                        break;
                    }
                }
            }

            Object chunkPos = construct(runtime.load("net.minecraft.world.level.ChunkPos"), chunkX, chunkZ);
            Object holderSet = call(structure, "biomes");
            Object predicate = biomePredicate(holderSet);
            Object context = constructGenerationContext(chunkPos, predicate);
            Object optional = call(structure, "findValidGenerationPoint", context);
            if (!(optional instanceof Optional<?>)) {
                return new VanillaGenerationCheck(placementCandidate, Optional.empty(), "", "Unexpected generation-point response");
            }
            Optional<?> generationPoint = (Optional<?>) optional;
            if (generationPoint.isEmpty()) {
                return new VanillaGenerationCheck(placementCandidate, Optional.of(false), "", "");
            }
            Object stub = generationPoint.get();
            Object pos = call(stub, "position");
            String location = blockPosString(pos);
            return new VanillaGenerationCheck(placementCandidate, Optional.of(true), location, "");
        }

        private Object constructGenerationContext(Object chunkPos, Object predicate) throws ReflectiveOperationException {
            Class<?> contextClass = runtime.load("net.minecraft.world.level.levelgen.structure.Structure$GenerationContext");
            for (Constructor<?> ctor : contextClass.getDeclaredConstructors()) {
                if (ctor.getParameterCount() == 9) {
                    ctor.setAccessible(true);
                    return ctor.newInstance(
                        runtime.registryAccess,
                        runtime.generator,
                        biomeSource,
                        randomState,
                        null,
                        seed,
                        chunkPos,
                        heightAccessor,
                        predicate
                    );
                }
            }
            throw new NoSuchMethodException("Structure.GenerationContext constructor with 9 parameters");
        }

        private Object biomePredicate(Object holderSet) throws ClassNotFoundException {
            Class<?> predicateClass = runtime.load("java.util.function.Predicate");
            InvocationHandler handler = (proxy, method, args) -> {
                if ("test".equals(method.getName()) && args != null && args.length == 1) {
                    try {
                        return call(holderSet, "contains", args[0]);
                    } catch (ReflectiveOperationException ex) {
                        throw new IllegalStateException(ex);
                    }
                }
                if ("toString".equals(method.getName())) {
                    return "vanillaBiomePredicate";
                }
                if ("hashCode".equals(method.getName())) {
                    return System.identityHashCode(proxy);
                }
                if ("equals".equals(method.getName())) {
                    return proxy == args[0];
                }
                return null;
            };
            return Proxy.newProxyInstance(runtime.loader, new Class<?>[] {predicateClass}, handler);
        }
    }

    private static final class VanillaGenerationCheck {
        final boolean placementCandidate;
        final Optional<Boolean> validGenerationPoint;
        final String generationPoint;
        final String message;

        VanillaGenerationCheck(boolean placementCandidate, Optional<Boolean> validGenerationPoint, String generationPoint, String message) {
            this.placementCandidate = placementCandidate;
            this.validGenerationPoint = validGenerationPoint;
            this.generationPoint = nullToEmpty(generationPoint);
            this.message = nullToEmpty(message);
        }

        static VanillaGenerationCheck notRun(String message) {
            return new VanillaGenerationCheck(false, Optional.empty(), "", message);
        }
    }

    private static final class DataRepository {
        private final Path root;
        private final Map<String, List<String>> aliases;
        private final Map<String, Set<String>> biomeCache = new ConcurrentHashMap<>();
        private final Set<String> knownStructures;

        DataRepository(Path root) throws IOException {
            this.root = root;
            if (!Files.isDirectory(root)) {
                throw new IOException("Minecraft data root not found: " + root);
            }
            this.knownStructures = loadKnownStructures(root.resolve(Path.of("worldgen", "structure")));
            this.aliases = buildAliases(knownStructures);
        }

        List<String> resolveStructureCandidates(String structure) {
            String normalized = normalizeStructureId(structure);
            List<String> mapped = aliases.get(normalized);
            if (mapped != null) {
                return mapped;
            }
            return knownStructures.contains(normalized) ? List.of(normalized) : List.of();
        }

        Set<String> requiredBiomes(String structureName) {
            return biomeCache.computeIfAbsent(structureName, this::loadRequiredBiomesUnchecked);
        }

        private Set<String> loadRequiredBiomesUnchecked(String structureName) {
            try {
                Path file = root.resolve(Path.of("worldgen", "structure", structureName + ".json"));
                Object json = Json.parse(Files.readString(file, StandardCharsets.UTF_8));
                Object biomes = Json.member(json, "biomes");
                LinkedHashSet<String> out = new LinkedHashSet<>();
                collectBiomeRefs(out, biomes, new HashSet<>());
                return Collections.unmodifiableSet(out);
            } catch (IOException | RuntimeException ex) {
                throw new IllegalStateException("Unable to load structure biomes for " + structureName + ": " + brief(ex), ex);
            }
        }

        private void collectBiomeRefs(Set<String> out, Object value, Set<String> seenTags) throws IOException {
            if (value instanceof String) {
                String ref = (String) value;
                if (ref.startsWith("#")) {
                    expandBiomeTag(out, ref.substring(1), seenTags);
                } else {
                    out.add(normalizeResourceId(ref));
                }
                return;
            }
            if (value instanceof Iterable<?>) {
                for (Object item : (Iterable<?>) value) {
                    collectBiomeRefs(out, item, seenTags);
                }
            }
        }

        private void expandBiomeTag(Set<String> out, String tagId, Set<String> seenTags) throws IOException {
            String normalized = normalizeTagId(tagId);
            if (!seenTags.add(normalized)) {
                return;
            }
            Path tagFile = root.resolve(Path.of("tags", "worldgen", "biome", normalized + ".json"));
            Object json = Json.parse(Files.readString(tagFile, StandardCharsets.UTF_8));
            Object values = Json.member(json, "values");
            collectBiomeRefs(out, values, seenTags);
        }
    }

    private static Set<String> loadKnownStructures(Path structureDir) throws IOException {
        LinkedHashSet<String> out = new LinkedHashSet<>();
        if (!Files.isDirectory(structureDir)) {
            return out;
        }
        try (var stream = Files.list(structureDir)) {
            stream.filter(path -> path.getFileName().toString().endsWith(".json"))
                .map(path -> path.getFileName().toString())
                .map(name -> name.substring(0, name.length() - ".json".length()))
                .forEach(out::add);
        }
        return Collections.unmodifiableSet(out);
    }

    private static Map<String, List<String>> buildAliases(Set<String> knownStructures) {
        Map<String, List<String>> aliases = new HashMap<>();
        putAlias(aliases, knownStructures, "ancient_city", "ancient_city");
        putAlias(aliases, knownStructures, "bastion", "bastion_remnant");
        putAlias(aliases, knownStructures, "bastion_remnant", "bastion_remnant");
        putAlias(aliases, knownStructures, "buried_treasure", "buried_treasure");
        putAlias(aliases, knownStructures, "desert_pyramid", "desert_pyramid");
        putAlias(aliases, knownStructures, "desert_temple", "desert_pyramid");
        putAlias(aliases, knownStructures, "end_city", "end_city");
        putAlias(aliases, knownStructures, "fortress", "fortress");
        putAlias(aliases, knownStructures, "nether_fortress", "fortress");
        putAlias(aliases, knownStructures, "igloo", "igloo");
        putAlias(aliases, knownStructures, "jungle_pyramid", "jungle_pyramid");
        putAlias(aliases, knownStructures, "jungle_temple", "jungle_pyramid");
        putAlias(aliases, knownStructures, "mansion", "mansion");
        putAlias(aliases, knownStructures, "woodland_mansion", "mansion");
        putAlias(aliases, knownStructures, "mineshaft", "mineshaft");
        putAlias(aliases, knownStructures, "mineshaft_mesa", "mineshaft_mesa");
        putAlias(aliases, knownStructures, "monument", "monument");
        putAlias(aliases, knownStructures, "ocean_monument", "monument");
        putAlias(aliases, knownStructures, "ocean_ruin", "ocean_ruin_cold", "ocean_ruin_warm");
        putAlias(aliases, knownStructures, "ocean_ruin_cold", "ocean_ruin_cold");
        putAlias(aliases, knownStructures, "ocean_ruin_warm", "ocean_ruin_warm");
        putAlias(aliases, knownStructures, "pillager_outpost", "pillager_outpost");
        putAlias(aliases, knownStructures, "ruined_portal", "ruined_portal", "ruined_portal_desert", "ruined_portal_jungle",
            "ruined_portal_mountain", "ruined_portal_ocean", "ruined_portal_swamp");
        putAlias(aliases, knownStructures, "ruined_portal_standard", "ruined_portal");
        putAlias(aliases, knownStructures, "shipwreck", "shipwreck", "shipwreck_beached");
        putAlias(aliases, knownStructures, "shipwreck_beached", "shipwreck_beached");
        putAlias(aliases, knownStructures, "stronghold", "stronghold");
        putAlias(aliases, knownStructures, "swamp_hut", "swamp_hut");
        putAlias(aliases, knownStructures, "trail_ruin", "trail_ruins");
        putAlias(aliases, knownStructures, "trail_ruins", "trail_ruins");
        putAlias(aliases, knownStructures, "trial_chambers", "trial_chambers");
        putAlias(aliases, knownStructures, "trial_chamber", "trial_chambers");
        putAlias(aliases, knownStructures, "village", "village_plains", "village_desert", "village_savanna", "village_snowy", "village_taiga");
        putAlias(aliases, knownStructures, "village_desert", "village_desert");
        putAlias(aliases, knownStructures, "village_plains", "village_plains");
        putAlias(aliases, knownStructures, "village_savanna", "village_savanna");
        putAlias(aliases, knownStructures, "village_snowy", "village_snowy");
        putAlias(aliases, knownStructures, "village_taiga", "village_taiga");
        return Collections.unmodifiableMap(aliases);
    }

    private static void putAlias(Map<String, List<String>> aliases, Set<String> knownStructures, String alias, String... vanillaNames) {
        ArrayList<String> present = new ArrayList<>();
        for (String vanillaName : vanillaNames) {
            if (knownStructures.contains(vanillaName)) {
                present.add(vanillaName);
            }
        }
        if (!present.isEmpty()) {
            aliases.put(alias, List.copyOf(present));
        }
    }

    private static Object callStatic(Class<?> type, String name, Object... args) throws ReflectiveOperationException {
        Method method = findMethod(type, name, args);
        return method.invoke(null, args);
    }

    private static Object call(Object target, String name, Object... args) throws ReflectiveOperationException {
        Method method = findMethod(target.getClass(), name, args);
        return method.invoke(target, args);
    }

    private static Method findMethod(Class<?> type, String name, Object[] args) throws NoSuchMethodException {
        Method best = null;
        for (Method method : type.getMethods()) {
            if (!method.getName().equals(name) || method.getParameterCount() != args.length) {
                continue;
            }
            if (parametersMatch(method.getParameterTypes(), args)) {
                if (best == null || betterMethod(method, best, args)) {
                    best = method;
                }
            }
        }
        if (best != null) {
            best.setAccessible(true);
            return best;
        }
        throw new NoSuchMethodException(type.getName() + "." + name + " with " + args.length + " args");
    }

    private static boolean betterMethod(Method candidate, Method current, Object[] args) {
        Class<?>[] candidateTypes = candidate.getParameterTypes();
        Class<?>[] currentTypes = current.getParameterTypes();
        for (int i = 0; i < args.length; i++) {
            if (args[i] == null) {
                continue;
            }
            Class<?> argClass = args[i].getClass();
            boolean candidateExact = wrap(candidateTypes[i]).equals(argClass);
            boolean currentExact = wrap(currentTypes[i]).equals(argClass);
            if (candidateExact != currentExact) {
                return candidateExact;
            }
        }
        return false;
    }

    private static boolean parametersMatch(Class<?>[] parameterTypes, Object[] args) {
        for (int i = 0; i < parameterTypes.length; i++) {
            if (args[i] == null) {
                continue;
            }
            if (!wrap(parameterTypes[i]).isAssignableFrom(args[i].getClass())) {
                return false;
            }
        }
        return true;
    }

    private static Class<?> wrap(Class<?> type) {
        if (!type.isPrimitive()) {
            return type;
        }
        if (type == int.class) {
            return Integer.class;
        }
        if (type == long.class) {
            return Long.class;
        }
        if (type == boolean.class) {
            return Boolean.class;
        }
        if (type == double.class) {
            return Double.class;
        }
        if (type == float.class) {
            return Float.class;
        }
        if (type == short.class) {
            return Short.class;
        }
        if (type == byte.class) {
            return Byte.class;
        }
        if (type == char.class) {
            return Character.class;
        }
        return Void.class;
    }

    private static Object construct(Class<?> type, Object... args) throws ReflectiveOperationException {
        for (Constructor<?> ctor : type.getConstructors()) {
            if (ctor.getParameterCount() == args.length && parametersMatch(ctor.getParameterTypes(), args)) {
                ctor.setAccessible(true);
                return ctor.newInstance(args);
            }
        }
        throw new NoSuchMethodException(type.getName() + " constructor with " + args.length + " args");
    }

    private static Object getStaticField(Class<?> type, String name) throws ReflectiveOperationException {
        Field field = type.getField(name);
        field.setAccessible(true);
        return field.get(null);
    }

    private static String holderId(Object holder) throws ReflectiveOperationException {
        Object optional = call(holder, "unwrapKey");
        if (!(optional instanceof Optional<?>)) {
            return "";
        }
        Optional<?> key = (Optional<?>) optional;
        if (key.isEmpty()) {
            return "";
        }
        Object identifier = call(key.get(), "identifier");
        return identifier.toString();
    }

    private static String blockPosString(Object pos) throws ReflectiveOperationException {
        int x = ((Number) call(pos, "getX")).intValue();
        int y = ((Number) call(pos, "getY")).intValue();
        int z = ((Number) call(pos, "getZ")).intValue();
        return x + "," + y + "," + z;
    }

    private static String normalizeStructureId(String id) {
        String normalized = stripMinecraftNamespace(nullToEmpty(id).trim().toLowerCase(Locale.ROOT));
        normalized = normalized.replace('-', '_');
        normalized = normalized.replace(" ", "_");
        if (normalized.endsWith("s") && "trail_ruins".equals(normalized)) {
            return normalized;
        }
        return normalized;
    }

    private static String normalizeResourceId(String id) {
        String stripped = stripMinecraftNamespace(id);
        return "minecraft:" + stripped;
    }

    private static String normalizeTagId(String id) {
        return stripMinecraftNamespace(id);
    }

    private static String stripMinecraftNamespace(String id) {
        String value = nullToEmpty(id).trim();
        if (value.startsWith("minecraft:")) {
            return value.substring("minecraft:".length());
        }
        return value;
    }

    private static String nullToEmpty(String value) {
        return value == null ? "" : value;
    }

    private static String brief(Throwable ex) {
        Throwable t = ex;
        while (t.getCause() != null && (t instanceof ReflectiveOperationException || t instanceof IllegalStateException)) {
            t = t.getCause();
        }
        String message = t.getMessage();
        return t.getClass().getSimpleName() + (message == null || message.isBlank() ? "" : ": " + message);
    }

    private static final class Json {
        private Json() {
        }

        static Object parse(String text) {
            Parser parser = new Parser(text);
            Object value = parser.readValue();
            parser.skipWhitespace();
            if (!parser.end()) {
                throw new IllegalArgumentException("Trailing JSON data at " + parser.pos);
            }
            return value;
        }

        @SuppressWarnings("unchecked")
        static Object member(Object json, String name) {
            if (!(json instanceof Map<?, ?>)) {
                return null;
            }
            return ((Map<String, Object>) json).get(name);
        }

        private static final class Parser {
            private final String text;
            private int pos;

            Parser(String text) {
                this.text = text;
            }

            boolean end() {
                return pos >= text.length();
            }

            void skipWhitespace() {
                while (!end() && Character.isWhitespace(text.charAt(pos))) {
                    pos++;
                }
            }

            Object readValue() {
                skipWhitespace();
                if (end()) {
                    throw new IllegalArgumentException("Unexpected end of JSON");
                }
                char c = text.charAt(pos);
                if (c == '"') {
                    return readString();
                }
                if (c == '{') {
                    return readObject();
                }
                if (c == '[') {
                    return readArray();
                }
                if (c == 't' && text.startsWith("true", pos)) {
                    pos += 4;
                    return Boolean.TRUE;
                }
                if (c == 'f' && text.startsWith("false", pos)) {
                    pos += 5;
                    return Boolean.FALSE;
                }
                if (c == 'n' && text.startsWith("null", pos)) {
                    pos += 4;
                    return null;
                }
                return readNumber();
            }

            Map<String, Object> readObject() {
                expect('{');
                LinkedHashMap<String, Object> out = new LinkedHashMap<>();
                skipWhitespace();
                if (peek('}')) {
                    pos++;
                    return out;
                }
                while (true) {
                    skipWhitespace();
                    String key = readString();
                    skipWhitespace();
                    expect(':');
                    out.put(key, readValue());
                    skipWhitespace();
                    if (peek('}')) {
                        pos++;
                        return out;
                    }
                    expect(',');
                }
            }

            List<Object> readArray() {
                expect('[');
                ArrayList<Object> out = new ArrayList<>();
                skipWhitespace();
                if (peek(']')) {
                    pos++;
                    return out;
                }
                while (true) {
                    out.add(readValue());
                    skipWhitespace();
                    if (peek(']')) {
                        pos++;
                        return out;
                    }
                    expect(',');
                }
            }

            String readString() {
                expect('"');
                StringBuilder out = new StringBuilder();
                while (!end()) {
                    char c = text.charAt(pos++);
                    if (c == '"') {
                        return out.toString();
                    }
                    if (c != '\\') {
                        out.append(c);
                        continue;
                    }
                    if (end()) {
                        throw new IllegalArgumentException("Unterminated escape at " + pos);
                    }
                    char escaped = text.charAt(pos++);
                    switch (escaped) {
                        case '"':
                        case '\\':
                        case '/':
                            out.append(escaped);
                            break;
                        case 'b':
                            out.append('\b');
                            break;
                        case 'f':
                            out.append('\f');
                            break;
                        case 'n':
                            out.append('\n');
                            break;
                        case 'r':
                            out.append('\r');
                            break;
                        case 't':
                            out.append('\t');
                            break;
                        case 'u':
                            if (pos + 4 > text.length()) {
                                throw new IllegalArgumentException("Bad unicode escape at " + pos);
                            }
                            out.append((char) Integer.parseInt(text.substring(pos, pos + 4), 16));
                            pos += 4;
                            break;
                        default:
                            throw new IllegalArgumentException("Bad escape \\" + escaped + " at " + pos);
                    }
                }
                throw new IllegalArgumentException("Unterminated string");
            }

            Number readNumber() {
                int start = pos;
                while (!end()) {
                    char c = text.charAt(pos);
                    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                        pos++;
                    } else {
                        break;
                    }
                }
                String token = text.substring(start, pos);
                if (token.contains(".") || token.contains("e") || token.contains("E")) {
                    return Double.parseDouble(token);
                }
                return Long.parseLong(token);
            }

            boolean peek(char expected) {
                return !end() && text.charAt(pos) == expected;
            }

            void expect(char expected) {
                if (end() || text.charAt(pos) != expected) {
                    throw new IllegalArgumentException("Expected '" + expected + "' at " + pos);
                }
                pos++;
            }
        }
    }
}
