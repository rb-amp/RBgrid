/**
 * Dashboard layout (SPEC §2.4–2.6): an ordered list of blocks with three sizes, grouped in boards
 * (tabs). One document per hub (stored on the hub when it can, else in the browser):
 *
 *   { "v": 1, "boards": [ { "id": "main", "name": "", "w": [ ["now","half"], ["flow","full",{"view":"home"}] ] } ],
 *     "pins": ["0012"], "hide": ["0031"], "order": ["0012","0020"] }
 *
 * `name: ""` on the two standard boards = the name comes from the translation. Unknown blocks and
 * sizes are dropped when read; a block this hub can't show (no generation yet) is kept and skipped.
 */
import { SiteTraits, evLed, plantLed } from './siteTraits';

export type Size = 'third' | 'half' | 'full';
export const SIZES: Size[] = ['third', 'half', 'full'];

export type WidgetOpts = Record<string, unknown>;
/** One block: [widget id, size, options?] — compact on the wire. */
export type Placed = [string, Size, WidgetOpts?];

export interface Board { id: string; name: string; w: Placed[] }

export interface LayoutDoc {
  v: 1;
  boards: Board[];
  /** Points pinned first / hidden from the tiles / in a manual order (HEX4 point ids). */
  pins?: string[];
  hide?: string[];
  order?: string[];
  /** The boards are the hub's standard ones (only the point lists are the user's): they keep
   *  following what is wired, as if nothing were saved. */
  std?: boolean;
}

export const LIMITS = { boards: 8, blocks: 40, bytes: 8192 } as const;
export const MAIN_BOARD = 'main';
export const DETAIL_BOARD = 'detail';

/** Grid columns of a size per width (SPEC §2.4): lg ≥ 1100, md 820–1099, below that one column. */
export const SIZE_COLS: Record<Size, { xs: number; md: number; lg: number }> = {
  third: { xs: 12, md: 6, lg: 4 },
  half: { xs: 12, md: 6, lg: 6 },
  full: { xs: 12, md: 12, lg: 12 },
};

/** The standard boards for these traits (SPEC §2.5): «Overview» and «Details». */
export function defaultBoards(t: SiteTraits): LayoutDoc {
  const ev = evLed(t);
  const plant = plantLed(t);
  const main: Placed[] = [['setup', 'full']];
  if (ev) main.push(['ev-now', 'full']);
  // A plant sees today's and the month's money next to its attention list (role audit B3: no month figure).
  if (plant) main.push(['input-limit', 'full'], ['today', 'half'], ['alarms', 'half']);
  if (!ev && !plant) main.push(['now', 'half'], ['today', 'half']);
  if (ev) main.push(['ev-kpi', 'full'], ['ev-stations', 'full'], ['ev-power', 'full']);
  // A home keeps the phases in «Details» (role audit H-04: current imbalance, cos φ and «% of the breaker»
  // as the second block of a home's Overview); a car park or a plant watches them on the Overview.
  if (t.phases > 1 && (ev || plant)) main.push(['phases', 'full']);
  main.push(['flow', 'full', { view: ev || plant ? 'tree' : 'home' }]);
  if (!ev) main.push(['hours', 'full']);
  if (plant) main.push(['lines-table', 'full']);
  if (!plant && !ev && t.lineCount > 0) main.push(['tiles', 'full']);
  if (t.money) main.push(['insights', 'full', { collapsed: true }]);

  const detail: Placed[] = [['kpi', 'full']];
  if (t.phases > 1) detail.push(['phases', 'full']);
  detail.push(['hours', 'full'], ['alarms', 'half']);
  if (t.hasTariff) detail.push(['tariff', 'half']);
  detail.push(['flow', 'full', { view: 'flows' }]);
  if (t.lineCount > 0) detail.push(['points-table', 'full']);

  return { v: 1, boards: [{ id: MAIN_BOARD, name: '', w: main }, { id: DETAIL_BOARD, name: '', w: detail }] };
}

const isObj = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v);
const hexList = (v: unknown): string[] | undefined =>
  Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string' && x.length > 0 && x.length <= 16).slice(0, 200) : undefined;

/**
 * Read a stored document. Null for anything that isn't version 1 (the caller then uses the
 * defaults and does not overwrite the stored one until the user saves). `known(id)` = the widget
 * exists in this app; `sizesOf(id)` = its allowed sizes.
 */
export function sanitizeLayout(raw: unknown, known: (id: string) => boolean, sizesOf: (id: string) => Size[]): LayoutDoc | null {
  if (!isObj(raw) || raw.v !== 1 || !Array.isArray(raw.boards)) return null;
  const boards: Board[] = [];
  const ids = new Set<string>();
  for (const b of raw.boards.slice(0, LIMITS.boards)) {
    if (!isObj(b) || typeof b.id !== 'string' || !b.id || ids.has(b.id) || !Array.isArray(b.w)) continue;
    ids.add(b.id);
    const w: Placed[] = [];
    for (const p of b.w.slice(0, LIMITS.blocks)) {
      if (!Array.isArray(p) || typeof p[0] !== 'string' || !known(p[0])) continue;
      const allowed = sizesOf(p[0]);
      const size = SIZES.includes(p[1]) && allowed.includes(p[1]) ? (p[1] as Size) : allowed[0];
      w.push(isObj(p[2]) ? [p[0], size, p[2]] : [p[0], size]);
    }
    boards.push({ id: b.id, name: typeof b.name === 'string' ? b.name.slice(0, 40) : '', w });
  }
  if (!boards.length && raw.std !== true) return null;
  const out: LayoutDoc = { v: 1, boards, pins: hexList(raw.pins), hide: hexList(raw.hide), order: hexList(raw.order) };
  if (raw.std === true) out.std = true;
  return out;
}

/** Bytes on the wire (the hub takes up to LIMITS.bytes). */
export const layoutBytes = (d: LayoutDoc): number => encodeURIComponent(JSON.stringify(d)).replace(/%[0-9A-F]{2}/g, 'x').length;
