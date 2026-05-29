package GPULootSeedFinder;

import java.lang.reflect.Field;
import java.io.BufferedInputStream;
import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.zip.GZIPInputStream;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import kaptainwutax.featureutils.structure.generator.structure.VillageGenerator;
import kaptainwutax.mcutils.block.Block;
import kaptainwutax.mcutils.rand.ChunkRand;
import kaptainwutax.mcutils.state.Dimension;
import kaptainwutax.mcutils.util.block.BlockBox;
import kaptainwutax.mcutils.util.block.BlockDirection;
import kaptainwutax.mcutils.util.block.BlockMirror;
import kaptainwutax.mcutils.util.block.BlockRotation;
import kaptainwutax.mcutils.util.data.Pair;
import kaptainwutax.mcutils.util.data.Quad;
import kaptainwutax.mcutils.util.data.Triplet;
import kaptainwutax.mcutils.util.pos.BPos;
import kaptainwutax.mcutils.util.pos.CPos;
import kaptainwutax.mcutils.version.MCVersion;

final class VillageJigsawBuildingDetector {
    private static final Pattern POOL_ELEMENT = Pattern.compile(
        "\"element\"\\s*:\\s*\\{([\\s\\S]*?)\\}\\s*,\\s*\"weight\"\\s*:\\s*(\\d+)"
    );
    private static final Pattern LOCATION_FIELD = Pattern.compile("\"location\"\\s*:\\s*\"([^\"]+)\"");
    private static final Pattern PROJECTION_FIELD = Pattern.compile("\"projection\"\\s*:\\s*\"([^\"]+)\"");
    private static final Pattern FALLBACK_FIELD = Pattern.compile("\"fallback\"\\s*:\\s*\"([^\"]+)\"");
    private static final int MAX_DEPTH = 6;
    private static final int MAX_DISTANCE = 80;
    private static final List<String> ALL_STYLES = List.of("plains", "desert", "savanna", "snowy", "taiga");
    private static final Map<String, TemplatePool> POOL_CACHE = new HashMap<>();
    private static final Map<String, TemplateInfo> TEMPLATE_CACHE = new HashMap<>();
    private static final Map<String, BPos> STRUCTURE_SIZE = loadStructureSizes();

    private VillageJigsawBuildingDetector() {
    }

    static boolean villageHasAnyBuilding(
        String structure,
        String versionToken,
        long seed,
        int blockX,
        int blockZ,
        Path decompiledRoot,
        List<String> buildings
    ) {
        if (buildings.isEmpty()) {
            return true;
        }
        List<String> needles = new ArrayList<>();
        for (String building : buildings) {
            for (String needle : buildingNeedles(building)) {
                if (!needle.isEmpty()) {
                    needles.add(needle);
                }
            }
        }
        if (needles.isEmpty()) {
            return true;
        }

        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        MCVersion version = resolveVillageVersion(versionToken);
        for (String style : candidateStyles(structure, version, seed, blockX, blockZ)) {
            Detection result = simulateStyle(style, version, seed, chunkX, chunkZ, decompiledRoot, needles);
            if (result == Detection.MATCH || result == Detection.ZOMBIE_AND_MATCH) {
                return true;
            }
        }
        return false;
    }

    static boolean villageAbandoned(
        String structure,
        String versionToken,
        long seed,
        int blockX,
        int blockZ,
        Path decompiledRoot
    ) {
        int chunkX = Math.floorDiv(blockX, 16);
        int chunkZ = Math.floorDiv(blockZ, 16);
        MCVersion version = resolveVillageVersion(versionToken);
        for (String style : candidateStyles(structure, version, seed, blockX, blockZ)) {
            Detection result = simulateStyle(style, version, seed, chunkX, chunkZ, decompiledRoot, List.of());
            if (result == Detection.ZOMBIE || result == Detection.ZOMBIE_AND_MATCH) {
                return true;
            }
        }
        return false;
    }

    private enum Detection {
        NO_MATCH,
        NORMAL,
        ZOMBIE,
        ZOMBIE_AND_MATCH,
        MATCH
    }

