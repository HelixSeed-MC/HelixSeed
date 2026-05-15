package GPULootSeedFinder;

import GPULootSeedFinder.ui.StructureLootSelectorFrame;
import javax.swing.SwingUtilities;

public class Main {
    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            StructureLootSelectorFrame frame = new StructureLootSelectorFrame();
            frame.setVisible(true);
        });
    }
}

