/**
 * PriceRulesPanel — cheap windows (GET/POST /api/v1/rules, fw 1.2.0): up to 8 rules the hub
 * evaluates on the all-in buy price of each 15-min period, with their live state, plus the two
 * notification flags. The hub computes and notifies; it switches nothing. Saving replaces the list.
 */
import React, { useEffect, useRef, useState } from 'react';
import {
  Alert, Box, Button, Dialog, DialogActions, DialogContent, DialogTitle, FormControlLabel, Grid, IconButton, MenuItem, Stack,
  Switch, TextField, Typography,
} from '@mui/material';
import AddIcon from '@mui/icons-material/Add';
import EditOutlinedIcon from '@mui/icons-material/EditOutlined';
import DeleteOutlineIcon from '@mui/icons-material/DeleteOutline';
import { StatusChip } from '@rbgrid/design-system';
import { DecimalField } from '../common/DecimalField';
import { utf8Len } from '../common/utf8';
import { useConfirm } from '../common/ConfirmDialog';
import { DeviceClock, formatDeviceTime } from '../../core/deviceTime';
import { MAX_RULES, PriceRule, RULE_NAME_MAX, RuleKind, RulesDoc, SignalFlags } from '../../core/dynprice/types';
import { DynPriceError } from '../../core/dynprice/api';
import { t, intlLocale } from '../../i18n';

/** Why a rule can't be saved, or null. */
export function ruleProblem(r: PriceRule): string | null {
  if (!r.name.trim()) return t('rules.errName');
  // The hub keeps the name in 24 bytes (23 + NUL) and cuts it silently: count UTF-8 bytes, not letters.
  if (utf8Len(r.name) > RULE_NAME_MAX) return t('rules.errNameLong', { n: RULE_NAME_MAX });
  if (r.kind === 'threshold') {
    if (r.on_below == null || r.off_above == null) return t('rules.errThreshold');
    if (r.off_above < r.on_below) return t('rules.errHysteresis');
    return null;
  }
  const p = r.periods ?? 0;
  if (!Number.isInteger(p) || p < 1 || p > 96) return t('rules.errPeriods');
  return null;
}

export const nextRuleId = (rules: PriceRule[]): number => {
  for (let i = 1; i <= 255; i++) if (!rules.some((r) => r.id === i)) return i;
  return 255;
};

const blank = (kind: RuleKind, id: number): PriceRule => kind === 'threshold'
  ? { id, name: '', kind, enabled: true, on_below: 0, off_above: 0 }
  : kind === 'cheapest' ? { id, name: '', kind, enabled: true, from_h: 0, to_h: 24, periods: 8, consecutive: false }
    : { id, name: '', kind, enabled: true, deadline_h: 7, periods: 12, consecutive: false };

const hours = (max: number) => Array.from({ length: max + 1 }, (_, h) => h);
const hh = (h: number) => `${String(h).padStart(2, '0')}:00`;