    private record TemplateEntry(String name, int weight, VillageGenerator.PlacementBehaviour projection) {
    }

    private record TemplatePool(List<TemplateEntry> entries, String fallback) {
    }

    private record TemplateInfo(BPos size, List<JigsawTemplateBlock> jigsaws) {
    }

    private record JigsawTemplateBlock(
        String pool,
        String name,
        String target,
        String joint,
        String orientation,
        BPos pos,
        int selectionPriority,
        int placementPriority
    ) {
    }

    private record JigsawInfo(
        String pool,
        String name,
        String target,
        String joint,
        String orientation,
        BPos pos,
        BlockRotation rotation,
        int selectionPriority,
        int placementPriority
    ) {
        BlockDirection front() {
            if (orientation == null || orientation.isBlank()) {
                return BlockDirection.NORTH;
            }
            String[] parts = orientation.split("_");
            if (parts.length == 0 || parts[0].isBlank()) {
                return BlockDirection.NORTH;
            }
            BlockDirection direction = BlockDirection.fromString(parts[0]);
            if (direction == BlockDirection.UP || direction == BlockDirection.DOWN) {
                return direction;
            }
            return rotation.rotate(direction);
        }

        BlockDirection top() {
            if (orientation == null || orientation.isBlank()) {
                return BlockDirection.UP;
            }
            String[] parts = orientation.split("_");
            if (parts.length < 2 || parts[1].isBlank()) {
                return BlockDirection.UP;
            }
            BlockDirection direction = BlockDirection.fromString(parts[1]);
            if (direction == BlockDirection.UP || direction == BlockDirection.DOWN) {
                return direction;
            }
            return rotation.rotate(direction);
        }

        boolean rollable() {
            return "rollable".equals(normalizeId(joint));
        }
    }

    private static final class Piece {
        final String name;
        BPos pos;
        BlockBox box;
        final BlockRotation rotation;
        final VillageGenerator.PlacementBehaviour projection;
        final int depth;

        Piece(String name, BPos pos, BlockBox box, BlockRotation rotation, VillageGenerator.PlacementBehaviour projection, int depth) {
            this.name = normalizeTemplateName(name);
            this.pos = pos;
            this.box = box;
            this.rotation = rotation;
            this.projection = projection;
            this.depth = depth;
        }

        boolean rigid() {
            return projection == VillageGenerator.PlacementBehaviour.RIGID;
        }
    }

    private static Detection simulateStyle(
        String style,
        MCVersion version,
        long seed,
        int chunkX,
        int chunkZ,
        Path decompiledRoot,
        List<String> needles
    ) {
        TemplatePool startPool = loadPool(decompiledRoot, style + "/town_centers");
        if (startPool.entries().isEmpty()) {
            return Detection.NO_MATCH;
        }

        ChunkRand rand = new ChunkRand();
        rand.setCarverSeed(seed, chunkX, chunkZ, version);
        BlockRotation startRotation = BlockRotation.getRandom(rand);
        TemplateEntry startEntry = pickWeighted(startPool.entries(), rand);
        if (startEntry == null || "empty".equals(startEntry.name())) {
            return Detection.NO_MATCH;
        }

        BPos startSize = sizeOf(startEntry.name(), decompiledRoot);
        if (startSize == null) {
            return Detection.NO_MATCH;
        }
        BPos startPos = new CPos(chunkX, chunkZ).toBlockPos(0).add(0, 64, 0);
        BlockBox startBox = BlockBox.getBoundingBox(startPos, startRotation, BPos.ORIGIN, BlockMirror.NONE, startSize);
        int centerX = (startBox.minX + startBox.maxX) / 2;
        int centerZ = (startBox.minZ + startBox.maxZ) / 2;
        Piece startPiece = new Piece(startEntry.name(), startPos, startBox, startRotation, startEntry.projection(), 0);

        boolean zombie = isZombiePiece(startPiece.name);
        if (pieceMatches(startPiece.name, needles)) {
            return zombie ? Detection.ZOMBIE_AND_MATCH : Detection.MATCH;
        }

        TreeMap<Integer, ArrayDeque<Piece>> queue = new TreeMap<>(Comparator.reverseOrder());
        List<BlockBox> placed = new ArrayList<>();
        placed.add(startBox);
        processPiece(startPiece, centerX, centerZ, placed, queue, rand, decompiledRoot, needles);

        while (!queue.isEmpty()) {
            Map.Entry<Integer, ArrayDeque<Piece>> entry = queue.firstEntry();
            Piece source = entry.getValue().removeFirst();
            if (entry.getValue().isEmpty()) {
                queue.pollFirstEntry();
            }
            Detection result = processPiece(source, centerX, centerZ, placed, queue, rand, decompiledRoot, needles);
            if (result == Detection.ZOMBIE || result == Detection.ZOMBIE_AND_MATCH) {
                zombie = true;
            }
            if (result == Detection.MATCH || result == Detection.ZOMBIE_AND_MATCH) {
                return zombie ? Detection.ZOMBIE_AND_MATCH : Detection.MATCH;
            }
        }

        return zombie ? Detection.ZOMBIE : Detection.NORMAL;
    }

