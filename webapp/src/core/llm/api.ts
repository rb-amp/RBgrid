/**
 * The hub's LLM API admin surface (fw 1.3.0, root ai-llm-page-contract; protocol v1 §2, §6):
 * GET /api/v1/llm → state, pending pairing codes, paired clients (no hashes), the newest 50 calls;
 * POST /api/v1/llm {enabled?, read_only?, revoke?} → the same doc. REST admin token, like the
 * other settings. `enabled: true` answers 409 while the hub has no admin token.
 * Pairing codes are the secret the user reads to the assistant: never logged here.
 */
import axios from 'axios';
import { getBaseUrl, isDemo } from '../../services/connection';
import { tokenStore } from '../auth/authWiring';

type Obj = Record<string, unknown>;
const num = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const numOrNull = (v: unknown): number | null => (typeof v === 'number' && Number.isFinite(v) && v > 0 ? v : null);
const str = (v: unknown, d = ''): string => (typeof v === 'string' ? v : d);
const arr = (v: unknown): Obj[] => (Array.isArray(v) ? v.filter((x): x is Obj => !!x && typeof x === 'object') : []);
const strs = (v: unknown): string[] => (Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string') : []);

export interface LlmPairing { pair_id: string; client_name: string; scopes: string[]; code: string; expires_in: number }
/** What a client's config snippet needs (fw 1.3.0-dev 3b5781c): the hub's mDNS name first, its IP as a fallback. */
export interface LlmConnect { hub_id: string; name: string; mdns_host: string; ip: string; url: string; proto: string }
export interface LlmToken { id: string; client_name: string; scopes: string[]; created: number | null; last_used: number | null }
export interface LlmCall { ts: number; client: string; tool: string; result: string; ms: number; bytes: number; config_changed: boolean }
export interface LlmDoc {
  enabled: boolean;
  read_only: boolean;
  proto: string;
  pairing: LlmPairing[];
  tokens: LlmToken[];
  log: LlmCall[];
  log_total: number;
  connect: LlmConnect | null;
}

export function parseLlm(raw: unknown): LlmDoc {
  const b = (raw ?? {}) as Obj;
  return {
    enabled: b.enabled === true,
    read_only: b.read_only === true,
    proto: str(b.proto),
    pairing: arr(b.pairing).map((p) => ({ pair_id: str(p.pair_id), client_name: str(p.client_name), scopes: strs(p.scopes), code: str(p.code), expires_in: num(p.expires_in) }))
      .filter((p) => p.code),
    tokens: arr(b.tokens).map((x) => ({
      id: str(x.id), client_name: str(x.client_name), scopes: strs(x.scopes), created: numOrNull(x.created), last_used: numOrNull(x.last_used),
    })).filter((x) => x.id),
    log: arr(b.log).map((l) => ({
      ts: num(l.ts), client: str(l.client), tool: str(l.tool), result: str(l.result, 'ok'), ms: num(l.ms), bytes: num(l.bytes),
      config_changed: l.config_changed === true,
    })).sort((a, c) => c.ts - a.ts),
    log_total: num(b.log_total),
    connect: b.connect && typeof b.connect === 'object'
      ? (({ hub_id, name, mdns_host, ip, url, proto }: Obj) => ({ hub_id: str(hub_id), name: str(name), mdns_host: str(mdns_host), ip: str(ip), url: str(url), proto: str(proto) }))(b.connect as Obj)
      : null,
  };
}

// ── the page's own tool calls (POST /api/v1/llm/call, caller `web`; protocol v1 §4 envelope) ──

/** A config version the hub keeps (the last 5), newest first. `time` is ISO with offset, null before clock sync. */
export interface LlmVersion { version: number; time: string | null; source: string; summary: string; current: boolean }
export interface LlmDiff { path: string; from: string | number; to: string | number }
export interface LlmProposal { diff: LlmDiff[]; warnings: string[]; confirm_token: string; expires_in: number }
/** One entry of the hub's typed event ring (64 kept across restarts). */
export interface LlmEvent { seq: number; time: string | null; type: string; severity: 'info' | 'warning' | 'critical'; channel?: string; data: Obj; message: string }

export function parseVersions(r: unknown): LlmVersion[] {
  return arr((r as Obj)?.versions).map((v) => ({
    version: num(v.version), time: typeof v.time === 'string' ? v.time : null, source: str(v.source), summary: str(v.summary), current: v.current === true,
  })).sort((a, b) => b.version - a.version);
}
export function parseProposal(r: unknown): LlmProposal {
  const o = (r ?? {}) as Obj;
  const sv = (v: unknown): string | number => (typeof v === 'number' ? v : str(v));
  return {
    diff: arr(o.diff).map((d) => ({ path: str(d.path), from: sv(d.from), to: sv(d.to) })),
    warnings: strs(o.warnings), confirm_token: str(o.confirm_token), expires_in: num(o.expires_in, 600),
  };
}
export function parseEvents(r: unknown): { events: LlmEvent[]; next: number; more: boolean } {
  const o = (r ?? {}) as Obj;
  return {
    events: arr(o.events).map((e) => ({
      seq: num(e.seq), time: typeof e.time === 'string' ? e.time : null, type: str(e.type, 'other'),
      severity: e.severity === 'critical' || e.severity === 'warning' ? e.severity : 'info',
      ...(typeof e.channel === 'string' ? { channel: e.channel } : {}),
      data: (e.data && typeof e.data === 'object' ? e.data : {}) as Obj, message: str(e.message),
    })),
    next: num(o.next), more: o.more === true,
  };
}


const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** A refusal with its HTTP status: 409 = no admin token yet; 404/405 = no LLM API on this hub. */
export class LlmError extends Error {
  constructor(message: string, readonly status?: number) { super(message); }
}

/** A tool refused: the envelope's error code (`busy`, `not_found`, `confirm_expired`, `conflict` …). */
export class LlmCallError extends LlmError {
  constructor(message: string, readonly code: string, status?: number) { super(message, status); }
}

function wrap(e: unknown): never {
  const r = (e as { response?: { status?: number; data?: unknown } })?.response;
  if (r) {
    const d = (r.data ?? {}) as Obj;
    throw new LlmError(str(d.error) || str(d.message) || str(d.code) || `HTTP ${r.status}`, r.status);
  }
  throw new LlmError(e instanceof Error ? e.message : String(e));
}

const url = (path: string) => `${getBaseUrl()}${path}`;

export const llmApi = {
  get: async (): Promise<LlmDoc> => {
    if (isDemo()) throw new LlmError('demo', 404); // the public demo plays a hub without it
    try { return parseLlm((await axios.get(url('/api/v1/llm'), { headers: tokenStore.authHeader(), timeout: 8000 })).data); } catch (e) { return wrap(e); }
  },
  set: async (patch: { enabled?: boolean; read_only?: boolean; revoke?: string; pair_cancel?: string }): Promise<LlmDoc> => {
    if (isDemo()) throw new LlmError('demo', 404);
    try { return parseLlm((await axios.post(url('/api/v1/llm'), patch, { headers: tokenStore.authHeader(), timeout: 8000 })).data); } catch (e) { return wrap(e); }
  },
  /** Runs one tool as the page. The hub serves few calls at once: `busy` is retried after its retry_after_ms. */
  call: async (tool: string, args: Obj = {}): Promise<unknown> => {
    if (isDemo()) throw new LlmError('demo', 404);
    for (let attempt = 0; ; attempt++) {
      let env: Obj;
      try {
        env = ((await axios.post(url('/api/v1/llm/call'), { tool, args }, { headers: tokenStore.authHeader(), timeout: 15000, validateStatus: (s) => s < 500 || s === 503 })).data ?? {}) as Obj;
      } catch (e) { return wrap(e); }
      if (env.ok === true) return env.result;
      const err = (env.error ?? {}) as Obj;
      const code = str(err.code, 'internal');
      if (code === 'busy' && attempt < 5) { await sleep(Math.min(3000, num(err.retry_after_ms, 1000))); continue; }
      throw new LlmCallError(str(err.message) || code, code);
    }
  },
  versions: async (): Promise<LlmVersion[]> => parseVersions(await llmApi.call('list_config_versions')),
  /** Restore, step 1: what changes back (the diff) and the one-time token. */
  proposeRollback: async (version: number): Promise<LlmProposal> => parseProposal(await llmApi.call('propose_config', { rollback_to: version })),
  /** Restore, step 2. */
  rollback: async (version: number, confirm_token: string): Promise<void> => { await llmApi.call('rollback_config', { version, confirm_token }); },
  /** The newest `n` typed events, newest first (the ring is read oldest-first, 50 a call). */
  recentEvents: async (n = 20): Promise<LlmEvent[]> => {
    let since = 0; let all: LlmEvent[] = [];
    for (let i = 0; i < 4; i++) {
      const page = parseEvents(await llmApi.call('get_events', { since, limit: 50 }));
      all = all.concat(page.events);
      if (!page.more || page.next <= since) break;
      since = page.next;
    }
    return all.slice(-n).reverse();
  },
};

/** Where an assistant (the local bridge) reaches the hub's LLM API. */
export const llmEndpoint = (base: string): string => {
  const b = base || (typeof window !== 'undefined' ? window.location.origin : '');
  return `${b.replace(/\/+$/, '')}/api/llm/v1`;
};

/** Error codes the call log shows as text (protocol v1 §4.2); others pass through as they are. */
export const LLM_ERROR_CODES = [
  'llm_disabled', 'unauthorized', 'forbidden_scope', 'read_only_mode', 'unknown_tool', 'invalid_args', 'range_too_large',
  'out_of_history', 'not_found', 'busy', 'timeout', 'confirm_required', 'confirm_invalid', 'confirm_expired', 'conflict',
  'restart_pending', 'internal',
] as const;
