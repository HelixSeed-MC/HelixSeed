package GPULootSeedFinder.ui;

import GPULootSeedFinder.loot12111.ExactLootTableLibrary;
import GPULootSeedFinder.util.Reverser;
import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Dimension;
import java.awt.FlowLayout;
import java.awt.Font;
import java.awt.GridLayout;
import java.awt.Image;
import java.awt.event.InputEvent;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.nio.file.Files;
import java.nio.file.Path;
import java.text.DecimalFormat;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.stream.Collectors;
import javax.imageio.ImageIO;
import javax.swing.BorderFactory;
import javax.swing.ImageIcon;
import javax.swing.JButton;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JSpinner;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.SpinnerNumberModel;
import javax.swing.SwingWorker;
import kaptainwutax.mcutils.util.pos.CPos;
import kaptainwutax.mcutils.version.MCVersion;

public class StructureLootSelectorFrame extends JFrame {
    private final List<StructureDefinition> definitions = StructureDefinition.defaults();
    private final Map<String, JButton> structureButtons = new LinkedHashMap<>();
    private final Set<String> selected = new LinkedHashSet<>();
    private final TextureDownloader textureDownloader = new TextureDownloader();

    private final JTextArea infoArea = new JTextArea();
    private final JTextArea positionArea = new JTextArea();
    private final JTextField lootSeedsField = new JTextField();
    private final JSpinner radiusSpinner = new JSpinner(new SpinnerNumberModel(10, 1, 1000, 1));

    private static final String LOOT_VERSION = ExactLootTableLibrary.TARGET_VERSION;
    private static final MCVersion REVERSER_VERSION = MCVersion.v1_17_1;

    public StructureLootSelectorFrame() {
        super("GPULootSeedFinder - 26.1.1 / 26.1.2 Structure Loot");
        setDefaultCloseOperation(JFrame.DISPOSE_ON_CLOSE);
        setSize(1100, 760);
        setMinimumSize(new Dimension(920, 620));
        setLocationRelativeTo(null);
        setLayout(new BorderLayout(10, 10));

        JPanel leftPanel = buildStructurePanel();
        JPanel rightPanel = buildDetailPanel();

        add(leftPanel, BorderLayout.WEST);
        add(rightPanel, BorderLayout.CENTER);

        if (!definitions.isEmpty()) {
            selected.add(definitions.get(0).id);
            refreshSelectionStyles();
            refreshLootInfo();
        }
    }

    private JPanel buildStructurePanel() {
        JPanel wrapper = new JPanel(new BorderLayout(8, 8));
        wrapper.setBorder(BorderFactory.createEmptyBorder(10, 10, 10, 10));
        wrapper.setPreferredSize(new Dimension(340, 700));

        JLabel title = new JLabel("Structures (Ctrl+Click = Multi-select)");
        title.setFont(title.getFont().deriveFont(Font.BOLD, 14f));
        wrapper.add(title, BorderLayout.NORTH);

        JPanel buttonsPanel = new JPanel(new GridLayout(0, 1, 6, 6));
        for (StructureDefinition def : definitions) {
            JButton button = new JButton(def.displayName);
            button.setHorizontalAlignment(JButton.LEFT);
            button.setFocusable(false);
            button.setBorder(BorderFactory.createEmptyBorder(8, 8, 8, 8));
            button.addMouseListener(new MouseAdapter() {
                @Override
                public void mousePressed(MouseEvent e) {
                    boolean ctrl = (e.getModifiersEx() & InputEvent.CTRL_DOWN_MASK) != 0;
                    handleStructureClick(def, ctrl);
                }
            });
            structureButtons.put(def.id, button);
            loadTextureIcon(def, button);
            buttonsPanel.add(button);
        }
        wrapper.add(new JScrollPane(buttonsPanel), BorderLayout.CENTER);

        JPanel controls = new JPanel(new FlowLayout(FlowLayout.LEFT));
        JButton downloadSelected = new JButton("Download Textures");
        downloadSelected.addActionListener(e -> downloadTexturesForSelection());
        controls.add(downloadSelected);
        wrapper.add(controls, BorderLayout.SOUTH);
        return wrapper;
    }

