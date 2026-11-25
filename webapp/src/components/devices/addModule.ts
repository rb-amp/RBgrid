/**
 * Adding an rbAmp module on an I2C bus (v1.1.2 bus 0; hub 1.3.0 also bus 1, root i2c-bus1-support
 * 22.11). Every module leaves the factory at 0x50, and the hub adds only a module whose address no
 * configured module on THAT bus holds (the same address may sit on both buses — the bus tells them
 * apart). So before a new module goes on a bus, the module the hub has at 0x50 there moves to a free
 * address (UID-checked, its points keep measuring) — then the new one answers alone at 0x50 and is
 * added. Moving the NEW module instead would make the hub take it for the absent module at 0x50.
 *
 * The only limit is the hub's own `max_modules`, on either bus in any split (operator 22.11: no
 * per-bus rule). The step is derived from the hub's state, not remembered, so the dialog
 * can be closed for a power-off and picked up again.
 */
import type { I2cDevice, I2cState } from '../../types/i2c';

export const FACTORY_ADDR = 0x50;
/** The rbAmp address range (the hub identifies modules only there). */
export const RBAMP_FIRST = 0x50;
export const RBAMP_LAST = 0x5f;
/** The default bus (always on). */
export const BUS = 0;
const DEFAULT_MAX = 3;


export const hex = (a: number): string => `0x${a.toString(16).toUpperCase().padStart(2, '0')}`;

export const maxModules = (s: I2cState): number => (typeof s.max_modules === 'number' && s.max_modules > 0 ? s.max_modules : DEFAULT_MAX);

/** Modules configured on a bus (a module without `bus` is on bus 0 — older hubs). */
export const onBus = (s: I2cState, bus: number) => s.modules.filter((m) => (m.bus ?? 0) === bus);

/** Buses that are running, in order. */
export const busesUp = (s: I2cState): number[] => s.buses.filter((b) => b.up).map((b) => b.bus).sort((a, b) => a - b);

/** The configured module that holds the factory address on a bus, if any. */
export const atFactory = (s: I2cState, bus = BUS) => onBus(s, bus).find((m) => m.addr === FACTORY_ADDR);

/** First address after 0x50 that no configured module and no answering device on the bus uses. */
export function nextFreeAddr(s: I2cState, seen: I2cDevice[] = [], bus = BUS): number | null {
  const used = new Set([...onBus(s, bus).map((m) => m.addr), ...seen.map((d) => d.addr)]);
  for (let a = FACTORY_ADDR + 1; a <= RBAMP_LAST; a++) if (!used.has(a)) return a;
  return null;
}

/** rbAmp modules a scan found that the hub doesn't have yet. */
export const newModules = (devices: I2cDevice[]): I2cDevice[] => devices.filter((d) => d.kind === 'rbamp' && d.module == null);

/** Devices that answered but are not rbAmp modules (named so a stray address isn't a mystery). */
export const otherDevices = (devices: I2cDevice[]): I2cDevice[] => devices.filter((d) => d.kind !== 'rbamp');

export type AddStep =
  /** The hub already has its maximum. */
  | { kind: 'full'; max: number }
  /** Free 0x50 first: move this module to `to` (it must be answering — the hub checks its UID). */
  | { kind: 'free'; module: number; to: number | null; present: boolean; model?: string }
  /** 0x50 is free: connect the new module, then scan. */
  | { kind: 'connect'; count: number; max: number }
  /** v1.1.4: the slot at 0x50 is an empty placeholder — a module from the box connected there simply
   *  becomes that module (the hub identifies it at start-up); no move, no add. */
  | { kind: 'adopt'; module: number };

export function addStep(s: I2cState, bus = BUS): AddStep {
  const max = maxModules(s);
  const f = atFactory(s, bus);
  // A placeholder takes no new slot: a module from the box simply becomes it.
  if (f?.placeholder === true) return { kind: 'adopt', module: f.module };
  if (s.modules.length >= max) return { kind: 'full', max };
  const here = onBus(s, bus).length;
  if (f) return { kind: 'free', module: f.module, to: nextFreeAddr(s, [], bus), present: f.present, model: f.model };
  return { kind: 'connect', count: here, max };
}

/**
 * Wiring advice once the bus carries more than one module (count on that bus AFTER adding). rbamp-dev 03.11 via
 * root, cables up to 15 cm: up to three modules keep their on-board pull-ups as they are (3 modules,
 * all on, 15 cm, 50 kHz: 0 failures in 11 253 reads); from the 4th (v1.1.2 takes up to 8) all but one
 * module's pull-ups go, or one external pair — the rbAmp guide's table (calculated, not measured).
 */
export const pullupKey = (after: number): 'addmod.pullup' | 'addmod.pullup4' | null =>
  after >= 4 ? 'addmod.pullup4' : after >= 2 ? 'addmod.pullup' : null;
