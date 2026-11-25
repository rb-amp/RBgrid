/**
 * EconomicsPage — «Экономика»: ключевая биллинговая поверхность. Расход/доход по точке
 * и зонам, банк кредита взаимозачёта, прогрессивный тариф и оконные итоги — из
 * /api/v1/economics (накопительно) + /api/v1/summary (оконные итоги; по зонам накопительно).
 * Деньги — в валюте тарифа, энергия — в кВт·ч. Шов «накопительно vs за период» подписан,
 * чтобы цифры не путали. Оформление — на дизайн-системе, значения — ru-RU.
 */
import { topLevelKeys } from '../entities/nesting';
import { useTariffPlan } from '../hooks/useTariffPlan';
import { useDsTokens } from '../theme/useDsTokens';
import { StackedBar, zoneColorOf } from '../components/charts/StackedBar';
import React, { useEffect, useMemo, useState } from 'react';
import { StatsMediaAlert } from '../components/common/StatsMediaAlert';
import { unitMetricValue } from '../entities/model';
import type { WindowTotals } from './historyWindow';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { HelpTip } from '../components/common/HelpTip';
import { Box, Typography, Grid, Stack, Alert, CircularProgress, Link } from '@mui/material';
import { Link as RouterLink } from 'react-router-dom';
import { PeriodPicker, usePeriodWindow, usePeriodBack } from '../components/common/PeriodPicker';
import { MoneyStepsChart } from '../components/charts/MoneyStepsChart';
import { historyHref } from './historyLink';
import type { HistWindow } from './historyWindow';
import { Card, FormField, StatTile, InfoField, SettingRow, StatusChip } from '@rbgrid/design-system';
import { useSensors } from '../hooks/useSensors';
import { useEconomics, useConfigShadow } from '../core';
import type { JsonObject } from '../core';
import { enWh, nf } from '../components/common/ruFormat';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { t, tCode } from '../i18n';
import { useUnitsConfig } from '../hooks/useUnitsConfig';
import { usePeriodTotals, PeriodTotals, TotalsPeriod, isPastPeriod, pointCost, WIN_PERIOD } from '../hooks/usePeriodTotals';
import { UnitEntityGroup } from '../entities/model';
import { roleLabel, mainUnit, inputDayTotals, todaySeed, stationSets, deviceSets, lineUnits } from './overviewData';
import { useDeviceGroups } from '../hooks/useDeviceGroups';
import { fullDisplayName, isGenerationRole, hexKey, apiUnitId } from '../entities/siteModel';
import { useInputSeries } from '../hooks/useInputSeries';
import { useHardware } from '../hooks/useHardware';
import { isBasic, pointHardware } from '../entities/moduleLine';

/** History's windows → Economics' totals periods (one period picker for both, UI redesign N4). */

/** Период — «за …» (для подписи-плашки итогов). */
const PERIOD_ACC: Record<TotalsPeriod, string> = {
  yesterday: 'economics.forYesterday', lastMonth: 'economics.forLastMonth',
  hour: 'economics.forHour', day: 'economics.forDay', week: 'economics.forWeek',
  month: 'economics.forMonth', year: 'economics.forYear',
};

/** Русские подписи тарифных зон; незнакомые — гуманизируем. */
const ZONE_LABEL_RU: Record<string, string> = {
  peak: 'economics.zonePeak', off_peak: 'economics.zoneOffPeak', day: 'economics.zoneDay', night: 'economics.zoneNight',
  shoulder: 'economics.zoneShoulder', standard: 'economics.zoneStandard',
};
const zoneLabel = (z: string): string => {
  if (ZONE_LABEL_RU[z]) return t(ZONE_LABEL_RU[z]);
  const code = tCode('zone', z.toUpperCase());
  return code !== z.toUpperCase() ? code : z.replace(/_/g, ' ').replace(/^\w/, (c) => c.toUpperCase());
};

/** Энергия: контракт отдаёт кВт·ч, а enWh() ждёт Вт·ч → переводим. «—» для отсутствия. */
const en = (kwh: number | null | undefined): string => enWh(kwh == null ? null : kwh * 1000);

