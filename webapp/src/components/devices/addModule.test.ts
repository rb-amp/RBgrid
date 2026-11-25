import { addStep, nextFreeAddr, newModules, otherDevices, pullupKey, hex, busesUp } from './addModule';
import type { I2cModule, I2cState } from '../../types/i2c';

const mod = (module: number, addr: number, present = true): I2cModule => ({ module, bus: 0, addr, present });
const st = (modules: ReturnType<typeof mod>[], max = 3): I2cState => ({ buses: [{ bus: 0, up: true }], modules, max_modules: max });

describe('add an rbAmp module', () => {
  it('frees 0x50 first: the module there moves to the next free address', () => {
    expect(addStep(st([mod(0, 0x50)]))).toEqual({ kind: 'free', module: 0, to: 0x51, present: true, model: undefined });
    expect(addStep(st([mod(0, 0x51), mod(1, 0x50)]))).toMatchObject({ kind: 'free', module: 1, to: 0x52 });
  });
  it('v1.1.4: an empty placeholder at 0x50 is taken by the new module — no move', () => {
    expect(addStep(st([{ ...mod(0, 0x50, false), placeholder: true }, mod(1, 0x51)], 8))).toEqual({ kind: 'adopt', module: 0 });
    expect(addStep(st([{ ...mod(0, 0x50, false), placeholder: false }], 8))).toMatchObject({ kind: 'free', present: false });
  });
  it('an absent module at 0x50 cannot be moved', () => {
    expect(addStep(st([mod(0, 0x50, false)]))).toMatchObject({ kind: 'free', present: false });
  });
  it('0x50 free → connect; at the limit → full', () => {
    expect(addStep(st([mod(0, 0x51)]))).toEqual({ kind: 'connect', count: 1, max: 3 });
    expect(addStep(st([mod(0, 0x51), mod(1, 0x52), mod(2, 0x50)]))).toEqual({ kind: 'full', max: 3 });
    expect(addStep({ buses: [], modules: [mod(0, 0x51), mod(1, 0x52), mod(2, 0x53)] })).toEqual({ kind: 'full', max: 3 });
  });
  it('one limit: the hub\'s max_modules, on either bus in any split (operator 22.11)', () => {
    const five = [0, 1, 2, 3, 4].map((i) => mod(i, 0x51 + i));
    expect(addStep(st(five, 8))).toEqual({ kind: 'connect', count: 5, max: 8 });
    const two: I2cState = { buses: [{ bus: 0, up: true }, { bus: 1, up: true }], modules: five, max_modules: 8 };
    expect(addStep(two, 1)).toEqual({ kind: 'connect', count: 0, max: 8 });
    expect(busesUp(two)).toEqual([0, 1]);
    const eight = five.concat([5, 6, 7].map((i) => ({ ...mod(i, 0x51 + i - 5), bus: 1 })));
    expect(addStep({ ...two, modules: eight }, 1)).toEqual({ kind: 'full', max: 8 });
    expect(addStep({ ...two, modules: eight }, 0)).toEqual({ kind: 'full', max: 8 });
  });
  it('the same address on both buses: 0x50 is freed per bus', () => {
    const s2: I2cState = { buses: [{ bus: 0, up: true }, { bus: 1, up: true }], modules: [mod(0, 0x50), { ...mod(1, 0x50), bus: 1 }, { ...mod(2, 0x51), bus: 1 }], max_modules: 8 };
    expect(addStep(s2, 0)).toMatchObject({ kind: 'free', module: 0, to: 0x51 });
    expect(addStep(s2, 1)).toMatchObject({ kind: 'free', module: 1, to: 0x52 });
  });
  it('a free address skips configured modules and devices that answered', () => {
    expect(nextFreeAddr(st([mod(0, 0x50), mod(1, 0x51)]), [{ addr: 0x52, module: null, kind: 'unknown' }])).toBe(0x53);
    expect(nextFreeAddr(st(Array.from({ length: 16 }, (_, i) => mod(i, 0x50 + i)), 20))).toBeNull();
  });
  it('a scan splits new rbAmp modules from the rest', () => {
    const d = [
      { addr: 0x50, module: null, kind: 'rbamp' as const, fw: 'STD-1.7.2' },
      { addr: 0x51, module: 0, kind: 'rbamp' as const },
      { addr: 0x27, module: null, kind: 'device' as const },
    ];
    expect(newModules(d).map((x) => x.addr)).toEqual([0x50]);
    expect(otherDevices(d).map((x) => hex(x.addr))).toEqual(['0x27']);
  });
  it('pull-up advice by the count after adding', () => {
    expect(pullupKey(1)).toBeNull();
    expect(pullupKey(2)).toBe('addmod.pullup');
    expect(pullupKey(3)).toBe('addmod.pullup');
    expect(pullupKey(4)).toBe('addmod.pullup4');
    expect(pullupKey(8)).toBe('addmod.pullup4');
  });
});
