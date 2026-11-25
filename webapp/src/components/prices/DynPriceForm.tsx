/**
 * DynPriceForm — where the hub takes its price from and how it books it (POST /api/v1/dynprice,
 * fw 1.2.0): primary + optional backup provider from the hub's own catalogue (only `available`
 * ones), the zone (its currency must be the plan's), the contract formula for spot prices, floors,
 * the export price. Only changed fields are sent; a 400 {field} is said next to that field's name.
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Alert, Box, Button, FormControlLabel, Grid, MenuItem, Stack, Switch, TextField, Typography } from '@mui/material';
import { DecimalField } from '../common/DecimalField';
import { DynPriceDoc, DynPriceSettings, PriceProvider } from '../../core/dynprice/types';
import { DynPriceError } from '../../core/dynprice/api';
import { t } from '../../i18n';

type Draft = DynPriceSettings & { token: string; backup_token: string };

const FIELDS: (keyof DynPriceSettings)[] = ['enabled', 'source', 'backup_source', 'zone', 'ha_class', 'factor', 'markup', 'zones_stack',
  'hourly_mean', 'floor_buy', 'floor_sell', 'sell_mode', 'k_sell', 'feedin_fee', 'product', 'export_product'];

/** The fields that differ from what the hub has (a token only when typed). */
export function dynPricePatch(cur: DynPriceSettings, d: Draft): Partial<DynPriceSettings> & { token?: string; backup_token?: string } {
  const out: Record<string, unknown> = {};
  FIELDS.forEach((k) => { if (d[k] !== cur[k] && !(d[k] === undefined && cur[k] === undefined)) out[k] = d[k]; });
  if (d.token) out.token = d.token;
  if (d.backup_token) out.backup_token = d.backup_token;
  return out as Partial<DynPriceSettings> & { token?: string; backup_token?: string };
}

/** The zones offered: those of the network provider(s) chosen (the zone is shared by primary and backup). */
export function zonesFor(providers: PriceProvider[], source: string, backup: string): PriceProvider['zones'] {
  const net = [source, backup].map((id) => providers.find((p) => p.id === id)).filter((p): p is PriceProvider => !!p && !p.local);
  if (!net.length) return [];
  // Two network providers: only the zones both serve.
  return net[0].zones.filter((z) => net.every((p) => p.zones.some((x) => x.id === z.id)));
}

