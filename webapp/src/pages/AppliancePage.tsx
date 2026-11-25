/**
 * AppliancePage — one machine measured per phase, as one thing (role audit 22.11 H-02: a heat pump or
 * a three-phase charger opened only one phase at a time — a third of it). The appliance = a device
 * group (G5 `group_id`) whose points are the phases of one machine (overviewData.deviceSets).
 * Shows: the sum of its phases now (power and money per hour), today's energy and money, a trend of
 * the period (the shared period picker), and a row per phase leading to that point's page.
 */
import React, { useMemo } from 'react';
import { Box, Breadcrumbs, Button, Chip, Link as MuiLink, Stack, Typography } from '@mui/material';
import ArrowBackIcon from '@mui/icons-material/ArrowBack';
import { Link as RouterLink, useLocation, useNavigate, useParams } from 'react-router-dom';
import { Card, BlockHeader, StatTile } from '@rbgrid/design-system';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { useUnitsConfig } from '../hooks/useUnitsConfig';
import { useDeviceGroups } from '../hooks/useDeviceGroups';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { useHistory } from '../hooks/useHistory';
import { useDeviceClock, formatDeviceTime } from '../core/deviceTime';
import { useDsTokens } from '../theme/useDsTokens';
import { unitMetricValue, UnitEntityGroup } from '../entities/model';
import { fullPower, apiUnitId } from '../entities/siteModel';
import { ROLE_ICONS, roleIcon } from '../entities/roleVisual';
import { dayAvgRate } from '../economics/costRate';
import { lineUnits, deviceSets, fullNameOf } from './overviewData';
import { HIST_WINDOWS } from './historyWindow';
import { usePeriodTotals, isPastPeriod, pointCost, WIN_PERIOD } from '../hooks/usePeriodTotals';
import { PeriodPicker, usePeriodWindow, usePeriodBack } from '../components/common/PeriodPicker';
import { PageSkeleton } from '../components/common/PageSkeleton';
import { EmptyState } from '../components/common/EmptyState';
import { enWh, pwW } from '../components/common/ruFormat';
import type { HistoryEnergyRecord } from '../types/history';
import { t, dateLocale } from '../i18n';

const MAX_PHASES = 3;

