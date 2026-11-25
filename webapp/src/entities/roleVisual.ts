/**
 * Role → icon (UI redesign S7, SPEC §6.6): ONE map for a point's look, used by the Overview tile,
 * the appliance tile and the point page (the same machine looks the same everywhere). The role
 * decides; without one, the point's name is a hint (older configs).
 */
import type { UnitEntityGroup } from './model';

/** 24×24 stroke paths. */
export const ROLE_ICONS: Record<string, string> = {
  plug: 'M9 2v6M15 2v6M6 8h12v4a6 6 0 0 1-12 0zM12 18v4',
  sun: 'M12 17a5 5 0 1 0 0-10 5 5 0 0 0 0 10zM12 1v2M12 21v2M4.2 4.2l1.4 1.4M18.4 18.4l1.4 1.4M1 12h2M21 12h2M4.2 19.8l1.4-1.4M18.4 5.6l1.4-1.4',
  home: 'M3 10 12 3l9 7v10a1 1 0 0 1-1 1h-5v-6H9v6H4a1 1 0 0 1-1-1z',
  drop: 'M12 2.7l5.7 5.7a8 8 0 1 1-11.4 0z',
  flame: 'M12 2c1 3 5 5 5 10a5 5 0 0 1-10 0c0-3 2-4 2-7 1 1 2 2 3 4',
  bulb: 'M9 18h6M10 22h4M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z',
  car: 'M3 13l2-6h14l2 6v4H3zM6 17v2M18 17v2M7 13h.01M17 13h.01',
  tower: 'M12 2v20M5 22l7-20 7 20M7 9h10M6 14h12',
  battery: 'M3 7h16v10H3zM21 10v4M7 10v4M11 10v4',
  layers: 'M12 3 2 8l10 5 10-5zM2 13l10 5 10-5',
};

const BY_ROLE: Record<string, string> = {
  MAIN_SUPPLY: 'tower', ALTERNATIVE_SUPPLY: 'tower', CIRCUIT_BREAKER: 'tower',
  RENEWABLE_SOURCE: 'sun', STORAGE_SYSTEM: 'battery', EV_CHARGING: 'car',
  HEATING_COOLING: 'flame', ZONE_LOAD: 'home', DEVICE_LOAD: 'plug', SMART_APPLIANCE: 'plug',
};

/** The icon key of a point: its role, else a hint from its name. */
export function roleIcon(g: Pick<UnitEntityGroup, 'point' | 'name'>, role?: string): string {
  if (role && BY_ROLE[role]) {
    // A heating role on a water heater reads better as a drop.
    if (role === 'HEATING_COOLING' && /(boiler|бойлер|water|вод)/i.test(`${g.point ?? ''} ${g.name}`)) return 'drop';
    return BY_ROLE[role];
  }
  const n = `${g.point ?? ''} ${g.name}`.toLowerCase();
  if (/(ev|charg|заряд|авто|car)/.test(n)) return 'car';
  if (/(solar|солн|pv|генер)/.test(n)) return 'sun';
  if (/(boiler|бойлер|water|вода|hot)/.test(n)) return 'drop';
  if (/(heat|отопл|flame|печ|furnace)/.test(n)) return 'flame';
  if (/(light|освещ|lamp|свет)/.test(n)) return 'bulb';
  if (/(main|ввод|grid|сеть)/.test(n)) return 'tower';
  return 'plug';
}