    private static Detection processPiece(
        Piece source,
        int centerX,
        int centerZ,
        List<BlockBox> placed,
        TreeMap<Integer, ArrayDeque<Piece>> queue,
        ChunkRand rand,
        Path decompiledRoot,
        List<String> needles
    ) {
        boolean zombie = false;
        for (JigsawInfo sourceJigsaw : shuffledJigsaws(source, rand, decompiledRoot)) {
            Piece child = tryPlaceOne(source, sourceJigsaw, centerX, centerZ, placed, rand, decompiledRoot);
            if (child == null) {
                continue;
            }
            if (isZombiePiece(child.name)) {
                zombie = true;
            }
            placed.add(child.box);
            if (pieceMatches(child.name, needles)) {
                return zombie ? Detection.ZOMBIE_AND_MATCH : Detection.MATCH;
            }
            if (child.depth <= MAX_DEPTH) {
                queue.computeIfAbsent(sourceJigsaw.placementPriority(), ignored -> new ArrayDeque<>()).addLast(child);
            }
        }
        return zombie ? Detection.ZOMBIE : Detection.NORMAL;
    }

    private static Piece tryPlaceOne(
        Piece source,
        JigsawInfo sourceJigsaw,
        int centerX,
        int centerZ,
        List<BlockBox> placed,
        ChunkRand rand,
        Path decompiledRoot
    ) {
        TemplatePool targetPool = loadPool(decompiledRoot, normalizePoolName(sourceJigsaw.pool()));
        TemplatePool fallbackPool = targetPool.fallback().isEmpty()
            ? new TemplatePool(List.of(), "")
            : loadPool(decompiledRoot, targetPool.fallback());
        List<TemplateEntry> candidates = new ArrayList<>();
        if (source.depth != MAX_DEPTH) {
            candidates.addAll(shuffledEntries(targetPool.entries(), rand));
        }
        candidates.addAll(shuffledEntries(fallbackPool.entries(), rand));

        BPos sourceJigsawPos = sourceJigsaw.pos();
        BlockDirection sourceDirection = sourceJigsaw.front();
        BPos targetJigsawPos = sourceJigsawPos.relative(sourceDirection);
        int sourceJigsawLocalY = sourceJigsawPos.getY() - source.box.minY;

        for (TemplateEntry targetEntry : candidates) {
            if ("empty".equals(targetEntry.name())) {
                return null;
            }
            BPos targetSize = sizeOf(targetEntry.name(), decompiledRoot);
            if (targetSize == null) {
                continue;
            }

            for (BlockRotation targetRotation : BlockRotation.getShuffled(rand)) {
                    List<JigsawInfo> targetJigsaws = shuffledJigsaws(
                        targetEntry.name(),
                        BPos.ORIGIN,
                        targetRotation,
                        rand,
                        decompiledRoot
                    );
                for (JigsawInfo targetJigsaw : targetJigsaws) {
                    if (!canAttach(sourceJigsaw, targetJigsaw)) {
                        continue;
                    }
                    BPos targetLocalPos = targetJigsaw.pos();
                    BPos rawTargetPos = targetJigsawPos.subtract(targetLocalPos);
                    BlockBox rawTargetBox = BlockBox.getBoundingBox(rawTargetPos, targetRotation, BPos.ORIGIN, BlockMirror.NONE, targetSize);
                    int targetJigsawLocalY = targetLocalPos.getY();
                    int stepY = directionStepY(sourceDirection);
                    int deltaY = sourceJigsawLocalY - targetJigsawLocalY + stepY;
                    int targetBoxY = source.rigid() && targetEntry.projection() == VillageGenerator.PlacementBehaviour.RIGID
                        ? source.box.minY + deltaY
                        : sourceJigsawPos.getY() - targetJigsawLocalY;
                    int yOffset = targetBoxY - rawTargetBox.minY;
                    BlockBox targetBox = rawTargetBox.offset(0, yOffset, 0);
                    BPos targetPos = rawTargetPos.add(0, yOffset, 0);

                    if (!withinVillageRange(targetBox, centerX, centerZ) || overlapsPlaced(targetBox, placed)) {
                        continue;
                    }
                    return new Piece(
                        targetEntry.name(),
                        targetPos,
                        targetBox,
                        targetRotation,
                        targetEntry.projection(),
                        source.depth + 1
                    );
                }
            }
        }
        return null;
    }

