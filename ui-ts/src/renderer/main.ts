import "./styles.css";
import {
  autocompletion,
  snippetCompletion,
  startCompletion,
  type Completion,
  type CompletionContext,
  type CompletionResult
} from "@codemirror/autocomplete";
import { indentWithTab } from "@codemirror/commands";
import { HighlightStyle, StreamLanguage, syntaxHighlighting } from "@codemirror/language";
import { linter, lintGutter } from "@codemirror/lint";
import { EditorState } from "@codemirror/state";
import { EditorView, keymap } from "@codemirror/view";
import { basicSetup } from "codemirror";
import { tags } from "@lezer/highlight";
import {
  Activity,
  BookOpen,
  Blocks,
  Braces,
  Camera,
  Copy,
  Cpu,
  createIcons,
  Download,
  FileJson,
  FolderOpen,
  Gauge,
  ListChecks,
  Map as MapIcon,
  PackageCheck,
  Pause,
  Play,
  Plus,
  Radar,
  RefreshCw,
  Route,
  Settings as SettingsIcon,
  Sparkles,
  Square,
  Star,
  Terminal,
  Trash2,
  Upload,
  X
} from "lucide";
import type { AppContext, ScannerProgress } from "../shared/contracts";
import type { HelixSeedApi } from "../preload/preload";
import { LineChart } from "./telemetry/chart";
import { TelemetryStore, type SeriesDefinition, type TelemetrySnapshot } from "./telemetry/store";

type DimensionName = "overworld" | "nether" | "end";

type ConstraintRow = {
  id: string;
  structure: string;
  dimension: DimensionName;
  anchor: string;
  radius: string;
  mode: "strict" | "placement" | "mixed";
  structureSettings?: ConstraintStructureSettings;
};

type ConstraintStructureSettings = Record<string, string[]>;

type StructureSettingOption = {
  id: string;
  label: string;
  enforcedStructure?: string;
  disabled?: boolean;
  note?: string;
};

type StructureSettingGroup = {
  id: string;
  label: string;
  options: StructureSettingOption[];
};

type StructureSettingDefinition = {
  family: string;
  title: string;
  groups: StructureSettingGroup[];
};

type BiomeRow = {
  point: string;
  dimension: DimensionName;
  y: string;
  radius: string;
  allowed: string;
};

type LootRow = {
  constraint: string;
  source: string;
  required: string;
};

type ScriptConstraint = {
  id: string;
  structure: string;
  dimension: string;
  mode: string;
  within: {
    anchor: string;
    radius: number;
  };
  required: boolean;
  track_found: boolean;
  structure_settings?: ConstraintStructureSettings;
};

type ControlOp =
  | { op: "skip_if"; mask: number; expected: number }
  | { op: "stop_if"; mask: number; expected: number }
  | { op: "require_if"; mask: number; expected: number; also_id: string };

type SeedResult = {
  id: string;
  display: string;
  copy: string;
  detail?: string;
  stage?: "placement" | "strict";
};

type Settings = {
  scanMode: "placement" | "strict" | "mixed";
  mcVersion: string;
  start: string;
  count: string;
  batchSize: string;
  maxPrint: string;
  progressInterval: string;
  outputMode: "raw" | "lift64" | "both" | "world64";
  upper16: string;
  javaValidation: "auto" | "off" | "strict";
  safetyCompleteness: "exact" | "approximate";
  defaultAnchor: "origin" | "spawn";
  printClosest: boolean;
  terminalMode: boolean;
  executionMode: "auto" | "scalar" | "simd-x4" | "simd-x8" | "max-throughput";
  stageAMode: "auto" | "single" | "multi" | "cpu-a5" | "gpu-off";
  gpuPipeline: "auto" | "async" | "sync";
  gpuBackend: "auto" | "cuda" | "opencl";
  gpuInflightBatches: "auto" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8";
  strictSurrogate: "off" | "balanced" | "aggressive" | "ultra" | "turbo" | "lightspeed";
  strictWorkers: string;
  mixedQueueCapacity: string;
  mixedSurvivorCap: string;
  mixedAdaptiveThrottling: boolean;
  saveTxt: string;
  candidateCapMode: "adaptive" | "fixed" | "none";
  fixedCap: string;
};

type State = {
  context: AppContext | null;
  running: boolean;
  paused: boolean;
  useScriptSource: boolean;
  activeScanMode: Settings["scanMode"];
  resultStageFilter: "all" | "placement" | "strict";
  activeConstraintSettingsIndex: number | null;
  settings: Settings;
  constraints: ConstraintRow[];
  biomes: BiomeRow[];
  loot: LootRow[];
  foundSeeds: SeedResult[];
};

type ScriptScanOverrides = Partial<Pick<
  Settings,
  | "start"
  | "count"
  | "batchSize"
  | "maxPrint"
  | "progressInterval"
  | "outputMode"
  | "upper16"
  | "javaValidation"
  | "safetyCompleteness"
  | "printClosest"
  | "terminalMode"
  | "scanMode"
  | "executionMode"
  | "stageAMode"
  | "gpuPipeline"
  | "gpuBackend"
  | "gpuInflightBatches"
  | "strictWorkers"
  | "mixedQueueCapacity"
  | "mixedSurvivorCap"
  | "mixedAdaptiveThrottling"
  | "saveTxt"
>>;

type QueryObject = {
  version: 1;
  logic: string;
  anchor: { type: string };
  constraints: ScriptConstraint[];
  biome_filters: Array<{ point: string; dimension: string; y: number; radius: number; allowed: string[] }>;
  loot_filters: Array<{ constraint: string; structure: string; required: Record<string, number> }>;
  control_ops: ControlOp[];
  output: { detail: string };
  safety: { completeness: string };
  performance: {
    strict_workers: number;
    execution_mode: string;
    compiled_stage_a_mode: string;
    candidate_cap_mode: string;
    strict_surrogate: string;
    fixed_cap: number;
  };
};

type ScriptCompileResult = {
  query: QueryObject;
  scan: ScriptScanOverrides;
};

type ScriptLine = {
  text: string;
  line: number;
};

type ScriptFunctionDef = {
  params: string[];
  body: ScriptLine[];
};

const dimensions: DimensionName[] = ["overworld", "nether", "end"];

type StructureOption = {
  id: string;
  canonical: string;
  dimension: DimensionName;
  strictOnly?: boolean;
};

const structureOptions: StructureOption[] = [
  { id: "village", canonical: "village", dimension: "overworld" },
  { id: "village_plains", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "village_desert", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "village_savanna", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "village_snowy", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "village_taiga", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "plains_village", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "desert_village", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "savanna_village", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "snowy_village", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "taiga_village", canonical: "village", dimension: "overworld", strictOnly: true },
  { id: "desert_pyramid", canonical: "desert_pyramid", dimension: "overworld" },
  { id: "desert_temple", canonical: "desert_pyramid", dimension: "overworld" },
  { id: "igloo", canonical: "igloo", dimension: "overworld" },
  { id: "jungle_temple", canonical: "jungle_temple", dimension: "overworld" },
  { id: "jungle_pyramid", canonical: "jungle_temple", dimension: "overworld" },
  { id: "swamp_hut", canonical: "swamp_hut", dimension: "overworld" },
  { id: "witch_hut", canonical: "swamp_hut", dimension: "overworld" },
  { id: "ocean_ruin", canonical: "ocean_ruin", dimension: "overworld" },
  { id: "ocean_ruin_cold", canonical: "ocean_ruin", dimension: "overworld" },
  { id: "ocean_ruin_warm", canonical: "ocean_ruin", dimension: "overworld" },
  { id: "underwater_ruin", canonical: "ocean_ruin", dimension: "overworld" },
  { id: "shipwreck", canonical: "shipwreck", dimension: "overworld" },
  { id: "shipwreck_beached", canonical: "shipwreck", dimension: "overworld" },
  { id: "beached_shipwreck", canonical: "shipwreck", dimension: "overworld" },
  { id: "ruined_portal", canonical: "ruined_portal", dimension: "overworld" },
  { id: "ruined_portal_desert", canonical: "ruined_portal", dimension: "overworld" },
  { id: "ruined_portal_jungle", canonical: "ruined_portal", dimension: "overworld" },
  { id: "ruined_portal_mountain", canonical: "ruined_portal", dimension: "overworld" },
  { id: "ruined_portal_ocean", canonical: "ruined_portal", dimension: "overworld" },
  { id: "ruined_portal_swamp", canonical: "ruined_portal", dimension: "overworld" },
  { id: "ocean_monument", canonical: "ocean_monument", dimension: "overworld" },
  { id: "monument", canonical: "ocean_monument", dimension: "overworld" },
  { id: "woodland_mansion", canonical: "woodland_mansion", dimension: "overworld" },
  { id: "mansion", canonical: "woodland_mansion", dimension: "overworld" },
  { id: "trial_chambers", canonical: "trial_chambers", dimension: "overworld" },
  { id: "trial_chamber", canonical: "trial_chambers", dimension: "overworld" },
  { id: "pillager_outpost", canonical: "pillager_outpost", dimension: "overworld" },
  { id: "outpost", canonical: "pillager_outpost", dimension: "overworld" },
  { id: "buried_treasure", canonical: "buried_treasure", dimension: "overworld" },
  { id: "treasure", canonical: "buried_treasure", dimension: "overworld" },
  { id: "ancient_city", canonical: "ancient_city", dimension: "overworld" },
  { id: "trail_ruin", canonical: "trail_ruin", dimension: "overworld" },
  { id: "trail_ruins", canonical: "trail_ruin", dimension: "overworld" },
  { id: "mineshaft", canonical: "mineshaft", dimension: "overworld" },
  { id: "abandoned_mineshaft", canonical: "mineshaft", dimension: "overworld" },
  { id: "mineshaft_mesa", canonical: "mineshaft", dimension: "overworld" },
  { id: "mesa_mineshaft", canonical: "mineshaft", dimension: "overworld" },
  { id: "badlands_mineshaft", canonical: "mineshaft", dimension: "overworld" },
  { id: "desert_well", canonical: "desert_well", dimension: "overworld" },
  { id: "geode", canonical: "geode", dimension: "overworld" },
  { id: "stronghold", canonical: "stronghold", dimension: "overworld" },
  { id: "quad_hut", canonical: "swamp_hut", dimension: "overworld", strictOnly: true },
  { id: "quad_swamp_hut", canonical: "swamp_hut", dimension: "overworld", strictOnly: true },
  { id: "quad_witch_hut", canonical: "swamp_hut", dimension: "overworld", strictOnly: true },
  { id: "quad_monument", canonical: "ocean_monument", dimension: "overworld", strictOnly: true },
  { id: "quad_ocean_monument", canonical: "ocean_monument", dimension: "overworld", strictOnly: true },
  { id: "quad_desert_pyramid", canonical: "desert_pyramid", dimension: "overworld", strictOnly: true },
  { id: "quad_desert_temple", canonical: "desert_pyramid", dimension: "overworld", strictOnly: true },
  { id: "quad_igloo", canonical: "igloo", dimension: "overworld", strictOnly: true },
  { id: "quad_jungle_temple", canonical: "jungle_temple", dimension: "overworld", strictOnly: true },
  { id: "quad_jungle_pyramid", canonical: "jungle_temple", dimension: "overworld", strictOnly: true },
  { id: "fortress", canonical: "fortress", dimension: "nether" },
  { id: "nether_fortress", canonical: "fortress", dimension: "nether" },
  { id: "bastion_remnant", canonical: "bastion_remnant", dimension: "nether" },
  { id: "bastion", canonical: "bastion_remnant", dimension: "nether" },
  { id: "nether_fossil", canonical: "nether_fossil", dimension: "nether" },
  { id: "ruined_portal_nether", canonical: "ruined_portal_nether", dimension: "nether" },
  { id: "nether_ruined_portal", canonical: "ruined_portal_nether", dimension: "nether" },
  { id: "end_city", canonical: "end_city", dimension: "end" }
];

const structures = structureOptions.map((option) => option.id);
const structureOptionById = new Map(structureOptions.map((option) => [option.id, option]));

const lootItemSuggestions = [
  "obsidian",
  "flint_and_steel",
  "iron_nugget",
  "diamond",
  "emerald",
  "gold_ingot",
  "golden_apple",
  "enchanted_golden_apple",
  "apple",
  "bread",
  "iron_ingot",
  "saddle",
  "book",
  "paper",
  "map",
  "compass",
  "ender_eye",
  "ender_pearl",
  "fire_charge",
  "golden_carrot",
  "wheat",
  "coal",
  "string",
  "arrow",
  "crossbow",
  "tnt",
  "heart_of_the_sea",
  "nautilus_shell"
];

const structureSettingDefinitions: Record<string, StructureSettingDefinition> = {
  village: {
    family: "village",
    title: "Village settings",
    groups: [
      {
        id: "style",
        label: "Biome style",
        options: [
          { id: "plains", label: "Plains", enforcedStructure: "village_plains" },
          { id: "desert", label: "Desert", enforcedStructure: "village_desert" },
          { id: "savanna", label: "Savanna", enforcedStructure: "village_savanna" },
          { id: "snowy", label: "Snowy", enforcedStructure: "village_snowy" },
          { id: "taiga", label: "Taiga", enforcedStructure: "village_taiga" }
        ]
      },
      {
        id: "state",
        label: "State",
        options: [
          { id: "abandoned", label: "Abandoned" }
        ]
      },
      {
        id: "buildings",
        label: "Buildings",
        options: [
          { id: "village_armorer", label: "Armorer" },
          { id: "village_butcher", label: "Butcher" },
          { id: "village_cartographer", label: "Cartographer" },
          { id: "village_fisher", label: "Fisher" },
          { id: "village_fletcher", label: "Fletcher" },
          { id: "village_mason", label: "Mason" },
          { id: "village_shepherd", label: "Shepherd" },
          { id: "village_tannery", label: "Tannery" },
          { id: "village_temple", label: "Temple" },
          { id: "village_toolsmith", label: "Toolsmith" },
          { id: "village_weaponsmith", label: "Blacksmith / weaponsmith" }
        ]
      }
    ]
  },
  ruined_portal: {
    family: "ruined_portal",
    title: "Ruined portal settings",
    groups: [
      {
        id: "type",
        label: "Type",
        options: [
          { id: "desert", label: "Desert", disabled: true, note: "variant validation not wired yet" },
          { id: "jungle", label: "Jungle", disabled: true, note: "variant validation not wired yet" },
          { id: "mountain", label: "Mountain", disabled: true, note: "variant validation not wired yet" },
          { id: "ocean", label: "Ocean", disabled: true, note: "variant validation not wired yet" },
          { id: "swamp", label: "Swamp", disabled: true, note: "variant validation not wired yet" },
          { id: "nether", label: "Nether", disabled: true, note: "variant validation not wired yet" }
        ]
      }
    ]
  },
  shipwreck: {
    family: "shipwreck",
    title: "Shipwreck settings",
    groups: [
      {
        id: "type",
        label: "Type",
        options: [
          { id: "ocean", label: "Ocean", disabled: true, note: "variant validation not wired yet" },
          { id: "beached", label: "Beached", disabled: true, note: "variant validation not wired yet" }
        ]
      }
    ]
  },
  ocean_ruin: {
    family: "ocean_ruin",
    title: "Ocean ruin settings",
    groups: [
      {
        id: "temperature",
        label: "Temperature",
        options: [
          { id: "cold", label: "Cold", disabled: true, note: "variant validation not wired yet" },
          { id: "warm", label: "Warm", disabled: true, note: "variant validation not wired yet" }
        ]
      },
      {
        id: "size",
        label: "Size",
        options: [
          { id: "big", label: "Big", disabled: true, note: "variant validation not wired yet" },
          { id: "small", label: "Small", disabled: true, note: "variant validation not wired yet" }
        ]
      }
    ]
  },
  mineshaft: {
    family: "mineshaft",
    title: "Mineshaft settings",
    groups: [
      {
        id: "type",
        label: "Type",
        options: [
          { id: "standard", label: "Standard", disabled: true, note: "variant validation not wired yet" },
          { id: "badlands", label: "Badlands / mesa", disabled: true, note: "variant validation not wired yet" }
        ]
      }
    ]
  },
  bastion_remnant: {
    family: "bastion_remnant",
    title: "Bastion settings",
    groups: [
      {
        id: "type",
        label: "Type",
        options: [
          { id: "bridge", label: "Bridge", disabled: true, note: "variant validation not wired yet" },
          { id: "hoglin_stable", label: "Hoglin stable", disabled: true, note: "variant validation not wired yet" },
          { id: "housing", label: "Housing", disabled: true, note: "variant validation not wired yet" },
          { id: "treasure", label: "Treasure", disabled: true, note: "variant validation not wired yet" }
        ]
      }
    ]
  },
  trial_chambers: {
    family: "trial_chambers",
    title: "Trial chambers settings",
    groups: [
      {
        id: "vaults",
        label: "Vaults",
        options: [
          { id: "reward_common", label: "Common reward", disabled: true, note: "vault validation not wired yet" },
          { id: "reward_rare", label: "Rare reward", disabled: true, note: "vault validation not wired yet" },
          { id: "reward_unique", label: "Unique reward", disabled: true, note: "vault validation not wired yet" },
          { id: "ominous_common", label: "Ominous common", disabled: true, note: "vault validation not wired yet" },
          { id: "ominous_rare", label: "Ominous rare", disabled: true, note: "vault validation not wired yet" },
          { id: "ominous_unique", label: "Ominous unique", disabled: true, note: "vault validation not wired yet" }
        ]
      }
    ]
  }
};

const exactLootSources = [
  "buried_treasure",
  "blacksmith",
  "ruined_portal",
  "village_weaponsmith"
];

const lootSourceAliases: Record<string, string> = {
  blacksmith: "village_weaponsmith",
  weaponsmith: "village_weaponsmith",
  village_blacksmith: "village_weaponsmith"
};

const lootSourceFamilies: Record<string, string> = {
  abandoned_mineshaft: "mineshaft",
  ancient_city: "ancient_city",
  ancient_city_ice_box: "ancient_city",
  bastion_bridge: "bastion_remnant",
  bastion_hoglin_stable: "bastion_remnant",
  bastion_other: "bastion_remnant",
  bastion_treasure: "bastion_remnant",
  buried_treasure: "buried_treasure",
  desert_pyramid: "desert_pyramid",
  end_city_treasure: "end_city",
  igloo_chest: "igloo",
  jungle_temple: "jungle_temple",
  jungle_temple_dispenser: "jungle_temple",
  nether_bridge: "fortress",
  pillager_outpost: "pillager_outpost",
  ruined_portal: "ruined_portal",
  ruined_portal_nether: "ruined_portal",
  shipwreck_map: "shipwreck",
  shipwreck_supply: "shipwreck",
  shipwreck_treasure: "shipwreck",
  stronghold_corridor: "stronghold",
  stronghold_crossing: "stronghold",
  stronghold_library: "stronghold",
  trial_chambers_corridor: "trial_chambers",
  trial_chambers_entrance: "trial_chambers",
  trial_chambers_intersection: "trial_chambers",
  trial_chambers_intersection_barrel: "trial_chambers",
  trial_chambers_reward: "trial_chambers",
  trial_chambers_reward_common: "trial_chambers",
  trial_chambers_reward_ominous: "trial_chambers",
  trial_chambers_reward_ominous_common: "trial_chambers",
  trial_chambers_reward_ominous_rare: "trial_chambers",
  trial_chambers_reward_ominous_unique: "trial_chambers",
  trial_chambers_reward_rare: "trial_chambers",
  trial_chambers_reward_unique: "trial_chambers",
  trial_chambers_supply: "trial_chambers",
  underwater_ruin_big: "ocean_ruin",
  underwater_ruin_small: "ocean_ruin",
  blacksmith: "village",
  village_armorer: "village",
  village_butcher: "village",
  village_cartographer: "village",
  village_desert_house: "village",
  village_fisher: "village",
  village_fletcher: "village",
  village_mason: "village",
  village_plains_house: "village",
  village_savanna_house: "village",
  village_shepherd: "village",
  village_snowy_house: "village",
  village_taiga_house: "village",
  village_tannery: "village",
  village_temple: "village",
  village_toolsmith: "village",
  village_weaponsmith: "village",
  woodland_mansion: "woodland_mansion"
};

const lootFamilyBaseSources: Record<string, string[]> = {
  ancient_city: ["ancient_city", "ancient_city_ice_box"],
  bastion_remnant: ["bastion_bridge", "bastion_hoglin_stable", "bastion_other", "bastion_treasure"],
  end_city: ["end_city_treasure"],
  fortress: ["nether_bridge"],
  igloo: ["igloo_chest"],
  jungle_temple: ["jungle_temple", "jungle_temple_dispenser"],
  mineshaft: ["abandoned_mineshaft"],
  ocean_ruin: ["underwater_ruin_big", "underwater_ruin_small"],
  shipwreck: ["shipwreck_supply", "shipwreck_map", "shipwreck_treasure"],
  stronghold: ["stronghold_corridor", "stronghold_crossing", "stronghold_library"],
  trial_chambers: [
    "trial_chambers_corridor",
    "trial_chambers_entrance",
    "trial_chambers_intersection",
    "trial_chambers_intersection_barrel",
    "trial_chambers_reward",
    "trial_chambers_reward_common",
    "trial_chambers_reward_rare",
    "trial_chambers_reward_unique",
    "trial_chambers_reward_ominous",
    "trial_chambers_reward_ominous_common",
    "trial_chambers_reward_ominous_rare",
    "trial_chambers_reward_ominous_unique",
    "trial_chambers_supply"
  ],
  village: [
    "blacksmith",
    "village_armorer",
    "village_butcher",
    "village_cartographer",
    "village_desert_house",
    "village_fisher",
    "village_fletcher",
    "village_mason",
    "village_plains_house",
    "village_savanna_house",
    "village_shepherd",
    "village_snowy_house",
    "village_taiga_house",
    "village_tannery",
    "village_temple",
    "village_toolsmith"
  ],
  woodland_mansion: ["woodland_mansion"]
};