export const AppliancePage: React.FC = () => {
  const { groupId } = useParams();
  const gid = Number(groupId);
  const navigate = useNavigate();
  const loc = useLocation();
  const fromOverview = (loc.state as { from?: string } | null)?.from === 'dashboard';
  const { data } = useTelemetryEntities();
  const unitsCfg = useUnitsConfig();
  const { names } = useDeviceGroups();
  const mf = useMoneyFormat(data?.system.totals?.tariff?.currency);
  const ds = useDsTokens();
  const clock = useDeviceClock();
  const [win, setWin] = usePeriodWindow();
  // ‹ › whole periods back (round 2 uiux U12).
  const step = usePeriodBack(win);
  const span = step.span;

  const set = useMemo(() => {
    if (!data) return null;
    return deviceSets(lineUnits(data.units, unitsCfg), unitsCfg).find((s) => s.groupId === gid && s.units.length > 1) ?? null;
  }, [data, unitsCfg, gid]);
  const units: UnitEntityGroup[] = set?.units ?? [];
  // A fixed number of history hooks (one per possible phase), summed per step.
  const ids = [0, 1, 2].map((i) => (units[i] ? unitsCfg.forGroup(units[i])?.hex ?? apiUnitId(units[i].unitKey) : null));
  const h0 = useHistory(ids[0], span.step, 'energy', span.limit, { from: span.from, to: span.to });
  const h1 = useHistory(ids[1], span.step, 'energy', span.limit, { from: span.from, to: span.to });
  const h2 = useHistory(ids[2], span.step, 'energy', span.limit, { from: span.from, to: span.to });
  const steps = useMemo(() => {
    const sum = new Map<number, number>();
    [h0, h1, h2].slice(0, Math.min(MAX_PHASES, units.length)).forEach((h) => (h.records as HistoryEnergyRecord[]).forEach((r) => {
      if (typeof r.ts !== 'number' || r.ts < span.from || r.ts >= span.to) return;
      sum.set(r.ts, (sum.get(r.ts) ?? 0) + (r.ec ?? 0));
    }));
    return Array.from(sum.entries()).sort((a, b) => a[0] - b[0]);
  }, [h0.records, h1.records, h2.records, units.length, span.from, span.to]); // eslint-disable-line react-hooks/exhaustive-deps

  // The period's sum by the Economics rule (round 3 home R3-02/03/06, installer N10, uiux V03): each
  // phase's priced records plus today's counter — one figure with Economics and History, today included.
  const period = WIN_PERIOD[win];
  const past = isPastPeriod(period) || step.back > 0;
  const todayOf = (i: number): { inWh: number; outWh: number; cost: number; revenue: number; n: number } | null => {
    const g = units[i];
    const v = g && g.present ? unitMetricValue(g, 'energy_day_in') : null;
    return v == null ? null : { inWh: Math.round(v), outWh: 0, cost: 0, revenue: 0, n: 1 };
  };
  const t0 = todayOf(0), t1 = todayOf(1), t2 = todayOf(2);
  const k0 = useMemo(() => t0, [t0?.inWh]); // eslint-disable-line react-hooks/exhaustive-deps
  const k1 = useMemo(() => t1, [t1?.inWh]); // eslint-disable-line react-hooks/exhaustive-deps
  const k2 = useMemo(() => t2, [t2?.inWh]); // eslint-disable-line react-hooks/exhaustive-deps
  const p0 = usePeriodTotals(ids[0], period, { today: past ? null : k0, back: step.back });
  const p1 = usePeriodTotals(ids[1], period, { today: past ? null : k1, back: step.back });
  const p2 = usePeriodTotals(ids[2], period, { today: past ? null : k2, back: step.back });

  if (!data) return <PageSkeleton kind="dashboard" />;
  if (!set) {
    return <EmptyState title={t('appliance.notFound')} why={t('appliance.notFoundWhy')} action={{ label: t('shell.navPoints'), to: '/devices' }} />;
  }

  const name = names.get(gid) || fullNameOf(set.lead, unitsCfg);
  const live = units.filter((g) => g.present);
  const powerW = live.length ? live.reduce((s, g) => s + Math.abs(fullPower(g, unitsCfg.forGroup(g)).w ?? 0), 0) : null;
  const dayIn = units.reduce((s, g) => s + (g.present ? unitMetricValue(g, 'energy_day_in') ?? 0 : 0), 0);
  const avg = dayAvgRate(data.system.totals);
  const liveRate = data.system.totals?.tariff?.buy_rate;
  const buy = avg.buy ?? (liveRate && liveRate > 0 ? liveRate : null);
  const perHour = powerW != null && liveRate && liveRate > 0 ? (powerW / 1000) * liveRate : null;
  const dayMoney = buy != null ? (dayIn / 1000) * buy : null;
  const role = unitsCfg.forGroup(set.lead)?.role;
  const maxStep = Math.max(0.001, ...steps.map(([, v]) => v), !past && span.step === 'day' ? dayIn : 0);
  const probes = [p0, p1, p2].slice(0, Math.min(MAX_PHASES, units.length));
  const periodWh = probes.reduce((a, p) => a + (p?.inWh ?? 0), 0);
  const todayParts = [k0, k1, k2];
  const periodMoney = (() => {
    let sum = 0;
    for (let i = 0; i < probes.length; i += 1) {
      const c = pointCost(probes[i], past ? 0 : todayParts[i]?.inWh ?? 0, buy).value;
      if (c == null) return null;
      sum += c;
    }
    return periodWh > 0 ? sum : null;
  })();
  // Today's column in a day-stepped window: the closed days stop at yesterday (round 3: «7 days»
  // labelled «16–22 Nov» drew only to the 21st).
  const todayStart = clock.dayStart(Math.floor(Date.now() / 1000));
  const chartSteps: [number, number][] = !past && span.step === 'day' && dayIn > 0 && !steps.some(([ts]) => ts >= todayStart)
    ? [...steps, [todayStart, dayIn]] : steps;
  // Labels under every n-th column so they never collide; the scale's top value on the left.
  const labelEvery = Math.max(1, Math.ceil(chartSteps.length / 8));
  const stepFmt: Intl.DateTimeFormatOptions = span.step === 'hour' ? { hour: '2-digit' } : span.step === 'month' ? { month: 'short' } : { day: 'numeric', month: 'short' };

  return (
    <Box>
      <Stack direction="row" alignItems="center" spacing={1} sx={{ mb: 1, ml: -1 }}>
        <Button size="small" startIcon={<ArrowBackIcon />} onClick={() => (window.history.length > 1 ? navigate(-1) : navigate('/devices'))}>{t('common.back')}</Button>
        <Breadcrumbs aria-label={t('point.crumbsAria')} sx={{ fontSize: 13 }}>
          <MuiLink component={RouterLink} to={fromOverview ? '/dashboard' : '/devices'} underline="hover" color="text.secondary">
            {t(fromOverview ? 'shell.navOverview' : 'shell.navPoints')}
          </MuiLink>
          <Typography sx={{ fontSize: 13 }} color="text.primary">{name}</Typography>
        </Breadcrumbs>
      </Stack>

      <Stack direction="row" spacing={2} alignItems="center" sx={{ mb: 2 }}>
        <Box sx={{ width: 44, height: 44, borderRadius: 1.5, flexShrink: 0, border: `1px solid ${ds.border}`, bgcolor: ds.surfaceRaised, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke={live.length ? ds.inkMuted : ds.statusOffline} strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round">
            <path d={ROLE_ICONS[roleIcon(set.lead, role)]} />
          </svg>
        </Box>
        <Box sx={{ flex: 1, minWidth: 0 }}>
          <Typography variant="h4" fontWeight={700} noWrap>{name}</Typography>
          <Stack direction="row" spacing={1} alignItems="center">
            <Chip size="small" variant="outlined" label={t('pointTile.phasesN', { n: units.length })} />
            <Typography variant="body2" color={live.length < units.length ? 'warning.main' : 'text.secondary'}>
              {live.length < units.length ? t('pointTile.partsOffline', { n: units.length - live.length, of: units.length }) : t('pointTile.allPhases')}
            </Typography>
          </Stack>
        </Box>
      </Stack>

      {/* Money first, energy under it (round 2 home N-04: kWh big, € in a small grey line). */}
      <Box sx={{ display: 'grid', gap: 2, gridTemplateColumns: { xs: '1fr 1fr', sm: 'repeat(3, minmax(0, 1fr))' }, mb: 2 }}>
        <StatTile label={t('appliance.now')} value={powerW == null ? '—' : pwW(powerW)} sub={perHour == null ? undefined : `≈ ${mf.money(perHour)}${t('units.perHour')}`} />
        <StatTile label={t('appliance.today')} value={dayMoney != null ? `≈ ${mf.money(dayMoney)}` : enWh(dayIn)} sub={dayMoney != null ? enWh(dayIn) : undefined} emphasis />
        {!(win === 'today' && step.back === 0) && <StatTile label={`${t('appliance.periodTotal')} · ${step.back > 0 ? step.label : t(HIST_WINDOWS.find((w) => w.value === win)?.label ?? 'history.winToday').toLowerCase()}`}
          value={periodMoney != null ? `≈ ${mf.money(periodMoney)}` : enWh(periodWh)} sub={periodMoney != null ? enWh(periodWh) : undefined} />}
      </Box>

      <Card>
        <BlockHeader title={t('appliance.trend')} caption={t(`economics.chartBy_${span.step}`)} />
        <Stack direction="row" flexWrap="wrap" gap={1.5} sx={{ mb: 1.5 }}><PeriodPicker win={win} onWin={setWin} back={step.back} onBack={step.setBack} label={step.label} canBack={!(step.back > 0 && !steps.length && !h0.loading)} /></Stack>
        {!chartSteps.length ? (
          <Typography variant="body2" color="text.secondary" sx={{ py: 3, textAlign: 'center' }}>{t('economics.chartEmpty')}</Typography>
        ) : (
          <Box role="img" aria-label={t('appliance.trendAria', { name })}>
            <Box sx={{ display: 'flex', gap: 1 }}>
              {/* The scale: its top and zero (round 2 home N-02 / uiux N01: no axis, no units). */}
              <Box sx={{ display: 'flex', flexDirection: 'column', justifyContent: 'space-between', height: 180, fontSize: 12, color: 'text.secondary', textAlign: 'right', minWidth: 64, fontVariantNumeric: 'tabular-nums' }}>
                <span>{enWh(maxStep)}</span><span>0</span>
              </Box>
              <Box sx={{ flex: 1, minWidth: 0, display: 'flex', alignItems: 'flex-end', gap: chartSteps.length > 40 ? '1px' : '3px', height: 180, borderBottom: 1, borderColor: 'divider',
                backgroundImage: `linear-gradient(${ds.border} 1px, transparent 1px)`, backgroundSize: '100% 50%' }}>
                {chartSteps.map(([ts, wh]) => (
                  <Box key={ts} title={`${formatDeviceTime(clock, ts, stepFmt, dateLocale())}: ${enWh(wh)}${buy != null ? ` · ≈ ${mf.money((wh / 1000) * buy)}` : ''}`}
                    sx={{ flex: 1, minWidth: '1px', maxWidth: 56, mx: 'auto', height: `${Math.max(1, (wh / maxStep) * 100)}%`, bgcolor: ds.flowImport, borderRadius: '3px 3px 0 0', '&:hover': { opacity: 0.8 } }} />
                ))}
              </Box>
            </Box>
            <Box sx={{ display: 'flex', gap: 1, mt: 0.5 }}>
              <Box sx={{ minWidth: 64 }} />
              <Box sx={{ flex: 1, minWidth: 0, display: 'flex', gap: chartSteps.length > 40 ? '1px' : '3px' }}>
                {chartSteps.map(([ts], i) => (
                  <Box key={ts} sx={{ flex: 1, minWidth: '1px', maxWidth: 56, mx: 'auto', fontSize: 12, color: 'text.secondary', whiteSpace: 'nowrap', overflow: 'visible', textAlign: 'center' }}>
                    {i % labelEvery === 0 ? formatDeviceTime(clock, ts, stepFmt, dateLocale()) : ''}
                  </Box>
                ))}
              </Box>
            </Box>
            <Typography variant="body2" color="text.secondary" sx={{ mt: 1 }}>
              {t('appliance.periodSum', { wh: enWh(periodWh) })}{periodMoney != null ? ` · ≈ ${mf.money(periodMoney)} ${t('appliance.atAvg')}` : ''}
            </Typography>
          </Box>
        )}
      </Card>

      <Box sx={{ mt: 2 }}>
        <Card>
          <BlockHeader title={t('appliance.phases')} caption={t('appliance.phasesCaption')} />
          <Stack spacing={0.5}>
            {units.map((g) => {
              const c = unitsCfg.forGroup(g);
              const w = g.present ? Math.abs(fullPower(g, c).w ?? 0) : null;
              return (
                <Stack key={g.unitKey} direction="row" alignItems="center" spacing={2} component={RouterLink} to={`/point/${g.unitKey}`}
                  sx={{ py: 1, px: 1, borderRadius: 1, color: 'inherit', textDecoration: 'none', '&:hover': { bgcolor: 'action.hover' } }}>
                  <Typography sx={{ fontWeight: 700, minWidth: 28 }}>{c?.phase ? `L${c.phase}` : '—'}</Typography>
                  <Typography sx={{ flex: 1, minWidth: 0 }} noWrap>{fullNameOf(g, unitsCfg)}</Typography>
                  <Typography sx={{ fontVariantNumeric: 'tabular-nums', color: g.present ? 'text.primary' : 'warning.main', fontWeight: 600 }}>
                    {w == null ? t('pointTile.offline') : pwW(w)}
                  </Typography>
                  <Typography variant="body2" color="text.secondary" sx={{ fontVariantNumeric: 'tabular-nums', minWidth: 90, textAlign: 'right' }}>
                    {g.present ? enWh(unitMetricValue(g, 'energy_day_in') ?? 0) : ''}
                  </Typography>
                  <Typography aria-hidden sx={{ color: 'text.secondary', fontSize: 18 }}>›</Typography>
                </Stack>
              );
            })}
          </Stack>
        </Card>
      </Box>
    </Box>
  );
};