export const EconomicsPage: React.FC = () => {
  const ds = useDsTokens();
  const { sensors } = useSensors();
  const unitsCfg = useUnitsConfig();
  const cfgOf = (key: string) => unitsCfg.forGroup({ unitKey: key } as UnitEntityGroup);
  // Points with energy only (the voltage bus has none — on .110 it was the default and read all
  // zeros, UX audit HEU-09/RES-03); the main input first, then by name.
  const unitsShadow = useConfigShadow('units');
  const units = useMemo(() => {
    // Bound sensors + every configured point (ESP-NOW stations have no sensor entry — the demo's
    // ЗС-1/ЗС-2 were missing from the point list and the balance).
    const cfgIds = (Array.isArray(unitsShadow.reported.units) ? (unitsShadow.reported.units as JsonObject[]) : [])
      .map((u) => hexKey(u.unit_id)).filter((k): k is string => !!k);
    const ids = Array.from(new Set([...sensors.filter((s) => s.unit_id).map((s) => (s.unit_id as string).toUpperCase()), ...cfgIds]))
      .filter((id) => cfgOf(id)?.type !== 'VOLTAGE_BUS');
    const rank = (id: string) => (cfgOf(id)?.role === 'MAIN_SUPPLY' ? 0 : 1);
    return ids.sort((a, b) => rank(a) - rank(b));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sensors, unitsCfg, unitsShadow.reported]);
  const pointLabel = (id: string): string => {
    const c = cfgOf(id);
    const name = fullDisplayName({ unitKey: id } as UnitEntityGroup, c);
    const role = c?.role ? roleLabel(c.role) : '';
    return role && role !== name ? `${name} · ${role}` : name;
  };
  const [unitId, setUnitId] = useState<string | null>(null);
  // A BASIC point counts consumption only (root dd861a54): no export, revenue or net figures.
  const hardware = useHardware();
  const basicUnit = !!unitId && pointHardware(unitId, hardware.sensors, hardware.modules).basic;
  // The period is the one History uses (UI redesign N4): one picker, shared for the tab's session.
  const [win, setWin] = usePeriodWindow();
  const period: TotalsPeriod = WIN_PERIOD[win];
  // ‹ › whole periods back, as in History (round 2 uiux U12); a stepped period is a closed one.
  const step = usePeriodBack(win);
  const back = step.back;
  const past = isPastPeriod(period) || back > 0;

  const telem = useTelemetryEntities();
  // The input = the hub's main point: on a per-phase input its L2/L3 points are MAIN_SUPPLY too.
  const hubMain = telem.data ? mainUnit(telem.data.units, unitsCfg) : undefined;
  const hubMainHex = hubMain ? (unitsCfg.forGroup(hubMain)?.hex ?? apiUnitId(hubMain.unitKey))?.toUpperCase() ?? null : null;
  const mainId = (hubMainHex && units.includes(hubMainHex) ? hubMainHex : null)
    ?? units.find((id) => cfgOf(id)?.role === 'MAIN_SUPPLY') ?? null;
  // Its history/money as one series — all phases summed where the hub can (hooks/useInputSeries).
  const inputSeries = useInputSeries(mainId);
  const seriesOf = (id: string | null) => (id != null && id === mainId ? inputSeries : id);
  // Default = the main input; re-pick once units-config arrives if the first guess was a bus.
  useEffect(() => { if ((!unitId || !units.includes(unitId)) && units.length) setUnitId(mainId ?? units[0]); }, [units, unitId, mainId]);
  const eco = useEconomics(seriesOf(unitId));
  // The net-metering credit means nothing while the plan has net metering off; the progressive block
  // has no setting in Tariffs — shown only when the hub runs one (docs review 04.11).
  // Only once the plan is read and says so (Kimi #19: the card flashed while loading). A plan that
  // can't be read leaves it to the hub's own figures: a credit on record means it's in use.
  const { plan: tariffPlan, loaded: planLoaded } = useTariffPlan();
  const netMeteringOn = planLoaded && (tariffPlan ? tariffPlan.net_metering_enabled === true
    : (eco.data?.net_metering?.credit_balance_kwh ?? 0) !== 0);
  // The period from the hub's closed records by its clock (hooks/usePeriodTotals): the hub's
  // /summary without from/to sums its whole ring, and a running month has no record yet.
  // «Сегодня» of the INPUT = the hub's day counters (the Overview's figure); other points and
  // the hours before today come from closed records.
  // Today by the Overview's rule (overviewData.inputDayTotals, Kimi #5).
  const sysT = inputDayTotals(telem.data?.system.totals, hubMain);
  // The INPUT's today, whichever point is picked: Generation, Charging and Balance always count the input
  // (round 3 home R3-05: picking a heat-pump phase changed the balance's «unmetered» from 18 % to 3 %).
  const inputToday = useMemo(() => (sysT && sysT.e_day_in != null ? todaySeed(sysT) : null),
    [Math.round(sysT?.e_day_in ?? -1), Math.round(sysT?.e_day_out ?? -1), sysT?.cost_day, sysT?.rev_day]); // eslint-disable-line react-hooks/exhaustive-deps
  // The picked point's today: the input's counters for the input, else its closed hours.
  const todayLive = unitId != null && unitId === mainId ? inputToday : null;
  const totals = usePeriodTotals(seriesOf(unitId), period, { today: period === 'hour' || isPastPeriod(period) ? null : todayLive, back });
  // Generation points (RENEWABLE_SOURCE) — the «Генерация» section (UX audit W2-4).
  // Charging stations: their fees for the period (role audit EV-01: Economics had only the grid side).
  const stationIds = useMemo(() => units.filter((id) => cfgOf(id)?.role === 'EV_CHARGING'), [units, cfgOf]); // eslint-disable-line react-hooks/exhaustive-deps
  const genIds = useMemo(() => units.filter((id) => isGenerationRole(cfgOf(id)?.role)),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [units, unitsCfg]);
  const noExport = !!totals && totals.outWh < 1 && totals.revenue <= 0 && genIds.length === 0;
  const mainBasic = !!mainId && pointHardware(mainId, hardware.sensors, hardware.modules).basic;
  // Metered lines (not the input, the voltage bus or a source) — the balance's «lines» side; a point
  // measured inside another line (fw 1.1.8 `inside`) is in its parent's energy, not added again.
  const lineIds = useMemo(() => topLevelKeys(units.filter((id) => {
    const c = cfgOf(id);
    return c?.role !== 'MAIN_SUPPLY' && c?.role !== 'ALTERNATIVE_SUPPLY' && c?.type !== 'VOLTAGE_BUS' && !isGenerationRole(c?.role);
  }), cfgOf),
  // eslint-disable-next-line react-hooks/exhaustive-deps
  [units, unitsCfg]);
  // Currency/precision from the device (response ISO code → schema.money) — never hardcoded.
  const mf = useMoneyFormat(eco.data?.currency);

  return (
    <Box>
      <Typography variant="h4" fontWeight={800} sx={{ mb: 2.5 }}>{t('economics.title')}</Typography>
      <StatsMediaAlert />

      <Grid container spacing={2} sx={{ mb: 2 }}>
        <Grid item xs={12} sm={6}>
          <FormField
            label={t('economics.point')} value={unitId ?? ''} onChange={setUnitId}
            options={units.length ? units.map((u) => ({ value: u, label: pointLabel(u) })) : [{ value: '', label: t('economics.noPoints') }]}
          />
        </Grid>
      </Grid>
      <Stack direction="row" flexWrap="wrap" gap={1.5} alignItems="center" sx={{ mb: 2, minWidth: 0 }}>
        <PeriodPicker win={win} onWin={setWin} back={back} onBack={step.setBack} label={step.label} canBack={!(back > 0 && totals && !totals.loading && !totals.n)} />
        {/* The hours behind these totals (UI redesign N5): History in money, the same point and period. */}
        {unitId && (
          <Link component={RouterLink} to={historyHref({ point: unitId, win, metric: 'money' })} underline="hover" sx={{ fontSize: 14, ml: { sm: 'auto' } }}>
            {t('economics.byHour')}
          </Link>
        )}
      </Stack>

      {/* The period's steps as columns above the totals (UI redesign U8). */}
      <Box sx={{ mb: 2 }}><MoneyStepsChart seriesId={seriesOf(unitId)} win={win} back={back} fmt={(v) => mf.money(v)} /></Box>

      {/* Оконные итоги */}
      <Box sx={{ mb: 2 }}>
        <Card>
          <Stack direction="row" alignItems="center" spacing={1.5} mb={1.5}>
            <Typography variant="h6" fontWeight={700}>{t('economics.totals')}</Typography>
            <StatusChip status="present" label={back > 0 ? step.label : t(PERIOD_ACC[period])} />
          </Stack>
          {totals?.loading && !totals.n && <Box sx={{ display: 'flex', justifyContent: 'center', py: 3 }}><CircularProgress size={22} aria-label={t('common.loading')} /></Box>}
          {!totals?.loading && !totals?.n && <Alert severity="info">{unitId ? t('economics.noDataForPoint') : t('economics.selectPoint')}</Alert>}
          {!!totals?.n && (
            <>
              <Grid container spacing={2}>
                {/* Nothing exported and no generation: no «exported 0 / earnings 0 / net = cost» rows
                    (round 2 home H-07); the cost is the one big figure. */}
                <Grid item xs={6} sm={3}><StatTile label={t('economics.imported')} value={enWh(totals.inWh)} emphasis={basicUnit} /></Grid>
                {!basicUnit && !noExport && <Grid item xs={6} sm={3}><StatTile label={t('economics.exported')} value={enWh(totals.outWh)} subColor="success.main" /></Grid>}
                <Grid item xs={6} sm={3}><StatTile label={t('economics.cost')} value={mf.money(totals.cost)} emphasis={noExport && !basicUnit} /></Grid>
                {!basicUnit && !noExport && <Grid item xs={6} sm={3}><StatTile label={t('economics.revenue')} value={mf.money(totals.revenue)} subColor="success.main" /></Grid>}
                {!basicUnit && !noExport && <Grid item xs={6} sm={3}><StatTile label={t('economics.netEnergy')} value={enWh(totals.inWh - totals.outWh)} /></Grid>}
                {!basicUnit && !noExport && <Grid item xs={6} sm={3}><StatTile label={t('economics.netCost')} value={mf.money(totals.cost - totals.revenue)} emphasis /></Grid>}
              </Grid>
              {/* A station's «earnings» are its customers' fees, under «Charging» — not «no revenue» (round 3 ev N-03). */}
              {basicUnit && <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1.5 }}>{t(unitId && stationIds.includes(unitId) ? 'economics.stationNoExport' : 'economics.basicNoExport')}</Typography>}
              <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1.5 }}>{past ? t('economics.closedPast') : todayLive && period !== 'hour' ? t('economics.todayByCounters') : t('economics.closedOnly')}<HelpTip topic="closedRecords" /></Typography>
            </>
          )}
        </Card>
      </Box>

      {genIds.length > 0 && mainId && (
        <GenerationSection genIds={genIds} mainId={inputSeries ?? mainId} period={period} back={back} mf={mf} todayLive={inputToday} />
      )}
      {stationIds.length > 0 && mainId && (
        <ChargingSection stationIds={stationIds} mainId={inputSeries ?? mainId} period={period} back={back} mf={mf} todayLive={inputToday} nameOf={pointLabel} selected={unitId} />
      )}
      {lineIds.length > 0 && mainId && (
        <BalanceSection lineIds={lineIds} genIds={genIds} mainId={inputSeries ?? mainId} period={period} back={back} todayLive={inputToday} nameOf={pointLabel} mf={mf} />
      )}

      {/* Накопительное состояние биллинга */}
      {eco.loading && <Box sx={{ display: 'flex', justifyContent: 'center', py: 4 }}><CircularProgress aria-label={t('common.loading')} /></Box>}
      {/* The hub's running billing since its restart, NOT the picked period (root ui-round-data-asks: zones
          are not stored per period before a later firmware). Hidden for a past period, where it stood next
          to the period's totals as if it were theirs (role audit B1: 905 € vs 11 €); else said plainly. */}
      {!eco.loading && eco.data && !past && (
        <Typography sx={{ fontSize: 14, color: 'text.secondary', mt: 1, mb: 1 }}>{t('economics.sinceRestartNote')}</Typography>
      )}
      {!eco.loading && eco.data && !past && (
        <Grid container spacing={2}>
          <Grid item xs={12} md={6}>
            <Card outlined>
              <Typography sx={{ fontSize: 16, fontWeight: 700 }}>{t('economics.byZones')}<HelpTip topic="zone" /></Typography>
              {/* One bar of the zones' cost — where the money went at a glance (charts audit K) — the rows
                  below keep the exact figures and any revenue. */}
              <Box sx={{ mt: 1, mb: 1.5 }}>
                <StackedBar fmt={(v) => mf.money(v)} total={mf.money(eco.data.zones.reduce((a, z) => a + z.cost, 0))}
                  parts={eco.data.zones.filter((z) => z.cost > 0).map((z) => ({ key: z.zone, label: zoneLabel(z.zone), value: z.cost, color: zoneColorOf(z.zone, ds.series) }))} />
              </Box>
              <Stack spacing={0.5} sx={{ mt: 1 }}>
                {/* Only zones with charges in the period — the hub reports all 8 fixed slots (TAR-09). */}
                {eco.data.zones.filter((z) => z.cost > 0 || z.revenue > 0).map((z) => (
                  <SettingRow key={z.zone} label={zoneLabel(z.zone)}
                    value={z.revenue > 0 ? t('economics.zoneCostRevenue', { cost: mf.money(z.cost), revenue: mf.money(z.revenue) }) : mf.money(z.cost)} />
                ))}
                {!eco.data.zones.some((z) => z.cost > 0 || z.revenue > 0) && (
                  <Typography variant="body2" color="text.secondary">{t('economics.noZoneCharges')}</Typography>
                )}
              </Stack>
            </Card>
          </Grid>
          <Grid item xs={12} md={6}>
            <Stack spacing={2}>
              {netMeteringOn && <Card outlined>
                <Typography variant="overline" color="text.secondary">{t('economics.netMetering')}</Typography>
                {mainBasic && <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>{t('economics.basicNoNetMetering')}</Typography>}
                <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap sx={{ mt: 1 }}>
                  <InfoField label={t('economics.creditBalance')} value={en(eco.data.net_metering.credit_balance_kwh)} />
                  <InfoField label={t('economics.periodImport')} value={en(eco.data.net_metering.period_import_kwh)} />
                  <InfoField label={t('economics.periodExport')} value={en(eco.data.net_metering.period_export_kwh)} />
                  <InfoField label={t('economics.lifetimeEarned')} value={en(eco.data.net_metering.lifetime_earned_kwh)} />
                  <InfoField label={t('economics.lifetimeUsed')} value={en(eco.data.net_metering.lifetime_used_kwh)} />
                </Stack>
              </Card>}
              {eco.data.progressive.enabled && <Card outlined>
                <Stack direction="row" alignItems="center" spacing={1.5} mb={1}>
                  <Typography variant="overline" color="text.secondary">{t('economics.progressive')}</Typography>
                  <StatusChip status={eco.data.progressive.enabled ? 'live' : 'dormant'} label={eco.data.progressive.enabled ? t('economics.on') : t('economics.off')} />
                </Stack>
                {eco.data.progressive.enabled && (
                  <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap>
                    <InfoField label={t('economics.tier')} value={String(eco.data.progressive.tier)} />
                    <InfoField label={t('economics.cumulative')} value={en(eco.data.progressive.cumulative_import_kwh)} />
                    <InfoField label={t('economics.multiplier')} value={nf(eco.data.progressive.rate_mult, 2)} />
                  </Stack>
                )}
              </Card>}
            </Stack>
          </Grid>
        </Grid>
      )}
    </Box>
  );
};

