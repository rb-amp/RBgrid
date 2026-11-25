/**
 * useLayout — the dashboard layout in effect (SPEC §2.5–2.6): the saved document (sanitised) or,
 * until one is saved, the standard boards built from the site traits.
 *
 * Where it lives (pack H, fw 1.3.0 `/api/system/ui-state`, root ui-state-store):
 *  - a hub with the store: the hub; the browser keeps a copy, drawn at once while the hub answers;
 *    a save carries the revision it started from — 409 means another device saved first, and the
 *    user picks «keep mine» / «take the hub's»; a layout kept in the browser before the hub had the
 *    store goes to the hub with the first «Done»;
 *  - «My own view on this device» (a wall tablet): this browser only, the hub untouched;
 *  - a hub without the store, and the demo: this browser.
 */
import { useCallback, useEffect, useMemo, useState } from 'react';
import { defaultBoards, sanitizeLayout, LayoutDoc } from './layout';
import { SiteTraits } from './siteTraits';
import { knownWidget, widgetSizes } from './registry';
import { readBrowser, writeBrowser, hubKey } from './layoutStore';
import { getApiClient } from '../services/api';
import { isDemo } from '../services/connection';

export interface LayoutState {
  doc: LayoutDoc;
  /** The user has a saved layout (else the defaults are shown). */
  custom: boolean;
  /** The standard boards for this hub (what «Back to standard» gives). */
  defaults: LayoutDoc;
  /** Where a save goes. */
  where: 'hub' | 'browser' | 'device';
  /** The hub can hold the layout (offer «My own view on this device»). */
  hubCapable: boolean;
  localOnly: boolean;
  setLocalOnly(v: boolean): void;
  /** Another device saved first: the draft that was refused, and the hub's revision. */
  conflict: { draft: LayoutDoc; rev: number } | null;
  keepMine(): Promise<void>;
  takeHubs(): Promise<void>;
  save(doc: LayoutDoc): Promise<void>;
  reset(): Promise<void>;
  /** Pin / hide / order points without touching the boards (saved at once). */
  setPoints(p: Pick<LayoutDoc, 'pins' | 'hide' | 'order'>): Promise<void>;
}

const sanitize = (raw: unknown): LayoutDoc | null => sanitizeLayout(raw, knownWidget, widgetSizes);
const localFlagKey = () => `rbgrid.ui_state.local_on.${hubKey()}`;
const readLocalFlag = (): boolean => { try { return localStorage.getItem(localFlagKey()) === '1'; } catch { return false; } };

/** A save the hub refused because another device saved first. */
class ConflictError extends Error { constructor(public rev: number) { super('conflict'); } }

export function useLayout(traits: SiteTraits, hubStore = false): LayoutState {
  const hubCapable = hubStore && !isDemo();
  const [localOnly, setLocalOnlyState] = useState<boolean>(readLocalFlag);
  const local = hubCapable && localOnly;
  const where: LayoutState['where'] = local ? 'device' : hubCapable ? 'hub' : 'browser';
  const [saved, setSaved] = useState<{ doc: LayoutDoc | null; rev: number }>(() => {
    const s = readBrowser(local);
    return { doc: s ? sanitize(s.doc) : null, rev: s?.rev ?? 0 };
  });
  const [conflict, setConflict] = useState<{ draft: LayoutDoc; rev: number } | null>(null);

  // Re-read the browser copy when the store changes (the «this device» switch).
  useEffect(() => {
    const s = readBrowser(local);
    setSaved({ doc: s ? sanitize(s.doc) : null, rev: s?.rev ?? 0 });
  }, [local]);

  // The hub's layout wins over the browser copy and becomes the copy. Nothing on the hub (404):
  // the browser copy stays and goes to the hub with the first «Done» (from revision 0).
  const pull = useCallback(async () => {
    const r = await getApiClient().getUiState();
    if (!r) { setSaved((cur) => ({ ...cur, rev: 0 })); return; }
    const doc = sanitize(r.doc);
    setSaved({ doc, rev: r.rev });
    writeBrowser(doc, r.rev);
  }, []);
  useEffect(() => {
    if (where !== 'hub') return;
    pull().catch(() => undefined); // offline / no read token: the browser copy stays
  }, [where, pull]);

  const defaults = useMemo(() => defaultBoards(traits), [traits]);

  const write = useCallback(async (doc: LayoutDoc, rev: number) => {
    if (where !== 'hub') {
      writeBrowser(doc, rev + 1, local);
      setSaved({ doc, rev: rev + 1 });
      return;
    }
    const r = await getApiClient().putUiState(doc, rev);
    if (!r.ok) throw new ConflictError(r.rev);
    writeBrowser(doc, r.rev);
    setSaved({ doc, rev: r.rev });
  }, [where, local]);

  const attempt = useCallback(async (doc: LayoutDoc, rev: number) => {
    try {
      await write(doc, rev);
    } catch (e) {
      if (e instanceof ConflictError) { setConflict({ draft: doc, rev: e.rev }); return; }
      throw e;
    }
  }, [write]);

  const save = useCallback((doc: LayoutDoc) => attempt(doc, saved.rev), [attempt, saved.rev]);

  const keepMine = useCallback(async () => {
    if (!conflict) return;
    const c = conflict;
    setConflict(null);
    await attempt(c.draft, c.rev);
  }, [conflict, attempt]);

  const takeHubs = useCallback(async () => {
    setConflict(null);
    await pull().catch(() => undefined);
  }, [pull]);

  const reset = useCallback(async () => {
    if (where === 'hub') await getApiClient().deleteUiState();
    writeBrowser(null, 0, local);
    setSaved({ doc: null, rev: 0 });
  }, [where, local]);

  const setLocalOnly = useCallback((v: boolean) => {
    try { if (v) localStorage.setItem(localFlagKey(), '1'); else localStorage.removeItem(localFlagKey()); } catch { /* per tab */ }
    setLocalOnlyState(v);
  }, []);

  const custom = saved.doc != null && !saved.doc.std;
  const doc: LayoutDoc = saved.doc ? (saved.doc.std ? { ...saved.doc, boards: defaults.boards } : saved.doc) : defaults;
  const setPoints = useCallback(async (p: Pick<LayoutDoc, 'pins' | 'hide' | 'order'>) => {
    const next: LayoutDoc = custom ? { ...doc, ...p } : { v: 1, boards: defaults.boards, ...p, std: true };
    await save(next);
  }, [custom, doc, defaults.boards, save]);

  return { doc, custom, defaults, where, hubCapable, localOnly, setLocalOnly, conflict, keepMine, takeHubs, save, reset, setPoints };
}