export const PriceRulesPanel: React.FC<{
  doc: RulesDoc;
  clock: DeviceClock;
  fmtRate: (v: number) => string;
  canWrite: boolean;
  onSaveRules: (rules: PriceRule[]) => Promise<void>;
  onSaveSignal: (s: SignalFlags) => Promise<void>;
}> = ({ doc, clock, fmtRate, canWrite, onSaveRules, onSaveSignal }) => {
  const confirm = useConfirm();
  const [edit, setEdit] = useState<PriceRule | null>(null);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error'; text: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const at = (ts: number) => formatDeviceTime(clock, ts, { weekday: 'short', hour: '2-digit', minute: '2-digit' }, intlLocale());

  const run = async (fn: () => Promise<void>, ok: string) => {
    setBusy(true); setMsg(null);
    try { await fn(); setMsg({ sev: 'success', text: ok }); } catch (e) {
      const err = e instanceof DynPriceError ? e : null;
      setMsg({ sev: 'error', text: err?.status === 401 || err?.status === 403 ? t('common.adminTokenRequired') : t('rules.saveFailed', { why: err?.field ? `${err.field}: ${err.message}` : (e as Error).message }) });
    } finally { setBusy(false); }
  };
  const saveList = (rules: PriceRule[]) => run(() => onSaveRules(rules), t('rules.saved'));

  const describe = (r: PriceRule): string => {
    if (r.kind === 'threshold') return t('rules.descThreshold', { on: fmtRate(r.on_below ?? 0), off: fmtRate(r.off_above ?? 0) });
    const block = r.consecutive ? t('rules.inARow') : '';
    if (r.kind === 'cheapest') {
      const all = r.from_h === r.to_h || (r.from_h === 0 && r.to_h === 24);
      return t('rules.descCheapest', { n: r.periods ?? 0, h: ((r.periods ?? 0) / 4).toLocaleString(intlLocale()), block, window: all ? t('rules.wholeDay') : `${hh(r.from_h ?? 0)}–${hh(r.to_h ?? 24)}` });
    }
    return t('rules.descDeadline', { n: r.periods ?? 0, h: ((r.periods ?? 0) / 4).toLocaleString(intlLocale()), block, at: hh(r.deadline_h ?? 7) });
  };
  const stateOf = (r: PriceRule): { label: string; on: boolean } | null => {
    if (!r.enabled) return { label: t('rules.off'), on: false };
    const s = r.state;
    if (!s) return null;
    if (s.active) return { label: s.active_until ? t('rules.activeUntil', { at: at(s.active_until) }) : t('rules.active'), on: true };
    if (s.next_start) return { label: t('rules.nextAt', { at: at(s.next_start) }), on: false };
    // A threshold has no schedule; a window whose periods are behind it waits for the next window.
    if (r.kind === 'threshold') return { label: t('rules.waitingPrice'), on: false };
    return { label: s.window_until ? t('rules.noneBefore', { at: at(s.window_until) }) : t('rules.noNext'), on: false };
  };

  return (
    <Box>
      <Typography variant="caption" color="text.secondary" component="div" sx={{ mb: 1.5 }}>{t('rules.hint')}</Typography>
      {doc.rules.length === 0 && <Typography color="text.secondary" sx={{ mb: 1 }}>{t('rules.none')}</Typography>}
      <Stack spacing={1}>
        {doc.rules.map((r) => {
          const st = stateOf(r);
          return (
            <Stack key={r.id} direction="row" alignItems="center" spacing={1} sx={{ p: 1.25, border: 1, borderColor: 'divider', borderRadius: 2 }}>
              <Box sx={{ flex: 1, minWidth: 0 }}>
                <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap>
                  <Typography fontWeight={700}>{r.name}</Typography>
                  {st && <StatusChip status={st.on ? 'live' : 'dormant'} label={st.label} />}
                </Stack>
                <Typography variant="body2" color="text.secondary">{describe(r)}</Typography>
              </Box>
              <Switch checked={r.enabled} disabled={!canWrite || busy} inputProps={{ 'aria-label': t('rules.enabledAria', { name: r.name }) }}
                onChange={(e) => saveList(doc.rules.map((x) => (x.id === r.id ? { ...x, enabled: e.target.checked } : x)))} />
              <IconButton size="small" aria-label={t('rules.editAria', { name: r.name })} disabled={!canWrite || busy} onClick={() => setEdit(r)}><EditOutlinedIcon fontSize="small" /></IconButton>
              <IconButton size="small" aria-label={t('rules.deleteAria', { name: r.name })} disabled={!canWrite || busy}
                onClick={async () => { if (await confirm({ title: t('rules.deleteTitle', { name: r.name }), body: t('rules.deleteBody'), confirmLabel: t('common.delete'), danger: true })) saveList(doc.rules.filter((x) => x.id !== r.id)); }}>
                <DeleteOutlineIcon fontSize="small" />
              </IconButton>
            </Stack>
          );
        })}
      </Stack>
      <Button size="small" startIcon={<AddIcon />} sx={{ mt: 1 }} disabled={!canWrite || doc.rules.length >= MAX_RULES} onClick={() => setEdit(blank('cheapest', nextRuleId(doc.rules)))}>
        {doc.rules.length >= MAX_RULES ? t('rules.max', { n: MAX_RULES }) : t('rules.add')}
      </Button>
      {msg && <Alert severity={msg.sev} sx={{ mt: 1 }}>{msg.text}</Alert>}

      <SignalForm signal={doc.signal} canWrite={canWrite} busy={busy} onSave={(s) => run(() => onSaveSignal(s), t('rules.signalSaved'))} />

      {edit && (
        <RuleDialog rule={edit} onClose={() => setEdit(null)}
          onSave={(r) => { setEdit(null); saveList(doc.rules.some((x) => x.id === r.id) ? doc.rules.map((x) => (x.id === r.id ? r : x)) : [...doc.rules, r]); }} />
      )}
    </Box>
  );
};

