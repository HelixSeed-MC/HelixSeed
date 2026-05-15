export type ScannerProgress = {
  processed?: number;
  total?: number;
  pct?: number;
  rate_sps?: number;
  stage_a_survivors?: number;
  stage_a5_survivors?: number;
  stage_b_structure_survivors?: number;
  stage_c_biome_survivors?: number;
  stage_a_rejects?: number;
  stage_a5_rejects?: number;
  stage_b_exact_rejects?: number;
  stage_c_biome_rejects?: number;
  gpu_hits?: number;
  cpu_hits?: number;
  mismatches?: number;
  biome_rejected?: number;
  sculpt_rejected?: number;
  java_worldgen_rejected?: number;
  loot_rejected?: number;
  cap_pruned?: number;
  gpu_sec?: number;
  gpu_pipeline?: string;
  scan_mode?: string;
  raw_placement_seeds_per_sec?: number;
  placement_rate_sps?: number;
  strict_rate_sps?: number;
  strict_consumed_per_sec?: number;
  queue_depth?: number;
  queue_size?: number;
  queue_capacity?: number;
  queue_saturation_percent?: number;
  queue_saturation_pct?: number;
  strict_worker_count?: number;
  placement_survivors_per_sec?: number;
  strict_verified_per_sec?: number;
  dropped_survivors?: number;
  queue_full_events?: number;
  queue_push_wait_time_ms?: number;
  placement_gpu_busy_percent?: number;
  gpu_busy_percent?: number;
  gpu_utilization_estimate?: number;
  strict_sec?: number;
  sculpt_sec?: number;
  java_worldgen_sec?: number;
  loot_sec?: number;
  compiled_stage_a_mode?: string;
  terminal_mode?: boolean;
  done?: boolean;
};

export type ScanStartRequest = {
  query: unknown;
  args: string[];
  terminalMode: boolean;
};

export type ScanStartResult = {
  ok: boolean;
  error?: string;
  command?: string;
  scannerPath?: string;
  repoRoot?: string;
};

export type AppContext = {
  repoRoot: string;
  scannerPath: string;
  scannerExists: boolean;
  packaged: boolean;
};

export type ScanExit = {
  code: number | null;
  signal: NodeJS.Signals | null;
};
