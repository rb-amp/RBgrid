import { nestedKeys, topLevelKeys } from './nesting';
import { normalizeTelemetryFrame } from './model';
import { buildUnitsJoin } from '../hooks/useUnitsConfig';
import { unmeteredW, linesCoverage, topLevelLines } from '../pages/overviewData';
import { buildFlowFrame } from '../components/flow/flowFrame';

// root point-nesting (fw 1.1.8): a garage sub-panel 6B08 metered, its EV charger 6B09 metered inside it.
const config = [
  { unit_id: 0x0006, role: 'MAIN_SUPPLY', type: 'CURRENT_LOAD' },
  { unit_id: 0x6B08, role: 'DEVICE_LOAD', inside: 0 },
  { unit_id: 0x6B09, role: 'EV_CHARGING', inside: 0x6B08 },
  { unit_id: 0x9020, role: 'DEVICE_LOAD' },
];
const frame = (p: Record<string, number>, present = true) => normalizeTelemetryFrame({
  v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0,
  units: Object.fromEntries(Object.entries(p).map(([k, w]) => [k, { src: 'i2c', present, v: 230, i: w / 230, p_act: w, dir: 4 }])),
} as never);

describe('point nesting (units-config `inside`, root point-nesting)', () => {
  it('reads `inside` as the parent\'s HEX4; 0, "" and null are top level', () => {
    const j = buildUnitsJoin([...config, { unit_id: 0x9021, inside: '0x6B08' }, { unit_id: 0x9022, inside: '' }, { unit_id: 0x9023, inside: null }]);
    expect(j.forKey!('6B09')?.inside).toBe('6B08');
    expect(j.forKey!('9021')?.inside).toBe('6B08');
    expect(j.forKey!('6B08')?.inside).toBeUndefined();
    expect(j.forKey!('9022')?.inside).toBeUndefined();
    expect(j.forKey!('9023')?.inside).toBeUndefined();
  });

  it('a point is nested only when its parent is in the same set; a chain keeps the top one', () => {
    const cfg: Record<string, { inside?: string }> = { A: {}, B: { inside: '000A' }, C: { inside: '000B' } };
    const of = (k: string) => ({ hex: `000${k}`, ...cfg[k] });
    expect(Array.from(nestedKeys(['A', 'B', 'C'], of))).toEqual(['B', 'C']);
    expect(topLevelKeys(['A', 'B', 'C'], of)).toEqual(['A']);
    // The parent not selected (History) / not a line: the point stands on its own.
    expect(topLevelKeys(['B', 'C'], of)).toEqual(['B']);
    expect(topLevelKeys(['C'], of)).toEqual(['C']);
    // A ⊃ B ⊃ C with B not in the set: C is still inside A (no double count).
    const byHex: Record<string, { hex: string; inside?: string }> = { '000A': { hex: '000A' }, '000B': { hex: '000B', inside: '000A' }, '000C': { hex: '000C', inside: '000B' } };
    expect(topLevelKeys(['000A', '000C'], (k) => byHex[k])).toEqual(['000A']);
    // A loop in a stale config ends.
    const loop: Record<string, { hex: string; inside?: string }> = { '000D': { hex: '000D', inside: '000E' }, '000E': { hex: '000E', inside: '000D' } };
    expect(topLevelKeys(['000D'], (k) => loop[k])).toEqual(['000D']);
    // Pointing at itself never hides it.
    expect(topLevelKeys(['A'], () => ({ hex: '000A', inside: '000A' }))).toEqual(['A']);
  });

  it('the remainder subtracts the charger once, through the sub-panel', () => {
    const join = buildUnitsJoin(config);
    // input 5 kW = sub-panel 3.5 kW (incl. the charger's 3 kW) + 1 kW line + 0.5 kW unmetered
    const d = frame({ '0006': 5000, '6B08': 3500, '6B09': 3000, '9020': 1000 });
    expect(topLevelLines(d.units, join).map((g) => g.unitKey).sort()).toEqual(['6B08', '9020']);
    expect(unmeteredW(d.units, join, 5000)).toBeCloseTo(500, 6);
    expect(linesCoverage(d.units, join)).toBeCloseTo(0.9, 6);
  });

  it('without nesting the old sum stands (older hubs send no `inside`)', () => {
    const join = buildUnitsJoin(config.map(({ inside, ...u }) => u));
    const d = frame({ '0006': 5000, '6B08': 3500, '6B09': 3000, '9020': 1000 });
    expect(unmeteredW(d.units, join, 5000)).toBeCloseTo(-2500, 6);
  });

  it('a nested point offline leaves the remainder known', () => {
    const join = buildUnitsJoin(config);
    const d = normalizeTelemetryFrame({
      v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0, units: {
        '0006': { src: 'i2c', present: true, v: 230, i: 20, p_act: 5000 },
        '6B08': { src: 'espnow', present: true, v: 230, i: 15, p_act: 3500 },
        '6B09': { src: 'espnow', present: false, p_act: 0 },
        '9020': { src: 'i2c', present: true, v: 230, i: 4, p_act: 1000 },
      },
    } as never);
    expect(unmeteredW(d.units, join, 5000)).toBeCloseTo(500, 6);
  });

  it('the flow draws the sub-panel, not the charger on top of it; «other» is the real rest', () => {
    const join = buildUnitsJoin(config);
    const d = frame({ '0006': 5000, '6B08': 3500, '6B09': 3000, '9020': 1000 });
    const f = buildFlowFrame(d, { roleByUnit: join.roleByUnit, nestingOf: join.forGroup });
    expect(f.loads.map((l) => l.id).sort()).toEqual(['6B08', '9020', '__other__']);
    expect(f.loads.find((l) => l.id === '__other__')?.v).toBeCloseTo(0.5, 6);
  });
});
