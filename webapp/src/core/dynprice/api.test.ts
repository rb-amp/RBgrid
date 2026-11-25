import { parseDynPrice, parsePrice, parsePriceLog, parseRules, parseSignal, parsePeaks, ruleWire } from './api';
import dynprice from './fixtures/dynprice.json';
import price from './fixtures/price.json';
import priceLog from './fixtures/price-log.json';
import rules from './fixtures/rules.json';
import signal from './fixtures/signal.json';
import peaks from './fixtures/peaks.json';

// Replies of the 1.2.0-dev test hub (dev/esphome a000e6e, root dynprice-webapp 10.11).
describe('dynamic prices wire (fw 1.2.0)', () => {
  it('dynprice: settings, status and the provider catalogue', () => {
    const d = parseDynPrice(dynprice);
    expect(d.settings).toMatchObject({ enabled: true, source: 'energy_charts', backup_source: 'ha', zone: 'NL', floor_buy: null, sell_mode: 'source' });
    expect(d.status).toMatchObject({ price_state: 'ok', source_now: 'energy_charts', currency: 'EUR', last_error: 'none' });
    const ec = d.providers.find((p) => p.id === 'energy_charts')!;
    expect(ec.zones.find((z) => z.id === 'NL')).toEqual({ id: 'NL', name: 'Netherlands', country: 'NL', currency: 'EUR' });
    expect(ec.attribution).toBe('Bundesnetzagentur | SMARD.de / Energy-Charts (CC BY 4.0)');
    expect(d.providers.find((p) => p.id === 'octopus')?.class).toBe('all_in');
    expect(d.providers.find((p) => p.id === 'ha')).toMatchObject({ local: true, zones: [] });
  });

  it('price: resolved 15-min periods, sorted, buy all-in', () => {
    const p = parsePrice(price);
    expect(p.currency).toBe('EUR');
    expect(p.resolved.length).toBe(12);
    expect(p.resolved[0]).toMatchObject({ until: p.resolved[0].from + 900, prov: 'source' });
    expect(p.resolved.every((r, i) => i === 0 || r.from > p.resolved[i - 1].from)).toBe(true);
  });

  it('price log: read by column name', () => {
    const l = parsePriceLog(priceLog);
    expect(l.rows[0]).toEqual({ from: 1794273300, buy: 0.364068, sell: 0.08, prov: 'source' });
    expect(parsePriceLog({ cols: ['prov', 'from', 'buy', 'sell'], rows: [['backup+hourly_mean', 10, -0.01, null]] }).rows[0])
      .toEqual({ from: 10, buy: -0.01, sell: null, prov: 'backup+hourly_mean' });
  });

  it('rules: the three kinds with live state; written back without state', () => {
    const r = parseRules(rules);
    expect(r.signal).toEqual({ buy_cheap_below: 0.3, sell_low_below: 0.05 });
    expect(r.rules.map((x) => x.kind)).toEqual(['cheapest', 'threshold', 'deadline']);
    expect(r.rules[0].state).toMatchObject({ active: false, next_start: 1794348000 });
    expect(ruleWire(r.rules[1])).toEqual({ id: 2, name: 'Cheap', kind: 'threshold', enabled: true, on_below: 0.29, off_above: 0.31 });
    expect(ruleWire(r.rules[2])).toEqual({ id: 3, name: 'EV', kind: 'deadline', enabled: true, deadline_h: 7, periods: 12, consecutive: false });
  });

  it('signal and peaks', () => {
    const s = parseSignal(signal)!;
    expect(s).toMatchObject({ rank_buy: 12, periods_today: 96, buy_cheap: false, windows: expect.arrayContaining([expect.objectContaining({ name: 'Boiler' })]) });
    expect(parseSignal({})).toBeNull();
    expect(parsePeaks(peaks)).toMatchObject({ cycle_day: 1, max_15min_w: 0, top_days: [] });
  });
});
