/**
 * Waiting for a restart (UI redesign SPEC §5.3): settings the hub stored but reads only at start.
 * A panel that got «requires reboot» marks it here; ONE strip (Settings and the Overview) says
 * «Waits for a restart: N changes · Restart now» instead of a restart button in every panel.
 * The marks clear when the hub restarts (a new `boot_id` in telemetry) — or by the strip's restart.
 * Kept for the tab's session, per hub.
 */
import { useEffect, useState } from 'react';
import { getBaseUrl, isDemo } from '../services/connection';

const keyFor = () => `rbgrid.rebootPending.${isDemo() ? 'demo' : getBaseUrl() || 'local'}`;
const subs = new Set<(items: string[]) => void>();

function read(): string[] {
  try { const v = JSON.parse(sessionStorage.getItem(keyFor()) ?? '[]'); return Array.isArray(v) ? v.filter((x) => typeof x === 'string') : []; } catch { return []; }
}
function write(items: string[]): void {
  try { if (items.length) sessionStorage.setItem(keyFor(), JSON.stringify(items)); else sessionStorage.removeItem(keyFor()); } catch { /* per page */ }
  subs.forEach((f) => f(items));
}

/** A change that takes effect after a restart (`what` = the panel's name). */
export function markRebootPending(what: string): void {
  const cur = read();
  if (!cur.includes(what)) write([...cur, what]);
}

export function clearRebootPending(): void { write([]); }

let lastBoot: number | null = null;
/** Telemetry saw `boot_id`: a different one than before means the hub restarted. */
export function noteHubBoot(bootId: number): void {
  if (lastBoot != null && bootId !== lastBoot) clearRebootPending();
  lastBoot = bootId;
}

export function useRebootPending(): string[] {
  const [items, setItems] = useState<string[]>(read);
  useEffect(() => { subs.add(setItems); setItems(read()); return () => { subs.delete(setItems); }; }, []);
  return items;
}

/** Mark while `flag` (a panel's «saved, needs a restart» message is up). */
export function useMarkReboot(flag: boolean, what: string): void {
  useEffect(() => { if (flag) markRebootPending(what); }, [flag, what]);
}