const RuleDialog: React.FC<{ rule: PriceRule; onClose: () => void; onSave: (r: PriceRule) => void }> = ({ rule, onClose, onSave }) => {
  const [r, setR] = useState<PriceRule>(rule);
  const [fieldErr, setFieldErr] = useState<Record<string, string | null>>({});
  useEffect(() => { setR(rule); }, [rule]);
  const set = (p: Partial<PriceRule>) => setR((x) => ({ ...x, ...p }));
  const problem = ruleProblem(r) ?? (Object.values(fieldErr).find(Boolean) ?? null);
  const numF = (k: keyof PriceRule) => (v: number | null, e: string | null) => { setFieldErr((x) => ({ ...x, [k]: e })); if (v !== null) set({ [k]: v } as Partial<PriceRule>); };
  return (
    <Dialog open onClose={onClose} fullWidth maxWidth="sm">
      <DialogTitle>{rule.name ? t('rules.editTitle', { name: rule.name }) : t('rules.newTitle')}</DialogTitle>
      <DialogContent>
        <Grid container spacing={2} sx={{ pt: 1 }}>
          <Grid item xs={12} sm={7}>
            <TextField fullWidth size="small" label={t('rules.name')} value={r.name} onChange={(e) => set({ name: e.target.value })} />
          </Grid>
          <Grid item xs={12} sm={5}>
            <TextField select fullWidth size="small" label={t('rules.kind')} value={r.kind}
              onChange={(e) => { setFieldErr({}); setR({ ...blank(e.target.value as RuleKind, r.id), name: r.name, enabled: r.enabled }); }}>
              {(['cheapest', 'deadline', 'threshold'] as const).map((k) => <MenuItem key={k} value={k}>{t(`rules.kind_${k}`)}</MenuItem>)}
            </TextField>
          </Grid>
          <Grid item xs={12}><Typography variant="caption" color="text.secondary">{t(`rules.kindHint_${r.kind}`)}</Typography></Grid>
          {r.kind === 'threshold' && (
            <>
              <Grid item xs={6}><DecimalField label={t('rules.onBelow')} value={r.on_below} min={-100} max={100} decimals={5} onChange={numF('on_below')} /></Grid>
              <Grid item xs={6}><DecimalField label={t('rules.offAbove')} value={r.off_above} min={-100} max={100} decimals={5} helper={t('rules.offAboveHint')} onChange={numF('off_above')} /></Grid>
            </>
          )}
          {r.kind === 'cheapest' && (
            <>
              <Grid item xs={6}>
                <TextField select fullWidth size="small" label={t('rules.fromH')} value={r.from_h ?? 0} onChange={(e) => set({ from_h: Number(e.target.value) })}>
                  {hours(23).map((h) => <MenuItem key={h} value={h}>{hh(h)}</MenuItem>)}
                </TextField>
              </Grid>
              <Grid item xs={6}>
                <TextField select fullWidth size="small" label={t('rules.toH')} value={r.to_h ?? 24} helperText={t('rules.toHHint')} onChange={(e) => set({ to_h: Number(e.target.value) })}>
                  {hours(24).map((h) => <MenuItem key={h} value={h}>{hh(h)}</MenuItem>)}
                </TextField>
              </Grid>
            </>
          )}
          {r.kind === 'deadline' && (
            <Grid item xs={6}>
              <TextField select fullWidth size="small" label={t('rules.deadlineH')} value={r.deadline_h ?? 7} onChange={(e) => set({ deadline_h: Number(e.target.value) })}>
                {hours(23).map((h) => <MenuItem key={h} value={h}>{hh(h)}</MenuItem>)}
              </TextField>
            </Grid>
          )}
          {r.kind !== 'threshold' && (
            <>
              <Grid item xs={6}>
                <DecimalField label={t('rules.periods')} value={r.periods} min={1} max={96} decimals={0}
                  helper={t('rules.periodsHint', { h: ((r.periods ?? 0) / 4).toLocaleString(intlLocale()) })} onChange={numF('periods')} />
              </Grid>
              <Grid item xs={12}>
                <FormControlLabel label={t('rules.consecutive')} control={<Switch checked={!!r.consecutive} onChange={(e) => set({ consecutive: e.target.checked })} />} />
                <Typography variant="caption" color="text.secondary" component="div">{t('rules.consecutiveHint')}</Typography>
              </Grid>
            </>
          )}
        </Grid>
        {problem && <Alert severity="warning" sx={{ mt: 2 }}>{problem}</Alert>}
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose}>{t('common.cancel')}</Button>
        <Button variant="contained" disabled={!!problem} onClick={() => onSave({ ...r, name: r.name.trim() })}>{t('common.save')}</Button>
      </DialogActions>
    </Dialog>
  );
};