    private static boolean canAttach(JigsawInfo source, JigsawInfo target) {
        String sourceTarget = normalizeId(source.target());
        String targetName = normalizeId(target.name());
        if (!sourceTarget.equals(targetName)) {
            return false;
        }
        return source.front() == opposite(target.front()) && (source.rollable() || source.top() == target.top());
    }

    private static BlockDirection opposite(BlockDirection direction) {
        if (direction == BlockDirection.NORTH) {
            return BlockDirection.SOUTH;
        }
        if (direction == BlockDirection.SOUTH) {
            return BlockDirection.NORTH;
        }
        if (direction == BlockDirection.WEST) {
            return BlockDirection.EAST;
        }
        if (direction == BlockDirection.EAST) {
            return BlockDirection.WEST;
        }
        if (direction == BlockDirection.UP) {
            return BlockDirection.DOWN;
        }
        return BlockDirection.UP;
    }

    private static int directionStepY(BlockDirection direction) {
        if (direction == BlockDirection.UP) {
            return 1;
        }
        if (direction == BlockDirection.DOWN) {
            return -1;
        }
        return 0;
    }

    private static boolean overlapsPlaced(BlockBox target, List<BlockBox> placed) {
        for (BlockBox existing : placed) {
            if (overlapsDeflated(existing, target)) {
                return true;
            }
        }
        return false;
    }

    private static boolean overlapsDeflated(BlockBox existing, BlockBox target) {
        return existing.minX < target.maxX + 0.75 && existing.maxX + 1.0 > target.minX + 0.25
            && existing.minY < target.maxY + 0.75 && existing.maxY + 1.0 > target.minY + 0.25
            && existing.minZ < target.maxZ + 0.75 && existing.maxZ + 1.0 > target.minZ + 0.25;
    }

    private static boolean withinVillageRange(BlockBox box, int centerX, int centerZ) {
        return box.minX >= centerX - MAX_DISTANCE
            && box.maxX <= centerX + MAX_DISTANCE
            && box.minZ >= centerZ - MAX_DISTANCE
            && box.maxZ <= centerZ + MAX_DISTANCE;
    }

    private static List<JigsawInfo> shuffledJigsaws(Piece piece, ChunkRand rand, Path decompiledRoot) {
        return shuffledJigsaws(piece.name, piece.pos, piece.rotation, rand, decompiledRoot);
    }

