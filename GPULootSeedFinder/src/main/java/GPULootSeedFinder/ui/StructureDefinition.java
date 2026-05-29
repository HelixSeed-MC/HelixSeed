package GPULootSeedFinder.ui;

import java.util.ArrayList;
import java.util.List;

public class StructureDefinition {
    public final String id;
    public final String displayName;
    public final String lootTableId;
    public final Integer decorationSalt;
    public final String textureUrl;

    public StructureDefinition(String id, String displayName, String lootTableId, Integer decorationSalt, String textureUrl) {
        this.id = id;
        this.displayName = displayName;
        this.lootTableId = lootTableId;
        this.decorationSalt = decorationSalt;
        this.textureUrl = textureUrl;
    }

    public Integer resolveDecorationSalt() {
        return this.decorationSalt;
    }

    public static List<StructureDefinition> defaults() {
        List<StructureDefinition> defs = new ArrayList<>();
        defs.add(
            new StructureDefinition(
                "desert_pyramid",
                "Desert Pyramid",
                "chests/desert_pyramid",
                40003,
                "https://raw.githubusercontent.com/PrismarineJS/minecraft-assets/master/data/1.17.1/blocks/chiseled_sandstone.png"
            )
        );
        defs.add(
            new StructureDefinition(
                "buried_treasure",
                "Buried Treasure",
                "chests/buried_treasure",
                30001,
                "https://raw.githubusercontent.com/PrismarineJS/minecraft-assets/master/data/1.17.1/items/heart_of_the_sea.png"
            )
        );
        defs.add(
            new StructureDefinition(
                "shipwreck_supply",
                "Shipwreck Supply",
                "chests/shipwreck_supply",
                40006,
                "https://raw.githubusercontent.com/PrismarineJS/minecraft-assets/master/data/1.17.1/blocks/oak_planks.png"
            )
        );
        defs.add(
            new StructureDefinition(
                "shipwreck_map",
                "Shipwreck Map",
                "chests/shipwreck_map",
                40006,
                "https://raw.githubusercontent.com/PrismarineJS/minecraft-assets/master/data/1.17.1/items/filled_map.png"
            )
        );
        defs.add(
            new StructureDefinition(
                "shipwreck_treasure",
                "Shipwreck Treasure",
                "chests/shipwreck_treasure",
                40006,
                "https://raw.githubusercontent.com/PrismarineJS/minecraft-assets/master/data/1.17.1/items/emerald.png"
            )
        );
        return defs;
    }
}
