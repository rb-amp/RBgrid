import { parseLlm, llmEndpoint, parseVersions, parseProposal, parseEvents } from './api';

// Shape as the S3 bench hub (fw 1.3.0-dev) answers GET /api/v1/llm.
const RAW = {
  enabled: true, read_only: false, proto: '1.0',
  pairing: [{ client_name: 'Claude <b>x</b>', scopes: ['read', 'config'], code: '123456', expires_in: 540 }, { client_name: 'junk' }],
  tokens: [
    { id: '1eedb2d2', client_name: 'Claude (user simulation)', scopes: ['read'], created: 1795098991, last_used: 1795102081 },
    { id: '4129f81c', client_name: 'cfg_test', scopes: ['read', 'config'], created: null, last_used: null },
  ],
  log: [
    { ts: 100, client: 'a', tool: 'get_config', result: 'ok', ms: 56, bytes: 537, config_changed: false },
    { ts: 200, client: 'a', tool: 'apply_config', result: 'ok', ms: 90, bytes: 100, config_changed: true },
    { ts: 150, client: 'a', tool: 'propose_config', result: 'forbidden_scope', ms: 3, bytes: 80 },
  ],
  log_total: 3,
};

describe('parseLlm', () => {
  it('reads the hub answer, drops pairings without a code, sorts the log newest first', () => {
    const d = parseLlm(RAW);
    expect(d.enabled).toBe(true);
    expect(d.pairing).toEqual([{ pair_id: '', client_name: 'Claude <b>x</b>', scopes: ['read', 'config'], code: '123456', expires_in: 540 }]);
    expect(d.tokens[1]).toEqual({ id: '4129f81c', client_name: 'cfg_test', scopes: ['read', 'config'], created: null, last_used: null });
    expect(d.log.map((l) => l.ts)).toEqual([200, 150, 100]);
    expect(d.log[0].config_changed).toBe(true);
    expect(d.log[1].result).toBe('forbidden_scope');
    expect(d.log_total).toBe(3);
  });
  it('survives an empty or odd body', () => {
    expect(parseLlm(null)).toEqual({ enabled: false, read_only: false, proto: '', pairing: [], tokens: [], log: [], log_total: 0, connect: null });
    expect(parseLlm({ tokens: 'x', log: [1, null] }).tokens).toEqual([]);
  });
});

describe('llmEndpoint', () => {
  it('appends the protocol path to the hub address', () => {
    expect(llmEndpoint('http://192.168.1.50/')).toBe('http://192.168.1.50/api/llm/v1');
  });
});

describe('page tool answers (fw 1.3.0-dev 3b5781c shapes)', () => {
  it('reads the connect block and pair_id', () => {
    const d = parseLlm({ pairing: [{ pair_id: 'ab12', client_name: 'x', scopes: ['read'], code: '111222', expires_in: 600 }],
      connect: { hub_id: 'd8132a03bfac', name: 'Home', mdns_host: 'rbgrid-bfac.local', ip: '192.168.1.50', url: 'http://192.168.1.50/', proto: '1.0' } });
    expect(d.pairing[0].pair_id).toBe('ab12');
    expect(d.connect?.mdns_host).toBe('rbgrid-bfac.local');
  });
  it('versions newest first, time null before clock sync', () => {
    const v = parseVersions({ versions: [{ version: 3, time: null, source: 'hub', summary: 'first recorded state', current: false },
      { version: 4, time: '2026-11-20T10:00:00+01:00', source: 'llm:Claude', summary: '1 change(s)', current: true }] });
    expect(v.map((x) => x.version)).toEqual([4, 3]);
    expect(v[1].time).toBeNull();
    expect(v[0].current).toBe(true);
  });
  it('a rollback proposal keeps numbers and strings of the diff', () => {
    const p = parseProposal({ diff: [{ path: 'channels.0006.name', from: 'PageTest', to: 'Boiler' }, { path: 'notifications.peaks_cycle_day', from: 1, to: 15 }],
      warnings: ['w'], restart_required: false, confirm_token: 'f'.repeat(32), expires_in: 600 });
    expect(p.diff[1]).toEqual({ path: 'notifications.peaks_cycle_day', from: 1, to: 15 });
    expect(p.confirm_token).toHaveLength(32);
  });
  it('events with data and the cursor', () => {
    const e = parseEvents({ events: [{ seq: 7, time: null, type: 'config_changed', severity: 'info', data: { version: 4, source: 'web' }, message: 'm' }], next: 7, more: true });
    expect(e.events[0].data.version).toBe(4);
    expect(e.more).toBe(true);
  });
});
