/**
 * EvOverview — the «Станции» Overview layout (spec §4.3, review 04 §5.4, persona 09).
 *
 * Order (desktop and phone alike — the primary answer first):
 *   1. ParkingHero — «N из M заряжают» + load balance vs the input limit (one parking-power
 *      number, one definition);
 *   2. KPI — Энергия сегодня · Выручка сегодня (only when a price is set) · Сессий сегодня;
 *   3. StationCard grid (3 / 2 / 1 per row);
 *   4. «Мощность паркинга · сегодня» — line + limit (sev-critical);
 *   5. EnergyFlowTree (stations vs infrastructure).
 * No point tiles, no home hourly chart; solar/export only appear if a generation line exists.
 * A component (not a render function) so it can own the history hook.
 */
import React from 'react';
import { Alert, Box, Grid } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { UnitEntityGroup } from '../../entities/model';
import { UnitsConfigJoin } from '../../hooks/useUnitsConfig';
import { KpiRow, KpiItem } from '../dashboard/KpiRow';
import { EnergyFlowTree } from '../flow/EnergyFlowTree';
import { StationCard, StationView } from './StationCard';
import { ParkingHero } from './ParkingHero';
import { ParkingPowerChart, useParkingHistory } from './ParkingPowerChart';
import { EvThresholds } from './evStatus';
import {
  buildEvTree, buildStations, buildLoadSegments, stationCounts, stationSets, lineUnits, isStation, isGenerationUnit,
} from '../../pages/overviewData';
import { useDeviceGroups } from '../../hooks/useDeviceGroups';
import { enWh } from '../common/ruFormat';
import { useLensFormat } from '../../economics/MoneyLens';
import { t } from '../../i18n';

export interface EvOverviewProps {
  units: UnitEntityGroup[];
  join: UnitsConfigJoin;
  ev: EvThresholds;
  /** Grid power for the tree root (kW) + whether it carries a direction. */
  grid: number;
  gridHasDirection: boolean;
  /** Price per kWh for EV sales; null = not set → the revenue KPI isn't drawn. */
  pricePerKwh: number | null;
  formatMoney: (v: number | null) => string;
  lost: boolean;
  onOpenPoint: (key: string) => void;
}

/** Everything the car-park blocks show, computed once (the parking history is fetched here). */
export function useEvModel({ units, join, ev, grid, gridHasDirection, pricePerKwh, formatMoney, lost }: Omit<EvOverviewProps, 'onOpenPoint'>) {
  const { names: groupNames } = useDeviceGroups();
  const stations = buildStations(units, join, ev, groupNames);
  const counts = stationCounts(stations);
  const offline = stations.filter((s) => s.status === 'nodata').length;
  const segments = buildLoadSegments(units, join, groupNames);
  // Input limit from the device only (main unit rated_power_w) — never substituted (U3).
  const limitW = join.input.ratedPowerW;

  // Parking lines = the balance bar's set (stations + non-generation infrastructure).
  const parking = lineUnits(units, join).filter((g) => isStation(g, join) || !isGenerationUnit(g, join));
  const history = useParkingHistory(
    parking.map((g) => g.unitKey),
    stationSets(units, join).map((st) => st.keys),
    ev.chargeThresholdW,
    join.input.hex,
  );

  const evEnergyTodayKwh = stations.reduce((s, st) => s + (st.sessionKwh ?? 0), 0);
  const tree = buildEvTree(units, join, grid, gridHasDirection, lost);

  const lens = useLensFormat();
  const evWh = evEnergyTodayKwh * 1000;
  const revenue = pricePerKwh != null ? evEnergyTodayKwh * pricePerKwh : null;
  const kpis: KpiItem[] = [];
  if (lens.moneyMode && revenue != null) {
    // Money lens: what the parking earned and what is left after the energy bought for it.
    const buyRate = lens.dayRate?.buy ?? lens.rate.buy;
    const buyCost = buyRate != null ? (evWh / 1000) * buyRate : null;
    kpis.push({ label: t('stations.revenueToday'), value: `${t('common.approx')} ${formatMoney(revenue)}`, sub: `${enWh(evWh)} · ${t('stations.atCurrentPrice')}` });
    if (buyCost != null) {
      // One buying figure for both (round 2 ev N-02: the margin used the plain rate, «purchase» the rate with VAT and fees).
      kpis.push({ label: t('stations.marginToday'), value: `${t('common.approx')} ${formatMoney(revenue - buyCost)}`, sub: t('stations.purchase', { money: `${t('common.approx')} ${formatMoney(buyCost)}` }) });
    }
  } else {
    const e = lens.energy(evWh);
    kpis.push({ label: t('stations.energyToday'), value: e.main, sub: e.money ? e.sub ?? undefined : t('stations.allStations') });
    // §14 W4: no price → the revenue KPI is not drawn (not «0 ₽», not «—»).
    if (revenue != null) {
      kpis.push({ label: t('stations.revenueToday'), value: `${t('common.approx')} ${formatMoney(revenue)}`, sub: t('stations.atCurrentPrice') });
    }
  }
  kpis.push(history.sessions != null
    ? { label: t('stations.sessionsToday'), value: `${history.sessions}`, sub: t('stations.sessionsEstimate') }
    : { label: t('stations.chargingNow'), value: `${counts.charging}`, sub: t('stations.sessionsUnavailable') });

  return { stations, counts, offline, segments, limitW, history, kpis, tree, lost, pricePerKwh };
}

