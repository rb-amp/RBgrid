/**
 * PeaksCard — import peaks at the grid input per billing cycle (GET/POST /api/v1/peaks, fw 1.2.0):
 * the highest 15-min and hour averages, the three highest days, this hour so far, the previous cycle.
 * The base of capacity tariffs; metering only — it changes no money.
 */
import React, { useState } from 'react';
import { Alert, Box, Button, MenuItem, Stack, TextField, Typography } from '@mui/material';
import { useConfirm } from '../common/ConfirmDialog';
import { DeviceClock, formatDeviceTime } from '../../core/deviceTime';
import { PeaksCycle, PeaksDoc } from '../../core/dynprice/types';
import { t, intlLocale } from '../../i18n';

const kw = (w: number) => `${(w / 1000).toLocaleString(intlLocale(), { maximumFractionDigits: 2, minimumFractionDigits: 2 })} ${t('units.kW')}`;

export const PeaksCard: React.FC<{
  doc: PeaksDoc;
  clock: DeviceClock;
  canWrite: boolean;
  onSet: (d: { cycle_day: number } | { reset: true }) => Promise<void>;
}> = ({ doc, clock, canWrite, onSet }) => {
  const confirm = useConfirm();
  const [err, setErr] = useState<string | null>(null);
  const at = (ts: number) => formatDeviceTime(clock, ts, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' }, intlLocale());
  const day = (ts: number) => formatDeviceTime(clock, ts, { day: 'numeric', month: 'short', year: 'numeric' }, intlLocale());
  const act = async (d: { cycle_day: number } | { reset: true }) => {
    setErr(null);
    try { await onSet(d); } catch (e) { setErr((e as Error).message); }
  };

  const cycle = (c: PeaksCycle) => (
    <Box sx={{ display: 'grid', gap: 2, gridTemplateColumns: { xs: 'minmax(0, 1fr)', sm: 'repeat(3, minmax(0, 1fr))' } }}>
      <Fig label={t('peaks.max15')} value={c.max_15min_w > 0 ? kw(c.max_15min_w) : '—'} sub={c.max_15min_w > 0 && c.max_15min_end ? t('peaks.endingAt', { at: at(c.max_15min_end) }) : undefined} />
      <Fig label={t('peaks.maxHour')} value={c.max_hour_w > 0 ? kw(c.max_hour_w) : '—'} sub={c.max_hour_w > 0 && c.max_hour_start ? t('peaks.from', { at: at(c.max_hour_start) }) : undefined} />
      <Fig label={t('peaks.top3')} value={c.top3_days_mean_w > 0 ? kw(c.top3_days_mean_w) : '—'}
        sub={c.top_days.length ? c.top_days.map((d) => `${kw(d.w)} · ${at(d.hour_start)}`).join('; ') : undefined} />
    </Box>
  );

  return (
    <Box>
      <Typography variant="caption" color="text.secondary" component="div" sx={{ mb: 1.5 }}>{t('peaks.hint')}</Typography>
      <Typography variant="body2" sx={{ mb: 1 }}>{doc.cycle_start ? t('peaks.cycleSince', { date: day(doc.cycle_start) }) : t('peaks.cycleNotStarted')}</Typography>
      {cycle(doc)}
      {doc.hour_wh_so_far != null && doc.hour_start ? (
        <Typography variant="body2" color="text.secondary" sx={{ mt: 1.5 }}>
          {t('peaks.thisHour', { at: at(doc.hour_start), kwh: (doc.hour_wh_so_far / 1000).toLocaleString(intlLocale(), { maximumFractionDigits: 3 }) })}
        </Typography>
      ) : null}
      {doc.prev && doc.prev.cycle_start > 0 && (
        <Box sx={{ mt: 2 }}>
          <Typography variant="subtitle2" fontWeight={700} sx={{ mb: 1 }}>{t('peaks.prev', { date: day(doc.prev.cycle_start) })}</Typography>
          {cycle(doc.prev)}
        </Box>
      )}
      <Stack direction="row" spacing={1} alignItems="center" sx={{ mt: 2 }} flexWrap="wrap" useFlexGap>
        <TextField select size="small" label={t('peaks.cycleDay')} value={doc.cycle_day} disabled={!canWrite} sx={{ minWidth: 200 }}
          onChange={async (e) => {
            const n = Number(e.target.value);
            // A new start day opens a new cycle at once; the current figures move to «previous» (root dynprice-writes).
            if (await confirm({ title: t('peaks.cycleDayTitle', { n }), body: t('peaks.cycleDayBody'), confirmLabel: t('peaks.cycleDayConfirm') })) act({ cycle_day: n });
          }}>
          {Array.from({ length: 28 }, (_, i) => i + 1).map((d) => <MenuItem key={d} value={d}>{t('peaks.dayN', { n: d })}</MenuItem>)}
        </TextField>
        <Button size="small" color="error" disabled={!canWrite}
          onClick={async () => { if (await confirm({ title: t('peaks.resetTitle'), body: t('peaks.resetBody'), confirmLabel: t('peaks.reset'), danger: true })) act({ reset: true }); }}>
          {t('peaks.reset')}
        </Button>
      </Stack>
      {err && <Alert severity="error" sx={{ mt: 1 }}>{err}</Alert>}
    </Box>
  );
};

const Fig: React.FC<{ label: string; value: string; sub?: string }> = ({ label, value, sub }) => (
  <Box>
    <Typography variant="caption" color="text.secondary" component="div">{label}</Typography>
    <Typography variant="h6" fontWeight={700} sx={{ fontVariantNumeric: 'tabular-nums' }}>{value}</Typography>
    {sub && <Typography variant="caption" color="text.secondary" sx={{ overflowWrap: 'anywhere' }}>{sub}</Typography>}
  </Box>
);
