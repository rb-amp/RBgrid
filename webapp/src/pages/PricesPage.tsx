/**
 * PricesPage — dynamic (spot) prices on the hub (fw 1.2.0, root dynprice-webapp; shown where the hub
 * reports `capabilities.dynamicTariffs`). The same information set the Home Assistant panel has:
 * the price in force and its source, today's and tomorrow's 15-min prices, where the hub takes them
 * from and how it books them, cheap windows and the notification flags, import peaks, and the
 * audit log of applied prices. The hub owns the loop; this page reads and configures it.
 * Refreshes when the telemetry's `price_gen` moves (new prices or state), rules every 30 s.
 */
import { PageSkeleton } from '../components/common/PageSkeleton';
import React, { useCallback, useEffect, useState } from 'react';
import { Alert, Box, FormControlLabel, Stack, Switch, Typography } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useCapabilities, useDeviceSession } from '../core';
import { useDeviceClock } from '../core/deviceTime';
import { dynpriceApi } from '../core/dynprice/api';
import { DynPriceDoc, PeaksDoc, PriceDoc, PriceSignal, RulesDoc } from '../core/dynprice/types';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { PriceChart } from '../components/prices/PriceChart';
import { PriceStatus, Attribution, shownProviders } from '../components/prices/PriceStatus';
import { DynPriceForm } from '../components/prices/DynPriceForm';
import { PriceRulesPanel } from '../components/prices/PriceRulesPanel';
import { PeaksCard } from '../components/prices/PeaksCard';
import { PriceLogCard } from '../components/prices/PriceLogCard';
import { t } from '../i18n';
import { money } from '../components/common/ruFormat';

const Section: React.FC<{ title: string; children: React.ReactNode; right?: React.ReactNode }> = ({ title, children, right }) => (
  <Box sx={{ mb: 2 }}>
    <Card>
      <Stack direction="row" alignItems="center" sx={{ mb: 1.5 }}>
        <Typography variant="h6" fontWeight={700}>{title}</Typography>
        {right && <Box sx={{ ml: 'auto' }}>{right}</Box>}
      </Stack>
      {children}
    </Card>
  </Box>
);

