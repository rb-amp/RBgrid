/**
 * Dashboard data (UI redesign 2026-11-22, SPEC §2.1): everything the Overview blocks read, computed
 * once per page — the telemetry frame, the flow frame, the units config, the mains system, the
 * input, today's totals, the money lens, the input's history series and the month to date.
 * Blocks take it with `useDashboardData()`; the page owns loading / empty / stale states.
 *
 * Moved from the body of DashboardPage unchanged (the numbers on every block stay the same).
 */
import React, { createContext, useContext, useMemo, useRef } from 'react';
import { useNavigate } from 'react-router-dom';
import { useTelemetryEntities, TelemetryConnState } from '../hooks/useTelemetryEntities';
import { useCapabilities, useConfigShadow } from '../core';
import { isOssBuild } from '../core/schema/capabilities';
import { useTariffPlan } from '../hooks/useTariffPlan';
import { useUnitsConfig, UnitsConfigJoin } from '../hooks/useUnitsConfig';
import { useDeviceGroups } from '../hooks/useDeviceGroups';
import { useGridSystem, GridSystemState } from '../hooks/useGridSystem';
import { usePeriodTotals } from '../hooks/usePeriodTotals';
import { useInputSeries } from '../hooks/useInputSeries';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { buildFlowFrame, FlowFrame } from '../components/flow/flowFrame';
import { buildFlowGroups } from '../components/flow/flowGroups';
import { fullPower, VBUS_KEY, apiUnitId } from '../entities/siteModel';
import { liveRate, LiveRate } from '../economics/costRate';
import { evThresholds, EvThresholds } from '../components/stations/evStatus';
import {
  mainUnit, nameOf, fullNameOf, lineUnits, gridPhaseVoltages, isGenerationUnit, inputDayTotals, todaySeed,
} from '../pages/overviewData';
import { unitMetricValue, UnitEntityGroup, NormalizedTelemetry } from '../entities/model';
import { EvConfig } from '../types/config';
import { deriveSiteTraits, SiteTraits } from './siteTraits';
import type { TelemetrySystemTotals } from '../types/telemetry-units';

export const byPowerDesc = (a: UnitEntityGroup, b: UnitEntityGroup): number => {
  if (a.present !== b.present) return a.present ? -1 : 1;
  return Math.abs(unitMetricValue(b, 'power') ?? 0) - Math.abs(unitMetricValue(a, 'power') ?? 0);
};

/** The raw page state before a frame arrives (loading / error), and the hooks' shared inputs. */
export interface DashboardInputs {
  data: NormalizedTelemetry | null;
  conn: TelemetryConnState;
  lastUpdate: number | null;
  error: string | null;
  authRejected: boolean;
  seqGap: boolean;
  stale: boolean;
  frame: FlowFrame | null;
  unitsCfg: UnitsConfigJoin;
  grid: GridSystemState;
  oss: boolean;
  /** Money can be shown: a live buy rate and not the OSS build. */
  moneyAvailable: boolean;
  rate: LiveRate;
  mf: ReturnType<typeof useMoneyFormat>;
  /** Phases the input reported per phase this session (numbers only). */
  measuredLegs: number[];
  mainHistId: string | null;
  inputSeries: ReturnType<typeof useInputSeries>;
  genHistId: string | null;
  genLiveOut: number | null;
  monthTot: ReturnType<typeof usePeriodTotals>;
  dayTotals: TelemetrySystemTotals | undefined;
  ev: EvThresholds;
  pricePerKwh: number | null;
  /** `system.site_profile` as stored on the hub — a hint for the default layout only. */
  profileHint: unknown;
}