const biomeSuggestions = [
  "plains",
  "forest",
  "desert",
  "savanna",
  "taiga",
  "snowy_plains",
  "beach",
  "ocean",
  "deep_ocean",
  "meadow",
  "cherry_grove",
  "pale_garden",
  "dripstone_caves",
  "lush_caves",
  "deep_dark",
  "nether_wastes",
  "soul_sand_valley",
  "crimson_forest",
  "warped_forest",
  "basalt_deltas",
  "the_end",
  "small_end_islands",
  "end_midlands",
  "end_highlands",
  "end_barrens"
];

function normalizeDimensionName(value: string | undefined, fallback: DimensionName = "overworld"): DimensionName {
  const normalized = normalizeName(value ?? "");
  if (!normalized) return fallback;
  if (normalized === "nether" || normalized === "the_nether" || normalized === "hell") return "nether";
  if (normalized === "end" || normalized === "the_end") return "end";
  return "overworld";
}

function structureOptionFor(structure: string): StructureOption | undefined {
  return structureOptionById.get(normalizeName(structure));
}

function canonicalStructureName(structure: string): string {
  return structureOptionFor(structure)?.canonical ?? normalizeName(structure);
}

function canonicalLootSourceName(source: string): string {
  const normalized = normalizeName(source);
  return lootSourceAliases[normalized] ?? canonicalStructureName(normalized);
}

function defaultDimensionForStructure(structure: string): DimensionName {
  return structureOptionFor(structure)?.dimension ?? "overworld";
}

function structureRequiresStrict(structure: string): boolean {
  return structureOptionFor(structure)?.strictOnly === true;
}

function structuresForDimension(dimension: string): string[] {
  const normalized = normalizeDimensionName(dimension);
  return structureOptions.filter((structure) => structure.dimension === normalized).map((structure) => structure.id);
}

function isExactLootVersion(version: string): boolean {
  const token = version.trim();
  return token === "26.1.2" || token === "26.1.1" || token === "1.21.11";
}

function isLegacyLootVersion(version: string): boolean {
  const token = version.trim();
  return token === "1.17" || token === "1.17.1";
}

function supportedLootSourcesForVersion(version: string): string[] {
  return isExactLootVersion(version)
    ? exactLootSources
    : isLegacyLootVersion(version)
      ? ["buried_treasure", "desert_pyramid", "ruined_portal", "shipwreck"]
      : [];
}

function lootFamilyForSource(source: string): string {
  const canonical = canonicalLootSourceName(source);
  return lootSourceFamilies[normalizeName(source)] ?? lootSourceFamilies[canonical] ?? canonical;
}

function structureSettingsFamily(structure: string): string {
  return lootFamilyForSource(structure);
}

function structureSettingsDefinitionFor(structure: string): StructureSettingDefinition | undefined {
  return structureSettingDefinitions[structureSettingsFamily(structure)];
}

function optionIdsForGroup(group: StructureSettingGroup): Set<string> {
  return new Set(group.options.filter((option) => !option.disabled).map((option) => option.id));
}

function normalizedStructureSettings(
  structure: string,
  settings: ConstraintStructureSettings | undefined
): ConstraintStructureSettings {
  const def = structureSettingsDefinitionFor(structure);
  if (!def || !settings) {
    return {};
  }
  const out: ConstraintStructureSettings = {};
  for (const group of def.groups) {
    const allowed = optionIdsForGroup(group);
    const values = [...new Set(settings[group.id] ?? [])].filter((value) => allowed.has(value));
    if (values.length > 0) {
      out[group.id] = values;
    }
  }
  return out;
}

function settingsForConstraint(row: ConstraintRow): ConstraintStructureSettings {
  return normalizedStructureSettings(row.structure, row.structureSettings);
}

function selectedStructureSettingCount(row: ConstraintRow): number {
  return Object.values(settingsForConstraint(row)).reduce((sum, values) => sum + values.length, 0);
}

function structureSettingsNeedStrictStage(row: ConstraintRow): boolean {
  return selectedStructureSettingCount(row) > 0;
}

function selectedStructureSettingLabels(row: ConstraintRow): string[] {
  const def = structureSettingsDefinitionFor(row.structure);
  if (!def) {
    return [];
  }
  const settings = settingsForConstraint(row);
  const labels: string[] = [];
  for (const group of def.groups) {
    const selected = new Set(settings[group.id] ?? []);
    for (const option of group.options) {
      if (selected.has(option.id)) {
        labels.push(option.label);
      }
    }
  }
  return labels;
}

function effectiveStructureForConstraint(row: ConstraintRow): string {
  const base = normalizeName(row.structure || "village");
  const def = structureSettingsDefinitionFor(base);
  if (!def) {
    return base;
  }
  const settings = settingsForConstraint(row);
  for (const group of def.groups) {
    const selected = settings[group.id] ?? [];
    const enforced = group.options.filter((option) => selected.includes(option.id) && option.enforcedStructure);
    if (enforced.length === 1) {
      return enforced[0].enforcedStructure ?? base;
    }
  }
  return base;
}

function pruneConstraintStructureSettings(row: ConstraintRow): void {
  const normalized = settingsForConstraint(row);
  row.structureSettings = Object.keys(normalized).length > 0 ? normalized : undefined;
}

function lootSourcesForConstraintStructure(structure: string, version: string): string[] {
  const supported = new Set(supportedLootSourcesForVersion(version));
  const family = lootFamilyForSource(structure);
  const out = new Set<string>();
  if (supported.has(family)) {
    out.add(family);
  }
  for (const source of lootFamilyBaseSources[family] ?? []) {
    if (supported.has(source)) {
      out.add(source);
    }
  }
  return [...out];
}

function constraintRowById(id: string): ConstraintRow | undefined {
  return state.constraints.find((row) => row.id.trim() === id.trim());
}

function lootSourcesForRow(row: LootRow): string[] {
  const sourceSet = new Set<string>(["auto"]);
  const constraint = constraintRowById(row.constraint);
  if (constraint) {
    for (const source of lootSourcesForConstraintStructure(constraint.structure, state.settings.mcVersion)) {
      sourceSet.add(source);
    }
    const inferred = inferredLootSourceForConstraint(constraint, state.settings.mcVersion);
    if (inferred) {
      sourceSet.add(inferred === "village_weaponsmith" ? "blacksmith" : inferred);
    }
  }
  return [...sourceSet];
}

function inferredLootSourceForConstraint(row: ConstraintRow | undefined, version: string): string | undefined {
  if (!row) {
    return undefined;
  }
  const supported = new Set(supportedLootSourcesForVersion(version).map(canonicalLootSourceName));
  const settings = settingsForConstraint(row);
  const buildingSources = [...new Set((settings.buildings ?? []).map(canonicalLootSourceName))]
    .filter((source) => supported.has(source));
  return buildingSources.length === 1 ? buildingSources[0] : undefined;
}

const state: State = {
  context: null,
  running: false,
  paused: false,
  useScriptSource: false,
  activeScanMode: "strict",
  resultStageFilter: "all",
  activeConstraintSettingsIndex: null,
  settings: {
    scanMode: "strict",
    mcVersion: "26.1.2",
    start: "",
    count: "1000000",
    batchSize: "balanced",
    maxPrint: "20",
    progressInterval: "1",
    outputMode: "raw",
    upper16: "0",
    javaValidation: "auto",
    safetyCompleteness: "exact",
    defaultAnchor: "origin",
    printClosest: false,
    terminalMode: false,
    executionMode: "auto",
    stageAMode: "auto",
    gpuPipeline: "auto",
    gpuBackend: "auto",
    gpuInflightBatches: "auto",
    strictSurrogate: "off",
    strictWorkers: "0",
    mixedQueueCapacity: "1000000",
    mixedSurvivorCap: "0",
    mixedAdaptiveThrottling: true,
    saveTxt: "",
    candidateCapMode: "none",
    fixedCap: "128"
  },
  constraints: [{ id: "v1", structure: "village", dimension: "overworld", anchor: "origin", radius: "64", mode: "strict" }],
  biomes: [],
  loot: [],
  foundSeeds: []
};

const seedIconUrl = new URL("../../assets/Seed.png", import.meta.url).href;
let queryEditor: EditorView | null = null;
let syncingQueryEditor = false;
const iconSet = {
  Activity,
  BookOpen,
  Blocks,
  Braces,
  Camera,
  Copy,
  Cpu,
  FileJson,
  Download,
  FolderOpen,
  Gauge,
  ListChecks,
  Map: MapIcon,
  PackageCheck,
  Pause,
  Play,
  Plus,
  Radar,
  RefreshCw,
  Route,
  Settings: SettingsIcon,
  Sparkles,
  Square,
  Star,
  Terminal,
  Trash2,
  Upload,
  X
};

type DerivedRates = {
  stageARate: number;
  stageBRate: number;
  biomeRate: number;
  javaRate: number;
};

type RejectAccumulator = {
  t: number;
  stageA: number;
  stageB: number;
  biome: number;
  java: number;
};

const TELEMETRY_SERIES: SeriesDefinition[] = [
  { key: "rate", label: "Seeds / sec", color: "#7cd6ff", unit: "seeds/sec", visible: true, group: "primary" },
  { key: "rawPlacementRate", label: "Raw placement / sec", color: "#5bd0a5", unit: "seeds/sec", visible: true, group: "primary" },
  { key: "placementSurvivorRate", label: "Placement survivors / sec", color: "#72a8ff", unit: "seeds/sec", visible: true, group: "primary" },
  { key: "strictConsumedRate", label: "Strict consumed / sec", color: "#d19cff", unit: "seeds/sec", visible: true, group: "primary" },
  { key: "verifiedRate", label: "Verified / sec", color: "#f6d66b", unit: "seeds/sec", visible: true, group: "primary" },
  { key: "queueSaturation", label: "Queue %", color: "#ff8a8a", unit: "percent", visible: false, group: "primary" },
  { key: "queueSize", label: "Queue size", color: "#6fa8ff", visible: false, group: "primary" },
  { key: "queueCapacity", label: "Queue cap", color: "#4262a8", visible: false, group: "primary" },
  { key: "strictWorkers", label: "Strict workers", color: "#b4c2d4", visible: false, group: "primary" },
  { key: "droppedSurvivors", label: "Dropped survivors", color: "#ff5f72", visible: false, group: "primary" },
  { key: "queueFullEvents", label: "Queue full events", color: "#ff8a8a", visible: false, group: "primary" },
  { key: "queuePushWaitMs", label: "Queue push wait", color: "#ffaa5b", unit: "ms", visible: false, group: "timing" },
  { key: "gpuUtil", label: "GPU busy", color: "#75d99c", unit: "percent", visible: false, group: "primary" },
  { key: "gpuPipelineFill", label: "GPU pipeline fill", color: "#7ee787", unit: "percent", visible: false, group: "primary" },
  { key: "gpuBatchKernelMs", label: "GPU batch kernel", color: "#9ad1ff", unit: "ms", visible: false, group: "timing" },
  { key: "gpuHostGapMs", label: "GPU host gap", color: "#ffcc7a", unit: "ms", visible: false, group: "timing" },
  { key: "gpuInflight", label: "GPU in-flight", color: "#b6e3ff", unit: "count", visible: false, group: "primary" },
  { key: "gpuSlots", label: "GPU slots", color: "#6f8cff", unit: "count", visible: false, group: "primary" },
  { key: "placementBatchSize", label: "Placement batch", color: "#a5d6ff", unit: "count", visible: false, group: "primary" },
  { key: "stageA", label: "Stage A rejects/s", color: "#f0a25b", unit: "rejects/sec", visible: false, group: "rejects" },
  { key: "stageB", label: "Stage B rejects/s", color: "#d19cff", unit: "rejects/sec", visible: false, group: "rejects" },
  { key: "biome", label: "Biome rejects/s", color: "#75d99c", unit: "rejects/sec", visible: false, group: "rejects" },
  { key: "java", label: "Java rejects/s", color: "#ff8a8a", unit: "rejects/sec", visible: false, group: "rejects" }
];

const telemetryStore = new TelemetryStore({ historyMs: 60_000 });
telemetryStore.registerSeriesBatch(TELEMETRY_SERIES);

let telemetryChart: LineChart | null = null;
let telemetryFpsVisible = false;
let lastRejectAcc: RejectAccumulator | null = null;

function deriveRejectRates(payload: ScannerProgress): DerivedRates {
  const now = performance.now();
  const stageA = nonNegativeNumber(payload.stage_a_rejects);
  const stageB = nonNegativeNumber(payload.stage_b_exact_rejects);
  const biome = nonNegativeNumber(payload.biome_rejected ?? payload.stage_c_biome_rejects);
  const java = nonNegativeNumber(payload.java_worldgen_rejected);

  const previous = lastRejectAcc;
  lastRejectAcc = { t: now, stageA, stageB, biome, java };

  if (!previous) {
    return { stageARate: 0, stageBRate: 0, biomeRate: 0, javaRate: 0 };
  }
  const dt = (now - previous.t) / 1000;
  if (dt <= 0) {
    return { stageARate: 0, stageBRate: 0, biomeRate: 0, javaRate: 0 };
  }
  const safe = (current: number, prior: number): number => {
    const delta = current - prior;
    return delta > 0 ? delta / dt : 0;
  };
  return {
    stageARate: safe(stageA, previous.stageA),
    stageBRate: safe(stageB, previous.stageB),
    biomeRate: safe(biome, previous.biome),
    javaRate: safe(java, previous.java)
  };
}

function resetRejectAccumulator(): void {
  lastRejectAcc = null;
}

function nonNegativeNumber(value: unknown): number {
  const n = Number(value ?? 0);
  return Number.isFinite(n) && n >= 0 ? n : 0;
}

const queryEditorTheme = EditorView.theme(
  {
    "&": {
      height: "100%",
      color: "#d8d8d8",
      backgroundColor: "#0d0d0d",
      fontSize: "12px"
    },
    "&.cm-focused": {
      outline: "none"
    },
    ".cm-scroller": {
      fontFamily: '"JetBrains Mono", "Cascadia Mono", Consolas, monospace',
      lineHeight: "1.5"
    },
    ".cm-content": {
      padding: "12px 0"
    },
    ".cm-line": {
      padding: "0 12px"
    },
    ".cm-gutters": {
      backgroundColor: "#0d0d0d",
      color: "#5a6470",
      borderRight: "1px solid #1a1a1a"
    },
    ".cm-activeLine, .cm-activeLineGutter": {
      backgroundColor: "#14181d"
    },
    ".cm-selectionBackground, &.cm-focused .cm-selectionBackground": {
      backgroundColor: "#2d4c63"
    },
    ".cm-cursor": {
      borderLeftColor: "#f0d889"
    },
    ".cm-matchingBracket": {
      color: "#f0d889",
      backgroundColor: "#27313a",
      outline: "1px solid #5a6a78"
    },
    ".cm-nonmatchingBracket": {
      color: "#ff6b6b",
      backgroundColor: "#2a1010"
    },
    ".cm-tooltip": {
      backgroundColor: "#111111",
      border: "1px solid #2a2a2a",
      color: "#e6e6e6"
    },
    ".cm-tooltip-autocomplete ul li[aria-selected]": {
      backgroundColor: "#ffffff",
      color: "#0a0a0a"
    },
    ".cm-diagnostic": {
      borderLeftColor: "#c97070"
    }
  },
  { dark: true }
);

const queryHighlightStyle = HighlightStyle.define([
  { tag: tags.propertyName, color: "#8cc8ff" },
  { tag: tags.variableName, color: "#8cc8ff" },
  { tag: tags.string, color: "#f07878" },
  { tag: tags.number, color: "#75d99c" },
  { tag: tags.bool, color: "#d19cff" },
  { tag: tags.null, color: "#d19cff", fontStyle: "italic" },
  { tag: tags.keyword, color: "#d19cff" },
  { tag: tags.operator, color: "#f0d889" },
  { tag: tags.separator, color: "#9aa6b2" },
  { tag: tags.punctuation, color: "#9aa6b2" },
  { tag: tags.bracket, color: "#f0d889", fontWeight: "600" },
  { tag: tags.squareBracket, color: "#7cd6ff", fontWeight: "600" },
  { tag: tags.brace, color: "#f0d889", fontWeight: "600" },
  { tag: tags.invalid, color: "#ff6b6b", textDecoration: "underline" },
  { tag: tags.comment, color: "#6f7a85", fontStyle: "italic" }
]);

const helixScriptLanguage = StreamLanguage.define({
  token(stream) {
    if (stream.eatSpace()) {
      return null;
    }
    if (stream.match(/#.*/u)) {
      return "comment";
    }
    if (stream.match(/"(?:[^"\\]|\\.)*"/u)) {
      return "string";
    }
    if (stream.match(/\b-?\d+(?:\.\d+)?\b/u)) {
      return "number";
    }
    if (stream.match(/\b(let|set|const|fn|function|call|if|elif|else|end|and|or|not|is|contains|for|in|find|as|within|of|strict|placement|mixed|anchor|logic|biome|dimension|overworld|nether|end|y|radius|structure|mode|allow|loot|from|require|assert|skip|stop|continue|pause|scan|output|performance|workers|queue|survivor_cap|adaptive|completeness|safety|execution|stage|gpu_pipeline|pipeline|gpu_backend|backend|gpu_inflight|inflight|cap|surrogate|fixed_cap)\b/u)) {
      return "keyword";
    }
    if (stream.match(/\b(true|false|found|optional|auto|origin|spawn|exact|approximate|adaptive|fixed|none|balanced|aggressive|ultra|turbo|lightspeed|max-throughput|scalar|multi|single|cpu-a5|gpu-off|cuda|opencl)\b/u)) {
      return "atom";
    }
    if (stream.match(/[A-Za-z_][A-Za-z0-9_:-]*/u)) {
      return "variableName";
    }
    stream.next();
    return null;
  },
  languageData: {
    commentTokens: { line: "#" }
  }
});

const commandCompletions: Completion[] = [
  snippetCompletion("let ${name} = ${value}", {
    label: "let",
    type: "variable",
    detail: "create a variable",
    boost: 101
  }),
  snippetCompletion("const ${name} = ${value}", {
    label: "const",
    type: "variable",
    detail: "create a read-only variable",
    boost: 100
  }),
  snippetCompletion(
    `let structure = village
let radius = 64
if radius >= 64 and structure is village
  find $structure in overworld as v1 within $radius of origin strict
end`,
    { label: "if block", type: "class", detail: "conditional commands", boost: 99 }
  ),
  snippetCompletion(
    `if $radius of \${id} == \${radius}
  find ruined_portal in overworld as rp1 within 96 of constraint:\${id} strict
end`,
    { label: "if rule field", type: "class", detail: "condition using a rule id", boost: 98 }
  ),
  snippetCompletion("find ${structure} in overworld as ${id} within ${radius} of origin strict", {
    label: "find",
    type: "function",
    detail: "add a structure search rule",
    boost: 97
  }),
  snippetCompletion("let ${name} = [${a}, ${b}, ${c}]", {
    label: "list",
    type: "variable",
    detail: "create a list",
    boost: 96
  }),
  snippetCompletion(
    `for \${item} in [\${a}, \${b}, \${c}]
  find \${item} in overworld as \${id}_\${item} within 64 of origin strict
end`,
    { label: "for", type: "class", detail: "iterate over a list", boost: 95 }
  ),
  snippetCompletion("if ${list} contains ${value}", {
    label: "contains",
    type: "keyword",
    detail: "list / string membership",
    boost: 94
  }),
  snippetCompletion("assert ${condition} message ${message}", {
    label: "assert",
    type: "function",
    detail: "fail the script if a condition is false",
    boost: 94
  }),
  snippetCompletion("skip if ${condition}", {
    label: "skip if",
    type: "keyword",
    detail: "skip the current branch when true",
    boost: 93
  }),
  snippetCompletion("stop if ${condition}", {
    label: "stop if",
    type: "keyword",
    detail: "stop compiling the script when true",
    boost: 92
  }),
  snippetCompletion("output ${mode}", {
    label: "output",
    type: "function",
    detail: "seed output mode: raw, lift64, both, world64",
    boost: 91
  }),
  snippetCompletion(
    `function \${name}(\${params})
  \${body}
end`,
    { label: "function", type: "function", detail: "reusable HelixScript block", boost: 91 }
  ),
  snippetCompletion(
    `scan
  start 0
  count 1000000
  batch balanced
  max_print 20
  progress 1
  mode mixed
end`,
    { label: "scan block", type: "class", detail: "scan runtime settings", boost: 90 }
  ),
  snippetCompletion("biome origin in overworld y 64 radius ${radius} allow ${biome}", {
    label: "biome",
    type: "function",
    detail: "add a biome filter",
    boost: 90
  }),
  snippetCompletion("loot ${constraint} from auto require ${item}:${count}", {
    label: "loot",
    type: "function",
    detail: "add a strict loot requirement",
    boost: 89
  }),
  snippetCompletion(
    `performance
  workers 0
  queue 1000000
  adaptive on
  completeness exact
  execution auto
  stage auto
  cap none
  surrogate off
  gpu_pipeline auto
  gpu_backend auto
  gpu_inflight auto
  fixed_cap 128
end`,
    { label: "performance", type: "class", detail: "scanner tuning block", boost: 88 }
  ),
  { label: "anchor", type: "keyword", detail: "anchor origin|spawn", apply: "anchor origin" },
  { label: "output raw", type: "keyword", detail: "print lower48 seeds", apply: "output raw" },
  { label: "output both", type: "keyword", detail: "print raw and lifted seeds", apply: "output both" },
  { label: "logic", type: "keyword", detail: "logic and", apply: "logic and" },
  { label: "and", type: "keyword", detail: "condition operator", apply: "and" },
  { label: "or", type: "keyword", detail: "condition operator", apply: "or" },
  { label: "not", type: "keyword", detail: "condition operator", apply: "not" },
  { label: "$radius of v1", type: "variable", detail: "field lookup by rule id", apply: "$radius of v1" },
  { label: "$structure of v1", type: "variable", detail: "field lookup by rule id", apply: "$structure of v1" },
  { label: "$mode of v1", type: "variable", detail: "field lookup by rule id", apply: "$mode of v1" },
  { label: "$anchor of v1", type: "variable", detail: "field lookup by rule id", apply: "$anchor of v1" }
];