/** One generation point's period totals, reported up (hooks can't run in a loop). */
const GenProbe: React.FC<{ id: string; period: TotalsPeriod; back: number; today: WindowTotals | null; money?: boolean; onTotals: (id: string, v: PeriodTotals | null) => void }> = ({ id, period, back, today, money = false, onTotals }) => {
  // Today = the point's own day counter (like the input's), earlier = closed records.
  const v = usePeriodTotals(id, period, { money, today: period === 'hour' || isPastPeriod(period) ? null : today, back });
  useEffect(() => { onTotals(id, v); }, [id, v, onTotals]);
  return null;
};


/**
 * «Генерация» for the period (UX audit W2-4 / GEN-05): produced = the generation points' output;
 * what left through the main input was sold (its revenue), the rest the site used itself and did
 * not buy — valued at the period's average buying price of the input (≈).
 */
/**
 * «Charging» for the period (role audit 22.11 EV-01): the stations' energy, what that energy cost at the
 * input's average buying price and — on a car park (2+ stations, or a hub set to «ev») — the fees at the
 * price for customers and the margin. A home's own charger shows energy and cost only (round 2 home
 * N-01: «fees from customers» read as earnings). A three-phase charger is one row (stationSets). The hub
 * keeps no history of the customers' price (root ui-round-data-asks): the fees are «≈ at the current price».
 */