/** `embedded`: inside «Tariffs & prices» (its own title and tabs above). */
export const PricesPage: React.FC<{ embedded?: boolean }> = ({ embedded = false }) => {
  const session = useDeviceSession();
  const clock = useDeviceClock();
  const tel = useTelemetryEntities();
  const gen = tel.data?.system.totals?.tariff?.price_gen;
  const [doc, setDoc] = useState<DynPriceDoc | null>(null);
  const [price, setPrice] = useState<PriceDoc | null>(null);
  const [rules, setRules] = useState<RulesDoc | null>(null);
  const [signal, setSignal] = useState<PriceSignal | null>(null);
  const [peaks, setPeaks] = useState<PeaksDoc | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [showSell, setShowSell] = useState(true);
  const [now, setNow] = useState(() => Math.floor(Date.now() / 1000));
  const mf = useMoneyFormat(doc?.status.currency ?? null);
  // Opened by its address on a hub without dynamic prices (before 1.2.0): say so, ask nothing (it read «HTTP 405»).
  const caps = useCapabilities();
  const unsupported = caps.fromSchema && !caps.flags.dynamicTariffs;
  const direct = session.transport.kind === 'direct' && !unsupported;

  const dayFrom = clock.dayStart(now);
  const dayUntil = clock.dayStart(dayFrom + 50 * 3600); // the end of tomorrow (23/25-h days included)

  const loadPrices = useCallback(async () => {
    const [d, p, s] = await Promise.all([dynpriceApi.get(), dynpriceApi.price(dayFrom, dayUntil), dynpriceApi.signal().catch(() => null)]);
    setDoc(d); setPrice(p); setSignal(s); setErr(null);
  }, [dayFrom, dayUntil]);
  const loadRules = useCallback(async () => { setRules(await dynpriceApi.rules()); }, []);

  useEffect(() => {
    if (!direct) return;
    loadPrices().catch((e) => setErr((e as Error).message));
    loadRules().catch(() => undefined);
    dynpriceApi.peaks().then(setPeaks).catch(() => undefined);
  }, [direct, loadPrices, loadRules, gen]);

  // Rules state and «now» move with time, not with price_gen.
  useEffect(() => {
    if (!direct) return undefined;
    const id = setInterval(() => { setNow(Math.floor(Date.now() / 1000)); loadRules().catch(() => undefined); dynpriceApi.signal().then(setSignal).catch(() => undefined); }, 30000);
    return () => clearInterval(id);
  }, [direct, loadRules]);

  if (unsupported) return <Alert severity="info">{t('prices.notOnThisHub')}</Alert>;
  if (!direct) return <Alert severity="info">{t('prices.directOnly')}</Alert>;
  if (err && !doc) return <Alert severity="error">{t('prices.loadFailed', { why: err })}</Alert>;
  if (!doc) return <PageSkeleton />;

  const fmt = (v: number) => mf.rate(v);
  const anySell = (price?.resolved ?? []).some((p) => p.sell != null);

  return (
    <Box>
      {!embedded && <Typography variant="h4" fontWeight={800} sx={{ mb: 0.5 }}>{t('prices.title')}</Typography>}
      <Typography color="text.secondary" sx={{ mb: 2 }}>{t('prices.lead')}</Typography>
      {err && <Alert severity="warning" sx={{ mb: 2 }}>{t('prices.loadFailed', { why: err })}</Alert>}

      <Section title={t('prices.nowTitle')}>
        <PriceStatus doc={doc} signal={signal} clock={clock} fmtRate={fmt} />
      </Section>

      {doc.status.price_state !== 'disabled' && (
        <Section title={t('prices.chartTitle')}
          right={anySell ? <FormControlLabel label={t('prices.showSell')} control={<Switch size="small" checked={showSell} onChange={(e) => setShowSell(e.target.checked)} />} /> : undefined}>
          <PriceChart periods={price?.resolved ?? []} from={dayFrom} until={dayUntil} now={now} clock={clock} fmt={fmt} fmtAxis={(v, d) => money(v, mf.currencyCode, d)} primary={doc.settings.source} showSell={showSell && anySell} />
          {(price?.resolved.length ?? 0) > 0 && !(price?.resolved ?? []).some((p) => p.from >= clock.dayStart(dayFrom + 26 * 3600)) && (
            <Typography variant="caption" color="text.secondary" component="div" sx={{ mt: 1 }}>{t('prices.tomorrowLater')}</Typography>
          )}
          <Attribution providers={shownProviders(doc)} />
        </Section>
      )}

      <Section title={t('prices.settingsTitle')}>
        <DynPriceForm doc={doc} canWrite={direct} onSave={async (patch) => { setDoc(await dynpriceApi.set(patch)); loadPrices().catch((e) => setErr((e as Error).message)); }} />
      </Section>

      {rules && (
        <Section title={t('rules.title')}>
          <PriceRulesPanel doc={rules} clock={clock} fmtRate={fmt} canWrite={direct}
            onSaveRules={async (r) => { setRules(await dynpriceApi.setRules(r)); }}
            onSaveSignal={async (s) => { setRules(await dynpriceApi.setSignal(s)); }} />
        </Section>
      )}

      {peaks && (
        <Section title={t('peaks.title')}>
          <PeaksCard doc={peaks} clock={clock} canWrite={direct} onSet={async (d) => { setPeaks(await dynpriceApi.setPeaks(d)); }} />
        </Section>
      )}

      <Section title={t('priceLog.title')}>
        <PriceLogCard clock={clock} now={now} fmtRate={fmt} load={dynpriceApi.log} />
      </Section>
    </Box>
  );
};

export default PricesPage;
