/**
 * Dynamic prices in the demo (hub 1.2.0): the hub's /dynprice, /price, /rules, /signal and /peaks
 * answered in the browser. Made-up day-ahead prices (a morning and an evening peak, a midday dip); the
 * providers list is the one hub 1.2.0 sends (fixtures/dynprice.json).
 *
 * The public demo takes its prices from Home Assistant: a made-up price must not carry a real data
 * provider's attribution. The docs build (mock/demoDocs) shows Energy-Charts, DE-LU, for the frames.
 *
 * `?prices=` picks the state a frame needs: ok (default) · tomorrow (tomorrow's prices not in yet) ·
 * backup · plan · error (the provider answered with an error; tomorrow missing) · allin (EnergyZero, NL) · off.
 */
import fixture from '../core/dynprice/fixtures/dynprice.json';
import { demoRegion, demoTariff } from './demo';
import { demoDocs, docsHubVersion } from './demoDocs';

/** Where the demo plays a 1.2.0 hub with dynamic prices: the docs build as 1.2.0, and the public demo's
 *  euro sites (the plan's currency must be the zone's). */
export const demoHasPrices = (): boolean => (demoDocs() ? docsHubVersion() === '1.2.0' : !demoRegion().us);

const Q = 900;
const VAT = 0.19;
const FEES = 0.165; // the plan's per-kWh fees, before VAT

type Obj = Record<string, unknown>;
type Mode = 'ok' | 'tomorrow' | 'backup' | 'plan' | 'error' | 'allin' | 'off';

const mode = (): Mode => {
  const m = /[?&]prices=([a-z]+)/.exec(typeof window !== 'undefined' ? window.location.search : '')?.[1];
  return m === 'tomorrow' || m === 'backup' || m === 'plan' || m === 'error' || m === 'allin' || m === 'off' ? m : 'ok';
};

const nowS = () => Math.floor(Date.now() / 1000);
const dayStart = (ts: number) => { const d = new Date(ts * 1000); d.setHours(0, 0, 0, 0); return Math.floor(d.getTime() / 1000); };
const nextDay = (ts: number) => dayStart(ts + 26 * 3600);
const r5 = (v: number) => Math.round(v * 100000) / 100000;
const bell = (h: number, at: number, w: number) => Math.exp(-((h - at) ** 2) / (2 * w * w));

/** The day-ahead (market) price of the period starting at `from`, EUR/kWh. */
function rawAt(from: number): number {
  const d = new Date(from * 1000);
  const h = d.getHours() + d.getMinutes() / 60;
  const day = Math.floor(dayStart(from) / 86400);
  const wobble = 0.012 * Math.sin(day * 1.7 + h * 0.9);
  const jit = ((Math.sin(from * 12.9898) * 43758.5453) % 1) * 0.009; // the quarter's own scatter
  return r5(0.086 + 0.055 * bell(h, 8, 1.4) + 0.12 * bell(h, 19.25, 1.7) - 0.08 * bell(h, 13.25, 2.1) + wobble + jit);
}

let settings: Obj = {
  enabled: true, ...(demoDocs() ? { source: 'energy_charts', backup_source: 'ha', zone: 'DE-LU' } : { source: 'ha', backup_source: 'none', zone: '' }), ha_class: 'spot',
  factor: 1, markup: 0.018, zones_stack: false, hourly_mean: false, floor_buy: null, floor_sell: null,
  sell_mode: 'formula', k_sell: 1, feedin_fee: 0.012,
};
const effective = (): Obj => {
  const m = mode();
  if (m === 'allin') return { ...settings, source: 'energyzero', backup_source: 'none', zone: 'NL', sell_mode: 'source' };
  if (m === 'off') return { ...settings, enabled: false };
  return settings;
};

const allIn = () => effective().source === 'energyzero';
const buyOf = (raw: number) => (allIn() ? r5((raw + 0.152) * 1.21) : r5((raw * Number(effective().factor) + Number(effective().markup) + FEES) * (1 + VAT)));
const sellOf = (raw: number) => (effective().sell_mode === 'formula' ? r5(raw * Number(effective().k_sell) - Number(effective().feedin_fee)) : r5(raw));
const planBuy = () => { const p = demoTariff(), h = new Date().getHours(); return r5((h >= p.dayFrom && h < p.dayTo ? p.day : p.night) * (1 + p.vat / 100)); };