const ChargingSection: React.FC<{ stationIds: string[]; mainId: string; period: TotalsPeriod; back: number; mf: ReturnType<typeof useMoneyFormat>; todayLive: WindowTotals | null; nameOf: (id: string) => string; selected?: string | null }> = ({ stationIds, mainId, period, back, mf, todayLive, nameOf, selected }) => {
  const [st, setSt] = useState<Record<string, PeriodTotals | null>>({});
  const onTotals = React.useCallback((id: string, v: PeriodTotals | null) => setSt((g) => (g[id] === v ? g : { ...g, [id]: v })), []);
  const input = usePeriodTotals(mainId, period, { today: period === 'hour' || isPastPeriod(period) ? null : todayLive, back });
  const sys = useConfigShadow('system');
  // The system domain is loaded by the Overview — opened directly, Economics loads it itself (round 2
  // ev N-01: «no customer price» on a bookmark or F5 while the price was set).
  const sysLoaded = sys.reported && Object.keys(sys.reported).length > 0;
  useEffect(() => { if (!sysLoaded) void sys.load().catch(() => undefined); }, [sysLoaded]); // eslint-disable-line react-hooks/exhaustive-deps
  const evCfg = sys.reported.ev as { price_per_kwh?: unknown } | undefined;
  const price = typeof evCfg?.price_per_kwh === 'number' && evCfg.price_per_kwh > 0 ? evCfg.price_per_kwh : null;
  const telem = useTelemetryEntities();
  const unitsCfg = useUnitsConfig();
  const key = stationIds.map((id) => {
    const g = telem.data?.units.find((u) => u.unitKey.toUpperCase() === id.toUpperCase());
    const v = g ? unitMetricValue(g, 'energy_day_in') : null;
    return `${id}:${v == null ? '' : Math.round(v)}`;
  }).join(',');
  const todayMap = useMemo(() => new Map(key.split(',').map((kv) => {
    const [id, v] = kv.split(':');
    return [id, v === '' || v === undefined ? null : { inWh: Number(v), outWh: 0, cost: 0, revenue: 0, n: 1 }] as const;
  })), [key]);
  // One row per station (a three-phase charger's phases summed).
  const sets = telem.data ? stationSets(telem.data.units, unitsCfg) : [];
  const idOf = (k: string) => (k.toUpperCase());
  const rows = sets.length
    ? sets.map((set) => ({ id: idOf(set.lead.unitKey), ids: set.keys.map(idOf).filter((k) => stationIds.includes(k)) })).filter((r) => r.ids.length)
    : stationIds.map((id) => ({ id, ids: [id] }));
  const park = rows.length >= 2 || sys.reported.site_profile === 'ev';
  const selectedRow = selected ? rows.find((r) => r.ids.includes(selected.toUpperCase()))?.id ?? null : null; // = isCarPark (overviewData)
  const rowWh = (r: { ids: string[] }) => r.ids.reduce((s0, id) => s0 + (st[id]?.inWh ?? 0), 0);
  const wh = stationIds.reduce((s0, id) => s0 + (st[id]?.inWh ?? 0), 0);
  const known = stationIds.some((id) => (st[id]?.n ?? 0) > 0);
  const loading = stationIds.some((id) => st[id] === undefined || st[id]?.loading);
  const avgBuy = input && input.inWh > 0 && input.cost > 0 ? input.cost / (input.inWh / 1000) : null;
  const fees = price != null ? (wh / 1000) * price : null;
  // What the stations' energy cost: each point's own priced records (pointCost), the live day estimated.
  const live = !(period === 'hour' || isPastPeriod(period) || back > 0);
  const costOf = (ids: string[]): number | null => {
    let sum = 0;
    for (const id of ids) {
      const c = pointCost(st[id], live ? todayMap.get(id)?.inWh ?? 0 : 0, avgBuy).value;
      if (c == null) return null;
      sum += c;
    }
    return sum;
  };
  const bought = costOf(stationIds);
  const money = (v: number | null) => (v == null ? '—' : `≈ ${mf.money(v)}`);
  return (
    <Box sx={{ mb: 2 }}>
      {stationIds.map((id) => <GenProbe key={id} id={id} period={period} back={back} today={todayMap.get(id) ?? null} money onTotals={onTotals} />)}
      <Card>
        <Typography variant="h6" fontWeight={700} sx={{ mb: 1.5 }}>
          {t(park ? 'economics.evTitle' : 'economics.evTitleHome')}
          {/* With one station picked above, say these are all of them; its row is marked below (round 3 ev N-03). */}
          {park && selectedRow && <Box component="span" sx={{ fontWeight: 400, fontSize: 15, color: 'text.secondary' }}>{` · ${t('economics.evAllStations')}`}</Box>}
        </Typography>
        {!known ? (
          <Typography variant="body2" color="text.secondary">{t(loading ? 'common.loading' : 'economics.evNoData')}</Typography>
        ) : (
          <>
            <Grid container spacing={2}>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.evEnergy')} value={enWh(wh)} /></Grid>
              {park && <Grid item xs={6} sm={3}><StatTile label={t('economics.evFees')} value={!sysLoaded ? '…' : money(fees)} /></Grid>}
              <Grid item xs={6} sm={3}><StatTile label={t(park ? 'economics.evBought' : 'economics.evCostHome')} value={money(bought)} emphasis={!park} /></Grid>
              {park && <Grid item xs={6} sm={3}><StatTile label={t('economics.evMargin')} value={!sysLoaded ? '…' : fees != null && bought != null ? money(fees - bought) : '—'} emphasis /></Grid>}
            </Grid>
            {rows.length > 1 && (
              // Headed columns, one row per station with the same figures as above (round 3 ev R3-01: a bare
              // «≈ 715,79 €» read as cost next to «where the energy went»).
              <Box sx={{ mt: 1.5, overflowX: 'auto' }}>
              <Box component="table" sx={{ borderCollapse: 'collapse', width: '100%', maxWidth: 760, '& td, & th': { py: 0.5, fontSize: 14, fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap' }, '& th': { fontSize: 12, fontWeight: 600, color: 'text.secondary', textAlign: 'left', borderBottom: 1, borderColor: 'divider' }, '& td + td, & th + th': { textAlign: 'right', pl: 2 } }}>
                <thead>
                  <tr>
                    <th>{t('economics.evColStation')}</th>
                    <th>{t('economics.evEnergy')}</th>
                    {park && <th>{t('economics.evFees')}</th>}
                    <th>{t(park ? 'economics.evBought' : 'economics.evCostHome')}</th>
                    {park && <th>{t('economics.evMargin')}</th>}
                  </tr>
                </thead>
                <tbody>
                  {rows.map((r) => {
                    const w = rowWh(r);
                    const f = price != null ? (w / 1000) * price : null;
                    const b = costOf(r.ids);
                    return (
                      <tr key={r.id} style={r.id === selectedRow ? { fontWeight: 700 } : undefined}>
                        <td>{nameOf(r.id)}{r.id === selectedRow ? ' ←' : ''}</td>
                        <td>{enWh(w)}</td>
                        {park && <td>{money(f)}</td>}
                        <td>{money(b)}</td>
                        {park && <td>{f != null && b != null ? money(f - b) : '—'}</td>}
                      </tr>
                    );
                  })}
                </tbody>
              </Box>
              </Box>
            )}
            <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1 }}>
              {!park ? t('economics.evNoteHome')
                : !sysLoaded ? t('common.loading')
                  : price == null ? t('economics.evNoPrice') : t('economics.evNote', { price: mf.money(price) })}
            </Typography>
          </>
        )}
      </Card>
    </Box>
  );
};

const GenerationSection: React.FC<{ genIds: string[]; mainId: string; period: TotalsPeriod; back: number; mf: ReturnType<typeof useMoneyFormat>; todayLive: WindowTotals | null }> = ({ genIds, mainId, period, back, mf, todayLive }) => {
  const [gen, setGen] = useState<Record<string, PeriodTotals | null>>({});
  const onTotals = React.useCallback((id: string, v: PeriodTotals | null) => setGen((g) => (g[id] === v ? g : { ...g, [id]: v })), []);
  const input = usePeriodTotals(mainId, period, { today: period === 'hour' || isPastPeriod(period) ? null : todayLive, back });
  const telem = useTelemetryEntities();
  // Rounded to 1 Wh and memoised by value — a new object per frame would re-run the probes.
  const genKey = genIds.map((id) => {
    const g = telem.data?.units.find((u) => u.unitKey.toUpperCase() === id.toUpperCase());
    const out = g ? unitMetricValue(g, 'energy_day_out') : null;
    return `${id}:${out == null ? '' : Math.round(out)}`;
  }).join(',');
  const genTodayMap = useMemo(() => new Map(genKey.split(',').map((kv) => {
    const [id, out] = kv.split(':');
    return [id, out === '' || out === undefined ? null : { inWh: 0, outWh: Number(out), cost: 0, revenue: 0, n: 1 }] as const;
  })), [genKey]);
  const genToday = (id: string): WindowTotals | null => genTodayMap.get(id) ?? null;
  const produced = genIds.reduce((s, id) => s + (gen[id]?.outWh ?? 0), 0);
  const known = genIds.some((id) => (gen[id]?.n ?? 0) > 0);
  const sold = Math.min(produced, input?.outWh ?? 0);
  const self = Math.max(0, produced - sold);
  const avgBuy = input && input.inWh > 0 && input.cost > 0 ? input.cost / (input.inWh / 1000) : null;
  const saved = avgBuy != null ? (self / 1000) * avgBuy : null;
  return (
    <Box sx={{ mb: 2 }}>
      {genIds.map((id) => <GenProbe key={id} id={id} period={period} back={back} today={genToday(id)} onTotals={onTotals} />)}
      <Card>
        <Typography variant="h6" fontWeight={700} sx={{ mb: 1.5 }}>{t('economics.genTitle')}</Typography>
        {!known ? (
          <Typography variant="body2" color="text.secondary">{t(genIds.some((id) => gen[id] === undefined || gen[id]?.loading) ? 'common.loading' : 'economics.genNoData')}</Typography>
        ) : (
          <>
            <Grid container spacing={2}>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genProduced')} value={enWh(produced)} emphasis /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genSelf')} value={enWh(self)} /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genSaved')} value={saved != null ? `≈ ${mf.money(saved)}` : '—'} subColor="success.main" /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genSold')} value={`${enWh(sold)} · ${mf.money(input?.revenue ?? 0)}`} subColor="success.main" /></Grid>
              {/* The two shares HA's Energy dashboard shows (HA-R08): how much of the sun the site used
                  itself, and how much of the site's need the sun covered. */}
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genSelfShare')} value={produced > 0 ? `${nf((self / produced) * 100)} %` : '—'} /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genAutarky')} value={self + (input?.inWh ?? 0) > 0 ? `${nf((self / (self + (input?.inWh ?? 0))) * 100)} %` : '—'} /></Grid>
            </Grid>
            <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1.5 }}>
              {avgBuy != null ? t('economics.genExplain', { rate: mf.rate(avgBuy) }) : t('economics.genExplainNoRate')}
              <HelpTip topic="notBought" />
            </Typography>
          </>
        )}
      </Card>
    </Box>
  );
};

