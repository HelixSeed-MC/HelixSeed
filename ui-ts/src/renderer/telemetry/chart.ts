import { TelemetryStore, type SeriesDefinition, type TelemetrySample, type TelemetrySnapshot } from "./store";

export type LineChartFormatOptions = {
  formatValue?: (value: number) => string;
  formatAxis?: (value: number) => string;
};

export type LineChartOptions = LineChartFormatOptions & {
  canvas: HTMLCanvasElement;
  store: TelemetryStore;
  background?: string;
  gridColor?: string;
  axisColor?: string;
  axisFont?: string;
  showFps?: boolean;
};

type Padding = { left: number; top: number; right: number; bottom: number };

const DEFAULT_BACKGROUND = "#0b0d0f";
const DEFAULT_GRID = "#161c22";
const DEFAULT_AXIS = "#5f6b74";
const DEFAULT_AXIS_FONT = '10px "JetBrains Mono", "Cascadia Mono", Consolas, monospace';

export class LineChart {
  private readonly canvas: HTMLCanvasElement;
  private readonly store: TelemetryStore;
  private readonly background: string;
  private readonly gridColor: string;
  private readonly axisColor: string;
  private readonly axisFont: string;
  private readonly formatValue: (value: number) => string;
  private readonly formatAxis: (value: number) => string;
  private readonly resizeObserver: ResizeObserver;
  private readonly unsubscribe: () => void;

  private snapshot: TelemetrySnapshot;
  private frame = 0;
  private hoverX: number | null = null;
  private showFps: boolean;
  private fpsLastTime = 0;
  private fpsFrames = 0;
  private fpsValue = 0;
  private destroyed = false;
  private lastIngestCount = -1;
  private autoDrive = false;

  constructor(opts: LineChartOptions) {
    this.canvas = opts.canvas;
    this.store = opts.store;
    this.background = opts.background ?? DEFAULT_BACKGROUND;
    this.gridColor = opts.gridColor ?? DEFAULT_GRID;
    this.axisColor = opts.axisColor ?? DEFAULT_AXIS;
    this.axisFont = opts.axisFont ?? DEFAULT_AXIS_FONT;
    this.formatValue = opts.formatValue ?? ((v) => v.toLocaleString());
    this.formatAxis = opts.formatAxis ?? this.formatValue;
    this.showFps = opts.showFps ?? false;

    this.snapshot = this.store.snapshot();
    this.lastIngestCount = this.snapshot.ingestCount;

    this.resizeObserver = new ResizeObserver(() => this.requestDraw());
    this.resizeObserver.observe(this.canvas);

    this.canvas.addEventListener("mousemove", this.onMouseMove);
    this.canvas.addEventListener("mouseleave", this.onMouseLeave);

    this.unsubscribe = this.store.subscribe((snapshot) => {
      const hadNewSample = snapshot.ingestCount !== this.lastIngestCount;
      this.lastIngestCount = snapshot.ingestCount;
      this.snapshot = snapshot;
      if (hadNewSample || !this.autoDrive) {
        this.requestDraw();
      }
      const shouldDrive = snapshot.running && !snapshot.paused;
      if (shouldDrive && !this.autoDrive) {
        this.autoDrive = true;
        this.requestDraw();
      } else if (!shouldDrive) {
        this.autoDrive = false;
      }
    });
  }

  setFpsVisible(visible: boolean): void {
    this.showFps = visible;
    this.requestDraw();
  }

  destroy(): void {
    this.destroyed = true;
    this.resizeObserver.disconnect();
    this.canvas.removeEventListener("mousemove", this.onMouseMove);
    this.canvas.removeEventListener("mouseleave", this.onMouseLeave);
    this.unsubscribe();
    if (this.frame !== 0) {
      cancelAnimationFrame(this.frame);
      this.frame = 0;
    }
  }

  requestDraw(): void {
    if (this.destroyed || this.frame !== 0) {
      return;
    }
    this.frame = window.requestAnimationFrame(this.loop);
  }

  private readonly loop = (timestamp: number): void => {
    this.frame = 0;
    if (this.destroyed) {
      return;
    }
    this.updateFps(timestamp);
    this.draw();
    if (this.autoDrive || this.hoverX !== null) {
      this.frame = window.requestAnimationFrame(this.loop);
    }
  };

