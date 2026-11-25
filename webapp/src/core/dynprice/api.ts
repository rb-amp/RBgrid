/**
 * Dynamic prices over REST (fw 1.2.0, root dynprice-webapp): /api/v1/dynprice, /price, /rules,
 * /signal, /peaks. Parsers are pure (tested against .139's replies); the fetchers carry the hub
 * token like the other admin reads. Writes refuse with 400 {error, field} → {@link DynPriceError}.
 */
import axios from 'axios';
import { getBaseUrl, isDemo } from '../../services/connection';
import { demoPrices } from '../../mock/demoPrices';
import { tokenStore } from '../auth/authWiring';
import {
  DynPriceDoc, DynPriceSettings, PriceDoc, PriceLog, PriceProvider, PriceRule, PriceSignal, PeaksDoc, RulesDoc,
  ResolvedPrice, SignalFlags,
} from './types';

type Obj = Record<string, unknown>;
const num = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const numOrNull = (v: unknown): number | null => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const str = (v: unknown, d = ''): string => (typeof v === 'string' ? v : d);
const arr = (v: unknown): Obj[] => (Array.isArray(v) ? v.filter((x): x is Obj => !!x && typeof x === 'object') : []);
const body = (d: unknown): Obj => { const o = (d ?? {}) as Obj; return (o.data && typeof o.data === 'object' ? o.data : o) as Obj; };

/** A refusal the form can put on its field: the hub's 400 {error, field}, or an HTTP status. */
export class DynPriceError extends Error {
  constructor(message: string, readonly status?: number, readonly field?: string) { super(message); }
}

function wrap(e: unknown): never {
  const r = (e as { response?: { status?: number; data?: unknown } })?.response;
  if (r) {
    const d = (r.data ?? {}) as Obj;
    throw new DynPriceError(str(d.error) || str(d.message) || `HTTP ${r.status}`, r.status, str(d.field) || undefined);
  }
  throw new DynPriceError(e instanceof Error ? e.message : String(e));
}

export function parseProviders(v: unknown): PriceProvider[] {
  return arr(v).map((p): PriceProvider => ({
    id: str(p.id), name: str(p.name, str(p.id)), class: p.class === 'all_in' ? 'all_in' : 'spot', local: p.local === true,
    available: p.available !== false, needs_token: p.needs_token === true, resolution_s: num(p.resolution_s, 900),
    zones: arr(p.zones).map((z) => ({ id: str(z.id), name: str(z.name, str(z.id)), country: str(z.country), currency: str(z.currency) })),
    ...(typeof p.terms_url === 'string' && p.terms_url ? { terms_url: p.terms_url } : {}),
    ...(typeof p.attribution === 'string' && p.attribution ? { attribution: p.attribution } : {}),
  })).filter((p) => p.id);
}

export function parseDynPrice(raw: unknown): DynPriceDoc {
  const b = body(raw);
  const settings: DynPriceSettings = {
    enabled: b.enabled === true, source: str(b.source, 'ha'), backup_source: str(b.backup_source, 'none'), zone: str(b.zone),
    ha_class: b.ha_class === 'all_in' ? 'all_in' : 'spot', factor: num(b.factor, 1), markup: num(b.markup),
    zones_stack: b.zones_stack === true, hourly_mean: b.hourly_mean === true,
    floor_buy: numOrNull(b.floor_buy), floor_sell: numOrNull(b.floor_sell),
    sell_mode: b.sell_mode === 'plan' || b.sell_mode === 'formula' ? b.sell_mode : 'source',
    k_sell: num(b.k_sell, 1), feedin_fee: num(b.feedin_fee),
    ...(typeof b.product === 'string' ? { product: b.product } : {}),
    ...(typeof b.export_product === 'string' ? { export_product: b.export_product } : {}),
  };
  return {
    settings,
    status: {
      price_state: str(b.price_state, 'disabled') as DynPriceDoc['status']['price_state'], source_now: str(b.source_now, 'plan'),
      backup_now: b.backup_now === true, valid_until: num(b.valid_until), last_fetch: num(b.last_fetch), next_fetch: num(b.next_fetch),
      last_error: str(b.last_error, 'none'), currency: str(b.currency),
      ...(typeof b.token_set === 'boolean' ? { token_set: b.token_set } : {}),
      ...(typeof b.backup_token_set === 'boolean' ? { backup_token_set: b.backup_token_set } : {}),
    },
    providers: parseProviders(b.providers),
  };
}