export const DynPriceForm: React.FC<{
  doc: DynPriceDoc;
  onSave: (patch: Partial<DynPriceSettings> & { token?: string; backup_token?: string }) => Promise<void>;
  canWrite: boolean;
}> = ({ doc, onSave, canWrite }) => {
  // The page re-reads the doc whenever prices move (price_gen); only a change of the SETTINGS
  // re-seeds the draft, and never over the user's edits.
  const settingsKey = JSON.stringify(doc.settings);
  const initial = useMemo<Draft>(() => ({ ...doc.settings, token: '', backup_token: '' }), [settingsKey]); // eslint-disable-line react-hooks/exhaustive-deps
  const [d, setD] = useState<Draft>(initial);
  const [errs, setErrs] = useState<Record<string, string | null>>({});
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error'; text: string } | null>(null);
  const dirtyRef = useRef(false);
  useEffect(() => { if (!dirtyRef.current) { setD(initial); setErrs({}); } }, [initial]);
  const set = (p: Partial<Draft>) => { setD((x) => ({ ...x, ...p })); setMsg(null); };
  const num = (k: string) => (v: number | null, e: string | null) => { setErrs((x) => ({ ...x, [k]: e })); if (v !== null) set({ [k]: v } as Partial<Draft>); };

  const available = doc.providers.filter((p) => p.available || p.id === doc.settings.source || p.id === doc.settings.backup_source);
  const prov = (id: string) => doc.providers.find((p) => p.id === id);
  const src = prov(d.source), bak = d.backup_source !== 'none' ? prov(d.backup_source) : undefined;
  const zones = zonesFor(doc.providers, d.source, d.backup_source);
  const planCcy = doc.status.currency;
  const zone = zones.find((z) => z.id === d.zone);
  const usesHa = d.source === 'ha' || d.backup_source === 'ha';
  // The contract formula applies to any spot-class provider in use — the backup too.
  const isSpot = (p?: PriceProvider) => !!p && p.class !== 'all_in' && !(p.local && d.ha_class === 'all_in');
  const spot = isSpot(src) || isSpot(bak);
  const octopus = d.source === 'octopus' || d.backup_source === 'octopus';
  const needsToken = !!src?.needs_token;
  const needsBackupToken = !!bak?.needs_token;
  const patch = dynPricePatch(doc.settings, d);
  const dirty = Object.keys(patch).length > 0;
  dirtyRef.current = dirty;
  // Only the fields on screen can block Save (a hidden field's stale error must not).
  const shown = new Set<string>(['floor_buy', 'floor_sell', ...(spot ? ['factor', 'markup'] : []), ...(d.sell_mode === 'formula' ? ['k_sell', 'feedin_fee'] : [])]);
  const hasErr = d.enabled && (Object.entries(errs).some(([k, e]) => !!e && shown.has(k)) || (zones.length > 0 && !zone) || d.backup_source === d.source);
  const label = (field: string) => t(`prices.f.${field}`) === `prices.f.${field}` ? field : t(`prices.f.${field}`);

  const save = async () => {
    setBusy(true); setMsg(null);
    try {
      await onSave(patch);
      setMsg({ sev: 'success', text: t('prices.saved') });
    } catch (e) {
      const err = e instanceof DynPriceError ? e : null;
      const text = err?.status === 401 || err?.status === 403 ? t('common.adminTokenRequired')
        : err?.field ? t('prices.rejectedField', { field: label(err.field), why: err.message }) : t('prices.saveFailed', { why: e instanceof Error ? e.message : String(e) });
      setMsg({ sev: 'error', text });
    } finally { setBusy(false); }
  };

  const provName = (p: PriceProvider) => `${p.name}${p.class === 'all_in' ? ` · ${t('prices.allIn')}` : ''}`;

  return (
    <Box>
      <FormControlLabel disabled={!canWrite} label={t('prices.enabled')}
        control={<Switch checked={d.enabled} onChange={(e) => set({ enabled: e.target.checked })} />} />
      <Typography variant="caption" color="text.secondary" component="div" sx={{ mb: 2 }}>
        {d.enabled ? t('prices.enabledHint') : t('prices.disabledHint')}
      </Typography>
      {d.enabled && (
        <Grid container spacing={2} sx={{ maxWidth: 760 }}>
          <Grid item xs={12} sm={6}>
            <TextField select fullWidth size="small" label={t('prices.f.source')} value={d.source} disabled={!canWrite}
              onChange={(e) => set({ source: e.target.value, ...(e.target.value === d.backup_source ? { backup_source: 'none' } : {}) })}>
              {available.map((p) => <MenuItem key={p.id} value={p.id}>{provName(p)}</MenuItem>)}
            </TextField>
          </Grid>
          <Grid item xs={12} sm={6}>
            <TextField select fullWidth size="small" label={t('prices.f.backup_source')} value={d.backup_source} disabled={!canWrite}
              helperText={t('prices.backupHint')} onChange={(e) => set({ backup_source: e.target.value })}>
              <MenuItem value="none">{t('prices.backupNone')}</MenuItem>
              {available.filter((p) => p.id !== d.source).map((p) => <MenuItem key={p.id} value={p.id}>{provName(p)}</MenuItem>)}
            </TextField>
          </Grid>
          {zones.length > 0 && (
            <Grid item xs={12} sm={6}>
              <TextField select fullWidth size="small" label={t('prices.f.zone')} value={zone ? d.zone : ''} disabled={!canWrite}
                SelectProps={{ displayEmpty: true }} InputLabelProps={{ shrink: true }}
                error={!!zone && !!planCcy && zone.currency !== planCcy}
                helperText={zone && planCcy && zone.currency !== planCcy ? t('prices.zoneCurrency', { zone: zone.currency, plan: planCcy }) : !zone ? t('prices.zonePick') : ' '}
                onChange={(e) => set({ zone: e.target.value })}>
                <MenuItem value="" disabled>{t('prices.zonePick')}</MenuItem>
                {zones.map((z) => <MenuItem key={z.id} value={z.id}>{`${z.name} · ${z.currency}`}</MenuItem>)}
              </TextField>
            </Grid>
          )}
          {usesHa && (
            <Grid item xs={12} sm={6}>
              <TextField select fullWidth size="small" label={t('prices.f.ha_class')} value={d.ha_class} disabled={!canWrite}
                helperText={t('prices.haClassHint')} onChange={(e) => set({ ha_class: e.target.value as Draft['ha_class'] })}>
                <MenuItem value="spot">{t('prices.class.spot')}</MenuItem>
                <MenuItem value="all_in">{t('prices.class.all_in')}</MenuItem>
              </TextField>
            </Grid>
          )}
          {needsToken && (
            <Grid item xs={12} sm={6}>
              <TextField fullWidth size="small" type="password" autoComplete="new-password" label={t('prices.f.token')} value={d.token} disabled={!canWrite}
                helperText={doc.status.token_set ? t('prices.tokenSet') : t('prices.tokenUnset')} onChange={(e) => set({ token: e.target.value })} />
            </Grid>
          )}
          {needsBackupToken && (
            <Grid item xs={12} sm={6}>
              <TextField fullWidth size="small" type="password" autoComplete="new-password" label={t('prices.f.backup_token')} value={d.backup_token} disabled={!canWrite}
                helperText={doc.status.backup_token_set ? t('prices.tokenSet') : t('prices.tokenUnset')} onChange={(e) => set({ backup_token: e.target.value })} />
            </Grid>
          )}
          {octopus && (
            <>
              <Grid item xs={12} sm={6}>
                <TextField fullWidth size="small" label={t('prices.f.product')} value={d.product ?? ''} disabled={!canWrite} onChange={(e) => set({ product: e.target.value.trim() })} />
              </Grid>
              <Grid item xs={12} sm={6}>
                <TextField fullWidth size="small" label={t('prices.f.export_product')} value={d.export_product ?? ''} disabled={!canWrite}
                  helperText={t('prices.exportProductHint')} onChange={(e) => set({ export_product: e.target.value.trim() })} />
              </Grid>
            </>
          )}

          <Grid item xs={12}><Typography variant="subtitle2" fontWeight={700}>{t('prices.formulaTitle')}</Typography>
            <Typography variant="caption" color="text.secondary">{spot ? t('prices.formulaSpot') : t('prices.formulaAllIn')}</Typography></Grid>
          {spot && (
            <>
              <Grid item xs={6} sm={3}><DecimalField label={t('prices.f.factor')} value={d.factor} min={0} max={10} decimals={4} disabled={!canWrite} onChange={num('factor')} /></Grid>
              <Grid item xs={6} sm={3}><DecimalField label={t('prices.f.markup')} value={d.markup} min={-100} max={100} decimals={5} disabled={!canWrite} helper={t('prices.perKwhHint')} onChange={num('markup')} /></Grid>
              <Grid item xs={12} sm={6}>
                <FormControlLabel disabled={!canWrite} label={t('prices.f.zones_stack')} control={<Switch checked={d.zones_stack} onChange={(e) => set({ zones_stack: e.target.checked })} />} />
                <Typography variant="caption" color="text.secondary" component="div">{t('prices.zonesStackHint')}</Typography>
              </Grid>
            </>
          )}
          <Grid item xs={12} sm={6}>
            <FormControlLabel disabled={!canWrite} label={t('prices.f.hourly_mean')} control={<Switch checked={d.hourly_mean} onChange={(e) => set({ hourly_mean: e.target.checked })} />} />
            <Typography variant="caption" color="text.secondary" component="div">{t('prices.hourlyMeanHint')}</Typography>
          </Grid>
          <Grid item xs={12} sm={6}><Floor label={t('prices.f.floor_buy')} value={d.floor_buy} disabled={!canWrite} onChange={(v, e) => { setErrs((x) => ({ ...x, floor_buy: e })); set({ floor_buy: v }); }} /></Grid>

          <Grid item xs={12}><Typography variant="subtitle2" fontWeight={700}>{t('prices.sellTitle')}</Typography></Grid>
          <Grid item xs={12} sm={6}>
            <TextField select fullWidth size="small" label={t('prices.f.sell_mode')} value={d.sell_mode} disabled={!canWrite}
              helperText={t(`prices.sellMode.${d.sell_mode}Hint`)} onChange={(e) => set({ sell_mode: e.target.value as Draft['sell_mode'] })}>
              {(['source', 'plan', 'formula'] as const).map((m) => <MenuItem key={m} value={m}>{t(`prices.sellMode.${m}`)}</MenuItem>)}
            </TextField>
          </Grid>
          {d.sell_mode === 'formula' && (
            <>
              <Grid item xs={6} sm={3}><DecimalField label={t('prices.f.k_sell')} value={d.k_sell} min={0} max={10} decimals={4} disabled={!canWrite} onChange={num('k_sell')} /></Grid>
              <Grid item xs={6} sm={3}><DecimalField label={t('prices.f.feedin_fee')} value={d.feedin_fee} min={-100} max={100} decimals={5} disabled={!canWrite} helper={t('prices.perKwhHint')} onChange={num('feedin_fee')} /></Grid>
            </>
          )}
          <Grid item xs={12} sm={6}><Floor label={t('prices.f.floor_sell')} value={d.floor_sell} disabled={!canWrite} onChange={(v, e) => { setErrs((x) => ({ ...x, floor_sell: e })); set({ floor_sell: v }); }} /></Grid>
        </Grid>
      )}
      {bak?.id === src?.id && bak && <Alert severity="error" sx={{ mt: 1 }}>{t('prices.backupSame')}</Alert>}
      <Stack direction="row" spacing={1} sx={{ mt: 2 }}>
        <Button size="small" variant="contained" disabled={!canWrite || !dirty || busy || hasErr} onClick={save}>{busy ? t('common.saving') : t('common.save')}</Button>
        <Button size="small" disabled={!dirty || busy} onClick={() => { dirtyRef.current = false; setD(initial); setErrs({}); setMsg(null); }}>{t('common.cancel')}</Button>
      </Stack>
      {msg && <Alert severity={msg.sev} sx={{ mt: 1.5 }}>{msg.text}</Alert>}
    </Box>
  );
};

/** A floor: none (negative prices pass through) or a value. */
const Floor: React.FC<{ label: string; value: number | null; disabled: boolean; onChange: (v: number | null, err: string | null) => void }> = ({ label, value, disabled, onChange }) => (
  <Box>
    <FormControlLabel disabled={disabled} label={label}
      control={<Switch checked={value !== null} onChange={(e) => onChange(e.target.checked ? 0 : null, null)} />} />
    {value !== null
      ? <DecimalField label={t('prices.floorValue')} value={value} min={-100} max={100} decimals={5} disabled={disabled} helper={t('prices.perKwhHint')} onChange={(v, e) => onChange(v ?? value, e)} />
      : <Typography variant="caption" color="text.secondary" component="div">{t('prices.floorNone')}</Typography>}
  </Box>
);
