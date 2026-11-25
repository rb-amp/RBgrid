import { defaultBoards, sanitizeLayout, layoutBytes, MAIN_BOARD, DETAIL_BOARD, Size } from './layout';
import { SiteTraits, evLed, plantLed } from './siteTraits';

const base: SiteTraits = {
  phases: 1, hasInput: true, hasGeneration: false, hasStorage: false, evStations: 0, hasInputLimit: false,
  lineCount: 3, hasTariff: true, hasDynamicPrice: false, money: true,
};
// «Left to set up» opens the Overview (it hides itself once all is set up).
const ids = (t: SiteTraits, board = MAIN_BOARD) => defaultBoards(t).boards.find((b) => b.id === board)!.w.map((p) => p[0]).filter((id) => id !== 'setup');

describe('default boards from what is wired (SPEC §2.5)', () => {
  it('a home: setup first, then now + today, flow, hours, tiles, insights last and folded', () => {
    expect(defaultBoards(base).boards[0].w[0][0]).toBe('setup');
    expect(ids(base)).toEqual(['now', 'today', 'flow', 'hours', 'tiles', 'insights']);
    const main = defaultBoards(base).boards[0];
    expect(main.w.find((p) => p[0] === 'insights')?.[2]).toEqual({ collapsed: true });
    expect(main.w.find((p) => p[0] === 'flow')?.[2]).toEqual({ view: 'home' });
  });
  it('a three-phase home keeps the phases in Details (role audit H-04); no money → no insights', () => {
    expect(ids({ ...base, phases: 3, money: false })).toEqual(['now', 'today', 'flow', 'hours', 'tiles']);
    expect(defaultBoards({ ...base, phases: 3, money: false }).boards[1].w.map((p) => p[0])).toContain('phases');
  });
  it('a home with an input limit and 6 lines stays a home; a hub hinted «home» too', () => {
    expect(plantLed({ ...base, hasInputLimit: true, lineCount: 6 })).toBe(false);
    expect(plantLed({ ...base, hasInputLimit: true, lineCount: 9, profileHint: 'home' })).toBe(false);
  });
  it('a plant (hinted «industrial», or a limit and 8+ lines): limit hero, lines table and attention, the tree, no tiles', () => {
    expect(plantLed({ ...base, hasInputLimit: true, lineCount: 8 })).toBe(true);
    const plant = { ...base, hasInputLimit: true, lineCount: 6, phases: 3 as const, profileHint: 'industrial' as const };
    expect(plantLed(plant)).toBe(true);
    expect(ids(plant)).toEqual(['input-limit', 'today', 'alarms', 'phases', 'flow', 'hours', 'lines-table', 'insights']);
    expect(defaultBoards(plant).boards[0].w.find((p) => p[0] === 'flow')?.[2]).toEqual({ view: 'tree' });
  });
  it('a car park (stations ≥ half the lines): the station blocks, no hours, no tiles', () => {
    const park = { ...base, evStations: 4, lineCount: 5, hasInputLimit: true };
    expect(evLed(park)).toBe(true);
    expect(evLed({ ...park, profileHint: 'home' })).toBe(false);
    expect(evLed({ ...base, evStations: 1, lineCount: 6, profileHint: 'ev' })).toBe(true);
    expect(plantLed(park)).toBe(false);
    expect(ids(park)).toEqual(['ev-now', 'ev-kpi', 'ev-stations', 'ev-power', 'flow', 'insights']);
  });
  it('a home with one charger stays a home', () => {
    expect(ids({ ...base, evStations: 1, lineCount: 4 })[0]).toBe('now');
  });
  it('Details: key figures, hours, attention, tariff, flows, points table', () => {
    expect(ids(base, DETAIL_BOARD)).toEqual(['kpi', 'hours', 'alarms', 'tariff', 'flow', 'points-table']);
    expect(ids({ ...base, hasTariff: false, lineCount: 0 }, DETAIL_BOARD)).toEqual(['kpi', 'hours', 'alarms', 'flow']);
  });
});

describe('reading a stored layout', () => {
  const known = (id: string) => ['now', 'today', 'tile', 'flow'].includes(id);
  const sizesOf = (id: string): Size[] => (id === 'tile' ? ['third'] : ['half', 'full']);

  it('drops unknown blocks, fixes bad sizes, keeps options and point lists', () => {
    const doc = sanitizeLayout({
      v: 1,
      boards: [{ id: 'main', name: '', w: [['now', 'half'], ['gone', 'full'], ['tile', 'full', { unit: '0012' }], ['today', 'huge'], 7] }],
      pins: ['0012', 3], hide: ['0031'],
    }, known, sizesOf);
    expect(doc?.boards[0].w).toEqual([['now', 'half'], ['tile', 'third', { unit: '0012' }], ['today', 'half']]);
    expect(doc?.pins).toEqual(['0012']);
    expect(doc?.hide).toEqual(['0031']);
  });
  it('another version or a broken document → null (defaults, not overwritten)', () => {
    expect(sanitizeLayout({ v: 2, boards: [] }, known, sizesOf)).toBeNull();
    expect(sanitizeLayout('nope', known, sizesOf)).toBeNull();
    expect(sanitizeLayout({ v: 1, boards: [{ id: '' }] }, known, sizesOf)).toBeNull();
  });
  it('boards keep their limits and unique ids', () => {
    const many = Array.from({ length: 12 }, (_, i) => ({ id: i < 2 ? 'same' : `b${i}`, name: 'x', w: [] }));
    const doc = sanitizeLayout({ v: 1, boards: many }, known, sizesOf)!;
    expect(doc.boards.length).toBe(7);
    expect(new Set(doc.boards.map((b) => b.id)).size).toBe(7);
  });
  it('a typical home layout is far below the hub limit', () => {
    expect(layoutBytes(defaultBoards(base))).toBeLessThan(1024);
  });
});
