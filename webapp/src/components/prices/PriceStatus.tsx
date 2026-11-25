/**
 * PriceStatus — the price in force now and where it comes from (dynprice status + the hub's
 * price notification, fw 1.2.0): state in plain words, the provider in use (backup said so),
 * how long the stored prices cover, the last fetch error, this period's buy / sell with its rank
 * in the day, the two flags. Plus every shown provider's attribution, verbatim (licence).
 */
import React from 'react';
import { Alert, Box, Link, Stack, Typography } from '@mui/material';
import { StatusChip } from '@rbgrid/design-system';
import { DeviceClock, formatDeviceTime } from '../../core/deviceTime';
import { DynPriceDoc, PriceProvider, PriceSignal, PriceState } from '../../core/dynprice/types';
import { t, intlLocale } from '../../i18n';

export function priceStateSev(s: PriceState | string): 'success' | 'info' | 'warning' | 'error' {
  switch (s) {
    case 'ok': return 'success';
    case 'backup': case 'today_only': return 'warning';
    case 'stale': case 'plan_fallback': return 'error';
    default: return 'info';
  }
}

/** The providers whose prices are on screen: the primary and the backup (a local one too, it has its own name). */
export function shownProviders(doc: DynPriceDoc): PriceProvider[] {
  return [doc.settings.source, doc.settings.backup_source].map((id) => doc.providers.find((p) => p.id === id)).filter((p): p is PriceProvider => !!p);
}

export const Attribution: React.FC<{ providers: PriceProvider[] }> = ({ providers }) => {
  const list = providers.filter((p) => p.attribution);
  if (!list.length) return null;
  return (
    <Stack spacing={0.25} sx={{ mt: 1 }}>
      {list.map((p) => (
        <Typography key={p.id} variant="caption" color="text.secondary">
          {p.terms_url ? <Link href={p.terms_url} target="_blank" rel="noopener noreferrer" color="inherit">{p.attribution}</Link> : p.attribution}
        </Typography>
      ))}
    </Stack>
  );
};

export const PriceStatus: React.FC<{
  doc: DynPriceDoc;
  signal: PriceSignal | null;
  clock: DeviceClock;
  fmtRate: (v: number) => string;
}> = ({ doc, signal, clock, fmtRate }) => {
  const st = doc.status;
  const at = (ts: number) => formatDeviceTime(clock, ts, { weekday: 'short', day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' }, intlLocale());
  const hm = (ts: number) => formatDeviceTime(clock, ts, { hour: '2-digit', minute: '2-digit' }, intlLocale());
  const name = (id: string) => (id === 'plan' ? t('prices.planName') : doc.providers.find((p) => p.id === id)?.name ?? id);
  const sev = priceStateSev(st.price_state);
  const chip = sev === 'success' ? 'live' : sev === 'error' ? 'error' : 'present';
  return (
    <Box>
      <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap>
        <StatusChip status={chip} label={t(`prices.state.${st.price_state}`)} />
        {st.price_state !== 'disabled' && (
          <Typography variant="body2" color="text.secondary">
            {t('prices.sourceNow', { name: name(st.source_now) })}{st.backup_now ? ` · ${t('prices.backupInUse')}` : ''}
          </Typography>
        )}
      </Stack>
      {sev !== 'success' && st.price_state !== 'disabled' && <Alert severity={sev} sx={{ mt: 1 }}>{t(`prices.stateHint.${st.price_state}`)}</Alert>}

      {signal && st.price_state !== 'disabled' && (
        <Box sx={{ display: 'grid', gap: 2, mt: 2, gridTemplateColumns: { xs: 'repeat(2, minmax(0, 1fr))', md: 'repeat(4, minmax(0, 1fr))' } }}>
          <Figure label={t('prices.buyNow', { from: hm(signal.period_from), until: hm(signal.period_until) })} value={fmtRate(signal.buy)}
            sub={signal.rank_buy ? t('prices.rank', { n: signal.rank_buy, of: signal.periods_today }) : undefined} />
          {signal.sell != null && <Figure label={t('prices.sellNow')} value={fmtRate(signal.sell)} />}
          <Figure label={t('prices.validUntil')} value={st.valid_until ? at(st.valid_until) : '—'} />
          <Box>
            {signal.buy_cheap && <StatusChip status="live" label={t('prices.flagCheap')} />}
            {signal.sell_low && <Box sx={{ mt: 0.5 }}><StatusChip status="present" label={t('prices.flagSellLow')} /></Box>}
          </Box>
        </Box>
      )}

      <Stack spacing={0.25} sx={{ mt: 1.5 }}>
        {st.last_fetch > 0 && <Typography variant="caption" color="text.secondary">{t('prices.lastFetch', { at: at(st.last_fetch) })}</Typography>}
        {st.next_fetch > 0 && <Typography variant="caption" color="text.secondary">{t('prices.nextFetch', { at: at(st.next_fetch) })}</Typography>}
        {st.last_error && st.last_error !== 'none' && (
          <Typography variant="caption" color="error.main">{t('prices.lastError', { why: t(`prices.err.${st.last_error}`) === `prices.err.${st.last_error}` ? st.last_error : t(`prices.err.${st.last_error}`) })}</Typography>
        )}
      </Stack>
      <Attribution providers={shownProviders(doc)} />
    </Box>
  );
};

const Figure: React.FC<{ label: string; value: string; sub?: string }> = ({ label, value, sub }) => (
  <Box>
    <Typography variant="caption" color="text.secondary" component="div">{label}</Typography>
    <Typography variant="h6" fontWeight={700} sx={{ fontVariantNumeric: 'tabular-nums' }}>{value}</Typography>
    {sub && <Typography variant="caption" color="text.secondary">{sub}</Typography>}
  </Box>
);