/** Today = the live day counter of a point (rounded, memoised by value), for a period probe. */
function useLiveToday(ids: string[], field: 'energy_day_in' | 'energy_day_out'): (id: string) => WindowTotals | null {
  const telem = useTelemetryEntities();
  const key = ids.map((id) => {
    const g = telem.data?.units.find((u) => u.unitKey.toUpperCase() === id.toUpperCase());
    const v = g && g.present ? unitMetricValue(g, field) : null;
    return `${id}:${v == null ? '' : Math.round(v)}`;
  }).join(',');
  const map = useMemo(() => new Map(key.split(',').map((kv) => {
    const [id, v] = kv.split(':');
    const wh = v === '' || v === undefined ? null : Number(v);
    return [id, wh == null ? null : { inWh: field === 'energy_day_in' ? wh : 0, outWh: field === 'energy_day_out' ? wh : 0, cost: 0, revenue: 0, n: 1 }] as const;
  })), [key, field]);
  return (id: string) => map.get(id) ?? null;
}

/**
 * «Баланс» for the period (rerun 16.10 SCADA-R11): the input against the sum of its metered
 * lines — the unmetered remainder in kWh and %. A line with no records for the period (offline,
 * new) makes the balance INCOMPLETE and says which (never «everything is unmetered»).
 */
