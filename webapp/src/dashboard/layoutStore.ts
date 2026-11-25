/**
 * Where the dashboard layout lives (SPEC §2.6). One interface, two stores:
 *  - browser: localStorage `rbgrid.ui_state.<hub>` (the demo: per demo site) — always there, the
 *    instant copy and the fallback for a hub without the store;
 *  - hub: GET / PUT / DELETE /api/system/ui-state (fw 1.3.0, root ui-state-store) — added in pack H.
 * A «this device only» layout is kept under `rbgrid.ui_state.local.<hub>` and never leaves the browser.
 */
import { getBaseUrl, isDemo } from '../services/connection';
import { demoVariant } from '../mock/demoVariant';
import type { LayoutDoc } from './layout';

export interface StoredLayout { doc: unknown; rev: number }

export type SaveResult = { ok: true; rev: number } | { ok: false; conflict: true; rev: number } | { ok: false; conflict: false; error: string };

export interface LayoutStore {
  kind: 'hub' | 'browser';
  load(): Promise<StoredLayout | null>;
  save(doc: LayoutDoc, rev: number): Promise<SaveResult>;
  clear(): Promise<void>;
}

export const hubKey = (): string => (isDemo() ? `demo.${demoVariant().id}` : getBaseUrl() || 'local');
export const browserKey = (local = false): string => `rbgrid.ui_state.${local ? 'local.' : ''}${hubKey()}`;

export function readBrowser(local = false): StoredLayout | null {
  try {
    const raw = localStorage.getItem(browserKey(local));
    if (!raw) return null;
    const v = JSON.parse(raw) as { doc?: unknown; rev?: unknown };
    return { doc: v.doc ?? null, rev: typeof v.rev === 'number' ? v.rev : 0 };
  } catch { return null; }
}

export function writeBrowser(doc: LayoutDoc | null, rev: number, local = false): void {
  try {
    if (doc == null) localStorage.removeItem(browserKey(local));
    else localStorage.setItem(browserKey(local), JSON.stringify({ doc, rev }));
  } catch { /* storage unavailable: the layout lasts for this tab */ }
}

export class BrowserLayoutStore implements LayoutStore {
  kind = 'browser' as const;
  constructor(private local = false) {}
  async load(): Promise<StoredLayout | null> { return readBrowser(this.local); }
  async save(doc: LayoutDoc, rev: number): Promise<SaveResult> { writeBrowser(doc, rev + 1, this.local); return { ok: true, rev: rev + 1 }; }
  async clear(): Promise<void> { writeBrowser(null, 0, this.local); }
}