    private static List<JigsawInfo> shuffledJigsaws(
        String templateName,
        BPos offset,
        BlockRotation rotation,
        ChunkRand rand,
        Path decompiledRoot
    ) {
        List<JigsawTemplateBlock> raw = jigsawBlocks(templateName, decompiledRoot);
        if (raw.isEmpty()) {
            return List.of();
        }
        ArrayList<JigsawInfo> out = new ArrayList<>(raw.size());
        for (JigsawTemplateBlock block : raw) {
            BPos pos = block.pos().transform(BlockMirror.NONE, rotation, BPos.ORIGIN).add(offset);
            out.add(new JigsawInfo(
                block.pool(),
                block.name(),
                block.target(),
                block.joint(),
                block.orientation(),
                pos,
                rotation,
                block.selectionPriority(),
                block.placementPriority()
            ));
        }
        rand.shuffle(out);
        out.sort(Comparator.comparingInt(JigsawInfo::selectionPriority).reversed());
        return out;
    }

    private static List<JigsawTemplateBlock> jigsawBlocks(String templateName, Path decompiledRoot) {
        return templateInfo(templateName, decompiledRoot).jigsaws();
    }

    private static TemplatePool loadPool(Path decompiledRoot, String poolName) {
        String normalizedPool = normalizePoolName(poolName);
        if (normalizedPool.isEmpty() || "empty".equals(normalizedPool)) {
            return new TemplatePool(List.of(), "");
        }
        String cacheKey = decompiledRoot.toAbsolutePath() + "|" + normalizedPool;
        TemplatePool cached = POOL_CACHE.get(cacheKey);
        if (cached != null) {
            return cached;
        }
        TemplatePool loaded = loadPoolUncached(decompiledRoot, normalizedPool);
        POOL_CACHE.put(cacheKey, loaded);
        return loaded;
    }

    private static TemplatePool loadPoolUncached(Path decompiledRoot, String poolName) {
        Path path = decompiledRoot.resolve("resources")
            .resolve("data")
            .resolve("minecraft")
            .resolve("worldgen")
            .resolve("template_pool")
            .resolve("village");
        for (String part : poolName.split("/")) {
            path = path.resolve(part);
        }
        path = path.resolveSibling(path.getFileName() + ".json");

        ArrayList<TemplateEntry> entries = new ArrayList<>();
        String fallback = "";
        try {
            if (!Files.isRegularFile(path)) {
                return new TemplatePool(List.of(), "");
            }
            String json = Files.readString(path, StandardCharsets.UTF_8);
            Matcher fallbackMatcher = FALLBACK_FIELD.matcher(json);
            if (fallbackMatcher.find()) {
                fallback = normalizePoolName(fallbackMatcher.group(1));
            }
            Matcher matcher = POOL_ELEMENT.matcher(json);
            while (matcher.find()) {
                String element = matcher.group(1);
                String location = matchField(LOCATION_FIELD, element);
                if (location.isEmpty()) {
                    continue;
                }
                int weight = Integer.parseInt(matcher.group(2));
                if (weight <= 0) {
                    continue;
                }
                String projection = normalizeId(matchField(PROJECTION_FIELD, element));
                VillageGenerator.PlacementBehaviour behaviour = "terrain_matching".equals(projection)
                    ? VillageGenerator.PlacementBehaviour.TERRAIN_MATCHING
                    : VillageGenerator.PlacementBehaviour.RIGID;
                entries.add(new TemplateEntry(normalizeTemplateName(location), weight, behaviour));
            }
        } catch (Exception ignored) {
            entries.clear();
            fallback = "";
        }
        return new TemplatePool(List.copyOf(entries), fallback);
    }

    private static String matchField(Pattern pattern, String text) {
        Matcher matcher = pattern.matcher(text);
        return matcher.find() ? matcher.group(1) : "";
    }

    private static List<TemplateEntry> shuffledEntries(List<TemplateEntry> entries, ChunkRand rand) {
        ArrayList<TemplateEntry> out = new ArrayList<>();
        for (TemplateEntry entry : entries) {
            for (int i = 0; i < entry.weight(); ++i) {
                out.add(entry);
            }
        }
        rand.shuffle(out);
        return out;
    }

    private static TemplateEntry pickWeighted(List<TemplateEntry> entries, ChunkRand rand) {
        int total = 0;
        for (TemplateEntry entry : entries) {
            total += Math.max(0, entry.weight());
        }
        if (total <= 0) {
            return null;
        }
        int roll = rand.nextInt(total);
        for (TemplateEntry entry : entries) {
            roll -= Math.max(0, entry.weight());
            if (roll < 0) {
                return entry;
            }
        }
        return entries.get(entries.size() - 1);
    }