// Below this the input reads ~0 kWh (a bench input, a solar house at noon): a share of it means nothing.
const BALANCE_PCT_FLOOR_WH = 10;

const BalanceSection: React.FC<{ lineIds: string[]; genIds: string[]; mainId: string; period: TotalsPeriod; back: number; todayLive: WindowTotals | null; nameOf: (id: string) => string; mf: ReturnType<typeof useMoneyFormat> }> = ({ lineIds, genIds, mainId, period, back, todayLive, nameOf, mf }) => {
  const [lines, setLines] = useState<Record<string, PeriodTotals | null>>({});
  const onTotals = React.useCallback((id: string, v: PeriodTotals | null) => setLines((g) => (g[id] === v ? g : { ...g, [id]: v })), []);
  const [gens, setGens] = useState<Record<string, PeriodTotals | null>>({});
  const onGen = React.useCallback((id: string, v: PeriodTotals | null) => setGens((g) => (g[id] === v ? g : { ...g, [id]: v })), []);
  const input = usePeriodTotals(mainId, period, { today: period === 'hour' || isPastPeriod(period) ? null : todayLive, back });
  const lineToday = useLiveToday(lineIds, 'energy_day_in');
  const genToday = useLiveToday(genIds, 'energy_day_out');
  const sum = lineIds.reduce((s, id) => s + (lines[id]?.inWh ?? 0), 0);
  const none = (v: PeriodTotals | null | undefined) => !v || v.n === 0;
  const missing = [...lineIds.filter((id) => none(lines[id])), ...genIds.filter((id) => none(gens[id]))];
  // With generation the lines are fed by the grid AND the panels: what the site used = import + produced
  // − exported. Import alone made a solar house's «Unmetered» negative (Demo Home −57 %).
  const solar = genIds.length > 0;
  const produced = genIds.reduce((s, id) => s + (gens[id]?.outWh ?? 0), 0);
  const inWh = solar ? Math.max(0, (input?.inWh ?? 0) + produced - (input?.outWh ?? 0)) : input?.inWh ?? 0;
  const rest = inWh - sum;
  // «Where the money went» (role audit H rec. 3): each line's own priced records (pointCost); the running
  // day and a line without its own cost at the period's average buying price.
  const avgBuy = input && input.inWh > 0 && input.cost > 0 ? input.cost / (input.inWh / 1000) : null;
  // An appliance measured per phase is one row (round 2 home: «where it went» listed a heat pump's phases).
  const telemB = useTelemetryEntities();
  const unitsCfgB = useUnitsConfig();
  const { names: applNames } = useDeviceGroups();
  const setOf = new Map<string, { key: string; name: string }>();
  if (telemB.data) {
    deviceSets(lineUnits(telemB.data.units, unitsCfgB), unitsCfgB).filter((st) => st.units.length > 1).forEach((st) => {
      const name = (st.groupId != null ? applNames.get(st.groupId) : undefined) || nameOf(st.lead.unitKey.toUpperCase());
      st.units.forEach((u) => setOf.set(u.unitKey.toUpperCase(), { key: `g:${st.groupId ?? st.lead.unitKey}`, name }));
    });
  }
  const liveDay = !(period === 'hour' || isPastPeriod(period) || back > 0);
  const byRow = new Map<string, { id: string; name: string; wh: number; cost: number | null }>();
  lineIds.forEach((id) => {
    const set = setOf.get(id.toUpperCase());
    const k = set?.key ?? id;
    const cur = byRow.get(k) ?? { id: k, name: set?.name ?? nameOf(id), wh: 0, cost: 0 };
    cur.wh += lines[id]?.inWh ?? 0;
    const c = pointCost(lines[id], liveDay ? lineToday(id)?.inWh ?? 0 : 0, avgBuy).value;
    cur.cost = cur.cost == null || c == null ? null : cur.cost + c;
    byRow.set(k, cur);
  });
  const ranked = Array.from(byRow.values()).filter((r) => r.wh > 0).sort((a, b) => b.wh - a.wh);
  const topWh = ranked[0]?.wh ?? 0;
  return (
    <Box sx={{ mb: 2 }}>
      {lineIds.map((id) => <GenProbe key={id} id={id} period={period} back={back} today={lineToday(id)} money onTotals={onTotals} />)}
      {genIds.map((id) => <GenProbe key={id} id={id} period={period} back={back} today={genToday(id)} onTotals={onGen} />)}
      <Card>
        <Typography variant="h6" fontWeight={700} sx={{ mb: 1.5 }}>{t('economics.balanceTitle')}</Typography>
        {!input?.n ? (
          <Typography variant="body2" color="text.secondary">{t(!input || input.loading ? 'common.loading' : 'economics.noDataForPoint')}</Typography>
        ) : (
          <>
            <Grid container spacing={2}>
              <Grid item xs={6} sm={3}><StatTile label={t(solar ? 'economics.balanceUsed' : 'economics.balanceInput')} value={enWh(inWh)} emphasis /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.balanceLines', { n: lineIds.filter((id) => !none(lines[id])).length })} value={enWh(sum)} /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.balanceRest')} value={missing.length ? '—' : enWh(rest)} /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t(solar ? 'economics.balanceRestPctUsed' : 'economics.balanceRestPct')} value={missing.length || inWh < BALANCE_PCT_FLOOR_WH ? '—' : `${nf((rest / inWh) * 100)} %`} /></Grid>
            </Grid>
            {missing.length > 0 && (
              <Alert severity="info" sx={{ mt: 1.5 }}>{t('economics.balanceIncomplete', { points: missing.map(nameOf).join(', ') })}</Alert>
            )}
            {ranked.length > 0 && (
              <Box sx={{ mt: 2 }}>
                <Typography sx={{ fontSize: 16, fontWeight: 700, mb: 1 }}>{t('economics.whereTitle')}</Typography>
                <Stack spacing={0.75}>
                  {ranked.map((r) => (
                    <Box key={r.id} sx={{ display: 'grid', gridTemplateColumns: { xs: '1fr auto', sm: 'minmax(120px, 220px) 1fr auto' }, columnGap: 1.5, alignItems: 'center' }}>
                      <Typography sx={{ fontSize: 14 }} noWrap title={r.name}>{r.name}</Typography>
                      <Box sx={{ display: { xs: 'none', sm: 'block' }, height: 8, borderRadius: 4, bgcolor: 'action.hover', overflow: 'hidden' }}>
                        <Box sx={{ height: '100%', width: `${Math.max(2, (r.wh / Math.max(1, topWh)) * 100)}%`, bgcolor: 'primary.main', borderRadius: 4 }} />
                      </Box>
                      <Typography sx={{ fontSize: 14, fontVariantNumeric: 'tabular-nums', textAlign: 'right', whiteSpace: 'nowrap' }}>
                        {r.cost != null ? `≈ ${mf.money(r.cost)} · ` : ''}{enWh(r.wh)}{inWh > 0 ? ` · ${nf((r.wh / inWh) * 100)} %` : ''}
                      </Typography>
                    </Box>
                  ))}
                </Stack>
                <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1 }}>{t(ranked.some((r) => r.cost != null) ? 'economics.whereNote' : 'economics.whereNoMoney')}</Typography>
              </Box>
            )}
          </>
        )}
      </Card>
    </Box>
  );
};