export function parseResolved(v: unknown): ResolvedPrice[] {
  return arr(v)
    .filter((r) => typeof r.from === 'number' && typeof r.until === 'number' && typeof r.buy === 'number')
    .map((r): ResolvedPrice => ({
      from: r.from as number, until: r.until as number, buy: r.buy as number, sell: numOrNull(r.sell),
      ...(r.raw !== undefined ? { raw: numOrNull(r.raw) } : {}), prov: str(r.prov, 'plan'),
      ...(r.hourly_mean === true ? { hourly_mean: true } : {}),
    }))
    .sort((a, b) => a.from - b.from);
}

export function parsePrice(raw: unknown): PriceDoc {
  const b = body(raw);
  return {
    now: num(b.now), price_state: str(b.price_state, 'disabled') as PriceDoc['price_state'], source_now: str(b.source_now, 'plan'),
    valid_until: num(b.valid_until), currency: str(b.currency), resolved: parseResolved(b.resolved),
  };
}

/** The audit log comes as columns + rows; read it by column name, not position. */
export function parsePriceLog(raw: unknown): PriceLog {
  const b = body(raw);
  const cols = Array.isArray(b.cols) ? (b.cols as unknown[]).map((c) => str(c)) : ['from', 'buy', 'sell', 'prov'];
  const at = (name: string) => cols.indexOf(name);
  // Without its time or buy column a row means nothing — none rather than zeros.
  const rows = (at('from') < 0 || at('buy') < 0 || !Array.isArray(b.rows) ? [] : (b.rows as unknown[])).filter(Array.isArray).map((r) => {
    const row = r as unknown[];
    return { from: num(row[at('from')]), buy: num(row[at('buy')]), sell: numOrNull(row[at('sell')]), prov: str(row[at('prov')], 'plan') };
  }).filter((r) => r.from > 0);
  return { currency: str(b.currency), rows, more: b.more === true };
}

export function parseRule(r: Obj): PriceRule {
  const kind = r.kind === 'threshold' || r.kind === 'deadline' ? r.kind : 'cheapest';
  const s = (r.state && typeof r.state === 'object' ? r.state : null) as Obj | null;
  return {
    id: num(r.id), name: str(r.name), kind, enabled: r.enabled !== false,
    ...(kind === 'threshold' ? { on_below: num(r.on_below), off_above: num(r.off_above) } : {}),
    ...(kind === 'cheapest' ? { from_h: num(r.from_h), to_h: num(r.to_h, 24) } : {}),
    ...(kind === 'deadline' ? { deadline_h: num(r.deadline_h, 7) } : {}),
    ...(kind !== 'threshold' ? { periods: num(r.periods, 4), consecutive: r.consecutive === true } : {}),
    ...(s ? { state: {
      active: s.active === true, active_until: num(s.active_until), next_start: num(s.next_start),
      ...(typeof s.window_from === 'number' ? { window_from: s.window_from } : {}),
      ...(typeof s.window_until === 'number' ? { window_until: s.window_until } : {}),
    } } : {}),
  };
}

export function parseRules(raw: unknown): RulesDoc {
  const b = body(raw);
  const sg = (b.signal && typeof b.signal === 'object' ? b.signal : {}) as Obj;
  return { rules: arr(b.rules).map(parseRule), signal: { buy_cheap_below: numOrNull(sg.buy_cheap_below), sell_low_below: numOrNull(sg.sell_low_below) } };
}

/** A rule as the hub takes it back: no `state`, only the fields of its kind. */
export function ruleWire(r: PriceRule): Obj {
  const base: Obj = { id: r.id, name: r.name, kind: r.kind, enabled: r.enabled };
  if (r.kind === 'threshold') return { ...base, on_below: r.on_below, off_above: r.off_above };
  if (r.kind === 'cheapest') return { ...base, from_h: r.from_h, to_h: r.to_h, periods: r.periods, consecutive: !!r.consecutive };
  return { ...base, deadline_h: r.deadline_h, periods: r.periods, consecutive: !!r.consecutive };
}