function wordCompletions(values: string[], detail: string): Completion[] {
  return values.map((value) => ({ label: value, type: "constant", detail, apply: value }));
}

const scriptBlockTemplates: Record<string, string> = {
  variable: "let radius = 64",
  list: "let biomes = [plains, meadow, cherry_grove]",
  for: `for biome in [plains, meadow, cherry_grove]
  biome origin in overworld y 64 radius 96 allow $biome
end`,
  if: `find village in overworld as v1 within 64 of origin strict
if $radius of v1 == 64 and $structure of v1 is village
  find ruined_portal in overworld as rp1 within 96 of constraint:v1 strict
end`,
  assert: "assert $radius of v1 > 0 message v1 radius must be positive",
  skip: `skip if $mode of v1 is placement
  find ruined_portal in overworld as rp1 within 96 of constraint:v1 strict
end`,
  stop: "stop if $radius of v1 > 512",
  output: "output raw",
  function: `function nearby_portal(anchor_id, portal_id, radius)
  find ruined_portal in overworld as $portal_id within $radius of constraint:$anchor_id strict
end

nearby_portal(v1, rp1, 96)`,
  scan: `scan
  start 0
  count 1000000
  batch balanced
  max_print 20
  progress 1
  mode mixed
end`,
  find: "find village in overworld as v1 within 64 of origin strict",
  biome: "biome origin in overworld y 64 radius 128 allow plains",
  loot: "loot v1 from auto require diamond:2",
  performance: `performance
  workers 0
  queue 1000000
  adaptive on
  completeness exact
  execution auto
  stage auto
  cap none
  surrogate off
  gpu_pipeline auto
  gpu_backend auto
  gpu_inflight auto
  fixed_cap 128
end`
};

function scriptCompletionSource(context: CompletionContext): CompletionResult | null {
  const token = context.matchBefore(/[A-Za-z0-9_:-]*/u);
  if (!token || (token.from === token.to && !context.explicit)) {
    return null;
  }
  const line = context.state.doc.lineAt(context.pos).text.slice(0, context.pos - context.state.doc.lineAt(context.pos).from);
  const trimmed = line.trimStart();
  const options =
    /^(let|set)\s+[A-Za-z0-9_:-]+\s*=\s*[A-Za-z0-9_:-]*$/u.test(trimmed)
      ? wordCompletions(["village", "origin", "64", "true", "false"], "starter value")
      : /^if\s+.*\s+(and|or|is|not)\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
        ? [...wordCompletions(["true", "false", "village", "strict", "placement", "origin", "spawn"], "condition value"), ...wordCompletions(Object.keys(scriptVariablesFromText(context.state.doc.toString())), "variable")]
        : /^if\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
          ? [...wordCompletions(["true", "false", "not"], "condition"), ...wordCompletions(Object.keys(scriptVariablesFromText(context.state.doc.toString())), "variable")]
      : /^find\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
        ? wordCompletions(structures, "structure")
      : /^find\s+\S+\s+in\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
        ? wordCompletions(dimensions, "dimension")
      : /^find\s+\S+(?:\s+in\s+\S+)?\s+as\s+\S+\s+within\s+\S+\s+of\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
        ? wordCompletions(currentConstraintAnchors(), "anchor")
        : /^find\s+\S+(?:\s+in\s+\S+)?\s+as\s+\S+\s+within\s+\S+\s+of\s+\S+\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
          ? wordCompletions(["strict", "placement", "mixed"], "mode")
          : /^anchor\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
            ? wordCompletions(["origin", "spawn"], "anchor")
            : /^biome\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
              ? wordCompletions(currentConstraintAnchors(), "sample point")
              : /^biome\s+\S+\s+in\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
                ? wordCompletions(dimensions, "dimension")
              : /^biome\s+\S+(?:\s+in\s+\S+)?\s+y\s+-?\d+\s+radius\s+\d+\s+allow\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
                ? wordCompletions(biomeSuggestions, "biome")
              : /^loot\s+\S+\s+from\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
                ? wordCompletions(["auto", ...supportedLootSourcesForVersion(state.settings.mcVersion)], "loot source")
                : /^\s*(mode|scan_mode|execution|stage|gpu_pipeline|pipeline|gpu_backend|backend|gpu_inflight|inflight|cap|surrogate|adaptive|completeness|safety)\s+[A-Za-z0-9_:-]*$/u.test(trimmed)
                  ? wordCompletions(["placement", "strict", "mixed", "exact", "approximate", "auto", "async", "sync", "scalar", "simd-x4", "simd-x8", "max-throughput", "single", "multi", "cpu-a5", "gpu-off", "adaptive", "fixed", "none", "off", "on", "balanced", "aggressive", "ultra", "turbo", "lightspeed", "cuda", "opencl"], "performance value")
                  : commandCompletions;
  return {
    from: token.from,
    options,
    validFor: /^[A-Za-z0-9_:-]*$/u
  };
}

function installDevPreviewApi(): void {
  if (window.helixSeed || !import.meta.env.DEV) {
    return;
  }

  const lineCallbacks: Array<(line: string) => void> = [];
  const progressCallbacks: Array<(payload: ScannerProgress) => void> = [];
  const seedCallbacks: Array<(seed: string) => void> = [];
  const exitCallbacks: Array<(payload: { code: number | null; signal: NodeJS.Signals | null }) => void> = [];
  let timers: number[] = [];

  const clearTimers = (): void => {
    for (const timer of timers) {
      window.clearTimeout(timer);
    }
    timers = [];
  };

  window.helixSeed = {
    getContext: async () => ({
      repoRoot: "dev-preview",
      scannerPath: "dev-preview",
      scannerExists: true,
      packaged: false
    }),
    openPath: async () => undefined,
    openExternal: async (url: string) => {
      window.open(url, "_blank");
    },
    openDocs: async () => {
      window.open("guide.html", "helixseed-docs", "width=980,height=820");
    },
    startScan: async (request) => {
      clearTimers();
      lineCallbacks.forEach((callback) => callback(`[dev] ${request.args.join(" ")}`));
      progressCallbacks.forEach((callback) => callback({ processed: 0, total: 1000000, pct: 0, rate_sps: 0 }));
      timers.push(
        window.setTimeout(() => {
          progressCallbacks.forEach((callback) =>
            callback({ processed: 250000, total: 1000000, pct: 25, rate_sps: 720000, stage_a_survivors: 12 })
          );
        }, 250),
        window.setTimeout(() => {
          seedCallbacks.forEach((callback) => callback("raw=123456789 lift64=987654321"));
          progressCallbacks.forEach((callback) =>
            callback({ processed: 1000000, total: 1000000, pct: 100, rate_sps: 980000, stage_a_survivors: 18, cpu_hits: 1, done: true })
          );
          exitCallbacks.forEach((callback) => callback({ code: 0, signal: null }));
        }, 600)
      );
      return { ok: true, command: request.args.join(" "), scannerPath: "dev-preview", repoRoot: "dev-preview" };
    },
    stopScan: async () => {
      clearTimers();
      exitCallbacks.forEach((callback) => callback({ code: 0, signal: null }));
      return true;
    },
    sendControl: async () => true,
    onProgress: (callback) => {
      progressCallbacks.push(callback);
      return () => progressCallbacks.splice(progressCallbacks.indexOf(callback), 1);
    },
    onLine: (callback) => {
      lineCallbacks.push(callback);
      return () => lineCallbacks.splice(lineCallbacks.indexOf(callback), 1);
    },
    onSeed: (callback) => {
      seedCallbacks.push(callback);
      return () => seedCallbacks.splice(seedCallbacks.indexOf(callback), 1);
    },
    onExit: (callback) => {
      exitCallbacks.push(callback);
      return () => exitCallbacks.splice(exitCallbacks.indexOf(callback), 1);
    }
  } satisfies HelixSeedApi;
}

installDevPreviewApi();

const $ = <T extends HTMLElement>(selector: string): T => {
  const node = document.querySelector<T>(selector);
  if (!node) {
    throw new Error(`Missing element: ${selector}`);
  }
  return node;
};

function queryEditorText(): string {
  return queryEditor?.state.doc.toString() ?? $<HTMLTextAreaElement>("#query-editor").value;
}

function setQueryEditorText(value: string): void {
  const backingTextarea = $<HTMLTextAreaElement>("#query-editor");
  backingTextarea.value = value;
  if (!queryEditor || queryEditor.state.doc.toString() === value) {
    return;
  }

  syncingQueryEditor = true;
  queryEditor.dispatch({
    changes: { from: 0, to: queryEditor.state.doc.length, insert: value }
  });
  syncingQueryEditor = false;
}

function showQueryAutocomplete(): void {
  if (!queryEditor) {
    return;
  }
  queryEditor.focus();
  startCompletion(queryEditor);
}

function insertScriptBlock(kind: string): void {
  if (!queryEditor) {
    return;
  }
  const firstConstraintId = state.constraints.find((row) => row.id.trim())?.id.trim();
  const block = kind === "loot" && firstConstraintId
    ? `loot ${firstConstraintId} from auto require diamond:2`
    : scriptBlockTemplates[kind];
  if (!block) {
    return;
  }
  const selection = queryEditor.state.selection.main;
  const before = queryEditor.state.sliceDoc(Math.max(0, selection.from - 1), selection.from);
  const after = queryEditor.state.sliceDoc(selection.to, Math.min(queryEditor.state.doc.length, selection.to + 1));
  const insert = `${before && before !== "\n" ? "\n" : ""}${block}${after && after !== "\n" ? "\n" : ""}`;
  queryEditor.focus();
  queryEditor.dispatch({
    changes: { from: selection.from, to: selection.to, insert },
    selection: { anchor: selection.from + insert.length }
  });
  state.useScriptSource = true;
  syncControlsFromState();
  updateCommandPreview();
}

function initializeQueryEditor(): void {
  if (queryEditor) {
    return;
  }

  const backingTextarea = $<HTMLTextAreaElement>("#query-editor");
  queryEditor = new EditorView({
    parent: $("#query-editor-host"),
    state: EditorState.create({
      doc: backingTextarea.value,
      extensions: [
        basicSetup,
        keymap.of([{ key: "Ctrl-Space", run: startCompletion }, indentWithTab]),
        helixScriptLanguage,
        syntaxHighlighting(queryHighlightStyle),
        linter((view) => {
          try {
            compileQueryScript(view.state.doc.toString());
            return [];
          } catch (error) {
            const lineNo = error instanceof ScriptParseError ? error.line : 1;
            const line = view.state.doc.line(Math.max(1, Math.min(view.state.doc.lines, lineNo)));
            return [{ from: line.from, to: line.to, severity: "error", message: error instanceof Error ? error.message : String(error) }];
          }
        }),
        lintGutter(),
        autocompletion({
          activateOnTyping: true,
          activateOnTypingDelay: 80,
          override: [scriptCompletionSource]
        }),
        queryEditorTheme,
        EditorView.lineWrapping,
        EditorView.updateListener.of((update) => {
          if (!update.docChanged) {
            return;
          }
          backingTextarea.value = update.state.doc.toString();
          if (syncingQueryEditor) {
            return;
          }
          state.useScriptSource = true;
          syncControlsFromState();
          updateCommandPreview();
        })
      ]
    })
  });
}

