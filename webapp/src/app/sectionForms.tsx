/**
 * Section forms (UI redesign 22.11, SPEC §5.3): one way to save a settings section. A panel with its
 * own draft (groups, time, browser access, MQTT broker) registers it here; the section's sticky bar
 * counts every draft and saves them all with one «Save» (and drops them with «Cancel»). Inside a
 * section the panel hides its own Save / Cancel; outside one (the setup wizard) it keeps them.
 * Actions with consequences (Wi-Fi, the hub's access point, the token, restore, restart, reset)
 * keep their own confirmed buttons and don't register.
 */
import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';

export interface SectionForm {
  /** Stable id of the panel. */
  id: string;
  /** Its name in the bar's messages («Groups»…). */
  label: string;
  dirty: boolean;
  /** A field is in error: the section can't be saved. */
  invalid: boolean;
  /** Write the draft; true when the hub took it (the panel shows the details either way). */
  save(): Promise<boolean>;
  /** Back to what the hub has. */
  reset(): void;
}

interface Registry { set(f: SectionForm): void; remove(id: string): void }

const RegCtx = createContext<Registry | null>(null);
const ListCtx = createContext<SectionForm[]>([]);

export const SectionFormsProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [forms, setForms] = useState<Record<string, SectionForm>>({});
  const reg = useMemo<Registry>(() => ({
    set: (f) => setForms((cur) => {
      const old = cur[f.id];
      if (old && old.dirty === f.dirty && old.invalid === f.invalid && old.label === f.label && old.save === f.save && old.reset === f.reset) return cur;
      return { ...cur, [f.id]: f };
    }),
    remove: (id) => setForms((cur) => { if (!(id in cur)) return cur; const next = { ...cur }; delete next[id]; return next; }),
  }), []);
  const list = useMemo(() => Object.values(forms), [forms]);
  return <RegCtx.Provider value={reg}><ListCtx.Provider value={list}>{children}</ListCtx.Provider></RegCtx.Provider>;
};

/** The registered forms (for the section bar). */
export const useSectionForms = (): SectionForm[] => useContext(ListCtx);

/**
 * Register a panel's draft with the section bar. Returns true inside a section (the panel then
 * hides its own Save / Cancel). `save` / `reset` may change every render — the latest is called.
 */
export function useSectionForm(f: Omit<SectionForm, 'save' | 'reset'> & { save: () => Promise<boolean>; reset: () => void }): boolean {
  const reg = useContext(RegCtx);
  const latest = useRef(f);
  latest.current = f;
  const save = useCallback(() => latest.current.save(), []);
  const reset = useCallback(() => latest.current.reset(), []);
  const { id, label, dirty, invalid } = f;
  useEffect(() => { reg?.set({ id, label, dirty, invalid, save, reset }); }, [reg, id, label, dirty, invalid, save, reset]);
  useEffect(() => () => reg?.remove(id), [reg, id]);
  return reg != null;
}