const SignalForm: React.FC<{ signal: SignalFlags; canWrite: boolean; busy: boolean; onSave: (s: SignalFlags) => void }> = ({ signal, canWrite, busy, onSave }) => {
  const [s, setS] = useState<SignalFlags>(signal);
  const [errs, setErrs] = useState<Partial<Record<keyof SignalFlags, string | null>>>({});
  const dirty = s.buy_cheap_below !== signal.buy_cheap_below || s.sell_low_below !== signal.sell_low_below;
  // The page re-reads the rules every 30 s: re-seed only when the hub's VALUES change, never over an edit.
  const dirtyRef = useRef(false);
  dirtyRef.current = dirty;
  useEffect(() => { if (!dirtyRef.current) { setS(signal); setErrs({}); } }, [signal.buy_cheap_below, signal.sell_low_below]); // eslint-disable-line react-hooks/exhaustive-deps
  const err = (Object.keys(errs) as (keyof SignalFlags)[]).some((k) => errs[k] && s[k] !== null);
  const flag = (k: keyof SignalFlags, label: string, hint: string) => (
    <Grid item xs={12} sm={6}>
      <FormControlLabel disabled={!canWrite} label={label} control={<Switch checked={s[k] !== null} onChange={(e) => { setErrs((x) => ({ ...x, [k]: null })); setS((x) => ({ ...x, [k]: e.target.checked ? 0 : null })); }} />} />
      {s[k] !== null
        ? <DecimalField label={t('rules.below')} value={s[k]} min={-100} max={100} decimals={5} disabled={!canWrite} helper={hint} onChange={(v, e) => { setErrs((x) => ({ ...x, [k]: e })); if (v !== null) setS((x) => ({ ...x, [k]: v })); }} />
        : <Typography variant="caption" color="text.secondary" component="div">{hint}</Typography>}
    </Grid>
  );
  return (
    <Box sx={{ mt: 3 }}>
      <Typography variant="subtitle2" fontWeight={700}>{t('rules.signalTitle')}</Typography>
      <Typography variant="caption" color="text.secondary" component="div" sx={{ mb: 1 }}>{t('rules.signalHint')}</Typography>
      <Grid container spacing={2} sx={{ maxWidth: 760 }}>
        {flag('buy_cheap_below', t('rules.buyCheap'), t('rules.buyCheapHint'))}
        {flag('sell_low_below', t('rules.sellLow'), t('rules.sellLowHint'))}
      </Grid>
      <Button size="small" variant="outlined" sx={{ mt: 1 }} disabled={!canWrite || !dirty || busy || err} onClick={() => onSave(s)}>{t('common.save')}</Button>
    </Box>
  );
};