    private JPanel buildDetailPanel() {
        JPanel root = new JPanel(new BorderLayout(8, 8));
        root.setBorder(BorderFactory.createEmptyBorder(10, 0, 10, 10));

        infoArea.setEditable(false);
        infoArea.setLineWrap(true);
        infoArea.setWrapStyleWord(true);
        positionArea.setEditable(false);
        positionArea.setLineWrap(true);
        positionArea.setWrapStyleWord(true);

        JPanel top = new JPanel(new BorderLayout(8, 8));
        top.add(new JLabel("Selected Structures: exact 26.1.1 / 26.1.2 loot preview + salt metadata"), BorderLayout.NORTH);
        top.add(new JScrollPane(infoArea), BorderLayout.CENTER);

        JPanel bottom = new JPanel(new BorderLayout(8, 8));
        JPanel controls = new JPanel(new FlowLayout(FlowLayout.LEFT));
        lootSeedsField.setPreferredSize(new Dimension(420, 26));
        lootSeedsField.setToolTipText("Comma-separated loot seeds, e.g. 1234,5678");
        controls.add(new JLabel("Loot seeds:"));
        controls.add(lootSeedsField);
        controls.add(new JLabel("Chunk radius:"));
        controls.add(radiusSpinner);
        JButton resolveBtn = new JButton("Resolve Positions");
        resolveBtn.addActionListener(e -> resolvePositionsForSelection());
        controls.add(resolveBtn);

        bottom.add(controls, BorderLayout.NORTH);
        bottom.add(new JScrollPane(positionArea), BorderLayout.CENTER);

        root.add(top, BorderLayout.NORTH);
        root.add(bottom, BorderLayout.CENTER);
        return root;
    }

    private void handleStructureClick(StructureDefinition def, boolean ctrlHeld) {
        if (ctrlHeld) {
            if (selected.contains(def.id)) {
                selected.remove(def.id);
            } else {
                selected.add(def.id);
            }
        } else {
            selected.clear();
            selected.add(def.id);
        }
        refreshSelectionStyles();
        refreshLootInfo();
    }

    private void refreshSelectionStyles() {
        for (StructureDefinition def : definitions) {
            JButton button = structureButtons.get(def.id);
            if (button == null) {
                continue;
            }
            boolean on = selected.contains(def.id);
            if (on) {
                button.setBackground(new Color(44, 118, 255));
                button.setForeground(Color.WHITE);
                button.setOpaque(true);
            } else {
                button.setBackground(null);
                button.setForeground(Color.BLACK);
                button.setOpaque(false);
            }
        }
    }

    private void refreshLootInfo() {
        if (selected.isEmpty()) {
            infoArea.setText("No structure selected.");
            return;
        }

        StringBuilder sb = new StringBuilder();
        for (StructureDefinition def : selectedDefinitions()) {
            sb.append("[").append(def.displayName).append("]").append('\n');
            Integer salt = def.resolveDecorationSalt();
            if (salt != null) {
                sb.append("Decoration salt: ").append(salt).append('\n');
            } else {
                sb.append("Decoration salt: n/a (not configured)").append('\n');
            }
            sb.append("Loot table: ").append(def.lootTableId).append(" (").append(LOOT_VERSION).append(")").append('\n');
            try {
                ExactLootTableLibrary.LootRollResult preview = ExactLootTableLibrary.rollTable(def.lootTableId, 1L);
                String previewText = preview.slots().stream()
                    .filter(slot -> !slot.isEmpty())
                    .map(slot -> slot.count() + "x " + shortItemName(slot.itemId()))
                    .limit(10)
                    .collect(Collectors.joining(", "));
                if (previewText.isBlank()) {
                    previewText = "(empty)";
                }
                sb.append("Loot sample (seed=1): ").append(previewText).append('\n');
            } catch (Exception ex) {
                sb.append("Loot sample unavailable: ").append(ex.getMessage()).append('\n');
            }
            Path texturePath = textureDownloader.getTexturePath(def);
            sb.append("Texture: ").append(Files.exists(texturePath) ? texturePath.toString() : "not downloaded").append('\n');
            if (salt == null) {
                sb.append("Position reverse: unavailable in the legacy salt bridge").append('\n');
            } else {
                sb.append("Position reverse: salt bridge via legacy featureutils math").append('\n');
            }
            sb.append('\n');
        }
        infoArea.setText(sb.toString());
        infoArea.setCaretPosition(0);
    }

    private void resolvePositionsForSelection() {
        List<Long> lootSeeds;
        try {
            lootSeeds = parseLootSeeds();
        } catch (IllegalArgumentException ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage(), "Invalid loot seeds", JOptionPane.ERROR_MESSAGE);
            return;
        }
        if (lootSeeds.isEmpty()) {
            JOptionPane.showMessageDialog(this, "Enter at least one loot seed.", "No seeds", JOptionPane.WARNING_MESSAGE);
            return;
        }
        List<StructureDefinition> defs = selectedDefinitions();
        if (defs.isEmpty()) {
            JOptionPane.showMessageDialog(this, "Select at least one structure first.", "No structure", JOptionPane.WARNING_MESSAGE);
            return;
        }
        int radius = (Integer) radiusSpinner.getValue();
        positionArea.setText("Resolving positions...\n");