  private readonly onMouseMove = (event: MouseEvent): void => {
    const rect = this.canvas.getBoundingClientRect();
    this.hoverX = event.clientX - rect.left;
    this.requestDraw();
  };

  private readonly onMouseLeave = (): void => {
    if (this.hoverX === null) {
      return;
    }
    this.hoverX = null;
    this.requestDraw();
  };

  private updateFps(timestamp: number): void {
    if (this.fpsLastTime === 0) {
      this.fpsLastTime = timestamp;
      return;
    }
    this.fpsFrames += 1;
    const elapsed = timestamp - this.fpsLastTime;
    if (elapsed >= 500) {
      this.fpsValue = (this.fpsFrames * 1000) / elapsed;
      this.fpsFrames = 0;
      this.fpsLastTime = timestamp;
    }
  }

  private draw(): void {
    const rect = this.canvas.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) {
      return;
    }
    const dpr = Math.max(1, Math.min(2, window.devicePixelRatio || 1));
    const targetW = Math.max(1, Math.floor(rect.width * dpr));
    const targetH = Math.max(1, Math.floor(rect.height * dpr));
    if (this.canvas.width !== targetW || this.canvas.height !== targetH) {
      this.canvas.width = targetW;
      this.canvas.height = targetH;
    }

    const ctx = this.canvas.getContext("2d");
    if (!ctx) {
      return;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const w = rect.width;
    const h = rect.height;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = this.background;
    ctx.fillRect(0, 0, w, h);

    const pad: Padding = { left: 56, right: 16, top: 16, bottom: 28 };
    const plotW = Math.max(1, w - pad.left - pad.right);
    const plotH = Math.max(1, h - pad.top - pad.bottom);
    const now = performance.now();
    const minT = now - this.snapshot.historyMs;
    const scaleMax = this.computeScaleMax();

    this.drawGrid(ctx, pad, plotW, plotH, scaleMax);

    const visibleSeries = this.snapshot.series.filter((series) => series.visible);
    for (const series of visibleSeries) {
      this.drawSeries(ctx, series, pad, plotW, plotH, minT, scaleMax);
    }

    this.drawIdleState(ctx, pad, plotW, plotH);
    this.drawFpsOverlay(ctx, w);

    if (this.hoverX !== null && this.snapshot.samples.length > 0) {
      this.drawHover(ctx, pad, plotW, plotH, visibleSeries, minT, scaleMax, now);
    }
  }

  private computeScaleMax(): number {
    let peak = 0;
    for (const series of this.snapshot.series) {
      if (!series.visible) {
        continue;
      }
      const stats = this.snapshot.stats[series.key];
      if (stats) {
        peak = Math.max(peak, stats.peak, stats.current);
      }
    }
    if (peak <= 0) {
      return 100;
    }
    return niceCeil(peak * 1.15);
  }

  private drawGrid(ctx: CanvasRenderingContext2D, pad: Padding, plotW: number, plotH: number, scaleMax: number): void {
    ctx.save();
    ctx.strokeStyle = this.gridColor;
    ctx.lineWidth = 1;
    ctx.fillStyle = this.axisColor;
    ctx.font = this.axisFont;
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";

    const rows = 4;
    for (let i = 0; i <= rows; i += 1) {
      const y = Math.round(pad.top + (plotH / rows) * i) + 0.5;
      ctx.beginPath();
      ctx.moveTo(pad.left, y);
      ctx.lineTo(pad.left + plotW, y);
      ctx.stroke();
      const value = scaleMax - (scaleMax / rows) * i;
      ctx.fillText(this.formatAxis(value), pad.left - 8, y);
    }

    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    const cols = 6;
    for (let i = 0; i <= cols; i += 1) {
      const x = Math.round(pad.left + (plotW / cols) * i) + 0.5;
      ctx.beginPath();
      ctx.moveTo(x, pad.top);
      ctx.lineTo(x, pad.top + plotH);
      ctx.stroke();
      const secondsAgo = Math.round(((cols - i) / cols) * (this.snapshot.historyMs / 1000));
      ctx.fillText(i === cols ? "now" : `-${secondsAgo}s`, x, pad.top + plotH + 8);
    }
    ctx.restore();
  }