function escapeHtml(value: unknown): string {
  return String(value)
    .replace(/&/gu, "&amp;")
    .replace(/</gu, "&lt;")
    .replace(/>/gu, "&gt;")
    .replace(/"/gu, "&quot;")
    .replace(/'/gu, "&#39;");
}

async function copyText(value: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(value);
    return true;
  } catch {
    const fallback = document.createElement("textarea");
    fallback.value = value;
    fallback.setAttribute("readonly", "true");
    fallback.style.position = "fixed";
    fallback.style.left = "-9999px";
    fallback.style.opacity = "0";
    document.body.append(fallback);
    fallback.focus();
    fallback.select();
    try {
      return document.execCommand("copy");
    } finally {
      fallback.remove();
    }
  }
}

function dropdownValues(values: string[], selected: string): string[] {
  const normalized = selected.trim();
  const all = values.includes(normalized) ? values : [normalized, ...values];
  return all.filter(Boolean);
}

function attrList(attrs: Record<string, string>): string {
  return Object.entries(attrs)
    .map(([key, value]) => `${key}="${escapeHtml(value)}"`)
    .join(" ");
}

function dropdownControl(
  values: string[],
  selected: string,
  attrs: Record<string, string>,
  controlAttrs: Record<string, string> = {}
): string {
  const normalized = selected.trim();
  const options = dropdownValues(values, normalized);
  const optionsData = options.map((value) => value.replace(/\\/gu, "\\\\").replace(/\|/gu, "\\|")).join("|");
  const items = options
    .map(
      (value) => `
        <button class="dropdown-option${value === normalized ? " selected" : ""}" type="button" role="option" data-value="${escapeHtml(value)}" aria-selected="${value === normalized ? "true" : "false"}">${escapeHtml(value)}</button>
      `
    )
    .join("");
  const label = options.includes(normalized) ? normalized : options[0] ?? "Select";
  return `
    <div class="dropdown-control" data-dropdown data-options="${escapeHtml(optionsData)}" ${attrList(controlAttrs)}>
      <input type="hidden" ${attrList(attrs)} value="${escapeHtml(normalized)}" />
      <button class="dropdown-button" type="button" aria-haspopup="listbox" aria-expanded="false">${escapeHtml(label)}</button>
      <div class="dropdown-menu" role="listbox">${items}</div>
    </div>
  `;
}

let openDropdown: HTMLElement | null = null;

function parseDropdownOptions(control: HTMLElement): string[] {
  const input = control.querySelector<HTMLInputElement>('input[type="hidden"]');
  const values: string[] = [];
  let escaped = false;
  let current = "";
  for (const char of control.dataset.options ?? "") {
    if (escaped) {
      current += char;
      escaped = false;
    } else if (char === "\\") {
      escaped = true;
    } else if (char === "|") {
      if (current) {
        values.push(current);
      }
      current = "";
    } else {
      current += char;
    }
  }
  if (current) {
    values.push(current);
  }
  if (input?.value && !values.includes(input.value)) {
    values.unshift(input.value);
  }
  return values;
}

function filterDropdownOptions(control: HTMLElement): void {
  const search = control.querySelector<HTMLInputElement>(".dropdown-search");
  const menu = control.querySelector<HTMLElement>(".dropdown-menu");
  if (!menu) {
    return;
  }
  const query = normalizeName(search?.value ?? "");
  let visible = 0;
  menu.querySelectorAll<HTMLButtonElement>(".dropdown-option").forEach((item) => {
    const value = normalizeName(item.dataset.value ?? item.textContent ?? "");
    const show = query.length === 0 || value.includes(query);
    item.hidden = !show;
    if (show) {
      visible += 1;
    }
  });
  let empty = menu.querySelector<HTMLElement>(".dropdown-empty");
  if (!empty) {
    empty = document.createElement("div");
    empty.className = "dropdown-empty";
    empty.textContent = "No matches";
    menu.append(empty);
  }
  empty.hidden = visible !== 0;
}

function closeDropdown(): void {
  if (!openDropdown) {
    return;
  }
  openDropdown.classList.remove("open");
  openDropdown.querySelector<HTMLButtonElement>(".dropdown-button")?.setAttribute("aria-expanded", "false");
  openDropdown = null;
}

function syncDropdownControl(control: HTMLElement): void {
  const input = control.querySelector<HTMLInputElement>('input[type="hidden"]');
  const button = control.querySelector<HTMLButtonElement>(".dropdown-button");
  const menu = control.querySelector<HTMLElement>(".dropdown-menu");
  if (!input || !button || !menu) {
    return;
  }
  const values = parseDropdownOptions(control);
  const selected = values.includes(input.value) ? input.value : values[0] ?? input.value;
  if (selected !== input.value) {
    input.value = selected;
  }
  button.textContent = selected || "Select";
  button.title = selected || "Select";
  menu.innerHTML = "";
  if (control.dataset.searchable === "true") {
    const search = document.createElement("input");
    search.type = "search";
    search.className = "dropdown-search";
    search.placeholder = control.dataset.searchPlaceholder ?? "Search";
    search.autocomplete = "off";
    search.spellcheck = false;
    menu.append(search);
  }
  values.forEach((value) => {
    const item = document.createElement("button");
    item.type = "button";
    item.className = "dropdown-option";
    item.role = "option";
    item.dataset.value = value;
    item.textContent = value;
    const active = value === selected;
    item.classList.toggle("selected", active);
    item.setAttribute("aria-selected", active ? "true" : "false");
    menu.append(item);
  });
  filterDropdownOptions(control);
}

function syncDropdowns(root: ParentNode = document): void {
  root.querySelectorAll<HTMLElement>("[data-dropdown]").forEach((control) => syncDropdownControl(control));
}

function setDropdownValue(control: HTMLElement, value: string): void {
  const input = control.querySelector<HTMLInputElement>('input[type="hidden"]');
  if (!input) {
    return;
  }
  input.value = value;
  syncDropdownControl(control);
  input.dispatchEvent(new Event("input", { bubbles: true }));
  input.dispatchEvent(new Event("change", { bubbles: true }));
}

document.addEventListener("click", (event) => {
  const target = event.target;
  if (!(target instanceof HTMLElement)) {
    closeDropdown();
    return;
  }
  const option = target.closest<HTMLElement>(".dropdown-option");
  if (option) {
    const control = option.closest<HTMLElement>("[data-dropdown]");
    if (control) {
      setDropdownValue(control, option.dataset.value ?? "");
      closeDropdown();
    }
    return;
  }
  const button = target.closest<HTMLButtonElement>(".dropdown-button");
  if (button) {
    const control = button.closest<HTMLElement>("[data-dropdown]");
    if (!control) {
      closeDropdown();
      return;
    }
    const opening = !control.classList.contains("open");
    closeDropdown();
    if (opening) {
      syncDropdownControl(control);
      control.classList.add("open");
      button.setAttribute("aria-expanded", "true");
      openDropdown = control;
    }
    return;
  }
  if (!target.closest("[data-dropdown]")) {
    closeDropdown();
  }
});

document.addEventListener("input", (event) => {
  const target = event.target;
  if (!(target instanceof HTMLInputElement) || !target.classList.contains("dropdown-search")) {
    return;
  }
  const control = target.closest<HTMLElement>("[data-dropdown]");
  if (control) {
    filterDropdownOptions(control);
  }
});

document.addEventListener("keydown", (event) => {
  const target = event.target;
  if (!(target instanceof HTMLElement)) {
    return;
  }
  const button = target.closest<HTMLButtonElement>(".dropdown-button");
  if (!button) {
    if (event.key === "Escape") {
      closeDropdown();
    }
    return;
  }
  const control = button.closest<HTMLElement>("[data-dropdown]");
  if (!control) {
    return;
  }
  if (event.key === "Escape") {
    closeDropdown();
    return;
  }
  if ((event.key === "ArrowUp" || event.key === "ArrowDown") && control.classList.contains("open")) {
    event.preventDefault();
    const input = control.querySelector<HTMLInputElement>('input[type="hidden"]');
    const values = parseDropdownOptions(control);
    if (!input || values.length === 0) {
      return;
    }
    const current = Math.max(0, values.indexOf(input.value));
    const delta = event.key === "ArrowDown" ? 1 : -1;
    setDropdownValue(control, values[(current + delta + values.length) % values.length]);
    return;
  }
  if (event.key === "Enter" || event.key === " " || event.key === "ArrowDown") {
    event.preventDefault();
    const opening = !control.classList.contains("open");
    closeDropdown();
    if (opening) {
      syncDropdownControl(control);
      control.classList.add("open");
      button.setAttribute("aria-expanded", "true");
      openDropdown = control;
    }
    return;
  }
});

function normalizeId(value: string): string {
  return value.trim();
}

function normalizeName(value: string): string {
  return value.trim().toLowerCase().replace(/[\s-]+/gu, "_");
}

function parseNumberText(value: string, field: string, min?: number): number {
  const raw = value.trim();
  if (!raw) {
    throw new Error(`${field} is required.`);
  }
  const n = Number.parseInt(raw, raw.startsWith("0x") ? 16 : 10);
  if (!Number.isFinite(n)) {
    throw new Error(`Invalid integer for ${field}: ${value}`);
  }
  if (min !== undefined && n < min) {
    throw new Error(`${field} must be >= ${min}.`);
  }
  return n;
}

function parseBigIntText(value: string, field: string, min?: bigint): bigint {
  const raw = value.trim();
  if (!raw) {
    throw new Error(`${field} is required.`);
  }
  try {
    const n = BigInt(raw);
    if (min !== undefined && n < min) {
      throw new Error(`${field} must be >= ${min.toString()}.`);
    }
    return n;
  } catch (error) {
    if (error instanceof Error && error.message.includes("must be")) {
      throw error;
    }
    throw new Error(`Invalid integer for ${field}: ${value}`);
  }
}

function parseChoice<T extends string>(value: string, allowed: readonly T[], field: string): T {
  const normalized = value.trim();
  if ((allowed as readonly string[]).includes(normalized)) {
    return normalized as T;
  }
  throw new Error(`${field} must be one of: ${allowed.join(", ")}.`);
}

function parseLootRequirements(text: string): Record<string, number> {
  const required: Record<string, number> = {};
  const parts = text.split(",").map((part) => part.trim()).filter(Boolean);
  if (parts.length === 0) {
    throw new Error("Loot filters need at least one required item.");
  }
  for (const part of parts) {
    const [rawItem, rawCount] = part.includes(":") ? part.split(/:(?=[^:]*$)/u) : [part, "1"];
    let item = normalizeName(rawItem);
    if (item.startsWith("minecraft:")) {
      item = item.slice("minecraft:".length);
    }
    if (!item) {
      throw new Error("Loot requirements need item names like diamond:2.");
    }
    const count = parseNumberText(rawCount || "1", `loot count for ${item}`, 1);
    required[item] = (required[item] ?? 0) + count;
  }
  return required;
}

function validateLootFilterRows(
  lootRows: LootRow[],
  constraintRows: ConstraintRow[],
  version: string
): void {
  const constraintsById = new Map(constraintRows.map((row) => [row.id.trim(), row]));
  const supported = supportedLootSourcesForVersion(version);
  for (const [index, row] of lootRows.entries()) {
    if (!row.constraint.trim() && !row.required.trim()) {
      continue;
    }
    const constraint = constraintsById.get(row.constraint.trim());
    if (!constraint) {
      throw new Error(`Loot row ${index + 1} references a missing constraint.`);
    }
    if (supported.length === 0) {
      throw new Error(`Loot filters are not supported for MC ${version}. Use 26.1.2, 26.1.1, 1.21.11, 1.17, or 1.17.1.`);
    }
    const constraintFamily = lootFamilyForSource(constraint.structure);
    const inferredSource = inferredLootSourceForConstraint(constraint, version);
    const rowSources = lootSourcesForConstraintStructure(constraint.structure, version);
    if (!inferredSource && rowSources.length === 0 && !supported.includes(constraintFamily)) {
      throw new Error(`Loot row ${index + 1}: ${constraintFamily} loot is not supported for MC ${version}.`);
    }
    const source = normalizeName(row.source || "auto");
    if (source !== "auto") {
      const sourceStructure = canonicalLootSourceName(source);
      const supportedCanonical = new Set(supported.map(canonicalLootSourceName));
      if (!supportedCanonical.has(sourceStructure)) {
        throw new Error(`Loot row ${index + 1}: ${sourceStructure} loot is not supported for MC ${version}.`);
      }
      if (lootFamilyForSource(sourceStructure) !== constraintFamily) {
        throw new Error(`Loot row ${index + 1}: loot source must belong to the referenced ${constraintFamily} constraint.`);
      }
    }
    parseLootRequirements(row.required);
  }
}

function validateQueryLootFilters(query: QueryObject, version: string): void {
  const rows = query.loot_filters.map((row) => ({
    constraint: row.constraint,
    source: row.structure,
    required: Object.entries(row.required).map(([item, count]) => `${item}:${count}`).join(",")
  }));
    const constraints = query.constraints.map((row) => ({
      id: row.id,
      structure: row.structure,
      dimension: row.dimension,
      anchor: row.within.anchor,
      radius: String(row.within.radius),
      mode: row.mode === "placement" ? "placement" : row.mode === "mixed" ? "mixed" : "strict",
      structureSettings: row.structure_settings
    }));
  for (const [index, row] of constraints.entries()) {
    if (structureRequiresStrict(row.structure) && row.mode !== "strict") {
      throw new Error(`Constraint ${row.id || index + 1}: ${row.structure} requires strict mode.`);
    }
  }
  validateLootFilterRows(rows, constraints, version);
}

function currentConstraintAnchors(): string[] {
  const ids = state.constraints.map((row) => row.id.trim()).filter(Boolean).map((id) => `constraint:${id}`);
  return ["origin", "spawn", ...ids];
}

class ScriptParseError extends Error {
  constructor(message: string, readonly line: number) {
    super(`Line ${line}: ${message}`);
  }
}

function scriptError(line: number, message: string): never {
  throw new ScriptParseError(message, line);
}

type ScriptList = { kind: "list"; items: ScriptValue[] };
type ScriptValue = string | number | boolean | ScriptList;

function isScriptList(value: ScriptValue): value is ScriptList {
  return typeof value === "object" && value !== null && (value as ScriptList).kind === "list";
}

function scriptValueToString(value: ScriptValue): string {
  if (isScriptList(value)) {
    return value.items.map(scriptValueToString).join(",");
  }
  return String(value);
}

function serializeScriptValue(value: ScriptValue): string {
  if (typeof value === "number" || typeof value === "boolean") {
    return String(value);
  }
  if (isScriptList(value)) {
    return `[${value.items.map(serializeScriptValue).join(", ")}]`;
  }
  return JSON.stringify(value);
}

function resolveForList(
  rhs: string,
  loopVar: string,
  variables: Record<string, ScriptValue>,
  lineNo: number
): ScriptList {
  const trimmed = rhs.trim();
  if (trimmed.startsWith("[")) {
    const substituted = substituteScriptVariables(trimmed, variables, lineNo);
    try {
      return parseListLiteral(substituted);
    } catch (error) {
      scriptError(lineNo, error instanceof Error ? error.message : String(error));
    }
  }
  const ref = trimmed.replace(/^\$\{?/u, "").replace(/\}$/u, "").trim();
  if (!(ref in variables)) {
    scriptError(lineNo, `Unknown variable "${ref}". Use a list literal like [a, b, c] or define ${loopVar}'s source with: let ${ref} = [a, b, c]`);
  }
  const value = variables[ref];
  if (!isScriptList(value)) {
    scriptError(lineNo, `"${ref}" is not a list. Use let ${ref} = [a, b, c] before iterating.`);
  }
  return value;
}

function parseListLiteral(raw: string): ScriptList {
  const trimmed = raw.trim();
  if (!trimmed.startsWith("[") || !trimmed.endsWith("]")) {
    throw new Error(`Expected list literal, got: ${raw}`);
  }
  const inner = trimmed.slice(1, -1).trim();
  const items: ScriptValue[] = [];
  if (!inner) {
    return { kind: "list", items };
  }
  let depth = 0;
  let quote: string | null = null;
  let start = 0;
  for (let i = 0; i < inner.length; i += 1) {
    const ch = inner[i];
    if (quote) {
      if (ch === "\\" && i + 1 < inner.length) {
        i += 1;
      } else if (ch === quote) {
        quote = null;
      }
      continue;
    }
    if (ch === '"' || ch === "'") {
      quote = ch;
      continue;
    }
    if (ch === "[" || ch === "(") {
      depth += 1;
    } else if (ch === "]" || ch === ")") {
      depth -= 1;
    } else if (ch === "," && depth === 0) {
      items.push(parseScriptValue(inner.slice(start, i)));
      start = i + 1;
    }
  }
  const tail = inner.slice(start).trim();
  if (tail) {
    items.push(parseScriptValue(tail));
  }
  return { kind: "list", items };
}

function tryEvaluateMath(input: string): number | null {
  const text = input.trim();
  if (!text) {
    return null;
  }
  if (!/[+\-*/%(]/u.test(text) && !/\b(min|max|abs|floor|ceil|round)\s*\(/u.test(text)) {
    return null;
  }
  if (!/^[\d+\-*/%().,\s\w]+$/u.test(text)) {
    return null;
  }
  let pos = 0;
  const skip = (): void => {
    while (pos < text.length && /\s/u.test(text[pos])) {
      pos += 1;
    }
  };
  const parseAtom = (): number | null => {
    skip();
    if (text[pos] === "(") {
      pos += 1;
      const value = parseAdd();
      skip();
      if (text[pos] !== ")") {
        return null;
      }
      pos += 1;
      return value;
    }
    const fn = text.slice(pos).match(/^(min|max|abs|floor|ceil|round)\s*\(/u);
    if (fn) {
      const name = fn[1];
      pos += fn[0].length;
      const args: number[] = [];
      const first = parseAdd();
      if (first === null) {
        return null;
      }
      args.push(first);
      skip();
      while (text[pos] === ",") {
        pos += 1;
        const next = parseAdd();
        if (next === null) {
          return null;
        }
        args.push(next);
        skip();
      }
      if (text[pos] !== ")") {
        return null;
      }
      pos += 1;
      switch (name) {
        case "min": return args.length > 0 ? Math.min(...args) : null;
        case "max": return args.length > 0 ? Math.max(...args) : null;
        case "abs": return Math.abs(args[0]);
        case "floor": return Math.floor(args[0]);
        case "ceil": return Math.ceil(args[0]);
        case "round": return Math.round(args[0]);
      }
      return null;
    }
    const numMatch = text.slice(pos).match(/^\d+(?:\.\d+)?/u);
    if (!numMatch) {
      return null;
    }
    pos += numMatch[0].length;
    return Number(numMatch[0]);
  };
  const parseUnary = (): number | null => {
    skip();
    if (text[pos] === "-") {
      pos += 1;
      const v = parseUnary();
      return v === null ? null : -v;
    }
    if (text[pos] === "+") {
      pos += 1;
      return parseUnary();
    }
    return parseAtom();
  };
  const parseMul = (): number | null => {
    let left = parseUnary();
    if (left === null) {
      return null;
    }
    while (true) {
      skip();
      const op = text[pos];
      if (op !== "*" && op !== "/" && op !== "%") {
        break;
      }
      pos += 1;
      const right = parseUnary();
      if (right === null) {
        return null;
      }
      if (op === "*") {
        left = left * right;
      } else if (op === "/") {
        if (right === 0) {
          return null;
        }
        left = left / right;
      } else {
        left = left % right;
      }
    }
    return left;
  };
  const parseAdd = (): number | null => {
    let left = parseMul();
    if (left === null) {
      return null;
    }
    while (true) {
      skip();
      const op = text[pos];
      if (op !== "+" && op !== "-") {
        break;
      }
      pos += 1;
      const right = parseMul();
      if (right === null) {
        return null;
      }
      left = op === "+" ? left + right : left - right;
    }
    return left;
  };
  const result = parseAdd();
  skip();
  if (pos !== text.length) {
    return null;
  }
  return result !== null && Number.isFinite(result) ? result : null;
}

function stripScriptComment(raw: string): string {
  let quote: string | null = null;
  for (let i = 0; i < raw.length; i += 1) {
    const ch = raw[i];
    const next = raw[i + 1];
    if (quote) {
      if (ch === "\\" && i + 1 < raw.length) {
        i += 1;
      } else if (ch === quote) {
        quote = null;
      }
      continue;
    }
    if (ch === '"' || ch === "'") {
      quote = ch;
      continue;
    }
    if (ch === "#") {
      return raw.slice(0, i);
    }
    if (ch === "/" && next === "/") {
      return raw.slice(0, i);
    }
  }
  return raw;
}

function cleanScriptLine(raw: string): string {
  return stripScriptComment(raw).trim().replace(/;+\s*$/u, "").trim();
}

function splitScriptArgs(raw: string): string[] {
  const args: string[] = [];
  let current = "";
  let quote: string | null = null;
  let depth = 0;
  for (let i = 0; i < raw.length; i += 1) {
    const ch = raw[i];
    if (quote) {
      current += ch;
      if (ch === "\\" && i + 1 < raw.length) {
        current += raw[i + 1];
        i += 1;
      } else if (ch === quote) {
        quote = null;
      }
      continue;
    }
    if (ch === '"' || ch === "'") {
      quote = ch;
      current += ch;
      continue;
    }
    if (ch === "(" || ch === "[") {
      depth += 1;
      current += ch;
      continue;
    }
    if (ch === ")" || ch === "]") {
      depth = Math.max(0, depth - 1);
      current += ch;
      continue;
    }
    if (ch === "," && depth === 0) {
      args.push(current.trim());
      current = "";
      continue;
    }
    current += ch;
  }
  if (current.trim() || raw.trim()) {
    args.push(current.trim());
  }
  return args.filter((arg) => arg.length > 0);
}

function substituteFunctionParams(line: string, params: string[], args: string[]): string {
  let out = line;
  params.forEach((param, index) => {
    const value = args[index] ?? "";
    const escaped = param.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&");
    out = out
      .replace(new RegExp(`\\$\\{${escaped}\\}`, "gu"), value)
      .replace(new RegExp(`\\$${escaped}\\b`, "gu"), value);
  });
  return out;
}

function expandScriptFunctions(text: string): ScriptLine[] {
  const functions = new Map<string, ScriptFunctionDef>();
  const topLevel: ScriptLine[] = [];
  const sourceLines = text.split(/\r?\n/u);

  for (let index = 0; index < sourceLines.length; index += 1) {
    const lineNo = index + 1;
    const line = cleanScriptLine(sourceLines[index]);
    const header = line.match(/^(?:fn|function)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)$/iu);
    if (!header) {
      topLevel.push({ text: sourceLines[index], line: lineNo });
      continue;
    }

    const name = header[1];
    if (functions.has(name)) {
      scriptError(lineNo, `Function "${name}" is already defined.`);
    }
    const params = header[2].split(",").map((part) => part.trim()).filter(Boolean);
    const body: ScriptLine[] = [];
    let foundEnd = false;
    let blockDepth = 0;
    for (index += 1; index < sourceLines.length; index += 1) {
      const bodyLineNo = index + 1;
      const bodyLine = cleanScriptLine(sourceLines[index]);
      if (bodyLine === "end" && blockDepth === 0) {
        foundEnd = true;
        break;
      }
      if (bodyLine === "end") {
        blockDepth = Math.max(0, blockDepth - 1);
      }
      body.push({ text: sourceLines[index], line: bodyLineNo });
      if (/^(if|skip|performance|scan|for)\b/iu.test(bodyLine)) {
        blockDepth += 1;
      }
    }
    if (!foundEnd) {
      scriptError(lineNo, `Function "${name}" is missing end.`);
    }
    functions.set(name, { params, body });
  }

  const expandLine = (entry: ScriptLine, depth: number): ScriptLine[] => {
    if (depth > 16) {
      scriptError(entry.line, "Function expansion is too deep. Check for recursive functions.");
    }
    const line = cleanScriptLine(entry.text);
    const call = line.match(/^(?:call\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)$/iu);
    if (!call || !functions.has(call[1])) {
      return [entry];
    }
    const fn = functions.get(call[1])!;
    const args = splitScriptArgs(call[2]);
    if (args.length !== fn.params.length) {
      scriptError(entry.line, `Function "${call[1]}" expects ${fn.params.length} args, got ${args.length}.`);
    }
    return fn.body.flatMap((bodyLine) =>
      expandLine({ text: substituteFunctionParams(bodyLine.text, fn.params, args), line: entry.line }, depth + 1)
    );
  };

  return topLevel.flatMap((line) => expandLine(line, 0));
}

function parseScriptValue(raw: string): ScriptValue {
  const value = raw.trim();
  if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
    return value.slice(1, -1);
  }
  if (value.startsWith("[") && value.endsWith("]")) {
    return parseListLiteral(value);
  }
  if (/^-?\d+(?:\.\d+)?$/u.test(value)) {
    return Number(value);
  }
  if (value === "true") {
    return true;
  }
  if (value === "false") {
    return false;
  }
  const math = tryEvaluateMath(value);
  if (math !== null) {
    return math;
  }
  return value;
}

function scriptVariablesFromText(text: string): Record<string, ScriptValue> {
  const variables: Record<string, ScriptValue> = {};
  for (const rawLine of text.split(/\r?\n/u)) {
    const line = cleanScriptLine(rawLine);
    const match = line.match(/^(?:let|set|const)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$/iu);
    if (match) {
      variables[match[1]] = parseScriptValue(match[2]);
    }
  }
  return variables;
}

function substituteScriptVariables(line: string, variables: Record<string, ScriptValue>, lineNo: number): string {
  return line.replace(
    /\$\{?([A-Za-z_][A-Za-z0-9_]*)((?:\.[A-Za-z_]+|\[\d+\]))?\}?/gu,
    (_match, name: string, accessor?: string) => {
      if (!(name in variables)) {
        scriptError(lineNo, `Unknown variable "${name}". Define it first with: let ${name} = value`);
      }
      const value = variables[name];
      if (!accessor) {
        return scriptValueToString(value);
      }
      if (accessor.startsWith("[")) {
        const idx = Number.parseInt(accessor.slice(1, -1), 10);
        if (!isScriptList(value)) {
          scriptError(lineNo, `Cannot index "${name}" because it is not a list. Use let ${name} = [a, b, c].`);
        }
        if (idx < 0 || idx >= value.items.length) {
          scriptError(lineNo, `Index ${idx} is out of range for list "${name}" (length ${value.items.length}).`);
        }
        return scriptValueToString(value.items[idx]);
      }
      const prop = accessor.slice(1).toLowerCase();
      if (prop === "length") {
        return isScriptList(value) ? String(value.items.length) : String(scriptValueToString(value).length);
      }
      if (prop === "first" || prop === "last") {
        if (!isScriptList(value)) {
          scriptError(lineNo, `Cannot read .${prop} on "${name}" because it is not a list.`);
        }
        if (value.items.length === 0) {
          scriptError(lineNo, `List "${name}" is empty; .${prop} has no value.`);
        }
        return scriptValueToString(prop === "first" ? value.items[0] : value.items[value.items.length - 1]);
      }
      scriptError(lineNo, `Unknown property ".${prop}" on "${name}". Use .length, .first, .last, or [index].`);
    }
  );
}

function constraintFieldValue(
  field: string,
  id: string,
  constraints: ScriptConstraint[],
  lineNo: number
): ScriptValue {
  const constraint = constraints.find((entry) => entry.id === id);
  if (!constraint) {
    scriptError(lineNo, `Unknown rule id "${id}". Add a find command before this condition.`);
  }

  const normalizedField = field.replace(/^\$[{]?/u, "").replace(/[}]$/u, "").toLowerCase();
  if (normalizedField === "id") {
    return constraint.id;
  }
  if (normalizedField === "structure") {
    return constraint.structure;
  }
  if (normalizedField === "dimension") {
    return constraint.dimension;
  }
  if (normalizedField === "mode") {
    return constraint.mode;
  }
  if (normalizedField === "anchor") {
    return constraint.within.anchor;
  }
  if (normalizedField === "radius") {
    return constraint.within.radius;
  }

  scriptError(lineNo, `Unknown field "${field}" for rule "${id}". Use radius, structure, dimension, mode, anchor, or id.`);
}

function resolveConditionValue(
  raw: string,
  variables: Record<string, ScriptValue>,
  constraints: ScriptConstraint[],
  lineNo: number
): ScriptValue {
  const scoped = raw.trim().match(/^(\$?\{?[A-Za-z_][A-Za-z0-9_]*\}?)\s+of\s+([A-Za-z_][A-Za-z0-9_-]*)$/iu);
  if (scoped) {
    return constraintFieldValue(scoped[1], scoped[2], constraints, lineNo);
  }

  const token = raw.trim().replace(/^\$[{]?/u, "").replace(/[}]$/u, "");
  if (token in variables) {
    return variables[token];
  }
  return parseScriptValue(raw);
}

function compareScriptValues(left: ScriptValue, op: string, right: ScriptValue): boolean {
  if (op === "contains") {
    if (isScriptList(left)) {
      const needle = scriptValueToString(right);
      return left.items.some((item) => scriptValueToString(item) === needle);
    }
    return scriptValueToString(left).includes(scriptValueToString(right));
  }
  if (typeof left === "number" && typeof right === "number") {
    if (op === ">") return left > right;
    if (op === ">=") return left >= right;
    if (op === "<") return left < right;
    if (op === "<=") return left <= right;
  }
  if (op === "==" || op === "is") {
    return scriptValueToString(left) === scriptValueToString(right);
  }
  if (op === "!=" || op === "is not") {
    return scriptValueToString(left) !== scriptValueToString(right);
  }
  return false;
}

function parseScriptBoolean(value: string, field: string, lineNo: number): boolean {
  const normalized = value.trim().toLowerCase();
  if (["on", "true", "yes", "1"].includes(normalized)) {
    return true;
  }
  if (["off", "false", "no", "0"].includes(normalized)) {
    return false;
  }
  scriptError(lineNo, `${field} expects on/off or true/false.`);
}

function applyScanSetting(key: string, value: string, scan: ScriptScanOverrides, lineNo: number): void {
  const normalized = key.trim().toLowerCase().replace(/-/gu, "_");
  const raw = value.trim();
  if (!raw) {
    scriptError(lineNo, `scan ${key} needs a value.`);
  }

  if (normalized === "start") {
    scan.start = raw;
  } else if (normalized === "mode" || normalized === "scan_mode") {
    scan.scanMode = raw as Settings["scanMode"];
  } else if (normalized === "count") {
    scan.count = raw;
  } else if (normalized === "batch" || normalized === "batch_size") {
    scan.batchSize = raw;
  } else if (normalized === "max_print" || normalized === "max") {
    scan.maxPrint = raw;
  } else if (normalized === "progress" || normalized === "progress_interval") {
    scan.progressInterval = raw;
  } else if (normalized === "output" || normalized === "output_mode") {
    scan.outputMode = raw as Settings["outputMode"];
  } else if (normalized === "upper16") {
    scan.upper16 = raw;
  } else if (normalized === "java" || normalized === "java_validation") {
    scan.javaValidation = raw as Settings["javaValidation"];
  } else if (normalized === "terminal" || normalized === "terminal_mode") {
    scan.terminalMode = parseScriptBoolean(raw, `scan ${key}`, lineNo);
  } else if (normalized === "print_closest" || normalized === "closest") {
    scan.printClosest = parseScriptBoolean(raw, `scan ${key}`, lineNo);
  } else if (normalized === "execution" || normalized === "route") {
    scan.executionMode = raw as Settings["executionMode"];
  } else if (normalized === "stage" || normalized === "stage_a") {
    scan.stageAMode = raw as Settings["stageAMode"];
  } else if (normalized === "gpu_pipeline" || normalized === "pipeline") {
    scan.gpuPipeline = raw as Settings["gpuPipeline"];
  } else if (normalized === "gpu_backend" || normalized === "backend") {
    scan.gpuBackend = raw as Settings["gpuBackend"];
  } else if (normalized === "gpu_inflight" || normalized === "inflight" || normalized === "gpu_inflight_batches") {
    scan.gpuInflightBatches = raw as Settings["gpuInflightBatches"];
  } else if (normalized === "workers" || normalized === "strict_workers") {
    scan.strictWorkers = raw;
  } else if (normalized === "queue" || normalized === "queue_capacity" || normalized === "mixed_queue_capacity") {
    scan.mixedQueueCapacity = raw;
  } else if (normalized === "survivor_cap" || normalized === "mixed_survivor_cap") {
    scan.mixedSurvivorCap = raw;
  } else if (normalized === "adaptive" || normalized === "adaptive_throttling" || normalized === "mixed_adaptive_throttling") {
    scan.mixedAdaptiveThrottling = parseScriptBoolean(raw, `scan ${key}`, lineNo);
  } else if (normalized === "save_txt") {
    scan.saveTxt = raw;
  } else {
    scriptError(lineNo, `Unknown scan setting "${key}".`);
  }
}

function evaluateCondition(
  expression: string,
  variables: Record<string, ScriptValue>,
  constraints: ScriptConstraint[],
  lineNo: number
): boolean {
  const normalizedExpression = expression.replace(/\s*&&\s*/gu, " and ").replace(/\s*\|\|\s*/gu, " or ");
  const orParts = normalizedExpression.split(/\s+or\s+/iu);
  return orParts.some((orPart) =>
    orPart.split(/\s+and\s+/iu).every((part) => {
      const trimmed = part.trim();
      if (!trimmed) {
        scriptError(lineNo, "Empty condition.");
      }
      if (trimmed.startsWith("not ")) {
        return !evaluateCondition(trimmed.slice(4), variables, constraints, lineNo);
      }
      if (trimmed.startsWith("!")) {
        return !evaluateCondition(trimmed.slice(1), variables, constraints, lineNo);
      }
      const match = trimmed.match(/^(.+?)\s+(contains|is\s+not|is|==|!=|>=|<=|>|<)\s+(.+)$/iu);
      if (match) {
        return compareScriptValues(
          resolveConditionValue(match[1], variables, constraints, lineNo),
          match[2].toLowerCase(),
          resolveConditionValue(match[3], variables, constraints, lineNo)
        );
      }
      const value = resolveConditionValue(trimmed, variables, constraints, lineNo);
      if (typeof value === "boolean") return value;
      if (typeof value === "number") return value !== 0;
      if (isScriptList(value)) return value.items.length > 0;
      return value.length > 0;
    })
  );
}

function buildBuilderQuery(): QueryObject {
  const seen = new Set<string>();
  const constraints = state.constraints
    .filter((row) => row.id.trim() || row.structure.trim())
    .map((row, index) => {
      const id = normalizeId(row.id || `c${index + 1}`);
      if (seen.has(id)) {
        throw new Error(`Duplicate constraint id: ${id}`);
      }
      seen.add(id);
      pruneConstraintStructureSettings(row);
      const structure = effectiveStructureForConstraint(row);
      if (structureRequiresStrict(structure)) {
        row.mode = "strict";
      }
      let mode = row.mode === "placement" ? "placement" : row.mode === "mixed" ? "mixed" : "strict";
      if (structureSettingsNeedStrictStage(row) && mode === "placement") {
        mode = state.settings.scanMode === "mixed" ? "mixed" : "strict";
        row.mode = mode;
      }
      if (structureRequiresStrict(structure) && mode !== "strict") {
        throw new Error(`${structure} requires strict mode.`);
      }
      if (structureSettingsNeedStrictStage(row) && mode === "placement") {
        throw new Error(`${structure} settings require strict or mixed mode.`);
      }
      const constraint: ScriptConstraint = {
        id,
        structure,
        dimension: normalizeDimensionName(row.dimension, defaultDimensionForStructure(row.structure || "village")),
        mode,
        within: {
          anchor: row.anchor.trim() || "origin",
          radius: parseNumberText(row.radius, `radius for ${id}`, 1)
        },
        required: true,
        track_found: false
      };
      const structureSettings = settingsForConstraint(row);
      if (Object.keys(structureSettings).length > 0) {
        constraint.structure_settings = structureSettings;
      }
      return constraint;
    });

  const biome_filters = state.biomes
    .filter((row) => row.allowed.trim())
    .map((row, index) => ({
      point: row.point.trim() || "origin",
      dimension: normalizeDimensionName(row.dimension),
      y: parseNumberText(row.y, `biome row ${index + 1} y`),
      radius: parseNumberText(row.radius, `biome row ${index + 1} radius`, 0),
      allowed: row.allowed.split(",").map(normalizeName).filter(Boolean)
    }));

  const loot_filters = state.loot
    .filter((row) => row.constraint.trim() || row.required.trim())
    .map((row, index) => {
      const constraint = row.constraint.trim();
      if (!constraint) {
        throw new Error(`Loot row ${index + 1} is missing a constraint id.`);
      }
      const constraintRow = constraintRowById(constraint);
      const autoSource = inferredLootSourceForConstraint(constraintRow, state.settings.mcVersion);
      return {
        constraint,
        structure: normalizeName(row.source || "auto") === "auto"
          ? autoSource ?? "auto"
          : canonicalLootSourceName(row.source),
        required: parseLootRequirements(row.required)
      };
    });

  if (constraints.length === 0 && biome_filters.length === 0 && loot_filters.length === 0) {
    throw new Error("Add at least one constraint, biome filter, or loot filter.");
  }

  return {
    version: 1,
    logic: "and",
    anchor: { type: state.settings.defaultAnchor },
    constraints,
    biome_filters,
    loot_filters,
    control_ops: [],
    output: { detail: "matched_set" },
    safety: { completeness: state.settings.safetyCompleteness },
    performance: {
      strict_workers: parseNumberText(state.settings.strictWorkers, "strict workers", 0),
      execution_mode: state.settings.executionMode,
      compiled_stage_a_mode: state.settings.stageAMode,
      candidate_cap_mode: state.settings.candidateCapMode,
      strict_surrogate: state.settings.strictSurrogate,
      fixed_cap: parseNumberText(state.settings.fixedCap, "fixed cap", 1)
    }
  };
}

type FoundClause = { mask: number; expected: number; refIds: string[] };

function parseFoundClauses(expr: string, constraints: ScriptConstraint[]): FoundClause[] | null {
  const norm = expr.replace(/\s*&&\s*/gu, " and ").replace(/\s*\|\|\s*/gu, " or ");
  const orParts = norm.split(/\s+or\s+/iu);
  const clauses: FoundClause[] = [];
  let sawFound = false;
  for (const orPart of orParts) {
    const andParts = orPart.split(/\s+and\s+/iu);
    let mask = 0;
    let expected = 0;
    const refIds: string[] = [];
    for (const rawTerm of andParts) {
      const term = rawTerm.trim();
      const m = term.match(/^(\$?[A-Za-z_][A-Za-z0-9_-]*)\s+(is\s+not|is|==|!=)\s+(\$?[A-Za-z_][A-Za-z0-9_-]*)$/iu);
      if (!m) {
        return null;
      }
      const lhs = m[1].replace(/^\$/u, "");
      const op = m[2].toLowerCase().replace(/\s+/gu, " ");
      const rhs = m[3].replace(/^\$/u, "");
      let id: string;
      if (rhs.toLowerCase() === "found") {
        id = lhs;
      } else if (lhs.toLowerCase() === "found") {
        id = rhs;
      } else {
        return null;
      }
      sawFound = true;
      const idx = constraints.findIndex((c) => c.id === id);
      if (idx < 0) {
        return null;
      }
      if (idx >= 31) {
        return null;
      }
      const bit = 1 << idx;
      mask |= bit;
      const negated = op === "!=" || op === "is not";
      if (!negated) {
        expected |= bit;
      }
      refIds.push(id);
    }
    clauses.push({ mask, expected, refIds });
  }
  return sawFound ? clauses : null;
}

function compileQueryScript(text: string): ScriptCompileResult {
  const constraints: ScriptConstraint[] = [];
  const biome_filters: Array<{ point: string; dimension: string; y: number; radius: number; allowed: string[] }> = [];
  const loot_filters: Array<{ constraint: string; structure: string; required: Record<string, number> }> = [];
  const controlOps: ControlOp[] = [];
  const scan: ScriptScanOverrides = {};
  const performanceConfig = {
    strict_workers: 0,
    execution_mode: "auto",
    compiled_stage_a_mode: "auto",
    candidate_cap_mode: "none",
    strict_surrogate: "off",
    fixed_cap: 128
  };
  let safetyCompleteness: Settings["safetyCompleteness"] = "exact";
  let anchorType = "origin";
  let logic = "and";
  let outputDetail = "matched_set";
  let inPerformance = false;
  let inScan = false;
  let stopped = false;
  const variables: Record<string, ScriptValue> = {};
  const constants = new Set<string>();
  const ifBlocks: Array<{ parentActive: boolean; condition: boolean; chainSatisfied: boolean; active: boolean; elseSeen: boolean; line: number }> = [];
  const seen = new Set<string>();
  const lines = expandScriptFunctions(text);
  const scriptActive = (): boolean => ifBlocks.every((block) => block.active);

  for (let index = 0; index < lines.length; index += 1) {
    const lineNo = lines[index].line;
    let line = cleanScriptLine(lines[index].text);
    if (!line) {
      continue;
    }

    let match = line.match(/^if\s+(.+)$/iu);
    if (match) {
      const expr = match[1];
      if (/\bfound\b/iu.test(expr)) {
        const clauses = parseFoundClauses(expr, constraints);
        if (!clauses) {
          scriptError(lineNo, `Cannot lower "if ${expr}". Inside an if-found block, conditions must be "<id> == found" or "<id> != found" joined by and/or, where each id was declared by a prior find (max 31 tracked ids).`);
        }
        const parentActive = scriptActive();
        if (!parentActive) {
          let depth = 1;
          while (index + 1 < lines.length && depth > 0) {
            index += 1;
            const inner = cleanScriptLine(lines[index].text);
            if (!inner) {
              continue;
            }
            if (
              /^if\s+/iu.test(inner) ||
              /^skip(?:\s+if\s+.+)?$/iu.test(inner) ||
              /^for\s+\S+\s+in\s+/iu.test(inner) ||
              inner === "performance" ||
              inner === "scan"
            ) {
              depth += 1;
            } else if (inner === "end") {
              depth -= 1;
            }
          }
          if (depth !== 0) {
            scriptError(lineNo, "if-found block is missing end.");
          }
          continue;
        }
        for (const clause of clauses) {
          for (const id of clause.refIds) {
            const c = constraints.find((entry) => entry.id === id);
            if (c) {
              c.track_found = true;
            }
          }
        }
        let bodyIndex = index + 1;
        let closed = false;
        while (bodyIndex < lines.length) {
          const bodyLineNo = lines[bodyIndex].line;
          const bodyLine = cleanScriptLine(lines[bodyIndex].text);
          if (!bodyLine) {
            bodyIndex += 1;
            continue;
          }
          if (bodyLine === "end") {
            closed = true;
            break;
          }
          if (bodyLine === "skip") {
            for (const c of clauses) {
              controlOps.push({ op: "skip_if", mask: c.mask, expected: c.expected });
            }
            bodyIndex += 1;
            continue;
          }
          if (bodyLine === "stop") {
            for (const c of clauses) {
              controlOps.push({ op: "stop_if", mask: c.mask, expected: c.expected });
            }
            bodyIndex += 1;
            continue;
          }
          const findMatch = bodyLine.match(/^find\s+(\S+)(?:\s+in\s+(\S+))?\s+as\s+(\S+)\s+within\s+(\d+)\s+of\s+(\S+)(?:\s+(strict|placement|mixed))?(?:\s+(optional))?$/iu);
          if (findMatch) {
            const structure = normalizeName(findMatch[1]);
            const id = normalizeId(findMatch[3]);
            if (seen.has(id)) {
              scriptError(bodyLineNo, `Duplicate rule id "${id}".`);
            }
            if (constraints.length >= 31) {
              scriptError(bodyLineNo, `Too many tracked finds; max 31 when using if-found.`);
            }
            seen.add(id);
            const mode = findMatch[6]?.toLowerCase();
            constraints.push({
              id,
              structure,
              dimension: normalizeDimensionName(findMatch[2], defaultDimensionForStructure(structure)),
              mode: mode === "placement" || mode === "mixed" ? mode : "strict",
              within: {
                anchor: findMatch[5],
                radius: parseNumberText(findMatch[4], `radius for ${id}`, 1)
              },
              required: false,
              track_found: false
            });
            for (const c of clauses) {
              controlOps.push({ op: "require_if", mask: c.mask, expected: c.expected, also_id: id });
            }
            bodyIndex += 1;
            continue;
          }
          scriptError(bodyLineNo, `"${bodyLine}" is not allowed inside an if-found block. Use only skip, stop, or a single find.`);
        }
        if (!closed) {
          scriptError(lineNo, "if-found block is missing end.");
        }
        index = bodyIndex;
        continue;
      }
      const parentActive = scriptActive();
      const condition = parentActive ? evaluateCondition(expr, variables, constraints, lineNo) : false;
      ifBlocks.push({ parentActive, condition, chainSatisfied: condition, active: parentActive && condition, elseSeen: false, line: lineNo });
      continue;
    }

    match = line.match(/^(?:elif|else\s+if)\s+(.+)$/iu);
    if (match) {
      const block = ifBlocks.at(-1);
      if (!block) {
        scriptError(lineNo, "elif needs a matching if.");
      }
      if (block.elseSeen) {
        scriptError(lineNo, "elif cannot come after else.");
      }
      const canMatch = block.parentActive && !block.chainSatisfied;
      const newCondition = canMatch ? evaluateCondition(match[1], variables, constraints, lineNo) : false;
      block.condition = newCondition;
      block.active = canMatch && newCondition;
      if (newCondition) {
        block.chainSatisfied = true;
      }
      continue;
    }

    if (line === "else") {
      const block = ifBlocks.at(-1);
      if (!block) {
        scriptError(lineNo, "else needs a matching if.");
      }
      if (block.elseSeen) {
        scriptError(lineNo, "This if block already has an else.");
      }
      block.elseSeen = true;
      block.active = block.parentActive && !block.chainSatisfied;
      continue;
    }

    match = line.match(/^skip(?:\s+if\s+(.+))?$/iu);
    if (match) {
      const parentActive = scriptActive();
      const shouldSkip = match[1] ? parentActive && evaluateCondition(match[1], variables, constraints, lineNo) : parentActive;
      ifBlocks.push({ parentActive, condition: !shouldSkip, chainSatisfied: !shouldSkip, active: parentActive && !shouldSkip, elseSeen: false, line: lineNo });
      continue;
    }

    match = line.match(/^continue(?:\s+if\s+(.+))?$/iu);
    if (match) {
      const block = ifBlocks.at(-1);
      if (!block) {
        scriptError(lineNo, "continue needs an enclosing if or skip block.");
      }
      if (scriptActive() && (!match[1] || evaluateCondition(match[1], variables, constraints, lineNo))) {
        block.active = false;
        block.chainSatisfied = true;
      }
      continue;
    }

    match = line.match(/^pause(?:\s+if\s+(.+))?$/iu);
    if (match) {
      if (scriptActive() && (!match[1] || evaluateCondition(match[1], variables, constraints, lineNo))) {
        for (const block of ifBlocks) {
          block.active = false;
          block.chainSatisfied = true;
        }
      }
      continue;
    }

    match = line.match(/^for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.+)$/iu);
    if (match) {
      const loopVar = match[1];
      const rhs = match[2].trim();
      let depth = 1;
      let bodyEnd = index + 1;
      while (bodyEnd < lines.length) {
        const inner = cleanScriptLine(lines[bodyEnd].text);
        if (inner === "end") {
          depth -= 1;
          if (depth === 0) {
            break;
          }
        } else if (
          /^if\s+/iu.test(inner) ||
          /^skip(?:\s+if\s+.+)?$/iu.test(inner) ||
          /^for\s+\S+\s+in\s+/iu.test(inner) ||
          inner === "performance" ||
          inner === "scan"
        ) {
          depth += 1;
        }
        bodyEnd += 1;
      }
      if (bodyEnd >= lines.length || depth !== 0) {
        scriptError(lineNo, "for block is missing end.");
      }
      const bodyLines = lines.slice(index + 1, bodyEnd);

      if (!scriptActive()) {
        index = bodyEnd;
        continue;
      }
      if (constants.has(loopVar)) {
        scriptError(lineNo, `Cannot use constant "${loopVar}" as a for-loop variable.`);
      }

      const list = resolveForList(rhs, loopVar, variables, lineNo);

      const unrolled: ScriptLine[] = [];
      for (const item of list.items) {
        unrolled.push({ text: `let ${loopVar} = ${serializeScriptValue(item)}`, line: lineNo });
        for (const bl of bodyLines) {
          unrolled.push(bl);
        }
      }
      lines.splice(index, bodyEnd - index + 1, ...unrolled);
      index -= 1;
      continue;
    }

    if (inPerformance) {
      if (line === "end") {
        inPerformance = false;
        continue;
      }
      if (!scriptActive()) {
        continue;
      }
      line = substituteScriptVariables(line, variables, lineNo);
      const [key, ...rest] = line.split(/\s+/u);
      const value = rest.join(" ");
      if (!value) {
        scriptError(lineNo, `Performance setting "${key}" needs a value.`);
      }
      if (key === "workers" || key === "strict_workers") {
        performanceConfig.strict_workers = parseNumberText(value, "strict workers", 0);
      } else if (key === "queue" || key === "queue_capacity" || key === "mixed_queue_capacity") {
        scan.mixedQueueCapacity = value;
      } else if (key === "survivor_cap" || key === "mixed_survivor_cap") {
        scan.mixedSurvivorCap = value;
      } else if (key === "adaptive" || key === "adaptive_throttling" || key === "mixed_adaptive_throttling") {
        scan.mixedAdaptiveThrottling = parseScriptBoolean(value, `performance ${key}`, lineNo);
      } else if (key === "completeness" || key === "safety") {
        safetyCompleteness = parseChoice(value, ["exact", "approximate"] as const, "completeness");
        scan.safetyCompleteness = safetyCompleteness;
      } else if (key === "execution" || key === "execution_mode") {
        performanceConfig.execution_mode = value;
      } else if (key === "stage" || key === "compiled_stage_a_mode") {
        performanceConfig.compiled_stage_a_mode = value;
      } else if (key === "gpu_pipeline" || key === "pipeline") {
        scan.gpuPipeline = value as Settings["gpuPipeline"];
      } else if (key === "gpu_backend" || key === "backend") {
        scan.gpuBackend = value as Settings["gpuBackend"];
      } else if (key === "gpu_inflight" || key === "inflight" || key === "gpu_inflight_batches") {
        scan.gpuInflightBatches = value as Settings["gpuInflightBatches"];
      } else if (key === "cap" || key === "candidate_cap_mode") {
        performanceConfig.candidate_cap_mode = value;
      } else if (key === "surrogate" || key === "strict_surrogate") {
        performanceConfig.strict_surrogate = value;
      } else if (key === "fixed_cap") {
        performanceConfig.fixed_cap = parseNumberText(value, "fixed cap", 1);
      } else {
        scriptError(lineNo, `Unknown performance setting "${key}".`);
      }
      continue;
    }

    if (inScan) {
      if (line === "end") {
        inScan = false;
        continue;
      }
      if (!scriptActive()) {
        continue;
      }
      line = substituteScriptVariables(line, variables, lineNo);
      const [key, ...rest] = line.split(/\s+/u);
      applyScanSetting(key, rest.join(" "), scan, lineNo);
      continue;
    }

    if (line === "end") {
      if (ifBlocks.length === 0) {
        scriptError(lineNo, "end needs a matching if, skip, scan, or performance block.");
      }
      ifBlocks.pop();
      continue;
    }

    if (!scriptActive()) {
      continue;
    }

    match = line.match(/^(let|set|const)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$/iu);
    if (match) {
      const kind = match[1].toLowerCase();
      const name = match[2];
      if (constants.has(name)) {
        scriptError(lineNo, `Cannot reassign constant "${name}".`);
      }
      variables[name] = parseScriptValue(substituteScriptVariables(match[3], variables, lineNo));
      if (kind === "const") {
        constants.add(name);
      }
      continue;
    }

    match = line.match(/^stop(?:\s+if\s+(.+))?$/iu);
    if (match) {
      if (!match[1] || evaluateCondition(match[1], variables, constraints, lineNo)) {
        stopped = true;
        break;
      }
      continue;
    }

    match = line.match(/^assert\s+(.+?)(?:\s+message\s+(.+))?$/iu);
    if (match) {
      if (!evaluateCondition(match[1], variables, constraints, lineNo)) {
        scriptError(lineNo, match[2]?.trim() || `Assertion failed: ${match[1]}`);
      }
      continue;
    }

    line = substituteScriptVariables(line, variables, lineNo);

    if (line === "performance") {
      inPerformance = true;
      continue;
    }

    if (line === "scan") {
      inScan = true;
      continue;
    }

    match = line.match(/^scan\s+(\S+)\s+(.+)$/iu);
    if (match) {
      applyScanSetting(match[1], match[2], scan, lineNo);
      continue;
    }

    match = line.match(/^anchor\s+(\S+)$/iu);
    if (match) {
      anchorType = match[1];
      continue;
    }

    match = line.match(/^logic\s+(\S+)$/iu);
    if (match) {
      logic = match[1];
      continue;
    }

    if (line === "output") {
      scan.outputMode = "raw";
      outputDetail = "matched_set";
      continue;
    }

    match = line.match(/^output\s+(\S+)$/iu);
    if (match) {
      const mode = match[1].toLowerCase();
      if (mode === "raw" || mode === "lift64" || mode === "both" || mode === "world64") {
        scan.outputMode = mode as Settings["outputMode"];
      } else {
        outputDetail = match[1];
      }
      continue;
    }

    match = line.match(/^detail\s+(\S+)$/iu);
    if (match) {
      outputDetail = match[1];
      continue;
    }

    match = line.match(/^find\s+(\S+)(?:\s+in\s+(\S+))?\s+as\s+(\S+)\s+within\s+(\d+)\s+of\s+(\S+)(?:\s+(strict|placement|mixed))?(?:\s+(optional))?$/iu);
    if (match) {
      const structure = normalizeName(match[1]);
      const id = normalizeId(match[3]);
      if (seen.has(id)) {
        scriptError(lineNo, `Duplicate rule id "${id}".`);
      }
      seen.add(id);
      const mode = match[6]?.toLowerCase();
      const resolvedMode = mode === "placement" || mode === "mixed" ? mode : "strict";
      if (structureRequiresStrict(structure) && resolvedMode !== "strict") {
        scriptError(lineNo, `${structure} requires strict mode.`);
      }
      constraints.push({
        id,
        structure,
        dimension: normalizeDimensionName(match[2], defaultDimensionForStructure(structure)),
        mode: resolvedMode,
        within: {
          anchor: match[5],
          radius: parseNumberText(match[4], `radius for ${id}`, 1)
        },
        required: !match[7],
        track_found: false
      });
      continue;
    }

    match = line.match(/^biome\s+(\S+)(?:\s+in\s+(\S+))?\s+y\s+(-?\d+)\s+radius\s+(\d+)\s+allow\s+(.+)$/iu);
    if (match) {
      const allowed = match[5].split(/[,\s]+/u).map(normalizeName).filter(Boolean);
      if (allowed.length === 0) {
        scriptError(lineNo, "Biome filter needs at least one allowed biome.");
      }
      biome_filters.push({
        point: match[1],
        dimension: normalizeDimensionName(match[2]),
        y: parseNumberText(match[3], "biome y"),
        radius: parseNumberText(match[4], "biome radius", 0),
        allowed
      });
      continue;
    }

    match = line.match(/^loot\s+(\S+)\s+from\s+(\S+)\s+require\s+(.+)$/iu);
    if (match) {
      loot_filters.push({
        constraint: match[1],
        structure: normalizeName(match[2]) === "auto" ? "auto" : canonicalLootSourceName(match[2]),
        required: parseLootRequirements(match[3])
      });
      continue;
    }

    scriptError(lineNo, `Unknown command "${line}". Try: find village in overworld as v1 within 64 of origin strict`);
  }

  if (!stopped && inPerformance) {
    scriptError(lines.length, "Performance block is missing end.");
  }
  if (!stopped && inScan) {
    scriptError(lines.length, "Scan block is missing end.");
  }
  if (!stopped && ifBlocks.length > 0) {
    scriptError(ifBlocks.at(-1)?.line ?? lines.length, "If block is missing end.");
  }
  if (constraints.length === 0 && biome_filters.length === 0 && loot_filters.length === 0) {
    scriptError(1, "Add at least one find, biome, or loot command.");
  }

  return {
    query: {
      version: 1,
      logic,
      anchor: { type: anchorType },
      constraints,
      biome_filters,
      loot_filters,
      control_ops: controlOps,
      output: { detail: outputDetail },
      safety: { completeness: safetyCompleteness },
      performance: performanceConfig
    },
    scan
  };
}

function buildCompiled(): ScriptCompileResult {
  const compiled = state.useScriptSource
    ? compileQueryScript(queryEditorText())
    : { query: buildBuilderQuery() as QueryObject, scan: {} };
  validateQueryLootFilters(compiled.query, state.settings.mcVersion);
  return compiled;
}

function buildQuery(): unknown {
  return buildCompiled().query;
}

function buildArgs(overrides: ScriptScanOverrides = {}, query?: { loot_filters?: unknown[] }): string[] {
  const settings = { ...state.settings, ...overrides };
  const args: string[] = [];
  const start = settings.start.trim();
  if (start) {
    args.push("--start", parseBigIntText(start, "start seed").toString());
  }

  const count = parseBigIntText(settings.count, "count", 1n);
  if (count > ((1n << 64n) - 1n)) {
    throw new Error("count cannot exceed 18446744073709551615 seeds.");
  }
  args.push("--count", count.toString());
  const scanMode = parseChoice(settings.scanMode, ["placement", "strict", "mixed"] as const, "scan mode");
  args.push("--scan-mode", scanMode);

  const batch = settings.batchSize.trim().toLowerCase();
  if (batch === "balanced" || batch === "max") {
    args.push("--batch-size", batch);
  } else {
    const batchSize = parseBigIntText(batch, "batch size", 1n);
    if (batchSize > 4294967295n) {
      throw new Error("batch size cannot exceed 4294967295 seeds.");
    }
    args.push("--batch-size", batchSize.toString());
  }

  args.push("--max-print", parseNumberText(settings.maxPrint, "max print", 1).toString());
  args.push("--mc-version", settings.mcVersion.trim() || "26.1.2");
  args.push("--execution-mode", parseChoice(settings.executionMode, ["auto", "scalar", "simd-x4", "simd-x8", "max-throughput"] as const, "execution mode"));
  args.push("--compiled-stage-a-mode", parseChoice(settings.stageAMode, ["auto", "single", "multi", "cpu-a5", "gpu-off"] as const, "Stage A mode"));
  args.push("--gpu-pipeline", parseChoice(settings.gpuPipeline, ["auto", "async", "sync"] as const, "GPU pipeline"));
  args.push("--gpu-backend", parseChoice(settings.gpuBackend, ["auto", "cuda", "opencl"] as const, "GPU backend"));
  args.push("--gpu-inflight-batches", parseChoice(settings.gpuInflightBatches, ["auto", "1", "2", "3", "4", "5", "6", "7", "8"] as const, "GPU in-flight batches"));
  args.push("--java-cubiomes-validation", parseChoice(settings.javaValidation, ["auto", "off", "strict"] as const, "Java validation"));
  args.push("--completeness", parseChoice(settings.safetyCompleteness, ["exact", "approximate"] as const, "completeness"));
  args.push("--progress-interval", String(Math.max(0.1, Number(settings.progressInterval) || 1)));

  const workers = parseNumberText(settings.strictWorkers, "strict workers", 0);
  if (workers > 0) {
    args.push("--strict-workers", String(workers));
  }
  args.push("--mixed-queue-capacity", parseBigIntText(settings.mixedQueueCapacity, "mixed queue capacity", 1n).toString());
  args.push("--mixed-survivor-cap", parseNumberText(settings.mixedSurvivorCap, "mixed survivor cap", 0).toString());
  args.push("--mixed-adaptive-throttling", settings.mixedAdaptiveThrottling ? "on" : "off");
  if (settings.saveTxt.trim()) {
    args.push("--save-txt", settings.saveTxt.trim());
  }
  if (settings.printClosest) {
    args.push("--print-closest");
  }

  const hasLoot = Array.isArray(query?.loot_filters) && query.loot_filters.length > 0;
  if (!hasLoot) {
    const outputMode = settings.outputMode === "world64" ? "both" : settings.outputMode;
    if (outputMode !== "raw") {
      args.push("--output-mode", outputMode);
      args.push("--upper16", parseNumberText(settings.upper16, "upper16").toString());
    }
  }
  return args;
}

function quoteArg(value: string): string {
  if (!/[ \t"]/u.test(value)) {
    return value;
  }
  return `"${value.replace(/\\/gu, "\\\\").replace(/"/gu, '\\"')}"`;
}

function commandPreview(): string {
  const scanner = state.context?.scannerPath ?? "native\\scanner_native.exe";
  const compiled = buildCompiled();
  const settings = { ...state.settings, ...compiled.scan };
  return [scanner, "--query-file", "<compiled-query>", ...buildArgs(compiled.scan, compiled.query), "--control-stdin", "--stream-seeds"]
    .concat(settings.terminalMode ? ["--terminal-mode"] : [])
    .map(quoteArg)
    .join(" ");
}

function scriptFromBuilder(): string {
  const lines = [
    "# HelixSeed Script",
    "# Variables use let name = value, then $name inside commands.",
    "# Conditions use: if radius >= 64 and structure is village ... end",
    "",
    `scan mode ${state.settings.scanMode}`,
    "",
    `anchor ${state.settings.defaultAnchor}`,
    "logic and",
    ""
  ];

  for (const row of state.constraints.filter((entry) => entry.id.trim() || entry.structure.trim())) {
    const id = normalizeId(row.id || "c1");
    const radiusVar = `${id}_radius`;
    const structureVar = `${id}_structure`;
    lines.push(`let ${structureVar} = ${effectiveStructureForConstraint(row)}`);
    lines.push(`let ${radiusVar} = ${row.radius || "64"}`);
    lines.push(`find $${structureVar} in ${row.dimension || defaultDimensionForStructure(row.structure)} as ${id} within $${radiusVar} of ${row.anchor || "origin"} ${row.mode || "strict"}`);
    const labels = selectedStructureSettingLabels(row);
    if (labels.length > 0) {
      lines.push(`# ${id} settings: ${labels.join(", ")}`);
    }
  }

  for (const row of state.biomes.filter((entry) => entry.allowed.trim())) {
    lines.push(`biome ${row.point || "origin"} in ${row.dimension || "overworld"} y ${row.y || "64"} radius ${row.radius || "0"} allow ${row.allowed}`);
  }

  for (const row of state.loot.filter((entry) => entry.constraint.trim() || entry.required.trim())) {
    lines.push(`loot ${row.constraint || "v1"} from ${normalizeName(row.source || "auto")} require ${row.required || "diamond:1"}`);
  }

  lines.push(
    "",
    "output matched_set",
    "",
    "performance",
    `  workers ${state.settings.strictWorkers}`,
    `  queue ${state.settings.mixedQueueCapacity}`,
    `  adaptive ${state.settings.mixedAdaptiveThrottling ? "on" : "off"}`,
    `  completeness ${state.settings.safetyCompleteness}`,
    `  execution ${state.settings.executionMode}`,
    `  stage ${state.settings.stageAMode}`,
    `  gpu_pipeline ${state.settings.gpuPipeline}`,
    `  gpu_backend ${state.settings.gpuBackend}`,
    `  gpu_inflight ${state.settings.gpuInflightBatches}`,
    `  cap ${state.settings.candidateCapMode}`,
    `  surrogate ${state.settings.strictSurrogate}`,
    `  fixed_cap ${state.settings.fixedCap}`,
    "end"
  );
  return lines.join("\n");
}

function syncScriptFromBuilder(): void {
  if (state.useScriptSource) {
    return;
  }
  try {
    setQueryEditorText(scriptFromBuilder());
  } catch {
    // Let command preview show the validation error while the user is typing.
  }
}

function updateCommandPreview(): void {
  const preview = $("#command-preview");
  try {
    buildQuery();
    preview.textContent = commandPreview();
    preview.classList.remove("invalid");
  } catch (error) {
    preview.textContent = `<invalid config: ${error instanceof Error ? error.message : String(error)}>`;
    preview.classList.add("invalid");
  }
}

function syncControlsFromState(): void {
  document.querySelectorAll<HTMLInputElement>("[data-setting]").forEach((node) => {
    const key = node.dataset.setting as keyof Settings;
    const value = state.settings[key];
    if (node instanceof HTMLInputElement && node.type === "checkbox") {
      node.checked = Boolean(value);
    } else {
      node.value = String(value);
    }
  });
  $<HTMLInputElement>("#use-script-source").checked = state.useScriptSource;
}

type LootAutocompleteToken = {
  start: number;
  end: number;
  leading: string;
  name: string;
  count: string;
};

let lootAutocompleteMenu: HTMLDivElement | null = null;
let activeLootAutocompleteInput: HTMLInputElement | null = null;
let activeLootAutocompleteItems: string[] = [];

function lootLabel(value: string): string {
  return value
    .split("_")
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(" ");
}

function lootTokenForInput(input: HTMLInputElement): LootAutocompleteToken {
  const value = input.value;
  const cursor = input.selectionStart ?? value.length;
  const start = value.lastIndexOf(",", Math.max(0, cursor - 1)) + 1;
  const nextComma = value.indexOf(",", cursor);
  const end = nextComma === -1 ? value.length : nextComma;
  const raw = value.slice(start, end);
  const leading = raw.match(/^\s*/u)?.[0] ?? "";
  const core = raw.trim();
  const colonIndex = core.indexOf(":");
  const name = colonIndex === -1 ? core : core.slice(0, colonIndex);
  const count = colonIndex === -1 ? "1" : core.slice(colonIndex + 1).trim() || "1";
  return { start, end, leading, name, count };
}

function filteredLootSuggestions(query: string): string[] {
  const normalized = normalizeName(query);
  if (!normalized) {
    return lootItemSuggestions.slice(0, 10);
  }
  return lootItemSuggestions
    .filter((item) => item.includes(normalized))
    .sort((a, b) => Number(!a.startsWith(normalized)) - Number(!b.startsWith(normalized)))
    .slice(0, 10);
}

function ensureLootAutocompleteMenu(): HTMLDivElement {
  if (!lootAutocompleteMenu) {
    lootAutocompleteMenu = document.createElement("div");
    lootAutocompleteMenu.className = "loot-autocomplete-menu";
    document.body.append(lootAutocompleteMenu);
  }
  return lootAutocompleteMenu;
}

function hideLootAutocomplete(): void {
  if (lootAutocompleteMenu) {
    lootAutocompleteMenu.hidden = true;
    lootAutocompleteMenu.innerHTML = "";
  }
  activeLootAutocompleteInput = null;
  activeLootAutocompleteItems = [];
}

function positionLootAutocomplete(input: HTMLInputElement, menu: HTMLDivElement): void {
  const rect = input.getBoundingClientRect();
  menu.style.left = `${Math.max(8, rect.left)}px`;
  menu.style.top = `${rect.bottom + 4}px`;
  menu.style.width = `${Math.max(240, rect.width)}px`;
  menu.style.maxHeight = `${Math.max(120, Math.min(260, window.innerHeight - rect.bottom - 12))}px`;
}

function showLootAutocomplete(input: HTMLInputElement): void {
  const token = lootTokenForInput(input);
  const items = filteredLootSuggestions(token.name);
  if (items.length === 0) {
    hideLootAutocomplete();
    return;
  }
  activeLootAutocompleteInput = input;
  activeLootAutocompleteItems = items;
  const menu = ensureLootAutocompleteMenu();
  menu.innerHTML = items
    .map((item) => `
      <button class="loot-autocomplete-option" type="button" data-loot-suggestion="${escapeHtml(item)}">
        <span>${escapeHtml(lootLabel(item))}</span>
        <small>${escapeHtml(`${item}:${token.count || "1"}`)}</small>
      </button>
    `)
    .join("");
  positionLootAutocomplete(input, menu);
  menu.hidden = false;
}

function applyLootAutocompleteSuggestion(input: HTMLInputElement, item: string): void {
  const token = lootTokenForInput(input);
  const replacement = `${token.leading}${item}:${token.count || "1"}`;
  input.value = `${input.value.slice(0, token.start)}${replacement}${input.value.slice(token.end)}`;
  const cursor = token.start + replacement.length;
  input.setSelectionRange(cursor, cursor);
  updateCollection(input);
  syncScriptFromBuilder();
  updateCommandPreview();
  hideLootAutocomplete();
  input.focus();
}

function renderConstraints(): void {
  const anchors = currentConstraintAnchors();
  $("#constraints-body").innerHTML = state.constraints
    .map((row, index) => {
      const dimension = row.dimension || defaultDimensionForStructure(row.structure);
      const visibleStructures = structuresForDimension(dimension);
      pruneConstraintStructureSettings(row);
      const effectiveStructure = effectiveStructureForConstraint(row);
      const requiresStrict = structureRequiresStrict(effectiveStructure);
      if (requiresStrict) {
        row.mode = "strict";
      }
      const settingsCount = selectedStructureSettingCount(row);
      const settingsTitle =
        settingsCount > 0
          ? `Additional settings: ${selectedStructureSettingLabels(row).join(", ")}`
          : "Additional settings";
      return `
        <div class="grid-row constraint-row">
          <input data-collection="constraints" data-index="${index}" data-field="id" value="${escapeHtml(row.id)}" />
          ${dropdownControl(
            visibleStructures,
            row.structure,
            { "data-collection": "constraints", "data-index": String(index), "data-field": "structure" },
            { "data-searchable": "true", "data-search-placeholder": `Search ${dimension} structures` }
          )}
          ${dropdownControl(dimensions, dimension, { "data-collection": "constraints", "data-index": String(index), "data-field": "dimension" })}
          ${dropdownControl(anchors, row.anchor, { "data-collection": "constraints", "data-index": String(index), "data-field": "anchor" })}
          <input data-collection="constraints" data-index="${index}" data-field="radius" value="${escapeHtml(row.radius)}" />
          ${dropdownControl(
            requiresStrict ? ["strict"] : ["strict", "placement", "mixed"],
            requiresStrict ? "strict" : row.mode,
            { "data-collection": "constraints", "data-index": String(index), "data-field": "mode" }
          )}
          <button class="icon-button row-action" data-action="remove-constraint" data-index="${index}" title="Remove constraint"><i data-lucide="trash-2"></i></button>
          <button class="icon-button row-action constraint-settings-button${settingsCount > 0 ? " active" : ""}" data-action="open-constraint-settings" data-index="${index}" title="${escapeHtml(settingsTitle)}"><i data-lucide="settings"></i></button>
        </div>
      `;
    })
    .join("");
}

function closeConstraintSettingsDialog(): void {
  state.activeConstraintSettingsIndex = null;
  document.querySelector<HTMLElement>("[data-constraint-settings-backdrop]")?.remove();
}

function renderConstraintSettingsDialog(index: number): void {
  const row = state.constraints[index];
  if (!row) {
    closeConstraintSettingsDialog();
    return;
  }
  state.activeConstraintSettingsIndex = index;
  pruneConstraintStructureSettings(row);
  const def = structureSettingsDefinitionFor(row.structure);
  const settings = settingsForConstraint(row);
  const title = def?.title ?? "Structure settings";
  const subtitle = `${row.id.trim() || `constraint ${index + 1}`} - ${row.structure || "structure"}`;
  const body = def
    ? def.groups
        .map((group) => {
          const selected = new Set(settings[group.id] ?? []);
          const options = group.options
            .map((option) => `
              <label class="settings-option${option.disabled ? " disabled" : ""}" title="${escapeHtml(option.note ?? option.label)}">
                <input
                  type="checkbox"
                  data-structure-setting="true"
                  data-index="${index}"
                  data-group="${escapeHtml(group.id)}"
                  data-value="${escapeHtml(option.id)}"
                  ${option.disabled ? "disabled" : ""}
                  ${selected.has(option.id) ? "checked" : ""}
                />
                <span>${escapeHtml(option.label)}${option.disabled ? `<small>${escapeHtml(option.note ?? "not available")}</small>` : ""}</span>
              </label>
            `)
            .join("");
          return `
            <section class="settings-group">
              <div class="settings-group-title">${escapeHtml(group.label)}</div>
              <div class="settings-option-grid">${options}</div>
            </section>
          `;
        })
        .join("")
    : `<div class="settings-empty">No extra settings are available for ${escapeHtml(row.structure || "this structure")} yet.</div>`;

  document.querySelector<HTMLElement>("[data-constraint-settings-backdrop]")?.remove();
  const host = document.createElement("div");
  host.className = "settings-backdrop";
  host.dataset.constraintSettingsBackdrop = "true";
  host.innerHTML = `
    <section class="settings-dialog" role="dialog" aria-modal="true" aria-label="${escapeHtml(title)}">
      <header class="settings-dialog-header">
        <div class="settings-dialog-title">
          <strong>${escapeHtml(title)}</strong>
          <span>${escapeHtml(subtitle)}</span>
        </div>
        <button class="icon-button" data-action="close-constraint-settings" title="Close"><i data-lucide="x"></i></button>
      </header>
      <div class="settings-dialog-body">${body}</div>
      <footer class="settings-dialog-footer">
        <button class="black-button small" data-action="clear-constraint-settings" data-index="${index}">Clear</button>
        <button class="black-button small primary" data-action="close-constraint-settings">Done</button>
      </footer>
    </section>
  `;
  document.body.append(host);
  refreshIcons();
}

function renderBiomes(): void {
  $("#biomes-body").innerHTML = state.biomes
    .map((row, index) => `
      <div class="grid-row biome-row">
        <input data-collection="biomes" data-index="${index}" data-field="point" value="${escapeHtml(row.point)}" />
        ${dropdownControl(dimensions, row.dimension || "overworld", { "data-collection": "biomes", "data-index": String(index), "data-field": "dimension" })}
        <input data-collection="biomes" data-index="${index}" data-field="y" value="${escapeHtml(row.y)}" />
        <input data-collection="biomes" data-index="${index}" data-field="radius" value="${escapeHtml(row.radius)}" />
        <input data-collection="biomes" data-index="${index}" data-field="allowed" value="${escapeHtml(row.allowed)}" placeholder="plains,soul_sand_valley,end_highlands" />
        <button class="icon-button row-action" data-action="remove-biome" data-index="${index}" title="Remove biome filter"><i data-lucide="trash-2"></i></button>
      </div>
    `)
    .join("");
}

function renderLoot(): void {
  const constraintIds = state.constraints
    .map((row) => row.id.trim())
    .filter(Boolean);
  $("#loot-body").innerHTML = state.loot
    .map((row, index) => {
      const sourceOptions = lootSourcesForRow(row);
      const selectedSource = sourceOptions.includes(row.source) ? row.source : "auto";
      row.source = selectedSource;
      return `
        <div class="grid-row loot-row">
          ${dropdownControl(
            constraintIds,
            row.constraint,
            { "data-collection": "loot", "data-index": String(index), "data-field": "constraint" },
            { "data-searchable": "true", "data-search-placeholder": "Search constraints" }
          )}
          ${dropdownControl(
            sourceOptions,
            selectedSource,
            { "data-collection": "loot", "data-index": String(index), "data-field": "source" },
            { "data-searchable": "true", "data-search-placeholder": "Search loot source" }
          )}
          <input data-collection="loot" data-index="${index}" data-field="required" data-loot-autocomplete="true" autocomplete="off" value="${escapeHtml(row.required)}" placeholder="diamond:2,emerald:5" />
          <button class="icon-button row-action" data-action="remove-loot" data-index="${index}" title="Remove loot filter"><i data-lucide="trash-2"></i></button>
        </div>
      `;
    })
    .join("");
}

function parseSeedPayload(payload: string): SeedResult {
  const text = payload.trim();
  const stageMatch = text.match(/\bstage=(placement|strict|verified)\b/iu);
  const stageToken = stageMatch?.[1]?.toLowerCase();
  const stage =
    stageToken === "placement"
      ? "placement"
      : stageToken === "strict" || stageToken === "verified"
        ? "strict"
        : state.activeScanMode === "placement" || state.activeScanMode === "strict"
          ? state.activeScanMode
          : undefined;
  const stageDetail = stage === "placement" ? "placement candidate" : stage === "strict" ? "strict verified" : undefined;
  const labeledValues = [...text.matchAll(/\b(raw|lift64|world64)=(-?\d+)\b/gu)];
  if (labeledValues.length > 0) {
    const byLabel = new Map(labeledValues.map((match) => [match[1], match[2]]));
    const preferred =
      state.settings.outputMode === "lift64"
        ? byLabel.get("lift64")
        : state.settings.outputMode === "world64"
          ? byLabel.get("world64") ?? byLabel.get("lift64")
          : byLabel.get("raw") ?? byLabel.get("world64") ?? byLabel.get("lift64");
    const copy = preferred ?? labeledValues[0][2];
    const alternates = labeledValues
      .map((match) => match[2])
      .filter((value, index, values) => value !== copy && values.indexOf(value) === index);
    const detailParts = [
      stageDetail,
      alternates.length > 0 ? `also ${alternates.join(" / ")}` : undefined
    ].filter((part): part is string => Boolean(part));
    return {
      id: stage ? `${stage}:${copy}` : copy,
      display: copy,
      copy,
      detail: detailParts.length > 0 ? detailParts.join(" | ") : undefined,
      stage
    };
  }

  const numericSeed = text.match(/-?\d+/u)?.[0] ?? text;
  return {
    id: stage ? `${stage}:${numericSeed}` : numericSeed,
    display: numericSeed,
    copy: numericSeed,
    detail: stageDetail,
    stage
  };
}

function renderSeeds(): void {
  const list = $("#found-list");
  const filter = state.resultStageFilter;
  const filteredSeeds =
    filter === "all" ? state.foundSeeds : state.foundSeeds.filter((seed) => seed.stage === filter);
  const placementCount = state.foundSeeds.filter((seed) => seed.stage === "placement").length;
  const strictCount = state.foundSeeds.filter((seed) => seed.stage === "strict").length;
  const countLabel = $("#found-filter-count");
  countLabel.textContent = `${formatNumber(filteredSeeds.length)} shown / ${formatNumber(state.foundSeeds.length)} total`;
  countLabel.title = `${formatNumber(placementCount)} placement, ${formatNumber(strictCount)} strict verified`;
  document.querySelectorAll<HTMLButtonElement>("[data-result-filter]").forEach((button) => {
    button.classList.toggle("active", button.dataset.resultFilter === filter);
  });
  if (state.foundSeeds.length === 0) {
    list.innerHTML = `<div class="empty-state">No seeds found yet.</div>`;
    return;
  }
  if (filteredSeeds.length === 0) {
    const label = filter === "placement" ? "placement seeds" : "strict verified seeds";
    list.innerHTML = `<div class="empty-state">No ${label} found yet.</div>`;
    return;
  }
  list.innerHTML = filteredSeeds
    .map(
      (seed, index) => `
        <button class="seed-pill${seed.stage ? ` stage-${seed.stage}` : ""}" data-seed="${escapeHtml(seed.copy)}" title="Copy ${escapeHtml(seed.copy)}">
          <img class="seed-pill-icon" src="${seedIconUrl}" alt="" />
          <span class="seed-index">${index + 1}</span>
          <span class="seed-main">${escapeHtml(seed.display)}</span>
          ${seed.detail ? `<small>${escapeHtml(seed.detail)}</small>` : ""}
        </button>
      `
    )
    .join("");
}

function refreshIcons(): void {
  createIcons({ icons: iconSet });
}

function renderAll(): void {
  syncControlsFromState();
  renderConstraints();
  renderBiomes();
  renderLoot();
  renderSeeds();
  syncScriptFromBuilder();
  updateCommandPreview();
  refreshIcons();
  syncDropdowns();
}

function setActiveTab(tabName: string): void {
  document.querySelectorAll<HTMLElement>("[data-tab-target]").forEach((button) => {
    const active = button.dataset.tabTarget === tabName;
    button.classList.toggle("active", active);
    button.setAttribute("aria-selected", active ? "true" : "false");
  });
  document.querySelectorAll<HTMLElement>("[data-tab-panel]").forEach((panel) => {
    panel.classList.toggle("active", panel.dataset.tabPanel === tabName);
  });
  if (tabName === "telemetry") {
    telemetryChart?.requestDraw();
  }
}

function appendOutput(line: string): void {
  const output = $("#terminal-output");
  output.textContent += `${line}\n`;
  output.scrollTop = output.scrollHeight;
}

function clearOutput(): void {
  $("#terminal-output").textContent = "";
  state.foundSeeds = [];
  renderSeeds();
  telemetryStore.reset();
  resetRejectAccumulator();
  applyProgress({});
}

function formatNumber(value: unknown): string {
  const n = Number(value ?? 0);
  return Number.isFinite(n) ? n.toLocaleString() : "0";
}

function formatRate(value: unknown): string {
  const n = Number(value ?? 0);
  return Number.isFinite(n) && n > 0 ? `${Math.round(n).toLocaleString()} seeds/sec` : "-- seeds/sec";
}

function formatCompactNumber(value: number): string {
  if (!Number.isFinite(value) || value <= 0) {
    return "0";
  }
  const units = [
    { suffix: "B", value: 1_000_000_000 },
    { suffix: "M", value: 1_000_000 },
    { suffix: "K", value: 1_000 }
  ];
  for (const unit of units) {
    if (value >= unit.value) {
      const scaled = value / unit.value;
      return `${scaled >= 10 ? scaled.toFixed(0) : scaled.toFixed(1)}${unit.suffix}`;
    }
  }
  return Math.round(value).toLocaleString();
}

function formatTelemetryRate(value: number): string {
  return `${formatCompactNumber(value)} seeds/sec`;
}

function niceCeil(value: number): number {
  if (!Number.isFinite(value) || value <= 0) {
    return 1;
  }
  const exponent = Math.floor(Math.log10(value));
  const magnitude = 10 ** exponent;
  const normalized = value / magnitude;
  const nice = normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10;
  return nice * magnitude;
}

function setText(selector: string, value: string): void {
  const node = document.querySelector<HTMLElement>(selector);
  if (node) {
    node.textContent = value;
  }
}

function initializeTelemetry(): void {
  const canvas = document.querySelector<HTMLCanvasElement>("#telemetry-rate-chart");
  if (canvas && !telemetryChart) {
    telemetryChart = new LineChart({
      canvas,
      store: telemetryStore,
      formatValue: formatTelemetryRate,
      formatAxis: formatCompactNumber,
      showFps: telemetryFpsVisible
    });
  }

  renderTelemetryLegend(telemetryStore.snapshot());
  updateTelemetryRangeLabel(telemetryStore.snapshot().historyMs);

  telemetryStore.subscribe((snapshot) => {
    updateTelemetryRangeLabel(snapshot.historyMs);
    const stats = snapshot.stats;
    const isMixed = state.settings.scanMode === "mixed" || snapshot.meta.route.endsWith("/ mixed");
    const rateStats =
      isMixed && stats.rawPlacementRate ? stats.rawPlacementRate : stats.rate ?? { current: 0, average: 0, peak: 0 };
    const meta = snapshot.meta;
    setText("#telemetry-current-rate", formatTelemetryRate(rateStats.current));
    setText("#telemetry-average-rate", formatTelemetryRate(rateStats.average));
    setText("#telemetry-peak-rate", formatTelemetryRate(rateStats.peak));
    setText("#telemetry-chart-label", formatTelemetryRate(rateStats.current));
    setText("#telemetry-window-label", `${Math.round(snapshot.historyMs / 1000)}s rolling window`);
    setText("#telemetry-processed", formatNumber(meta.processed));
    setText("#telemetry-valid", formatNumber(meta.valid));
    setText("#telemetry-route", meta.route || state.settings.stageAMode);
    const latestValues = snapshot.latest?.values ?? {};
    setText("#telemetry-stage-a-rejects", formatRatePerSecond(latestValues.stageA ?? 0));
    setText("#telemetry-stage-b-rejects", formatRatePerSecond(latestValues.stageB ?? 0));
    setText("#telemetry-biome-rejects", formatRatePerSecond(latestValues.biome ?? 0));
    setText("#telemetry-java-rejects", formatRatePerSecond(latestValues.java ?? 0));
    setText("#telemetry-timing", `${meta.strictSec.toFixed(3)}s / ${meta.javaSec.toFixed(3)}s`);
    setText("#telemetry-queue-size", `${formatNumber(snapshot.latest?.values.queueSize ?? 0)} / ${formatNumber(snapshot.latest?.values.queueCapacity ?? 0)}`);
    setText("#telemetry-queue-saturation", `${(snapshot.latest?.values.queueSaturation ?? 0).toFixed(1)}%`);
    setText("#telemetry-strict-workers", formatNumber(snapshot.latest?.values.strictWorkers ?? 0));
    setText("#telemetry-dropped-survivors", formatNumber(snapshot.latest?.values.droppedSurvivors ?? 0));
    setText("#telemetry-queue-full-events", formatNumber(snapshot.latest?.values.queueFullEvents ?? 0));
    setText("#telemetry-queue-push-wait", `${(snapshot.latest?.values.queuePushWaitMs ?? 0).toFixed(3)}ms`);
    setText("#telemetry-gpu-util", `${(snapshot.latest?.values.gpuUtil ?? 0).toFixed(1)}%`);
    setText("#telemetry-gpu-inflight", `${formatNumber(snapshot.latest?.values.gpuInflight ?? 0)} / ${formatNumber(snapshot.latest?.values.gpuSlots ?? 0)}`);
    setText("#telemetry-placement-batch", formatNumber(snapshot.latest?.values.placementBatchSize ?? 0));
    setText("#telemetry-gpu-kernel-ms", `${(snapshot.latest?.values.gpuBatchKernelMs ?? 0).toFixed(3)}ms`);
    setText("#telemetry-gpu-host-gap-ms", `${(snapshot.latest?.values.gpuHostGapMs ?? 0).toFixed(3)}ms`);
    setText("#telemetry-gpu-fill", `${(snapshot.latest?.values.gpuPipelineFill ?? 0).toFixed(1)}%`);
    const scanLed = document.querySelector("#telemetry-scan-led");
    scanLed?.classList.toggle("active", snapshot.running && !snapshot.paused);
    const gpuLed = document.querySelector("#telemetry-gpu-led");
    const gpuActive = snapshot.running && (meta.route.includes("gpu") || state.settings.stageAMode !== "gpu-off");
    gpuLed?.classList.toggle("active", gpuActive);
    const pauseIcon = document.querySelector<HTMLElement>('[data-action="toggle-telemetry"]');
    pauseIcon?.classList.toggle("active", snapshot.paused);
    pauseIcon?.setAttribute("title", snapshot.paused ? "Resume telemetry" : "Pause telemetry");
    renderTelemetryLegend(snapshot);
  });
}

function updateTelemetryRangeLabel(historyMs: number): void {
  const seconds = Math.round(historyMs / 1000);
  setText("#telemetry-range-label", `${seconds}s`);
  const range = document.querySelector<HTMLInputElement>("#telemetry-history");
  if (range && range.type === "range" && range.value !== String(historyMs)) {
    range.value = String(historyMs);
  }
}

function renderTelemetryLegend(snapshot: TelemetrySnapshot): void {
  const host = document.querySelector<HTMLElement>("#telemetry-legend");
  if (!host) {
    return;
  }
  const stats = snapshot.stats;
  const items = snapshot.series
    .map((series) => {
      const stat = stats[series.key];
      const value = formatTelemetryValue(series, stat?.current ?? 0);
      const active = series.visible ? "active" : "";
      return `<button type="button" class="telemetry-series ${active}" data-series="${escapeHtml(series.key)}" title="Toggle ${escapeHtml(series.label)}">
        <span class="telemetry-series-dot" style="background:${escapeHtml(series.color)}"></span>
        <span class="telemetry-series-label">${escapeHtml(series.label)}</span>
        <span class="telemetry-series-value">${escapeHtml(value)}</span>
      </button>`;
    })
    .join("");
  host.innerHTML = items;
}

function formatTelemetryValue(series: SeriesDefinition, value: number): string {
  if (series.unit === "seeds/sec") {
    return formatTelemetryRate(value);
  }
  if (series.unit === "percent") {
    return `${value.toFixed(1)}%`;
  }
  if (series.unit === "ms") {
    return `${value.toFixed(3)}ms`;
  }
  if (series.unit === "count") {
    return formatNumber(value);
  }
  return formatRatePerSecond(value);
}

function formatRatePerSecond(value: number): string {
  return `${formatCompactNumber(value)} /s`;
}

function exportTelemetrySnapshot(): void {
  const snapshot = telemetryStore.snapshot();
  const stats = snapshot.stats;
  const exportData = {
    exported_at: new Date().toISOString(),
    history_ms: snapshot.historyMs,
    paused: snapshot.paused,
    running: snapshot.running,
    series: snapshot.series.map((series) => ({
      key: series.key,
      label: series.label,
      unit: series.unit ?? null,
      visible: series.visible,
      current: stats[series.key]?.current ?? 0,
      average: stats[series.key]?.average ?? 0,
      peak: stats[series.key]?.peak ?? 0
    })),
    meta: snapshot.meta,
    samples: snapshot.samples.map((sample) => ({
      age_ms: Math.round(performance.now() - sample.t),
      ...sample.values,
      route: sample.meta.route,
      processed: sample.meta.processed,
      valid: sample.meta.valid,
      strict_sec: sample.meta.strictSec,
      java_sec: sample.meta.javaSec
    }))
  };
  void copyText(JSON.stringify(exportData, null, 2));
  $("#run-status").textContent = "Telemetry snapshot copied";
}

function applyProgress(payload: ScannerProgress): void {
  const processed = Number(payload.processed ?? 0);
  const total = Number(payload.total ?? 0);
  const pct = Math.max(0, Math.min(100, Number(payload.pct ?? 0)));
  const rawPlacementRate = nonNegativeNumber(payload.raw_placement_seeds_per_sec ?? payload.placement_rate_sps ?? payload.rate_sps);
  const placementSurvivorRate = nonNegativeNumber(payload.placement_survivors_per_sec);
  const strictConsumedRate = nonNegativeNumber(payload.strict_consumed_per_sec ?? payload.strict_rate_sps);
  const verifiedRate = nonNegativeNumber(payload.strict_verified_per_sec);
  const queueDepth = nonNegativeNumber(payload.queue_depth ?? payload.queue_size);
  const gpuInflight = nonNegativeNumber(payload.gpu_inflight_batches);
  const gpuSlots = nonNegativeNumber(payload.gpu_async_slots_available);
  const gpuFill = nonNegativeNumber(payload.gpu_pipeline_fill_percent);
  const isMixed = (payload.scan_mode ?? state.settings.scanMode) === "mixed";
  $("#progress-fill").style.width = `${pct}%`;
  $("#run-status").textContent = state.running ? `${state.paused ? "Paused" : "Running"} ${pct.toFixed(2)}%` : "Idle";
  if (state.running && isMixed && (payload.gpu_pipeline ?? "off") === "off") {
    $("#run-status").textContent = `Running ${pct.toFixed(2)}% - GPU placement inactive`;
  }
  $("#progress-rate").textContent = isMixed ? formatRate(rawPlacementRate) : formatRate(payload.rate_sps);
  $("#metric-processed").textContent = `${formatNumber(processed)} / ${formatNumber(total)}`;
  $("#metric-survivors").textContent = formatNumber(payload.stage_a_survivors ?? payload.gpu_hits ?? 0);
  $("#metric-valid").textContent = formatNumber(payload.cpu_hits ?? 0);
  $("#metric-java").textContent = formatNumber(payload.java_worldgen_rejected ?? 0);
  $("#metric-placement-rate").textContent = formatRate(rawPlacementRate);
  $("#metric-strict-rate").textContent = formatRate(strictConsumedRate);
  $("#metric-verified-rate").textContent = formatRate(verifiedRate);
  $("#metric-queue").textContent = `${formatNumber(queueDepth)} / ${formatNumber(payload.queue_capacity ?? 0)}`;
  const routeLabel = `${payload.compiled_stage_a_mode ?? state.settings.stageAMode} / ${
    payload.gpu_pipeline ?? state.settings.gpuPipeline
  } / ${payload.scan_mode ?? state.settings.scanMode}`;
  $("#metric-route").textContent = routeLabel;
  const strictSec = Number(payload.strict_sec ?? 0);
  const javaSec = Number(payload.java_worldgen_sec ?? 0);
  $("#metric-time").textContent = `${strictSec.toFixed(3)}s / ${javaSec.toFixed(3)}s`;
  $("#metric-gpu-fill").textContent = `${gpuFill.toFixed(1)}%`;
  $("#metric-gpu-slots").textContent = `${formatNumber(gpuInflight)} / ${formatNumber(gpuSlots)}`;
  const rejects = deriveRejectRates(payload);
  telemetryStore.ingest(
    {
      rate: nonNegativeNumber(payload.rate_sps),
      rawPlacementRate,
      placementSurvivorRate,
      strictConsumedRate,
      verifiedRate,
      queueSaturation: nonNegativeNumber(payload.queue_saturation_percent ?? payload.queue_saturation_pct),
      queueSize: queueDepth,
      queueCapacity: nonNegativeNumber(payload.queue_capacity),
      strictWorkers: nonNegativeNumber(payload.strict_worker_count),
      droppedSurvivors: nonNegativeNumber(payload.dropped_survivors),
      queueFullEvents: nonNegativeNumber(payload.queue_full_events),
      queuePushWaitMs: nonNegativeNumber(payload.queue_push_wait_time_ms),
      gpuUtil: nonNegativeNumber(payload.gpu_busy_percent ?? payload.placement_gpu_busy_percent ?? payload.gpu_utilization_estimate),
      gpuInflight,
      gpuSlots,
      placementBatchSize: nonNegativeNumber(payload.placement_batch_size),
      gpuBatchKernelMs: nonNegativeNumber(payload.gpu_batch_kernel_ms),
      gpuHostGapMs: nonNegativeNumber(payload.gpu_host_gap_ms),
      gpuPipelineFill: gpuFill,
      stageA: rejects.stageARate,
      stageB: rejects.stageBRate,
      biome: rejects.biomeRate,
      java: rejects.javaRate
    },
    {
      route: routeLabel,
      processed: nonNegativeNumber(payload.processed),
      valid: nonNegativeNumber(payload.cpu_hits),
      strictSec,
      javaSec
    }
  );
}

function setRunning(running: boolean): void {
  state.running = running;
  telemetryStore.setRunning(running);
  if (!running) {
    state.paused = false;
  }
  document.querySelector<HTMLButtonElement>('[data-action="run"]')!.disabled = running;
  document.querySelector<HTMLButtonElement>('[data-action="pause"]')!.disabled = !running;
  document.querySelector<HTMLButtonElement>('[data-action="snapshot"]')!.disabled = !running;
  document.querySelector<HTMLButtonElement>('[data-action="stop"]')!.disabled = !running;
  document.querySelector<HTMLButtonElement>('[data-action="pause"] span')!.textContent = state.paused ? "Resume" : "Pause";
  $("#run-status").textContent = running ? "Running" : "Idle";
}

function updateCollection(target: HTMLInputElement): void {
  const collection = target.dataset.collection;
  const index = Number(target.dataset.index ?? -1);
  const field = target.dataset.field;
  if (!collection || !field || !Number.isInteger(index) || index < 0) {
    return;
  }
  const value = target.value;
  if (collection === "constraints" && state.constraints[index]) {
    const beforeFamily = structureSettingsFamily(state.constraints[index].structure);
    (state.constraints[index] as unknown as Record<string, string>)[field] = value;
    if (field === "structure") {
      state.constraints[index].dimension = defaultDimensionForStructure(value);
      if (structureSettingsFamily(value) !== beforeFamily) {
        state.constraints[index].structureSettings = undefined;
      } else {
        pruneConstraintStructureSettings(state.constraints[index]);
      }
      if (structureRequiresStrict(effectiveStructureForConstraint(state.constraints[index]))) {
        state.constraints[index].mode = "strict";
      }
      renderConstraints();
      renderLoot();
      refreshIcons();
      syncDropdowns();
    }
    if (field === "dimension") {
      const dimension = normalizeDimensionName(value);
      state.constraints[index].dimension = dimension;
      const allowed = structuresForDimension(dimension);
      if (!allowed.includes(state.constraints[index].structure)) {
        state.constraints[index].structure = allowed[0] ?? state.constraints[index].structure;
        state.constraints[index].structureSettings = undefined;
      }
      renderConstraints();
      renderLoot();
      refreshIcons();
      syncDropdowns();
    }
    if (field === "id" || field === "mode") {
      if (field === "mode" && structureRequiresStrict(effectiveStructureForConstraint(state.constraints[index]))) {
        state.constraints[index].mode = "strict";
      }
      renderConstraints();
      renderLoot();
      refreshIcons();
      syncDropdowns();
    }
  } else if (collection === "biomes" && state.biomes[index]) {
    (state.biomes[index] as unknown as Record<string, string>)[field] = value;
  } else if (collection === "loot" && state.loot[index]) {
    (state.loot[index] as unknown as Record<string, string>)[field] = value;
    if (field === "constraint") {
      state.loot[index].source = "auto";
      renderLoot();
      refreshIcons();
      syncDropdowns();
    }
  }
}

function applyScriptToBuilder(): void {
  const compiled = compileQueryScript(queryEditorText());
  const raw = compiled.query as {
    anchor?: { type?: string };
    constraints?: Array<{
      id?: string;
      structure?: string;
      dimension?: string;
      mode?: string;
      within?: { anchor?: string; radius?: number } | number;
      structure_settings?: ConstraintStructureSettings;
    }>;
    biome_filters?: Array<{ point?: string; dimension?: string; y?: number; radius?: number; allowed?: string[] | string }>;
    loot_filters?: Array<{ constraint?: string; source?: string; structure?: string; required?: Record<string, number> | string[] | string }>;
    performance?: Record<string, unknown>;
    safety?: { completeness?: string };
  };

  state.constraints = Array.isArray(raw.constraints)
    ? raw.constraints.map((row, index) => {
        const within = typeof row.within === "object" && row.within ? row.within : { anchor: "origin", radius: row.within ?? 64 };
        return {
          id: String(row.id ?? `c${index + 1}`),
          structure: String(row.structure ?? "village"),
          dimension: normalizeDimensionName(row.dimension, defaultDimensionForStructure(String(row.structure ?? "village"))),
          anchor: String(within.anchor ?? "origin"),
          radius: String(within.radius ?? 64),
          mode: row.mode === "placement" ? "placement" : row.mode === "mixed" ? "mixed" : "strict",
          structureSettings: normalizedStructureSettings(String(row.structure ?? "village"), row.structure_settings)
        };
      })
    : [];

  state.biomes = Array.isArray(raw.biome_filters)
    ? raw.biome_filters.map((row) => ({
        point: String(row.point ?? "origin"),
        dimension: normalizeDimensionName(row.dimension),
        y: String(row.y ?? 64),
        radius: String(row.radius ?? 0),
        allowed: Array.isArray(row.allowed) ? row.allowed.join(",") : String(row.allowed ?? "")
      }))
    : [];

  state.loot = Array.isArray(raw.loot_filters)
    ? raw.loot_filters.map((row) => {
        const required = row.required;
        return {
          constraint: String(row.constraint ?? row.source ?? ""),
          source: String(row.structure ?? "auto"),
          required:
            typeof required === "object" && required && !Array.isArray(required)
              ? Object.entries(required).map(([key, value]) => `${key}:${value}`).join(",")
              : Array.isArray(required)
                ? required.join(",")
                : String(required ?? "")
        };
      })
    : [];
  state.loot.forEach((row) => {
    row.source = normalizeName(row.source) === "auto" ? "auto" : canonicalLootSourceName(row.source);
  });

  const perf = raw.performance ?? {};
  state.settings.defaultAnchor = raw.anchor?.type === "spawn" ? "spawn" : "origin";
  state.settings.safetyCompleteness =
    raw.safety?.completeness === "approximate" ? "approximate" : "exact";
  state.settings.strictWorkers = String(perf.strict_workers ?? state.settings.strictWorkers);
  state.settings.executionMode = String(perf.execution_mode ?? state.settings.executionMode) as Settings["executionMode"];
  state.settings.stageAMode = String(perf.compiled_stage_a_mode ?? state.settings.stageAMode).replace("_", "-") as Settings["stageAMode"];
  state.settings.candidateCapMode = String(perf.candidate_cap_mode ?? state.settings.candidateCapMode) as Settings["candidateCapMode"];
  state.settings.strictSurrogate = String(perf.strict_surrogate ?? state.settings.strictSurrogate).replace("light-speed", "lightspeed") as Settings["strictSurrogate"];
  state.settings.fixedCap = String(perf.fixed_cap ?? state.settings.fixedCap);
  Object.assign(state.settings, compiled.scan);
  state.useScriptSource = false;
  renderAll();
}

function applyMaxPerformanceSettings(): void {
  state.settings.batchSize = "max";
  state.settings.executionMode = "max-throughput";
  state.settings.stageAMode = "multi";
  state.settings.gpuPipeline = "async";
  state.settings.gpuInflightBatches = "auto";
  state.settings.safetyCompleteness = "approximate";
  state.settings.strictSurrogate = "lightspeed";
  state.settings.candidateCapMode = "adaptive";
  state.settings.mixedAdaptiveThrottling = true;
  state.settings.strictWorkers = "0";
}

function applyPreset(name: "starter" | "trial" | "ancient" | "max-performance" | "smart-mode"): void {
  if (name === "starter") {
    state.constraints = [
      { id: "v1", structure: "village", dimension: "overworld", anchor: "origin", radius: "96", mode: "strict" },
      { id: "rp1", structure: "ruined_portal", dimension: "overworld", anchor: "constraint:v1", radius: "80", mode: "strict" }
    ];
  } else if (name === "trial") {
    state.constraints = [{ id: "tc1", structure: "trial_chambers", dimension: "overworld", anchor: "origin", radius: "256", mode: "placement" }];
  } else if (name === "ancient") {
    state.constraints = [{ id: "ac1", structure: "ancient_city", dimension: "overworld", anchor: "origin", radius: "512", mode: "strict" }];
    state.biomes = [{ point: "origin", dimension: "overworld", y: "80", radius: "128", allowed: "cherry_grove,meadow" }];
  } else if (name === "max-performance") {
    applyMaxPerformanceSettings();
  } else if (name === "smart-mode") {
    state.settings.scanMode = "mixed";
    applyMaxPerformanceSettings();
  }
  renderAll();
}

document.addEventListener("input", (event) => {
  const target = event.target;
  if (!(target instanceof HTMLInputElement) && !(target instanceof HTMLTextAreaElement)) {
    return;
  }
  if (target.dataset.setting) {
    const key = target.dataset.setting as keyof Settings;
    (state.settings as unknown as Record<string, string | boolean>)[key] =
      target instanceof HTMLInputElement && target.type === "checkbox" ? target.checked : target.value;
    if (key === "mcVersion") {
      renderLoot();
      refreshIcons();
      syncDropdowns();
    }
    syncScriptFromBuilder();
    updateCommandPreview();
    return;
  }
  if (target.dataset.collection && target instanceof HTMLInputElement) {
    updateCollection(target);
    syncScriptFromBuilder();
    updateCommandPreview();
    if (target.dataset.lootAutocomplete === "true") {
      showLootAutocomplete(target);
    }
  }
});

document.addEventListener("focusin", (event) => {
  const target = event.target;
  if (target instanceof HTMLInputElement && target.dataset.lootAutocomplete === "true") {
    showLootAutocomplete(target);
  }
});

document.addEventListener("keydown", (event) => {
  const target = event.target;
  if (!(target instanceof HTMLInputElement) || target.dataset.lootAutocomplete !== "true") {
    return;
  }
  if (event.key === "Escape") {
    hideLootAutocomplete();
    return;
  }
  if ((event.key === "Enter" || event.key === "Tab") && activeLootAutocompleteInput === target && activeLootAutocompleteItems.length > 0) {
    event.preventDefault();
    applyLootAutocompleteSuggestion(target, activeLootAutocompleteItems[0]);
  }
});

document.addEventListener("change", (event) => {
  const target = event.target;
  if (target instanceof HTMLInputElement && target.dataset.structureSetting === "true") {
    const index = Number(target.dataset.index);
    const group = target.dataset.group ?? "";
    const value = target.dataset.value ?? "";
    const row = state.constraints[index];
    if (row && group && value) {
      const def = structureSettingsDefinitionFor(row.structure);
      const option = def?.groups.find((entry) => entry.id === group)?.options.find((entry) => entry.id === value);
      if (option?.disabled) {
        target.checked = false;
        return;
      }
      const next = settingsForConstraint(row);
      const values = new Set(next[group] ?? []);
      if (target.checked) {
        values.add(value);
      } else {
        values.delete(value);
      }
      if (values.size > 0) {
        next[group] = [...values];
      } else {
        delete next[group];
      }
      row.structureSettings = Object.keys(next).length > 0 ? next : undefined;
      if (structureRequiresStrict(effectiveStructureForConstraint(row))) {
        row.mode = "strict";
      } else if (structureSettingsNeedStrictStage(row) && row.mode === "placement") {
        row.mode = state.settings.scanMode === "mixed" ? "mixed" : "strict";
      }
      renderConstraints();
      renderConstraintSettingsDialog(index);
      syncScriptFromBuilder();
      updateCommandPreview();
      refreshIcons();
      syncDropdowns();
    }
    return;
  }
  if (target instanceof HTMLInputElement && target.id === "use-script-source") {
    state.useScriptSource = target.checked;
    updateCommandPreview();
    return;
  }
  if (
    target instanceof HTMLInputElement &&
    target.id === "telemetry-history"
  ) {
    const ms = Number.parseInt(target.value, 10);
    if (Number.isFinite(ms) && ms > 0) {
      telemetryStore.setHistoryMs(ms);
    }
  }
});

document.addEventListener("input", (event) => {
  const target = event.target;
  if (
    target instanceof HTMLInputElement &&
    target.id === "telemetry-history" &&
    target.type === "range"
  ) {
    const ms = Number.parseInt(target.value, 10);
    if (Number.isFinite(ms) && ms > 0) {
      telemetryStore.setHistoryMs(ms);
    }
  }
});

let kofiArmed = false;
let kofiTimer = 0;
const KOFI_LABEL = "Ko-fi";
const KOFI_PROMPT = "Click again to tip";

function setKofiLabel(text: string): void {
  const span = document.querySelector<HTMLSpanElement>('[data-action="open-kofi"] span');
  if (span) span.textContent = text;
}

function disarmKofi(): void {
  kofiArmed = false;
  if (kofiTimer) {
    window.clearTimeout(kofiTimer);
    kofiTimer = 0;
  }
  setKofiLabel(KOFI_LABEL);
}

document.addEventListener("click", async (event) => {
  const backdrop = (event.target as HTMLElement).closest<HTMLElement>("[data-constraint-settings-backdrop]");
  if (backdrop && event.target === backdrop) {
    closeConstraintSettingsDialog();
    return;
  }

  const tabButton = (event.target as HTMLElement).closest<HTMLElement>("[data-tab-target]");
  if (tabButton) {
    const targetTab = tabButton.dataset.tabTarget ?? "builder";
    if (targetTab === "docs") {
      await window.helixSeed.openDocs();
      return;
    }
    setActiveTab(targetTab);
    return;
  }

  const seriesToggle = (event.target as HTMLElement).closest<HTMLElement>("[data-series]");
  if (seriesToggle) {
    const key = seriesToggle.dataset.series ?? "";
    const series = telemetryStore.getSeries().find((entry) => entry.key === key);
    if (series) {
      telemetryStore.setSeriesVisible(key, !series.visible);
    }
    return;
  }

  const resultFilterButton = (event.target as HTMLElement).closest<HTMLButtonElement>("[data-result-filter]");
  if (resultFilterButton) {
    const filter = resultFilterButton.dataset.resultFilter;
    if (filter === "all" || filter === "placement" || filter === "strict") {
      state.resultStageFilter = filter;
      renderSeeds();
    }
    return;
  }

  const scriptBlock = (event.target as HTMLElement).closest<HTMLElement>("[data-script-block]");
  if (scriptBlock) {
    insertScriptBlock(scriptBlock.dataset.scriptBlock ?? "");
    return;
  }

  const lootSuggestion = (event.target as HTMLElement).closest<HTMLElement>("[data-loot-suggestion]");
  if (lootSuggestion && activeLootAutocompleteInput) {
    applyLootAutocompleteSuggestion(activeLootAutocompleteInput, lootSuggestion.dataset.lootSuggestion ?? "");
    return;
  }

  if (
    activeLootAutocompleteInput &&
    event.target !== activeLootAutocompleteInput &&
    !(event.target as HTMLElement).closest(".loot-autocomplete-menu")
  ) {
    hideLootAutocomplete();
  }

  const button = (event.target as HTMLElement).closest<HTMLElement>("[data-action]");
  if (!button) {
    const seed = (event.target as HTMLElement).closest<HTMLElement>("[data-seed]");
    if (seed) {
      const value = seed.dataset.seed ?? "";
      const copied = await copyText(value);
      seed.classList.add("copied");
      $("#run-status").textContent = copied ? `Copied ${value}` : `Copy failed: ${value}`;
      window.setTimeout(() => seed.classList.remove("copied"), 900);
    }
    return;
  }
  const action = button.dataset.action;
  try {
    if (action === "add-constraint") {
      state.constraints.push({ id: `c${state.constraints.length + 1}`, structure: "village", dimension: "overworld", anchor: "origin", radius: "64", mode: "strict" });
      renderAll();
    } else if (action === "remove-constraint") {
      state.constraints.splice(Number(button.dataset.index), 1);
      closeConstraintSettingsDialog();
      renderAll();
    } else if (action === "open-constraint-settings") {
      renderConstraintSettingsDialog(Number(button.dataset.index));
    } else if (action === "close-constraint-settings") {
      closeConstraintSettingsDialog();
    } else if (action === "clear-constraint-settings") {
      const index = Number(button.dataset.index);
      if (state.constraints[index]) {
        state.constraints[index].structureSettings = undefined;
        renderConstraints();
        renderConstraintSettingsDialog(index);
        syncScriptFromBuilder();
        updateCommandPreview();
        refreshIcons();
        syncDropdowns();
      }
    } else if (action === "add-biome") {
      state.biomes.push({ point: "origin", dimension: "overworld", y: "64", radius: "0", allowed: "plains" });
      renderAll();
    } else if (action === "remove-biome") {
      state.biomes.splice(Number(button.dataset.index), 1);
      renderAll();
    } else if (action === "add-loot") {
      state.loot.push({
        constraint: state.constraints.find((row) => row.id.trim())?.id ?? "",
        source: "auto",
        required: "diamond:2"
      });
      renderAll();
    } else if (action === "remove-loot") {
      state.loot.splice(Number(button.dataset.index), 1);
      renderAll();
    } else if (action === "preset-starter") {
      applyPreset("starter");
    } else if (action === "preset-trial") {
      applyPreset("trial");
    } else if (action === "preset-ancient") {
      applyPreset("ancient");
    } else if (action === "preset-max-performance") {
      applyPreset("max-performance");
    } else if (action === "preset-smart-mode") {
      applyPreset("smart-mode");
    } else if (action === "open-kofi") {
      if (kofiArmed) {
        disarmKofi();
        await window.helixSeed.openExternal("https://ko-fi.com/E1E41ZL4SR");
      } else {
        kofiArmed = true;
        setKofiLabel(KOFI_PROMPT);
        if (kofiTimer) window.clearTimeout(kofiTimer);
        kofiTimer = window.setTimeout(() => disarmKofi(), 2500);
      }
    } else if (action === "refresh-script") {
      state.useScriptSource = false;
      renderAll();
    } else if (action === "apply-script") {
      applyScriptToBuilder();
    } else if (action === "copy-command") {
      await copyText($("#command-preview").textContent ?? "");
    } else if (action === "autocomplete-script") {
      showQueryAutocomplete();
    } else if (action === "clear-output") {
      clearOutput();
    } else if (action === "toggle-telemetry") {
      telemetryStore.setPaused(!telemetryStore.isPaused());
    } else if (action === "export-telemetry") {
      exportTelemetrySnapshot();
    } else if (action === "toggle-fps") {
      telemetryFpsVisible = !telemetryFpsVisible;
      telemetryChart?.setFpsVisible(telemetryFpsVisible);
      button.classList.toggle("active", telemetryFpsVisible);
      button.setAttribute("title", telemetryFpsVisible ? "Hide FPS overlay" : "Show FPS overlay");
    } else if (action === "open-repo" && state.context) {
      await window.helixSeed.openPath();
    } else if (action === "run") {
      const compiled = buildCompiled();
      const query = compiled.query;
      const args = buildArgs(compiled.scan, query);
      const settings = { ...state.settings, ...compiled.scan };
      clearOutput();
      state.activeScanMode = settings.scanMode;
      setActiveTab("results");
      setRunning(true);
      const result = await window.helixSeed.startScan({ query, args, terminalMode: settings.terminalMode });
      if (!result.ok) {
        appendOutput(`[ui] ${result.error ?? "Failed to start scanner."}`);
        setRunning(false);
      }
    } else if (action === "pause") {
      if (!state.running) {
        return;
      }
      const next = state.paused ? "resume" : "pause";
      await window.helixSeed.sendControl(next);
      state.paused = !state.paused;
      document.querySelector<HTMLButtonElement>('[data-action="pause"] span')!.textContent = state.paused ? "Resume" : "Pause";
      $("#run-status").textContent = state.paused ? "Paused" : "Running";
    } else if (action === "snapshot") {
      if (!state.running) {
        return;
      }
      await window.helixSeed.sendControl("snapshot");
      $("#run-status").textContent = "Snapshot requested";
    } else if (action === "stop") {
      appendOutput("[ui] Stopping scanner...");
      await window.helixSeed.stopScan();
    }
  } catch (error) {
    appendOutput(`[ui] ${error instanceof Error ? error.message : String(error)}`);
    updateCommandPreview();
  }
});

window.helixSeed.onLine((line) => appendOutput(line));
window.helixSeed.onProgress((payload) => applyProgress(payload));
window.helixSeed.onSeed((seed) => {
  const parsed = parseSeedPayload(seed);
  if (!state.foundSeeds.some((result) => result.id === parsed.id)) {
    state.foundSeeds.push(parsed);
    renderSeeds();
  }
});
window.helixSeed.onExit((payload) => {
  appendOutput(`[ui] Scanner exited with code ${payload.code ?? "null"}${payload.signal ? ` (${payload.signal})` : ""}.`);
  setRunning(false);
});

function finishLoadingScreen(): void {
  const screen = document.querySelector<HTMLElement>("#loading-screen");
  document.body.classList.add("loading-ready");
  if (!screen) {
    document.body.classList.remove("loading");
    return;
  }
  screen.classList.add("fade-out");
  screen.addEventListener(
    "transitionend",
    () => {
      screen.remove();
      document.body.classList.remove("loading", "loading-ready");
    },
    { once: true }
  );
}

async function boot(): Promise<void> {
  state.context = await window.helixSeed.getContext();
  initializeQueryEditor();
  initializeTelemetry();
  renderAll();
  requestAnimationFrame(() => finishLoadingScreen());
}

void boot().catch((error) => {
  appendOutput(`[ui] Failed to initialize: ${error instanceof Error ? error.message : String(error)}`);
  requestAnimationFrame(() => finishLoadingScreen());
});