        SwingWorker<String, Void> worker = new SwingWorker<>() {
            @Override
            protected String doInBackground() {
                StringBuilder sb = new StringBuilder();
                DecimalFormat df = new DecimalFormat("#,###");
                for (StructureDefinition def : defs) {
                    Integer salt = def.resolveDecorationSalt();
                    sb.append("[").append(def.displayName).append("]").append('\n');
                    if (salt == null) {
                        sb.append("Positions unavailable: no configured decoration salt.\n\n");
                        continue;
                    }
                    Map<Long, CPos> map = Reverser.getStructureSeedsFromLootSeeds(lootSeeds, radius, REVERSER_VERSION, salt);
                    sb.append("Found ").append(df.format(map.size())).append(" structure seeds").append('\n');
                    int shown = 0;
                    for (Map.Entry<Long, CPos> e : map.entrySet()) {
                        if (shown >= 30) {
                            sb.append("... (truncated)\n");
                            break;
                        }
                        CPos pos = e.getValue();
                        sb.append("seed=").append(e.getKey())
                            .append("  chunk=").append(pos)
                            .append('\n');
                        shown++;
                    }
                    sb.append('\n');
                }
                return sb.toString();
            }

            @Override
            protected void done() {
                try {
                    positionArea.setText(get());
                    positionArea.setCaretPosition(0);
                } catch (Exception ex) {
                    positionArea.setText("Failed to resolve positions: " + ex.getMessage());
                }
            }
        };
        worker.execute();
    }

    private void downloadTexturesForSelection() {
        List<StructureDefinition> defs = selectedDefinitions();
        if (defs.isEmpty()) {
            defs = definitions;
        }
        infoArea.append("\nDownloading textures...\n");

        List<StructureDefinition> finalDefs = defs;
        SwingWorker<String, Void> worker = new SwingWorker<>() {
            @Override
            protected String doInBackground() {
                StringBuilder sb = new StringBuilder();
                for (StructureDefinition def : finalDefs) {
                    try {
                        Path path = textureDownloader.ensureTextureDownloaded(def);
                        sb.append("OK  ").append(def.displayName).append(" -> ").append(path).append('\n');
                    } catch (Exception ex) {
                        sb.append("ERR ").append(def.displayName).append(" -> ").append(ex.getMessage()).append('\n');
                    }
                }
                return sb.toString();
            }

            @Override
            protected void done() {
                try {
                    String out = get();
                    for (StructureDefinition def : finalDefs) {
                        JButton btn = structureButtons.get(def.id);
                        if (btn != null) {
                            loadTextureIcon(def, btn);
                        }
                    }
                    refreshLootInfo();
                    infoArea.append('\n' + out);
                } catch (Exception ex) {
                    infoArea.append("\nTexture download failed: " + ex.getMessage() + "\n");
                }
            }
        };
        worker.execute();
    }

    private List<Long> parseLootSeeds() {
        String raw = lootSeedsField.getText().trim();
        if (raw.isBlank()) {
            return List.of();
        }
        String[] parts = raw.split("[,\\s]+");
        List<Long> out = new ArrayList<>();
        for (String p : parts) {
            if (p.isBlank()) {
                continue;
            }
            try {
                out.add(Long.parseLong(p));
            } catch (NumberFormatException ex) {
                throw new IllegalArgumentException("Invalid loot seed: " + p);
            }
        }
        return out;
    }

    private List<StructureDefinition> selectedDefinitions() {
        List<StructureDefinition> out = new ArrayList<>();
        for (StructureDefinition def : definitions) {
            if (selected.contains(def.id)) {
                out.add(def);
            }
        }
        return out;
    }

    private void loadTextureIcon(StructureDefinition def, JButton button) {
        try {
            Path path = textureDownloader.getTexturePath(def);
            if (!Files.exists(path)) {
                button.setIcon(null);
                return;
            }
            Image image = ImageIO.read(path.toFile());
            if (image == null) {
                button.setIcon(null);
                return;
            }
            Image scaled = image.getScaledInstance(24, 24, Image.SCALE_SMOOTH);
            button.setIcon(new ImageIcon(scaled));
        } catch (Exception ignored) {
            button.setIcon(null);
        }
    }

    private static String shortItemName(String itemId) {
        if (itemId == null || itemId.isBlank()) {
            return itemId;
        }
        int idx = itemId.indexOf(':');
        return idx >= 0 ? itemId.substring(idx + 1) : itemId;
    }
}
