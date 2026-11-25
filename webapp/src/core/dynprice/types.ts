/**
 * Dynamic (spot) prices — the hub's wire shapes (fw 1.2.0, root dynprice-webapp; spec
 * DEVICE_API_SURFACE «Dynamic prices», RBGRID_CMD.md, design §6a). One core implementation behind
 * REST, MQTT `cmd` and ESPHome. Codes (states, provenance, errors) are enums the app translates;
 * times are UTC seconds; money is per kWh in the plan's currency, and may be negative.
 */

export type PriceState = 'ok' | 'backup' | 'today_only' | 'stale' | 'plan_fallback' | 'disabled';
export type PriceProv = 'source' | 'backup' | 'ha_push' | 'plan';
export type PriceError = 'none' | 'fetch_failed' | 'http_status' | 'parse' | 'auth' | 'no_data_for_zone' | 'rate_limited';
export type SellMode = 'source' | 'plan' | 'formula';
export type ProviderClass = 'spot' | 'all_in';

export interface PriceZone { id: string; name: string; country: string; currency: string }

export interface PriceProvider {
  id: string;
  name: string;
  class: ProviderClass;
  /** Local = Home Assistant pushes; the hub fetches nothing. */
  local: boolean;
  /** `ha`: only once a push has arrived (or in the ESPHome build). Hide when false. */
  available: boolean;
  needs_token: boolean;
  resolution_s: number;
  zones: PriceZone[];
  terms_url?: string;
  /** Shown verbatim wherever a price from this provider is shown (licence). */
  attribution?: string;
}

/** The fields POST /api/v1/dynprice merges. */
export interface DynPriceSettings {
  enabled: boolean;
  source: string;
  backup_source: string;
  zone: string;
  ha_class: ProviderClass;
  factor: number;
  markup: number;
  zones_stack: boolean;
  hourly_mean: boolean;
  floor_buy: number | null;
  floor_sell: number | null;
  sell_mode: SellMode;
  k_sell: number;
  feedin_fee: number;
  product?: string;
  export_product?: string;
}

export interface DynPriceStatus {
  price_state: PriceState;
  source_now: string;
  backup_now: boolean;
  valid_until: number;
  last_fetch: number;
  next_fetch: number;
  last_error: PriceError | string;
  currency: string;
  token_set?: boolean;
  backup_token_set?: boolean;
}

export interface DynPriceDoc { settings: DynPriceSettings; status: DynPriceStatus; providers: PriceProvider[] }

/** One 15-min period as the hub books it. `buy` is all-in. */
export interface ResolvedPrice { from: number; until: number; buy: number; sell: number | null; raw?: number | null; prov: PriceProv | string; hourly_mean?: boolean }

export interface PriceDoc { now: number; price_state: PriceState; source_now: string; valid_until: number; currency: string; resolved: ResolvedPrice[] }

/** The applied-price audit log row: closed 15-min periods, 40 days. `prov` may carry `+hourly_mean`. */
export interface PriceLogRow { from: number; buy: number; sell: number | null; prov: string }
export interface PriceLog { currency: string; rows: PriceLogRow[]; more: boolean }

export type RuleKind = 'threshold' | 'cheapest' | 'deadline';

export interface RuleState { active: boolean; active_until: number; next_start: number; window_from?: number; window_until?: number }

export interface PriceRule {
  id: number;
  name: string;
  kind: RuleKind;
  enabled: boolean;
  /** threshold */
  on_below?: number;
  off_above?: number;
  /** cheapest (local hours; to_h ≤ from_h crosses midnight, equal = 24 h) */
  from_h?: number;
  to_h?: number;
  /** deadline (local hour) */
  deadline_h?: number;
  /** cheapest / deadline */
  periods?: number;
  consecutive?: boolean;
  state?: RuleState;
}

export interface SignalFlags { buy_cheap_below: number | null; sell_low_below: number | null }
export interface RulesDoc { rules: PriceRule[]; signal: SignalFlags }

export interface PriceSignal {
  ts: number;
  period_from: number;
  period_until: number;
  currency: string;
  buy: number;
  sell: number | null;
  price_state: PriceState;
  provenance: string;
  rank_buy: number;
  rank_sell: number;
  periods_today: number;
  windows: { rule_id: number; name: string; active: boolean; active_until: number; next_start: number }[];
  sell_low: boolean;
  buy_cheap: boolean;
}

export interface PeakDay { hour_start: number; w: number }
export interface PeaksCycle {
  cycle_start: number;
  max_15min_w: number;
  max_15min_end: number;
  max_hour_w: number;
  max_hour_start: number;
  top_days: PeakDay[];
  top3_days_mean_w: number;
}
export interface PeaksDoc extends PeaksCycle { cycle_day: number; hour_start?: number; hour_wh_so_far?: number; prev?: PeaksCycle }

/** Limits from the spec. */
export const MAX_RULES = 8;
export const RULE_NAME_MAX = 23;