    private static BPos sizeOf(String templateName) {
        return sizeOf(templateName, null);
    }

    private static BPos sizeOf(String templateName, Path decompiledRoot) {
        TemplateInfo info = templateInfo(templateName, decompiledRoot);
        if (info.size() != null) {
            return info.size();
        }
        return STRUCTURE_SIZE.get(normalizeTemplateName(templateName));
    }

    private static TemplateInfo templateInfo(String templateName) {
        return templateInfo(templateName, null);
    }

    private static TemplateInfo templateInfo(String templateName, Path rootOverride) {
        String normalized = normalizeTemplateName(templateName);
        Path root = rootOverride == null ? null : rootOverride;
        String rootKey = root == null ? "" : root.toAbsolutePath().toString();
        String key = rootKey + "|" + normalized;
        TemplateInfo cached = TEMPLATE_CACHE.get(key);
        if (cached != null) {
            return cached;
        }
        TemplateInfo loaded = loadTemplateInfo(root == null ? null : root, normalized);
        TEMPLATE_CACHE.put(key, loaded);
        return loaded;
    }

    private static TemplateInfo loadTemplateInfo(Path root, String templateName) {
        Path base = root == null ? null : root.resolve("resources")
            .resolve("data")
            .resolve("minecraft")
            .resolve("structure")
            .resolve("village");
        if (base == null) {
            base = Path.of("resources", "data", "minecraft", "structure", "village");
        }
        Path path = base;
        for (String part : normalizeTemplateName(templateName).split("/")) {
            path = path.resolve(part);
        }
        path = path.resolveSibling(path.getFileName() + ".nbt");
        try {
            if (!Files.isRegularFile(path)) {
                return fallbackTemplateInfo(templateName);
            }
            Object rootTag = readNbt(path);
            if (!(rootTag instanceof Map<?, ?> rootMap)) {
                return fallbackTemplateInfo(templateName);
            }
            BPos size = parseSize(rootMap.get("size"));
            List<?> palette = rootMap.get("palette") instanceof List<?> p ? p : List.of();
            List<?> blocks = rootMap.get("blocks") instanceof List<?> b ? b : List.of();
            ArrayList<JigsawTemplateBlock> jigsaws = new ArrayList<>();
            for (Object blockObj : blocks) {
                if (!(blockObj instanceof Map<?, ?> blockMap)) {
                    continue;
                }
                int stateIndex = intValue(blockMap.get("state"), -1);
                if (stateIndex < 0 || stateIndex >= palette.size()) {
                    continue;
                }
                if (!(palette.get(stateIndex) instanceof Map<?, ?> stateMap)) {
                    continue;
                }
                if (!"minecraft:jigsaw".equals(stringValue(stateMap.get("Name")))) {
                    continue;
                }
                if (!(blockMap.get("nbt") instanceof Map<?, ?> nbtMap)) {
                    continue;
                }
                BPos pos = parseSize(blockMap.get("pos"));
                if (pos == null) {
                    continue;
                }
                String orientation = "";
                if (stateMap.get("Properties") instanceof Map<?, ?> props) {
                    orientation = normalizeId(stringValue(props.get("orientation")));
                }
                jigsaws.add(new JigsawTemplateBlock(
                    normalizePoolName(stringValue(nbtMap.get("pool"))),
                    normalizeId(stringValue(nbtMap.get("name"))),
                    normalizeId(stringValue(nbtMap.get("target"))),
                    normalizeId(stringValue(nbtMap.get("joint"))),
                    orientation,
                    pos,
                    intValue(nbtMap.get("selection_priority"), 0),
                    intValue(nbtMap.get("placement_priority"), 0)
                ));
            }
            return new TemplateInfo(size, List.copyOf(jigsaws));
        } catch (Exception ignored) {
            return fallbackTemplateInfo(templateName);
        }
    }