  private drawSeries(
    ctx: CanvasRenderingContext2D,
    series: SeriesDefinition,
    pad: Padding,
    plotW: number,
    plotH: number,
    minT: number,
    scaleMax: number
  ): void {
    const samples = this.snapshot.samples;
    if (samples.length === 0) {
      return;
    }
    const points = new Array<{ x: number; y: number }>(samples.length);
    for (let i = 0; i < samples.length; i += 1) {
      const s = samples[i];
      const v = s.values[series.key] ?? 0;
      points[i] = {
        x: pad.left + ((s.t - minT) / this.snapshot.historyMs) * plotW,
        y: pad.top + plotH - (Math.min(v, scaleMax) / scaleMax) * plotH
      };
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(pad.left, pad.top, plotW, plotH);
    ctx.clip();

    const isPrimary = series.group === "primary" || series.group === undefined;
    if (isPrimary && points.length >= 2) {
      const gradient = ctx.createLinearGradient(0, pad.top, 0, pad.top + plotH);
      const fill = colorAtAlpha(series.color, 0.22);
      const tail = colorAtAlpha(series.color, 0);
      gradient.addColorStop(0, fill);
      gradient.addColorStop(1, tail);
      ctx.beginPath();
      ctx.moveTo(points[0].x, pad.top + plotH);
      for (let i = 0; i < points.length; i += 1) {
        ctx.lineTo(points[i].x, points[i].y);
      }
      ctx.lineTo(points[points.length - 1].x, pad.top + plotH);
      ctx.closePath();
      ctx.fillStyle = gradient;
      ctx.fill();
    }

    ctx.beginPath();
    for (let i = 0; i < points.length; i += 1) {
      const p = points[i];
      if (i === 0) {
        ctx.moveTo(p.x, p.y);
        continue;
      }
      const prev = points[i - 1];
      const cx = (prev.x + p.x) / 2;
      const cy = (prev.y + p.y) / 2;
      ctx.quadraticCurveTo(prev.x, prev.y, cx, cy);
    }
    const last = points[points.length - 1];
    ctx.lineTo(last.x, last.y);
    ctx.strokeStyle = series.color;
    ctx.lineWidth = isPrimary ? 1.8 : 1.2;
    if (isPrimary) {
      ctx.shadowColor = colorAtAlpha(series.color, 0.35);
      ctx.shadowBlur = 10;
    }
    ctx.stroke();
    ctx.restore();
  }

  private drawIdleState(ctx: CanvasRenderingContext2D, pad: Padding, plotW: number, plotH: number): void {
    if (this.snapshot.samples.length > 0) {
      return;
    }
    ctx.save();
    ctx.fillStyle = "#3b454f";
    ctx.font = '12px "Inter", "Segoe UI", system-ui, sans-serif';
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    const text = this.snapshot.running ? "Awaiting first sample…" : "Idle — start a scan to stream telemetry";
    ctx.fillText(text, pad.left + plotW / 2, pad.top + plotH / 2);
    ctx.restore();
  }

  private drawFpsOverlay(ctx: CanvasRenderingContext2D, w: number): void {
    if (!this.showFps) {
      return;
    }
    const text = `${this.fpsValue.toFixed(0).padStart(2, " ")} fps`;
    ctx.save();
    ctx.font = '10px "JetBrains Mono", "Cascadia Mono", Consolas, monospace';
    ctx.textAlign = "right";
    ctx.textBaseline = "top";
    ctx.fillStyle = "rgba(124, 214, 255, 0.7)";
    ctx.fillText(text, w - 12, 8);
    ctx.restore();
  }

  private drawHover(
    ctx: CanvasRenderingContext2D,
    pad: Padding,
    plotW: number,
    plotH: number,
    visibleSeries: SeriesDefinition[],
    minT: number,
    scaleMax: number,
    now: number
  ): void {
    const hx = this.hoverX!;
    if (hx < pad.left || hx > pad.left + plotW) {
      return;
    }
    const samples = this.snapshot.samples;
    const targetT = minT + ((hx - pad.left) / plotW) * this.snapshot.historyMs;
    let nearest: TelemetrySample = samples[0];
    let nearestD = Math.abs(samples[0].t - targetT);
    for (let i = 1; i < samples.length; i += 1) {
      const d = Math.abs(samples[i].t - targetT);
      if (d < nearestD) {
        nearestD = d;
        nearest = samples[i];
      }
    }
    const sx = pad.left + ((nearest.t - minT) / this.snapshot.historyMs) * plotW;

    ctx.save();
    ctx.strokeStyle = "rgba(124, 214, 255, 0.32)";
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(sx, pad.top);
    ctx.lineTo(sx, pad.top + plotH);
    ctx.stroke();
    ctx.setLineDash([]);

    for (const series of visibleSeries) {
      const v = nearest.values[series.key] ?? 0;
      const sy = pad.top + plotH - (Math.min(v, scaleMax) / scaleMax) * plotH;
      ctx.fillStyle = series.color;
      ctx.beginPath();
      ctx.arc(sx, sy, 3, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.restore();

    const lines: { label: string; value: string; color: string }[] = visibleSeries.map((series) => ({
      label: series.label,
      value: this.formatValue(nearest.values[series.key] ?? 0),
      color: series.color
    }));
    const secondsAgo = Math.max(0, (now - nearest.t) / 1000);
    const footer = `-${secondsAgo.toFixed(1)}s · ${nearest.meta.route}`;

    ctx.save();
    ctx.font = '11px "Inter", "Segoe UI", system-ui, sans-serif';
    const lineH = 14;
    const padX = 10;
    const padY = 8;
    const labelW = Math.max(...lines.map((line) => ctx.measureText(line.label).width));
    const valueW = Math.max(...lines.map((line) => ctx.measureText(line.value).width));
    const footerW = ctx.measureText(footer).width;
    const tipW = Math.max(labelW + valueW + 18, footerW) + padX * 2;
    const tipH = (lines.length + 1) * lineH + padY * 2;
    let tipX = sx + 10;
    let tipY = pad.top + 10;
    if (tipX + tipW > pad.left + plotW) {
      tipX = sx - tipW - 10;
    }
    if (tipX < pad.left) {
      tipX = pad.left + 4;
    }
    if (tipY + tipH > pad.top + plotH - 4) {
      tipY = Math.max(pad.top + 4, pad.top + plotH - tipH - 4);
    }
    ctx.fillStyle = "rgba(11, 13, 16, 0.94)";
    ctx.strokeStyle = "#22272d";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.rect(tipX, tipY, tipW, tipH);
    ctx.fill();
    ctx.stroke();

    ctx.textBaseline = "top";
    let cursorY = tipY + padY;
    for (const line of lines) {
      ctx.fillStyle = line.color;
      ctx.textAlign = "left";
      ctx.fillText("●", tipX + padX - 2, cursorY);
      ctx.fillStyle = "#d0d6de";
      ctx.fillText(line.label, tipX + padX + 10, cursorY);
      ctx.textAlign = "right";
      ctx.fillStyle = "#e6e6e6";
      ctx.fillText(line.value, tipX + tipW - padX, cursorY);
      cursorY += lineH;
    }
    ctx.textAlign = "left";
    ctx.fillStyle = "#6f7a85";
    ctx.fillText(footer, tipX + padX, cursorY + 2);
    ctx.restore();
  }
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

function colorAtAlpha(hex: string, alpha: number): string {
  const value = hex.trim();
  if (value.startsWith("rgba(") || value.startsWith("rgb(")) {
    const parsed = value.match(/-?\d+(\.\d+)?/g);
    if (parsed && parsed.length >= 3) {
      return `rgba(${parsed[0]}, ${parsed[1]}, ${parsed[2]}, ${alpha})`;
    }
  }
  if (value.startsWith("#")) {
    const stripped = value.slice(1);
    const full = stripped.length === 3
      ? stripped
          .split("")
          .map((c) => c + c)
          .join("")
      : stripped;
    if (full.length === 6) {
      const r = parseInt(full.slice(0, 2), 16);
      const g = parseInt(full.slice(2, 4), 16);
      const b = parseInt(full.slice(4, 6), 16);
      return `rgba(${r}, ${g}, ${b}, ${alpha})`;
    }
  }
  return value;
}
