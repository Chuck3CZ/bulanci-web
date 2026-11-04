// The render check.  A phone starts the game at 75% of its screen's pixels (device.ts deviceDefaults): most phones'
// GPUs do not fill all of them at the display's rate, but some do.  While the player drives (the game's status
// "drive": a race or free roam, not the menus), this counts the frames the game made against the rate it paces to
// (status "rate"; a cutscene's 30 fps does not count) and takes each frame's GPU time (submit to done, the renderer's
// doneMs: every browser has it).  The first warm-up of driving is left out (loading hitches, pipelines still being
// made); once there was enough driving (a few minutes, so a phone that slows down as it warms up has done so) it
// judges once, from what came in between:
//   at 75%: the game kept its rate (97% of the frames it paces to) and the GPU's p90 frame time, scaled to all the
//     pixels (/ 0.75²), stays within 2/3 of a frame: this phone starts at 100% from its next game
//   at 100% (the check's own earlier verdict): the game fell behind (under 95%) and the GPU's p90 takes more than
//     2/3 of a frame: back to 75%
// The verdict stays in this browser (device.ts rememberRender).  Only while render is this device's default: a
// render the player set (settings.ini, the page address) is theirs.  The figures go to the log either way.

import type { RendererSubsystem } from './contract';
import type { StatusReader } from './status';
import { rememberRender } from './device';

export interface RenderCheckTimes { warmupMs: number; decideMs: number }
export const RENDER_CHECK_TIMES: RenderCheckTimes = { warmupMs: 60_000, decideMs: 180_000 };

const TICK_MS = 250;
const MIN_RATE = 50;            // below: a cutscene's 30 fps, not the game's own rate (fps = 30: no verdict)
const UP_HELD = 0.97, DOWN_HELD = 0.95, GPU_SHARE = 2 / 3;
const MAX_SAMPLES = 1 << 16;

const pct = (x: number) => `${Math.round(x * 100)}%`;
const min = (ms: number) => ms >= 60_000 ? `${(ms / 60_000).toFixed(1)} min` : `${Math.round(ms / 1000)} s`;

export class RenderCheck {
  private timer: ReturnType<typeof setInterval>;
  private readonly scale: number;
  /** the status block's last count, and the GPU times that came in since */
  private last: { frames: number; t: number; rate: number; drive: boolean } | null = null;
  private pend: number[] = [];
  /** driving so far; after the warm-up: its time, the frames made and the frames the rate asked for, GPU times */
  private drivingMs = 0;
  private windowMs = 0;
  private got = 0;
  private expected = 0;
  private gpu: number[] = [];
  private verdict: string | null = null;

  /** s: the running game (its status block and renderer attach later); render: the render value this device's
   *  default gave the game (scale:0.75, or backing after an earlier verdict) */
  constructor(private s: { status: StatusReader | null; renderer: RendererSubsystem | null }, render: string,
              private times: RenderCheckTimes, private log: (line: string) => void) {
    this.scale = render === 'backing' ? 1 : parseFloat(render.replace(/^scale:/, '')) || 1;
    this.timer = setInterval(() => this.tick(), TICK_MS);
  }

  stop() { clearInterval(this.timer); }

  /** tests and the page */
  state() {
    return { scale: this.scale, drivingMs: this.drivingMs, windowMs: this.windowMs, held: this.expected ? this.got / this.expected : 0,
             gpuP90: quantile(this.gpu.slice().sort((a, b) => a - b), 0.9), verdict: this.verdict };
  }

  private tick() {
    if (this.verdict !== null) return;                 // one verdict a game
    const done = this.s.renderer?.doneMs?.() ?? [];
    const st = this.s.status?.read();                   // null while the game writes it: the next tick
    const frames = st?.get('frames'), t = st?.get('frames_t');
    if (!st || frames === undefined || t === undefined) return;
    for (const x of done) this.pend.push(x);
    if (this.last && t === this.last.t) return;         // the status thread counts every 250 ms: not again yet
    const rate = st.get('rate') ?? 0, drive = ((st.get('drive') ?? 0) & 3) > 0 && !document.hidden;
    const prev = this.last, pend = this.pend;
    this.last = { frames, t, rate, drive };
    this.pend = [];
    // only time spent driving all along, at the game's own rate
    if (!prev || !prev.drive || !drive || rate !== prev.rate || rate < MIN_RATE || frames < prev.frames) return;
    const dt = t - prev.t;
    this.drivingMs += dt;
    if (this.drivingMs <= this.times.warmupMs) return;
    this.windowMs += dt;
    this.got += frames - prev.frames;
    this.expected += dt * rate / 1000;
    for (const x of pend) if (this.gpu.length < MAX_SAMPLES) this.gpu.push(x);
    if (this.drivingMs >= this.times.decideMs) this.decide();
  }

  private decide() {
    clearInterval(this.timer);
    const held = this.got / this.expected, frameMs = this.windowMs / this.expected;
    const s = this.gpu.slice().sort((a, b) => a - b), p50 = quantile(s, 0.5), p90 = quantile(s, 0.9);
    const full = p90 / (this.scale * this.scale);
    let verdict: string;
    if (!s.length) verdict = 'no GPU frame times, no change';
    else if (this.scale < 1) {
      if (held >= UP_HELD && full <= frameMs * GPU_SHARE) { rememberRender('backing'); verdict = 'room to spare: this phone starts at 100% from its next game'; }
      else verdict = `no room to spare: it stays at ${pct(this.scale)}`;
    } else if (held < DOWN_HELD && p90 > frameMs * GPU_SHARE) { rememberRender(null); verdict = 'behind: this phone starts at 75% again from its next game'; }
    else verdict = held < DOWN_HELD ? 'behind, but not for the GPU: it stays at 100%' : 'keeps up: it stays at 100%';
    this.verdict = verdict;
    this.log(`[app] render check after ${min(this.drivingMs)} of driving at ${pct(this.scale)}: ${this.got} frames of ` +
             `${Math.round(this.expected)} (${pct(held)}), a frame ${frameMs.toFixed(1)} ms, GPU p50 ${p50.toFixed(1)} / p90 ` +
             `${p90.toFixed(1)} ms` + (this.scale < 1 ? ` (${full.toFixed(1)} ms at 100%)` : '') + `: ${verdict}`);
  }
}

function quantile(sorted: number[], p: number) {
  return sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))] : 0;
}
