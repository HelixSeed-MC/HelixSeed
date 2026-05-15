export type TelemetryMeta = {
  route: string;
  processed: number;
  valid: number;
  strictSec: number;
  javaSec: number;
};

export type TelemetrySample = {
  t: number;
  values: Record<string, number>;
  meta: TelemetryMeta;
};

export type SeriesDefinition = {
  key: string;
  label: string;
  color: string;
  unit?: string;
  visible: boolean;
  group?: "primary" | "rejects" | "timing";
};

export type SeriesStats = {
  current: number;
  average: number;
  peak: number;
};

export type TelemetrySnapshot = {
  samples: TelemetrySample[];
  latest: TelemetrySample | null;
  series: SeriesDefinition[];
  historyMs: number;
  paused: boolean;
  running: boolean;
  stats: Record<string, SeriesStats>;
  meta: TelemetryMeta;
  ingestCount: number;
};

export type TelemetryListener = (snapshot: TelemetrySnapshot) => void;

const EMPTY_META: TelemetryMeta = {
  route: "auto",
  processed: 0,
  valid: 0,
  strictSec: 0,
  javaSec: 0
};

const HISTORY_MIN_MS = 15_000;
const HISTORY_MAX_MS = 300_000;

export class TelemetryStore {
  private samples: TelemetrySample[] = [];
  private listeners = new Set<TelemetryListener>();
  private historyMs: number;
  private paused = false;
  private running = false;
  private latest: TelemetrySample | null = null;
  private seriesOrder: string[] = [];
  private seriesMap = new Map<string, SeriesDefinition>();
  private meta: TelemetryMeta = { ...EMPTY_META };
  private ingestCount = 0;
  private emitScheduled = false;

  constructor(opts: { historyMs?: number } = {}) {
    this.historyMs = clampHistory(opts.historyMs ?? 60_000);
  }

  registerSeries(definition: SeriesDefinition): void {
    if (!this.seriesMap.has(definition.key)) {
      this.seriesOrder.push(definition.key);
    }
    this.seriesMap.set(definition.key, { ...definition });
    this.scheduleEmit();
  }

  registerSeriesBatch(definitions: SeriesDefinition[]): void {
    for (const definition of definitions) {
      if (!this.seriesMap.has(definition.key)) {
        this.seriesOrder.push(definition.key);
      }
      this.seriesMap.set(definition.key, { ...definition });
    }
    this.scheduleEmit();
  }

  setSeriesVisible(key: string, visible: boolean): void {
    const series = this.seriesMap.get(key);
    if (!series || series.visible === visible) {
      return;
    }
    series.visible = visible;
    this.scheduleEmit();
  }

  getSeries(): SeriesDefinition[] {
    return this.seriesOrder
      .map((key) => this.seriesMap.get(key))
      .filter((series): series is SeriesDefinition => Boolean(series))
      .map((series) => ({ ...series }));
  }

  subscribe(listener: TelemetryListener): () => void {
    this.listeners.add(listener);
    listener(this.snapshot());
    return () => {
      this.listeners.delete(listener);
    };
  }

  reset(): void {
    this.samples = [];
    this.latest = null;
    this.meta = { ...EMPTY_META };
    this.ingestCount = 0;
    this.scheduleEmit();
  }

  setRunning(running: boolean): void {
    if (this.running === running) {
      return;
    }
    this.running = running;
    this.scheduleEmit();
  }

  setPaused(paused: boolean): void {
    if (this.paused === paused) {
      return;
    }
    this.paused = paused;
    this.scheduleEmit();
  }

  isPaused(): boolean {
    return this.paused;
  }

  isRunning(): boolean {
    return this.running;
  }

  setHistoryMs(historyMs: number): void {
    const clamped = clampHistory(historyMs);
    if (clamped === this.historyMs) {
      return;
    }
    this.historyMs = clamped;
    this.trim(performance.now());
    this.scheduleEmit();
  }

  ingest(values: Record<string, number>, meta: Partial<TelemetryMeta> = {}): void {
    const now = performance.now();
    const sanitized: Record<string, number> = {};
    for (const key of this.seriesOrder) {
      const raw = Number(values[key]);
      sanitized[key] = Number.isFinite(raw) && raw >= 0 ? raw : 0;
    }
    const mergedMeta: TelemetryMeta = {
      route: meta.route ?? this.meta.route,
      processed: clampNonNegative(meta.processed, this.meta.processed),
      valid: clampNonNegative(meta.valid, this.meta.valid),
      strictSec: clampNonNegative(meta.strictSec, this.meta.strictSec),
      javaSec: clampNonNegative(meta.javaSec, this.meta.javaSec)
    };
    this.meta = mergedMeta;
    const sample: TelemetrySample = { t: now, values: sanitized, meta: mergedMeta };
    this.latest = sample;
    this.ingestCount += 1;
    if (!this.paused) {
      this.samples.push(sample);
      this.trim(now);
    }
    this.scheduleEmit();
  }

  snapshot(): TelemetrySnapshot {
    const stats: Record<string, SeriesStats> = {};
    for (const key of this.seriesOrder) {
      let sum = 0;
      let peak = 0;
      let count = 0;
      for (const sample of this.samples) {
        const v = sample.values[key] ?? 0;
        sum += v;
        if (v > peak) {
          peak = v;
        }
        count += 1;
      }
      const current = this.latest?.values[key] ?? 0;
      stats[key] = {
        current,
        average: count > 0 ? sum / count : 0,
        peak: Math.max(peak, current)
      };
    }
    return {
      samples: this.samples,
      latest: this.latest,
      series: this.getSeries(),
      historyMs: this.historyMs,
      paused: this.paused,
      running: this.running,
      stats,
      meta: { ...this.meta },
      ingestCount: this.ingestCount
    };
  }

  private trim(now: number): void {
    const cutoff = now - this.historyMs;
    let drop = 0;
    while (drop < this.samples.length && this.samples[drop].t < cutoff) {
      drop += 1;
    }
    if (drop > 0) {
      this.samples.splice(0, drop);
    }
  }

  private scheduleEmit(): void {
    if (this.emitScheduled || this.listeners.size === 0) {
      if (this.listeners.size === 0) {
        this.emitScheduled = false;
      }
      return;
    }
    this.emitScheduled = true;
    queueMicrotask(() => {
      this.emitScheduled = false;
      this.emit();
    });
  }

  private emit(): void {
    if (this.listeners.size === 0) {
      return;
    }
    const snapshot = this.snapshot();
    for (const listener of this.listeners) {
      listener(snapshot);
    }
  }
}

function clampHistory(ms: number): number {
  if (!Number.isFinite(ms)) {
    return 60_000;
  }
  return Math.max(HISTORY_MIN_MS, Math.min(HISTORY_MAX_MS, Math.round(ms)));
}

function clampNonNegative(next: number | undefined, fallback: number): number {
  if (next === undefined) {
    return fallback;
  }
  const n = Number(next);
  if (!Number.isFinite(n) || n < 0) {
    return fallback;
  }
  return n;
}