/** How the hub priced the period: a provider gap last night came from Home Assistant, the backup (`ok` state too). */
function provAt(from: number, now: number): string {
  const m = mode();
  const today = dayStart(now);
  const nowQ = now - (now % Q);
  if (m === 'plan' && from >= nowQ - 2 * Q && from < nowQ + 6 * Q) return 'plan';
  if (effective().source === 'ha') return 'ha_push';
  const bak = effective().backup_source === 'ha' ? 'ha_push' : 'backup';
  if (m === 'backup' && from >= nowQ - 4 * Q && from < nowQ + 8 * Q) return bak;
  if (!allIn() && effective().backup_source !== 'none' && from >= today + 2 * 3600 && from < today + 3 * 3600) return bak;
  return 'source';
}

const until = (now: number) => (mode() === 'tomorrow' || mode() === 'error' ? nextDay(now) : nextDay(nextDay(now)));

function period(from: number, now: number): Obj {
  const raw = rawAt(from), prov = provAt(from, now);
  const buy = prov === 'plan' ? planBuy() : buyOf(raw);
  const sell = prov === 'plan' ? Number(demoTariff().sell ?? 0) : sellOf(raw);
  return { from, until: from + Q, buy, sell, raw: prov === 'plan' ? null : raw, prov };
}

const state = (): string => {
  const m = mode();
  if (m === 'off') return 'disabled';
  return m === 'backup' ? 'backup' : m === 'plan' ? 'plan_fallback' : m === 'error' ? 'today_only' : 'ok';
};
const sourceNow = () => { const s = state(); return s === 'backup' ? String(effective().backup_source) : s === 'plan_fallback' ? 'plan' : String(effective().source); };

function resolved(from: number, to: number): Obj[] {
  const now = nowS(), end = Math.min(to, until(now));
  const out: Obj[] = [];
  for (let t = from - (from % Q); t < end; t += Q) out.push(period(t, now));
  return out;
}

// ── cheap windows ──
let rules: Obj[] = [
  { id: 1, name: 'Water heater', kind: 'cheapest', enabled: true, from_h: 10, to_h: 17, periods: 12, consecutive: true },
  { id: 2, name: 'Cheap power', kind: 'threshold', enabled: true, on_below: 0.2, off_above: 0.23 },
  { id: 3, name: 'Car by 07:00', kind: 'deadline', enabled: true, deadline_h: 7, periods: 16, consecutive: true },
];
let signalFlags: Obj = { buy_cheap_below: 0.25, sell_low_below: 0.03 };

const atHour = (day: number, h: number) => { const d = new Date(day * 1000); d.setHours(h, 0, 0, 0); return Math.floor(d.getTime() / 1000); };

/** The periods a cheapest/deadline window picks in [a, b). */
function pick(a: number, b: number, n: number, consecutive: boolean, now: number): number[] {
  const ps = [];
  for (let t = a; t < b; t += Q) ps.push({ t, buy: Number(period(t, now).buy) });
  if (!ps.length) return [];
  if (consecutive) {
    let best = 0, bestSum = Infinity;
    for (let i = 0; i + n <= ps.length; i++) { const s = ps.slice(i, i + n).reduce((x, p) => x + p.buy, 0); if (s < bestSum) { bestSum = s; best = i; } }
    return ps.slice(best, best + n).map((p) => p.t);
  }
  return [...ps].sort((x, y) => x.buy - y.buy).slice(0, n).map((p) => p.t).sort((x, y) => x - y);
}

function windowOf(r: Obj, now: number): [number, number] {
  const today = dayStart(now);
  if (r.kind === 'deadline') {
    let end = atHour(today, Number(r.deadline_h));
    if (end <= now) end = atHour(nextDay(today), Number(r.deadline_h));
    return [end - 24 * 3600, end];
  }
  const fh = Number(r.from_h), th = Number(r.to_h);
  let a = atHour(today, fh), b = th > fh ? atHour(today, th) : atHour(nextDay(today), th);
  if (b <= now) { a = atHour(nextDay(today), fh); b = th > fh ? atHour(nextDay(today), th) : atHour(nextDay(nextDay(today)), th); }
  return [a, b];
}