export type EvModel = ReturnType<typeof useEvModel>;

export const EvHeroBlock: React.FC<{ m: EvModel }> = ({ m }) => (
  <ParkingHero charging={m.counts.charging} total={m.counts.total} free={m.counts.free} offline={m.offline}
    segments={m.segments} limitW={m.limitW} lost={m.lost} />
);

export const EvKpiBlock: React.FC<{ m: EvModel }> = ({ m }) => <KpiRow items={m.kpis} />;

/** How the stations block lists them (its Customise settings — role audit EV rec. 5). */
export interface StationsListOpts { sort?: 'config' | 'name' | 'state'; free?: 'show' | 'hide' }
const STATE_RANK: Record<string, number> = { charging: 0, waiting: 1, nodata: 2, free: 3 };
export const orderStations = (list: StationView[], o: StationsListOpts = {}): StationView[] => {
  let out = o.free === 'hide' ? list.filter((s) => s.status !== 'free') : list.slice();
  if (o.sort === 'name') out = out.sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
  else if (o.sort === 'state') out = out.sort((a, b) => (STATE_RANK[a.status] ?? 9) - (STATE_RANK[b.status] ?? 9));
  return out;
};

export const EvStationsBlock: React.FC<{ m: EvModel; onOpenPoint: (key: string) => void; opts?: StationsListOpts }> = ({ m, onOpenPoint, opts }) => (
  m.stations.length > 0 ? (
    <Grid container spacing={2}>
      {orderStations(m.stations, opts).length === 0 && (
        <Grid item xs={12}><Alert severity="info">{t('stations.allFreeHidden', { n: m.stations.length })}</Alert></Grid>
      )}
      {orderStations(m.stations, opts).map((st) => (
        <Grid item xs={12} sm={6} lg={4} key={st.id}>
          <StationCard station={st} pricePerKwh={m.pricePerKwh} onClick={() => onOpenPoint(st.id)} />
        </Grid>
      ))}
    </Grid>
  ) : <Alert severity="info">{t('stations.noStations')}</Alert>
);

export const EvPowerBlock: React.FC<{ m: EvModel }> = ({ m }) => <ParkingPowerChart history={m.history} limitW={m.limitW} />;

export const EvOverview: React.FC<EvOverviewProps> = ({ onOpenPoint, ...props }) => {
  const m = useEvModel(props);
  const { stations, counts, offline, segments, limitW, history, tree, lost, pricePerKwh } = m;
  const kpis = m.kpis;
  return (
    <>
      <Box sx={{ mb: 2 }}>
        <ParkingHero
          charging={counts.charging} total={counts.total} free={counts.free} offline={offline}
          segments={segments} limitW={limitW} lost={lost}
        />
      </Box>
      <Box sx={{ mb: 2 }}><KpiRow items={kpis} /></Box>
      {stations.length > 0 ? (
        <Grid container spacing={2} sx={{ mb: 2 }}>
          {stations.map((st) => (
            <Grid item xs={12} sm={6} lg={4} key={st.id}>
              <StationCard station={st} pricePerKwh={pricePerKwh} onClick={() => onOpenPoint(st.id)} />
            </Grid>
          ))}
        </Grid>
      ) : (
        <Alert severity="info" sx={{ mb: 2 }}>{t('stations.noStations')}</Alert>
      )}
      <Box sx={{ mb: 2 }}><ParkingPowerChart history={history} limitW={limitW} /></Box>
      <Box sx={{ mb: 2 }}><Card><EnergyFlowTree data={tree} /></Card></Box>
    </>
  );
};