/** The ready model every block reads (a frame is there). */
export interface DashboardModel extends DashboardInputs {
  data: NormalizedTelemetry;
  frame: FlowFrame;
  units: UnitEntityGroup[];
  main: UnitEntityGroup | undefined;
  voltage: number | null;
  busPhaseV: { ph: number; v: number }[];
  freq: number | null;
  phaseSignMismatch: boolean;
  inputPowerW: number | null;
  inputEstimated: boolean;
  totals: TelemetrySystemTotals | undefined;
  liveTariff: TelemetrySystemTotals['tariff'] | undefined;
  dynNow: boolean;
  tariffActive: boolean;
  timeSynced: boolean;
  nameFor: (g: UnitEntityGroup) => string;
  shortNameFor: (g: UnitEntityGroup) => string;
  openPoint: (key: string) => void;
  navigate: ReturnType<typeof useNavigate>;
  /** What is wired to the hub (picks the default layout and the catalogue). */
  traits: SiteTraits;
}

/** All hooks of the Overview, in a fixed order (call once, at the top of the page). */
export function useDashboardInputs(): DashboardInputs {
  const { data, conn, lastUpdate, error, authRejected } = useTelemetryEntities();
  const caps = useCapabilities();
  const unitsCfg = useUnitsConfig();
  const grid = useGridSystem(unitsCfg);
  const { names: groupNames } = useDeviceGroups();
  const sysShadow = useConfigShadow('system');
  const mf = useMoneyFormat(data?.system.totals?.tariff?.currency);
  const { plan: tariffPlan } = useTariffPlan();
  const rate = liveRate(data?.system.totals?.tariff, tariffPlan);
  const oss = caps ? isOssBuild(caps) : false;
  const moneyAvailable = rate.buy != null && !oss;

  const measuredLegs = useRef<number[]>([]);
  const prevSeq = useRef<number | undefined>(undefined);
  let seqGap = false;
  const seq = data?.system.seq;
  if (seq !== undefined) {
    if (prevSeq.current !== undefined && seq - prevSeq.current > 2) seqGap = true;
    prevSeq.current = seq;
  }

  const stale = conn === 'error';
  const frame = useMemo(
    () => (data
      ? buildFlowFrame(data, {
        lost: stale,
        roleByUnit: unitsCfg.roleByUnit,
        conductorsByUnit: unitsCfg.conductorsByUnit,
        nameOf: (g) => nameOf(g, unitsCfg),
        nestingOf: unitsCfg.forGroup,
        nestingOfKey: unitsCfg.forKey,
        groups: buildFlowGroups(data.units.map((u) => {
          const c = unitsCfg.forGroup(u);
          return { key: u.unitKey, groupId: c?.groupId, trunk: c?.trunk, name: nameOf(u, unitsCfg) };
        }), groupNames),
      })
      : null),
    [data, stale, unitsCfg, groupNames],
  );

  const mainEarly = data ? mainUnit(data.units, unitsCfg) : undefined;
  if (mainEarly?.phases?.length) measuredLegs.current = mainEarly.phases.map((p) => p.ph);
  const mainHistId = mainEarly ? (unitsCfg.forGroup(mainEarly)?.hex ?? apiUnitId(mainEarly.unitKey)) : null;
  const inputSeries = useInputSeries(mainHistId);
  const genEarly = data ? lineUnits(data.units, unitsCfg).find((g) => isGenerationUnit(g, unitsCfg) && g.present) : undefined;
  const genHistId = genEarly ? (unitsCfg.forGroup(genEarly)?.hex ?? apiUnitId(genEarly.unitKey)) : null;
  const genLiveOut = genEarly ? unitMetricValue(genEarly, 'energy_out') : null;
  const dayTotals = data ? inputDayTotals(data.system.totals, mainEarly) : undefined;
  const seed = todaySeed(dayTotals);
  const monthSeed = useMemo(() => seed, [seed?.inWh, seed?.outWh, seed?.cost, seed?.revenue]); // eslint-disable-line react-hooks/exhaustive-deps
  const monthTot = usePeriodTotals(inputSeries, 'month', { today: monthSeed });

  const evCfg = sysShadow.reported.ev as unknown as EvConfig | undefined;
  const ev = evThresholds(evCfg);
  const pricePerKwh = evCfg && evCfg.price_per_kwh > 0 ? evCfg.price_per_kwh : null;

  return {
    data: data ?? null, conn, lastUpdate: lastUpdate ?? null, error, authRejected: !!authRejected, seqGap, stale, frame,
    unitsCfg, grid, oss, moneyAvailable, rate, mf, measuredLegs: measuredLegs.current, mainHistId, inputSeries,
    genHistId, genLiveOut, monthTot, dayTotals, ev, pricePerKwh, profileHint: sysShadow.reported.site_profile,
  };
}