function ruleState(r: Obj, now: number): Obj {
  const nowQ = now - (now % Q), end = until(now);
  if (!r.enabled || state() === 'disabled') return { active: false, active_until: 0, next_start: 0 };
  if (r.kind === 'threshold') {
    const on = Number(r.on_below), off = Number(r.off_above);
    const active = Number(period(nowQ, now).buy) <= on;
    let t = nowQ;
    if (active) { while (t < end && Number(period(t, now).buy) < off) t += Q; return { active, active_until: t, next_start: 0 }; }
    while (t < end && Number(period(t, now).buy) > on) t += Q;
    return { active, active_until: 0, next_start: t < end ? t : 0 };
  }
  const [a, b] = windowOf(r, now);
  const chosen = b <= end ? pick(a, b, Number(r.periods), r.consecutive === true, now) : [];
  const active = chosen.includes(nowQ);
  let activeUntil = 0;
  if (active) { activeUntil = nowQ; while (chosen.includes(activeUntil)) activeUntil += Q; }
  const next = chosen.find((t) => t > nowQ && !(active && t < activeUntil)) ?? 0;
  return { active, active_until: activeUntil, next_start: next, window_from: a, window_until: b };
}

const rulesDoc = (): Obj => {
  const now = nowS();
  return { type: 'rules', signal: signalFlags, rules: rules.map((r) => ({ ...r, state: ruleState(r, now) })) };
};

// ── peaks: the grid input's import, all phases together ──
const cycleStart = (now: number, day: number) => {
  const d = new Date(now * 1000); d.setHours(0, 0, 0, 0);
  if (d.getDate() < day) d.setMonth(d.getMonth() - 1);
  d.setDate(day);
  return Math.floor(d.getTime() / 1000);
};
let peaksCycleDay = 1;
let peaksSince: number | null = null; // a cycle opened by hand (reset / new cycle day)
let peaksPrev: Obj | null = null;

function cycle(start: number, scale: number): Obj {
  const day = (n: number, h: number, m = 0) => start + n * 86400 + h * 3600 + m * 60;
  const days = [{ hour_start: day(3, 18), w: Math.round(5920 * scale) }, { hour_start: day(1, 19), w: Math.round(5480 * scale) }, { hour_start: day(6, 7), w: Math.round(5110 * scale) }];
  return {
    cycle_start: start, max_15min_w: Math.round(7380 * scale), max_15min_end: day(3, 18, 45), max_hour_w: Math.round(5920 * scale), max_hour_start: day(3, 18),
    top_days: days, top3_days_mean_w: Math.round(days.reduce((s, d) => s + d.w, 0) / 3),
  };
}

function peaksDoc(): Obj {
  const now = nowS();
  const start = peaksSince ?? cycleStart(now, peaksCycleDay);
  const fresh = now - start < 4 * 86400;
  const cur = fresh
    ? { cycle_start: start, max_15min_w: 2310, max_15min_end: start + 45 * 60, max_hour_w: 1980, max_hour_start: start, top_days: [{ hour_start: start, w: 1980 }], top3_days_mean_w: 1980 }
    : cycle(start, 1);
  const hourStart = now - (now % 3600);
  const prev = peaksPrev ?? cycle(cycleStart(start - 86400, peaksCycleDay), 1.08);
  return { type: 'peaks', cycle_day: peaksCycleDay, ...cur, hour_start: hourStart, hour_wh_so_far: Math.round(((now - hourStart) / 3600) * 1840), prev };
}

// ── the hub's answers ──
const retrying = () => mode() === 'tomorrow' || mode() === 'error' || mode() === 'backup';
function dynDoc(): Obj {
  const now = nowS(), today = dayStart(now);
  const fetchedToday = now >= today + 13 * 3600 + 300;
  return {
    type: 'dynprice', ...effective(), token_set: false, backup_token_set: false, currency: 'EUR',
    price_state: state(), source_now: sourceNow(), backup_now: state() === 'backup',
    valid_until: state() === 'disabled' ? 0 : until(now),
    // Home Assistant pushes; the hub fetches nothing.
    last_fetch: effective().source === 'ha' ? 0 : fetchedToday ? today + 13 * 3600 + 312 : today - 11 * 3600 + 312,
    // As hub 1.2.0: a next fetch only while it retries (tomorrow missing, an error, no data for the zone):
    // every 15 min before 15:00 UTC, hourly after.
    next_fetch: retrying() ? now - (now % 60) + (now % 86400 < 15 * 3600 ? Q : 3600) : 0,
    last_error: state() === 'backup' ? 'no_data_for_zone' : mode() === 'error' ? 'http_status' : 'none',
    providers: (fixture as Obj).providers,
  };
}