export function parseSignal(raw: unknown): PriceSignal | null {
  const b = body(raw);
  if (typeof b.buy !== 'number') return null;
  return {
    ts: num(b.ts), period_from: num(b.period_from), period_until: num(b.period_until), currency: str(b.currency),
    buy: b.buy, sell: numOrNull(b.sell), price_state: str(b.price_state, 'disabled') as PriceSignal['price_state'],
    provenance: str(b.provenance, 'plan'), rank_buy: num(b.rank_buy), rank_sell: num(b.rank_sell), periods_today: num(b.periods_today, 96),
    windows: arr(b.windows).map((w) => ({ rule_id: num(w.rule_id), name: str(w.name), active: w.active === true, active_until: num(w.active_until), next_start: num(w.next_start) })),
    sell_low: b.sell_low === true, buy_cheap: b.buy_cheap === true,
  };
}

const cycleOf = (b: Obj) => ({
  cycle_start: num(b.cycle_start), max_15min_w: num(b.max_15min_w), max_15min_end: num(b.max_15min_end),
  max_hour_w: num(b.max_hour_w), max_hour_start: num(b.max_hour_start),
  top_days: arr(b.top_days).map((d) => ({ hour_start: num(d.hour_start), w: num(d.w) })), top3_days_mean_w: num(b.top3_days_mean_w),
});

export function parsePeaks(raw: unknown): PeaksDoc {
  const b = body(raw);
  return {
    ...cycleOf(b), cycle_day: num(b.cycle_day, 1),
    ...(typeof b.hour_start === 'number' ? { hour_start: b.hour_start } : {}),
    ...(typeof b.hour_wh_so_far === 'number' ? { hour_wh_so_far: b.hour_wh_so_far } : {}),
    ...(b.prev && typeof b.prev === 'object' ? { prev: cycleOf(b.prev as Obj) } : {}),
  };
}

// ── fetchers ──
const url = (path: string) => `${getBaseUrl()}${path}`;
const get = async (path: string, params?: Obj): Promise<unknown> => {
  try { return (await axios.get(url(path), { params, headers: tokenStore.authHeader(), timeout: 8000 })).data; } catch (e) { return wrap(e); }
};
const post = async (path: string, data: Obj): Promise<unknown> => {
  try { return (await axios.post(url(path), data, { headers: tokenStore.authHeader(), timeout: 8000 })).data; } catch (e) { return wrap(e); }
};

// The demo answers in the browser (the docs build's hub 1.2.0, mock/demoPrices).
const demo = <T>(f: () => unknown, parse: (raw: unknown) => T): Promise<T> => Promise.resolve(parse(f()));

export const dynpriceApi = {
  get: async () => (isDemo() ? demo(demoPrices.get, parseDynPrice) : parseDynPrice(await get('/api/v1/dynprice'))),
  /** Merges the given fields; the reply is the whole doc again. */
  set: async (patch: Partial<DynPriceSettings> & { token?: string; backup_token?: string }) =>
    (isDemo() ? demo(() => demoPrices.set(patch as Obj), parseDynPrice) : parseDynPrice(await post('/api/v1/dynprice', patch as Obj))),
  price: async (from?: number, until?: number) =>
    (isDemo() ? demo(() => demoPrices.price(from, until), parsePrice) : parsePrice(await get('/api/v1/price', { ...(from ? { from } : {}), ...(until ? { until } : {}) }))),
  log: async (from?: number, until?: number) =>
    (isDemo() ? demo(() => demoPrices.log(from, until), parsePriceLog) : parsePriceLog(await get('/api/v1/price', { log: 1, ...(from ? { from } : {}), ...(until ? { until } : {}) }))),
  rules: async () => (isDemo() ? demo(demoPrices.rules, parseRules) : parseRules(await get('/api/v1/rules'))),
  setRules: async (rules: PriceRule[]) =>
    (isDemo() ? demo(() => demoPrices.setRules({ rules: rules.map(ruleWire) }), parseRules) : parseRules(await post('/api/v1/rules', { rules: rules.map(ruleWire) }))),
  setSignal: async (signal: SignalFlags) =>
    (isDemo() ? demo(() => demoPrices.setRules({ signal: signal as unknown as Obj }), parseRules) : parseRules(await post('/api/v1/rules', { signal }))),
  signal: async () => (isDemo() ? demo(demoPrices.signal, parseSignal) : parseSignal(await get('/api/v1/signal'))),
  peaks: async () => (isDemo() ? demo(demoPrices.peaks, parsePeaks) : parsePeaks(await get('/api/v1/peaks'))),
  setPeaks: async (data: { cycle_day: number } | { reset: true }) =>
    (isDemo() ? demo(() => demoPrices.setPeaks(data as Obj), parsePeaks) : parsePeaks(await post('/api/v1/peaks', data))),
};
