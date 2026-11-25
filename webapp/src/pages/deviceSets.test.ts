/** One tile per appliance (UI redesign SPEC §2.8): per-phase points of one machine fold into one set. */
import { normalizeTelemetryFrame } from '../entities/model';
import { buildUnitsJoin } from '../hooks/useUnitsConfig';
import { deviceSets, lineUnits } from './overviewData';
import type { JsonObject } from '../core/json';

const join = buildUnitsJoin([
  { unit_id: '9001', role: 'MAIN_SUPPLY', phase: 4 },
  { unit_id: '9040', point: 'hp1', name: 'Heat pump L1', role: 'HEATING_COOLING', phase: 1, group_id: 2, rated_power_w: 1400 },
  { unit_id: '9041', point: 'hp2', name: 'Heat pump L2', role: 'HEATING_COOLING', phase: 2, group_id: 2, rated_power_w: 1400 },
  { unit_id: '9042', point: 'hp3', name: 'Heat pump L3', role: 'HEATING_COOLING', phase: 3, group_id: 2, rated_power_w: 1400 },
  // A room group: two different appliances on one phase — not one machine.
  { unit_id: '9020', point: 'k', name: 'Kitchen', role: 'ZONE_LOAD', phase: 1, group_id: 1, rated_power_w: 3680 },
  { unit_id: '9010', point: 'b', name: 'Boiler', role: 'HEATING_COOLING', phase: 1, group_id: 1, rated_power_w: 2300 },
  { unit_id: '9050', point: 'g', name: 'Garage', role: 'ZONE_LOAD', phase: 2 },
] as unknown as JsonObject[]);
const unit = (point: string, p: number) => ({ src: 'i2c', present: true, point, i: 1, p_act: p });
const data = normalizeTelemetryFrame({
  v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0,
  units: {
    '9001': { src: 'i2c', present: true, i: 30, p_act: 7000 },
    '9040': unit('hp1', 400), '9041': unit('hp2', 400), '9042': unit('hp3', 400),
    '9020': unit('k', 100), '9010': unit('b', 0), '9050': unit('g', 50),
  },
} as never);

describe('device sets', () => {
  const sets = deviceSets(lineUnits(data.units, join), join);
  it('three per-phase points of one group with one rating fold into one set', () => {
    const hp = sets.find((s) => s.units.length > 1);
    expect(hp?.groupId).toBe(2);
    expect(hp?.units.map((u) => u.unitKey).sort()).toEqual(['9040', '9041', '9042']);
  });
  it('a room group and an ungrouped line keep a set per point', () => {
    expect(sets.length).toBe(4);
    expect(sets.filter((s) => s.units.length === 1).map((s) => s.lead.unitKey).sort()).toEqual(['9010', '9020', '9050']);
  });
});