function signalDoc(): Obj {
  const now = nowS(), nowQ = now - (now % Q), p = period(nowQ, now);
  const day = resolved(dayStart(now), nextDay(now));
  const rank = (k: 'buy' | 'sell') => 1 + day.filter((x) => Number(x[k]) < Number(p[k])).length;
  const st = rulesDoc().rules as Obj[];
  return {
    v: 1, ts: now, period_from: nowQ, period_until: nowQ + Q, currency: 'EUR', buy: p.buy, sell: p.sell,
    price_state: state(), provenance: p.prov, rank_buy: rank('buy'), rank_sell: rank('sell'), periods_today: day.length,
    windows: st.filter((r) => r.enabled).map((r) => { const s = r.state as Obj; return { rule_id: r.id, name: r.name, active: s.active, active_until: s.active_until, next_start: s.next_start }; }),
    buy_cheap: signalFlags.buy_cheap_below != null && Number(p.buy) < Number(signalFlags.buy_cheap_below),
    sell_low: signalFlags.sell_low_below != null && Number(p.sell) < Number(signalFlags.sell_low_below),
  };
}

function logDoc(from: number, to: number): Obj {
  const now = nowS(), end = Math.min(to, now - (now % Q));
  const rows: unknown[][] = [];
  for (let t = from - (from % Q); t < end && rows.length < 400; t += Q) { const p = period(t, now); rows.push([t, p.buy, p.sell, p.prov]); }
  return { type: 'price_log', currency: 'EUR', cols: ['from', 'buy', 'sell', 'prov'], rows, more: false };
}

export const demoPrices = {
  get: (): Obj => dynDoc(),
  set: (patch: Obj): Obj => {
    const { token: _t, backup_token: _b, ...rest } = patch;
    settings = { ...settings, ...rest };
    return dynDoc();
  },
  price: (from?: number, to?: number): Obj => {
    const now = nowS();
    return { type: 'price', now, active: state() !== 'disabled', price_state: state(), source_now: sourceNow(), valid_until: until(now), currency: 'EUR',
      resolved: state() === 'disabled' ? [] : resolved(from ?? dayStart(now), to ?? until(now)) };
  },
  log: (from?: number, to?: number): Obj => logDoc(from ?? dayStart(nowS()), to ?? nextDay(nowS())),
  rules: (): Obj => rulesDoc(),
  setRules: (data: Obj): Obj => {
    if (Array.isArray(data.rules)) rules = data.rules as Obj[];
    if (data.signal && typeof data.signal === 'object') signalFlags = { ...signalFlags, ...(data.signal as Obj) };
    return rulesDoc();
  },
  signal: (): Obj => signalDoc(),
  peaks: (): Obj => peaksDoc(),
  setPeaks: (data: Obj): Obj => {
    const before = peaksDoc();
    const { type: _x, cycle_day: _d, hour_start: _h, hour_wh_so_far: _w, prev: _p, ...cur } = before;
    if (typeof data.cycle_day === 'number') peaksCycleDay = data.cycle_day;
    peaksPrev = cur; peaksSince = nowS() - (nowS() % Q);
    return peaksDoc();
  },
  /** The hub log's price lines (formats of release/v1.2.0 rbdynprice.c / rbactivation.c), after a boot at `t`. */
  logLines: (t: number): { ts: number; fail: boolean; msg: string }[] => {
    const now = nowS(), vu = until(now), wh = rulesDoc().rules as Obj[];
    const w = wh[0], ws = w.state as Obj;
    return [
      { ts: t + 9, fail: false, msg: 'price: config on, source energy_charts, backup ha, zone DE-LU' },
      { ts: t + 14, fail: true, msg: 'FAIL price: energy_charts DE-LU fetch failed (http_status), retry in 15 min' },
      { ts: t + 915, fail: false, msg: `price: energy_charts DE-LU stored 192 periods (valid until ${vu})` },
      { ts: t + 916, fail: false, msg: `price: state backup -> ok (source energy_charts, valid until ${vu})` },
      ws.active
        ? { ts: t + 1800, fail: false, msg: `price: window '${String(w.name)}' on until ${Number(ws.active_until)}` }
        : { ts: t + 1800, fail: false, msg: `price: window '${String(w.name)}' off, next ${Number(ws.next_start)}` },
    ].filter((l) => l.ts <= now);
  },
  /** The live tariff fields hub 1.2.0 adds to the telemetry while a dynamic price is set. */
  liveTariff: (): Obj => {
    const now = nowS(), p = period(now - (now % Q), now), on = state() !== 'disabled';
    return {
      dynamic: on, price_state: state(), price_source: sourceNow(), price_valid_until: on ? until(now) : 0, price_gen: 1,
      ...(on ? { buy_rate: p.buy, buy_rate_all_in: p.buy, sell_rate: p.sell } : {}),
    };
  },
};