    private static TemplateInfo fallbackTemplateInfo(String templateName) {
        BPos size = STRUCTURE_SIZE.get(normalizeTemplateName(templateName));
        return new TemplateInfo(size, List.of());
    }

    private static BPos parseSize(Object raw) {
        if (!(raw instanceof List<?> list) || list.size() < 3) {
            return null;
        }
        return new BPos(intValue(list.get(0), 0), intValue(list.get(1), 0), intValue(list.get(2), 0));
    }

    private static String stringValue(Object raw) {
        return raw == null ? "" : raw.toString();
    }

    private static int intValue(Object raw, int fallback) {
        if (raw instanceof Number number) {
            return number.intValue();
        }
        try {
            return raw == null ? fallback : Integer.parseInt(raw.toString());
        } catch (NumberFormatException ex) {
            return fallback;
        }
    }

    private static Object readNbt(Path path) throws IOException {
        byte[] bytes = Files.readAllBytes(path);
        try (InputStream raw = new BufferedInputStream(new ByteArrayInputStream(bytes));
             InputStream in = isGzip(bytes) ? new GZIPInputStream(raw) : raw;
             DataInputStream data = new DataInputStream(in)) {
            int rootType = data.readUnsignedByte();
            if (rootType == 0) {
                return Map.of();
            }
            readNbtString(data);
            return readNbtPayload(data, rootType);
        }
    }

    private static boolean isGzip(byte[] bytes) {
        return bytes.length >= 2 && (bytes[0] & 0xFF) == 0x1F && (bytes[1] & 0xFF) == 0x8B;
    }

    private static Object readNbtPayload(DataInputStream data, int type) throws IOException {
        return switch (type) {
            case 1 -> data.readByte();
            case 2 -> data.readShort();
            case 3 -> data.readInt();
            case 4 -> data.readLong();
            case 5 -> data.readFloat();
            case 6 -> data.readDouble();
            case 7 -> readNbtByteArray(data);
            case 8 -> readNbtString(data);
            case 9 -> readNbtList(data);
            case 10 -> readNbtCompound(data);
            case 11 -> readNbtIntArray(data);
            case 12 -> readNbtLongArray(data);
            default -> throw new EOFException("Unsupported NBT tag type " + type);
        };
    }

    private static byte[] readNbtByteArray(DataInputStream data) throws IOException {
        int length = data.readInt();
        byte[] out = new byte[Math.max(0, length)];
        data.readFully(out);
        return out;
    }