/** The ready model from the inputs (pure; no hooks). */
export function buildDashboardModel(i: DashboardInputs & { data: NormalizedTelemetry; frame: FlowFrame }, navigate: ReturnType<typeof useNavigate>): DashboardModel {
  const { data, unitsCfg } = i;
  const units = [...data.units].sort(byPowerDesc);
  const main = mainUnit(data.units, unitsCfg);
  const vbus = data.units.find((u) => u.unitKey === VBUS_KEY);
  // Only a PRESENT unit's reading is a measurement; an absent bus reports v=0 → «—», not «0 В».
  const phaseV = (main?.phases ?? []).map((p) => p.v).filter((v): v is number => v != null);
  const busPhaseV = gridPhaseVoltages(data.units);
  const voltage = (main?.present ? unitMetricValue(main, 'voltage') : null)
    ?? (vbus?.present ? unitMetricValue(vbus, 'voltage') : null)
    ?? (phaseV.length ? phaseV.reduce((a, b) => a + b, 0) / phaseV.length : null);
  // Frequency only when something measures it (older hubs lack freq_src → a present input/bus).
  const freqMeasured = data.system.freqSrc ? data.system.freqSrc === 'measured' : !!(main?.present || vbus?.present);
  const freq = freqMeasured ? data.system.freq ?? null : null;
  const phSum = (main?.phases ?? []).reduce((s, p) => s + (p.p ?? 0), 0);
  const mainP = main ? unitMetricValue(main, 'power') : null;
  const phaseSignMismatch = !!main?.present && mainP != null && Math.abs(mainP) > 50 && Math.abs(phSum) > 50
    && Math.sign(phSum) !== Math.sign(mainP);
  const inputFull = main ? fullPower(main, unitsCfg.forGroup(main)) : null;
  const inputPowerW = inputFull?.w ?? null;
  const inputEstimated = !!inputFull?.estimated && inputPowerW != null;
  const totals = i.dayTotals;
  const liveTariff = totals?.tariff;
  const dynNow = !!liveTariff?.dynamic && !!liveTariff.price_state && liveTariff.price_state !== 'disabled' && liveTariff.price_source !== 'plan';
  const tariffActive = (liveTariff?.buy_rate ?? 0) > 0 || dynNow;
  const tq = data.system.tq;
  return {
    ...i, units, main, voltage, busPhaseV, freq, phaseSignMismatch, inputPowerW, inputEstimated, totals, liveTariff,
    dynNow, tariffActive, timeSynced: tq === undefined || tq === 1,
    nameFor: (g) => fullNameOf(g, unitsCfg),
    shortNameFor: (g) => nameOf(g, unitsCfg),
    openPoint: (key) => navigate(`/point/${key}`, { state: { from: 'dashboard' } }),
    navigate,
    traits: deriveSiteTraits({
      units: data.units, join: unitsCfg, grid: i.grid, measuredLegs: i.measuredLegs, moneyAvailable: i.moneyAvailable,
      tariffActive, dynamicPrice: dynNow, profileHint: i.profileHint,
    }),
  };
}

const Ctx = createContext<DashboardModel | null>(null);

export const DashboardDataProvider: React.FC<{ model: DashboardModel; children: React.ReactNode }> = ({ model, children }) => (
  <Ctx.Provider value={model}>{children}</Ctx.Provider>
);

export function useDashboardData(): DashboardModel {
  const m = useContext(Ctx);
  if (!m) throw new Error('useDashboardData outside DashboardDataProvider');
  return m;
}