    private static String readNbtString(DataInputStream data) throws IOException {
        int length = data.readUnsignedShort();
        byte[] bytes = new byte[length];
        data.readFully(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static List<Object> readNbtList(DataInputStream data) throws IOException {
        int elementType = data.readUnsignedByte();
        int length = data.readInt();
        ArrayList<Object> out = new ArrayList<>(Math.max(0, length));
        for (int i = 0; i < length; ++i) {
            out.add(readNbtPayload(data, elementType));
        }
        return out;
    }

    private static Map<String, Object> readNbtCompound(DataInputStream data) throws IOException {
        HashMap<String, Object> out = new HashMap<>();
        while (true) {
            int type = data.readUnsignedByte();
            if (type == 0) {
                return out;
            }
            String name = readNbtString(data);
            out.put(name, readNbtPayload(data, type));
        }
    }

    private static int[] readNbtIntArray(DataInputStream data) throws IOException {
        int length = data.readInt();
        int[] out = new int[Math.max(0, length)];
        for (int i = 0; i < out.length; ++i) {
            out[i] = data.readInt();
        }
        return out;
    }

    private static long[] readNbtLongArray(DataInputStream data) throws IOException {
        int length = data.readInt();
        long[] out = new long[Math.max(0, length)];
        for (int i = 0; i < out.length; ++i) {
            out[i] = data.readLong();
        }
        return out;
    }

    @SuppressWarnings("unchecked")
    private static Map<String, BPos> loadStructureSizes() {
        try {
            Field field = VillageGenerator.class.getDeclaredField("STRUCTURE_SIZE");
            field.setAccessible(true);
            Object value = field.get(null);
            if (value instanceof Map<?, ?>) {
                HashMap<String, BPos> out = new HashMap<>();
                for (Map.Entry<?, ?> entry : ((Map<?, ?>) value).entrySet()) {
                    if (entry.getKey() instanceof String && entry.getValue() instanceof BPos) {
                        out.put(normalizeTemplateName((String) entry.getKey()), (BPos) entry.getValue());
                    }
                }
                return Map.copyOf(out);
            }
        } catch (Exception ignored) {
        }
        return Map.of();
    }

    private static List<String> candidateStyles(String structure, MCVersion version, long seed, int blockX, int blockZ) {
        String explicit = explicitVillageStyle(structure);
        if (!explicit.isEmpty()) {
            return List.of(explicit);
        }

        return ALL_STYLES;
    }

    private static String explicitVillageStyle(String structure) {
        String s = normalizeId(structure);
        if (!s.startsWith("village_")) {
            return "";
        }
        String tail = s.substring("village_".length());
        return ALL_STYLES.contains(tail) ? tail : "";
    }

    private static String styleFromTemplate(String templateName) {
        String s = normalizeTemplateName(templateName);
        if (s.startsWith("desert/")) {
            return "desert";
        }
        if (s.startsWith("savanna/")) {
            return "savanna";
        }
        if (s.startsWith("snowy/")) {
            return "snowy";
        }
        if (s.startsWith("taiga/")) {
            return "taiga";
        }
        return "plains";
    }

    private static boolean pieceMatches(String pieceName, List<String> needles) {
        if (needles.isEmpty()) {
            return false;
        }
        String piece = normalizeTemplateName(pieceName);
        for (String needle : needles) {
            if (!needle.isEmpty() && piece.contains(needle)) {
                return true;
            }
        }
        return false;
    }

    private static boolean isZombiePiece(String pieceName) {
        return normalizeTemplateName(pieceName).contains("/zombie/");
    }

    private static boolean canExpandFrom(String pieceName) {
        String piece = normalizeTemplateName(pieceName);
        return piece.contains("/town_centers/") || piece.contains("/streets/");
    }

    private static String stripZombie(String templateName) {
        return normalizeTemplateName(templateName).replace("/zombie/", "/");
    }

    private static List<String> buildingNeedles(String building) {
        String b = normalizeId(building);
        if (b.startsWith("village_")) {
            b = b.substring("village_".length());
        }
        return switch (b) {
            case "blacksmith", "weaponsmith" -> List.of("weaponsmith", "weapon_smith");
            case "toolsmith", "tool_smith" -> List.of("tool_smith");
            case "butcher" -> List.of("butcher");
            case "cartographer" -> List.of("cartographer");
            case "fisher" -> List.of("fisher");
            case "fletcher" -> List.of("fletcher");
            case "mason" -> List.of("mason");
            case "shepherd" -> List.of("shepherd");
            case "tannery", "leatherworker" -> List.of("tannery");
            case "temple", "cleric" -> List.of("temple");
            case "armorer" -> List.of("armorer");
            default -> b.isEmpty() ? List.of() : List.of(b);
        };
    }

    private static MCVersion resolveVillageVersion(String token) {
        String t = token == null ? "" : token.trim().toLowerCase(Locale.ROOT);
        if ("1.17".equals(t)) {
            return MCVersion.v1_17;
        }
        if ("1.17.1".equals(t)) {
            return MCVersion.v1_17_1;
        }
        return MCVersion.latest();
    }

    private static String normalizeId(String value) {
        String out = value == null ? "" : value.trim().toLowerCase(Locale.ROOT);
        if (out.startsWith("minecraft:")) {
            out = out.substring("minecraft:".length());
        }
        return out.replace('-', '_').replace(' ', '_');
    }

    private static String normalizePoolName(String value) {
        String out = normalizeId(value).replace('\\', '/');
        if (out.startsWith("village/")) {
            out = out.substring("village/".length());
        }
        return out;
    }

    private static String normalizeTemplateName(String value) {
        String out = normalizePoolName(value);
        while (out.startsWith("/")) {
            out = out.substring(1);
        }
        return out;
    }
}
