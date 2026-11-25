/**
 * Widget registry (SPEC §2.3): every Overview block with its stable id, catalogue texts, sizes,
 * where it is available and its renderer. Renderers are thin wrappers over the existing blocks:
 * they take the page model from `useDashboardData()` and pass the same props as before.
 */
import { usePeriodTotals } from '../hooks/usePeriodTotals';
import { SetupChecklist } from '../components/dashboard/SetupChecklist';
import { isDemo } from '../services/connection';
import React, { createContext, useContext, useState } from 'react';
import { Box, Button, Grid, IconButton, Stack, ToggleButton, ToggleButtonGroup } from '@mui/material';
import ChevronLeftIcon from '@mui/icons-material/ChevronLeft';
import ChevronRightIcon from '@mui/icons-material/ChevronRight';
import VisibilityIcon from '@mui/icons-material/VisibilityOutlined';
import VisibilityOffIcon from '@mui/icons-material/VisibilityOffOutlined';
import { Card, BlockHeader } from '@rbgrid/design-system';
import { useDashboardData, DashboardModel } from './DashboardData';
import { SiteTraits, evLed, plantLed } from './siteTraits';
import type { Size, WidgetOpts, LayoutDoc } from './layout';
import { PowerHero } from '../components/dashboard/PowerHero';
import { TodayCard } from '../components/dashboard/TodayCard';
import { PointTile } from '../components/dashboard/PointTile';
import { GroupTile } from '../components/dashboard/GroupTile';
import { OverviewEnergyChart } from '../components/dashboard/OverviewEnergyChart';
import { LineLoadTable } from '../components/dashboard/LineLoadTable';
import { PhaseCards } from '../components/dashboard/PhaseCards';
import { AlarmsPanel } from '../components/dashboard/AlarmsPanel';
import { InputLimitHero } from '../components/dashboard/InputLimitHero';
import { PointsTable } from '../components/dashboard/PointsTable';
import { KpiRow, KpiItem } from '../components/dashboard/KpiRow';
import { TariffRibbon } from '../components/dashboard/TariffRibbon';
import { SavingsCard } from '../components/dashboard/SavingsCard';
import { HomeFlowCard } from '../components/flow/HomeFlowCard';
import { EnergyFlowTree } from '../components/flow/EnergyFlowTree';
import { EnergyFlowSankey } from '../components/flow/EnergyFlowSankey';
import { EnergyFlowBalance } from '../components/flow/EnergyFlowBalance';
import { solarShare } from '../components/flow/flowFrame';
import { useEvModel, EvModel, EvHeroBlock, EvKpiBlock, EvStationsBlock, EvPowerBlock } from '../components/stations/EvOverview';
import { effectiveInputLimitW } from '../entities/gridSystem';
import { apiUnitId, isMainKey, signedPowerW, isStorageRole } from '../entities/siteModel';
import { unitMetricValue, UnitEntityGroup } from '../entities/model';
import {
  buildIndustrialTree, buildEvTree, lineUnits, topLevelLines, phaseLoads, unmeteredW, isGenerationUnit, deviceSets, DeviceSet,
} from '../pages/overviewData';
import { useLocalStorage } from '../hooks/useLocalStorage';
import { useDeviceGroups } from '../hooks/useDeviceGroups';
import { fullPower } from '../entities/siteModel';
import { useLensFormat } from '../economics/MoneyLens';
import { nf, zoneLabel, qCharacter } from '../components/common/ruFormat';
import { useDsTokens } from '../theme/useDsTokens';
import { t } from '../i18n';

/** One option of a block (SPEC §1.5): a choice from a list, or a metering point. */
export type SettingDef =
  | { key: string; labelKey: string; kind: 'choice'; options: { value: string | boolean; labelKey: string }[] }
  | { key: string; labelKey: string; kind: 'point' };

export interface WidgetDef {
  id: string;
  /** i18n keys of the catalogue card. */
  titleKey: string;
  descKey: string;
  sizes: Size[];
  defaultSize: Size;
  /** Can be placed more than once (a point tile, a chart). */
  multiple?: boolean;
  available(t: SiteTraits): boolean;
  /** Options when placed (a point tile needs its point). */
  defaults?: WidgetOpts;
  settings?: SettingDef[];
  Render: React.FC<{ opts: WidgetOpts; size: Size }>;
}

// ---------- shared bits ----------

const ratedOf = (m: DashboardModel) => (g: UnitEntityGroup) => {
  const c = m.unitsCfg.forGroup(g);
  return isMainKey(g.unitKey) || c?.role === 'MAIN_SUPPLY' || isGenerationUnit(g, m.unitsCfg) ? undefined : c?.ratedPowerW;
};

/** The points on the Overview the user pinned / hid (SPEC §2.8), and how to change them. */
export interface PointsPrefs { pins: string[]; hide: string[]; order: string[]; set(p: Partial<Pick<LayoutDoc, 'pins' | 'hide' | 'order'>>): void; editing: boolean }
const PrefsCtx = createContext<PointsPrefs>({ pins: [], hide: [], order: [], set: () => undefined, editing: false });
export const PointsPrefsProvider = PrefsCtx.Provider;

/** Car-park blocks share one model (one parking-history fetch for all of them). */
const EvCtx = createContext<EvModel | null>(null);
export const EvDataProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const m = useDashboardData();
  const ev = useEvModel({
    units: m.data.units, join: m.unitsCfg, ev: m.ev, grid: m.frame.grid, gridHasDirection: m.frame.gridHasDirection,
    pricePerKwh: m.pricePerKwh, formatMoney: (v) => m.mf.money(v), lost: m.stale,
  });
  return <EvCtx.Provider value={ev}>{children}</EvCtx.Provider>;
};
const useEv = (): EvModel | null => useContext(EvCtx);

/** A block whose own component has no card gets the standard one with a title. */
const Titled: React.FC<{ title: string; action?: React.ReactNode; children: React.ReactNode }> = ({ title, action, children }) => (
  <Card>
    <BlockHeader title={title} actions={action} />
    {children}
  </Card>
);

// ---------- renderers ----------

const NowW: React.FC = () => {
  const m = useDashboardData();
  const { main } = m;
  return (
    <PowerHero frame={m.frame} voltage={m.voltage} phaseVoltages={m.busPhaseV} freq={m.freq} anyPresent={m.data.units.some((u) => u.present)}
      phaseSignMismatch={m.phaseSignMismatch} present={main?.present ?? false} staleS={main?.present && main.stale ? main.ageS ?? 6 : null}
      // rbamp-dev (root 86f39446): the hub treats < 0,5 W (and < 2 mA) as no flow.
      idleW={main?.pResW != null ? Math.max(0.05, 2 * main.pResW) : 0.5} />
  );
};

const TodayW: React.FC = () => {
  const m = useDashboardData();
  const { totals, oss, monthTot, liveTariff, tariffActive, dynNow, rate } = m;
  const yTot = usePeriodTotals(m.inputSeries, 'yesterday');
  return (
    <TodayCard
      yesterday={yTot && !yTot.loading && yTot.n > 0 ? { wh: yTot.inWh, cost: oss ? null : yTot.cost } : null}
      month={monthTot && !monthTot.loading && monthTot.n > 0 ? { wh: monthTot.inWh, cost: oss ? null : monthTot.cost } : null}
      energyInWh={totals?.e_day_in ?? null}
      energyOutWh={totals?.e_day_out ?? null}
      timeSynced={m.timeSynced}
      costDay={oss ? null : totals?.cost_day ?? null}
      revDay={oss ? null : totals?.rev_day ?? null}
      zoneName={tariffActive ? (dynNow ? t('cards.dynamicPrice') : zoneLabel(liveTariff?.zone)) : null}
      zoneCaption={dynNow ? t('cards.priceNow') : undefined}
      rateText={tariffActive ? `${m.mf.rate(rate.buy)}${rate.gross ? ` ${t('cards.rateGross')}` : ''}` : null}
      oss={oss}
      placeholderTariff={liveTariff?.placeholder === true}
    />
  );
};

const InsightsW: React.FC<{ opts: WidgetOpts }> = ({ opts }) => {
  const m = useDashboardData();
  const { main, totals, oss } = m;
  return (
    <SavingsCard lost={m.stale} industrial={plantLed(m.traits)} collapsed={opts.collapsed !== false}
      hubBase={m.data.system.totals ? { w: m.data.system.totals.base_w, nights: m.data.system.totals.base_w_nights } : null}
      groups={m.data.units}
      join={m.unitsCfg}
      mainId={main ? (m.unitsCfg.forGroup(main)?.hex ?? apiUnitId(main.unitKey)) : null}
      inputDayWh={totals?.e_day_in ?? (main ? unitMetricValue(main, 'energy_day_in') : null)}
      costDay={totals?.cost_day ?? null}
      exportDayWh={totals?.e_day_out ?? null}
      revDay={oss ? null : totals?.rev_day ?? null}
      rate={m.rate}
      fmt={m.mf.money}
    />
  );
};

const PhasesW: React.FC = () => {
  const m = useDashboardData();
  return <PhaseCards groups={m.data.units} join={m.unitsCfg} lost={m.stale} grid={m.grid} nominalV={m.grid.nominalV} measuredLegs={m.measuredLegs} />;
};

type FlowView = 'home' | 'flows' | 'tree';
const FlowW: React.FC<{ opts: WidgetOpts }> = ({ opts }) => {
  const m = useDashboardData();
  const view: FlowView = opts.view === 'flows' || opts.view === 'tree' ? opts.view : 'home';
  const [flowMode, setFlowMode] = useLocalStorage<'sankey' | 'balance'>('rbgrid.analytics_flow', 'sankey');
  const { main, frame, unitsCfg } = m;
  if (view === 'tree') {
    const ev = evLed(m.traits);
    const tree = ev
      ? buildEvTree(m.data.units, unitsCfg, frame.grid, frame.gridHasDirection, m.stale)
      : buildIndustrialTree(m.data.units, unitsCfg, frame.grid, frame.gridHasDirection, m.stale, frame.groups, m.grid);
    return <Card><EnergyFlowTree data={tree} /></Card>;
  }
  if (view === 'flows') {
    return (
      <Titled title={t('dashboard.energyFlow')} action={(
        <ToggleButtonGroup size="small" exclusive value={flowMode} onChange={(_, v) => v && setFlowMode(v)} aria-label={t('dashboard.flowSchemeAria')}>
          <ToggleButton value="sankey">{t('dashboard.flowSankey')}</ToggleButton>
          <ToggleButton value="balance">{t('dashboard.flowBalance')}</ToggleButton>
        </ToggleButtonGroup>
      )}>
        {flowMode === 'sankey' ? <EnergyFlowSankey frame={frame} /> : <EnergyFlowBalance frame={frame} />}
      </Titled>
    );
  }
  return (
    <HomeFlowCard frame={frame} phases={main?.phases && main.phases.length > 1 ? {
      gridW: main.present ? m.inputPowerW : null,
      legs: main.phases.map((r) => ({ ph: r.ph, p: r.p ?? null, i: r.i ?? null })),
      // Sources stay on their phase too; a point inside another (fw 1.1.8 `inside`) is not a second line.
      lines: topLevelLines(m.data.units, unitsCfg).map((g) => ({
        id: g.unitKey, name: m.nameFor(g), phase: unitsCfg.forGroup(g)?.phase,
        kind: isGenerationUnit(g, unitsCfg) ? 'gen' as const : isStorageRole(unitsCfg.forGroup(g)?.role) ? 'storage' as const : 'load' as const,
        w: g.present ? signedPowerW(g) : null, onOpen: () => m.openPoint(g.unitKey),
      })),
      lost: m.stale,
    } : null} />
  );
};

const HoursW: React.FC = () => {
  const m = useDashboardData();
  const { main, totals } = m;
  return (
    <OverviewEnergyChart hasGeneration={m.frame.solar != null}
      liveEIn={(main?.present ? unitMetricValue(main, 'energy_in') : null) ?? totals?.e_in ?? null}
      liveEOut={(main?.present ? unitMetricValue(main, 'energy_out') : null) ?? totals?.e_out ?? null}
      {...(m.mainHistId ? { unitId: m.mainHistId, seriesId: m.inputSeries } : {})}
      genUnitId={m.genHistId} liveGenOut={m.genLiveOut} />
  );
};

/** A point set's id in pins / hide: the lead point's HEX4. */
const setKey = (m: DashboardModel, set: DeviceSet): string => m.unitsCfg.forGroup(set.lead)?.hex ?? set.lead.unitKey;
const setPower = (m: DashboardModel, set: DeviceSet): number =>
  set.units.reduce((s, g) => s + (g.present ? Math.abs(fullPower(g, m.unitsCfg.forGroup(g)).w ?? 0) : 0), 0);

/**
 * Point tiles (SPEC §2.8): one tile per appliance (per-phase points of one machine fold into one).
 * The order is the user's and stays put (operator 22.11: re-sorting by power on every frame made the
 * tiles «rotate»): set in «Customise» with ← → or by dragging a tile; points not placed yet follow
 * in the hub's order. Hidden tiles stay in the flow, the tables and Points. Every tile action lives
 * in «Customise» only.
 */
const TilesW: React.FC<{ opts: WidgetOpts; size: Size }> = ({ opts }) => {
  const m = useDashboardData();
  const prefs = useContext(PrefsCtx);
  const ds = useDsTokens();
  const { names: groupNames } = useDeviceGroups();
  const [dragKey, setDragKey] = useState<string | null>(null);
  // The hub's own order (stable across frames), not the power-sorted one.
  const lines = lineUnits(m.data.units, m.unitsCfg);
  const sets = opts.group === false ? lines.map((g) => ({ lead: g, units: [g] } as DeviceSet)) : deviceSets(lines, m.unitsCfg);
  const nameOfSet = (st: DeviceSet) => (st.units.length > 1 && st.groupId != null ? groupNames.get(st.groupId) : undefined) || m.nameFor(st.lead);
  const hidden = new Set(prefs.hide);
  const editing = prefs.editing;
  let list = sets.filter((st) => editing || !hidden.has(setKey(m, st)));
  if (opts.idle === 'hide' && !editing) list = list.filter((st) => setPower(m, st) >= 1);
  const rank = (st: DeviceSet) => { const i = prefs.order.indexOf(setKey(m, st)); return i < 0 ? 1e6 + sets.indexOf(st) : i; };
  list = [...list].sort(opts.sort === 'name' ? (a, b) => nameOfSet(a).localeCompare(nameOfSet(b)) : (a, b) => rank(a) - rank(b));
  const manual = opts.sort !== 'name';
  const keys = list.map((st) => setKey(m, st));
  const moveTo = (key: string, to: number) => {
    const cur = keys.filter((k) => k !== key);
    cur.splice(Math.max(0, Math.min(cur.length, to)), 0, key);
    prefs.set({ order: cur });
  };
  return (
    <Grid container spacing={2}>
      {list.map((st, idx) => {
        const key = setKey(m, st);
        const gen = isGenerationUnit(st.lead, m.unitsCfg);
        const isHidden = hidden.has(key);
        const tile = st.units.length > 1 ? (
          <GroupTile name={nameOfSet(st)} units={st.units} stale={m.stale} generation={gen} cfgOf={(g) => m.unitsCfg.forGroup(g)}
            partLabel={(g) => { const c = m.unitsCfg.forGroup(g); return c?.phase ? `L${c.phase} · ${m.nameFor(g)}` : m.nameFor(g); }}
            onOpen={m.openPoint} onOpenAppliance={st.groupId != null ? () => m.navigate(`/appliance/${st.groupId}`, { state: { from: 'dashboard' } }) : undefined} />
        ) : (
          <PointTile group={st.lead} name={m.nameFor(st.lead)} stale={m.stale} generation={gen} cfg={m.unitsCfg.forGroup(st.lead)} onClick={() => m.openPoint(st.lead.unitKey)} />
        );
        return (
          <Grid item xs={12} sm={6} lg={4} xl={3} key={key}>
            {editing ? (
              // Live inside the block frame (its content is otherwise not pressable while editing).
              <Box draggable={manual} onDragStart={(e) => { e.stopPropagation(); e.dataTransfer.setData('text/plain', key); setDragKey(key); }}
                onDragOver={(e) => { if (dragKey) { e.preventDefault(); e.stopPropagation(); } }}
                onDrop={(e) => { if (dragKey) { e.preventDefault(); e.stopPropagation(); moveTo(dragKey, idx); setDragKey(null); } }}
                onDragEnd={() => setDragKey(null)}
                sx={{ pointerEvents: 'auto', cursor: manual ? 'grab' : 'default', borderRadius: 2, outline: `1px dashed ${ds.border}`, outlineOffset: 3, opacity: dragKey === key ? 0.4 : 1 }}>
                <Box sx={{ opacity: isHidden ? 0.4 : 1, pointerEvents: 'none' }}>{tile}</Box>
                <Stack direction="row" alignItems="center" justifyContent="space-between" sx={{ mt: 0.5 }}>
                  <Stack direction="row">
                    {manual && <IconButton size="small" disabled={idx === 0} onClick={() => moveTo(key, idx - 1)} aria-label={t('board.tileLeft', { name: nameOfSet(st) })}><ChevronLeftIcon fontSize="small" /></IconButton>}
                    {manual && <IconButton size="small" disabled={idx === list.length - 1} onClick={() => moveTo(key, idx + 1)} aria-label={t('board.tileRight', { name: nameOfSet(st) })}><ChevronRightIcon fontSize="small" /></IconButton>}
                  </Stack>
                  <Button size="small" startIcon={isHidden ? <VisibilityOffIcon fontSize="small" /> : <VisibilityIcon fontSize="small" />}
                    onClick={() => prefs.set({ hide: isHidden ? prefs.hide.filter((x) => x !== key) : [...prefs.hide, key] })}
                    sx={{ textTransform: 'none' }} aria-pressed={!isHidden}>
                    {isHidden ? t('board.tileHidden') : t('board.tileShown')}
                  </Button>
                </Stack>
              </Box>
            ) : tile}
          </Grid>
        );
      })}
    </Grid>
  );
};

/** One point — or a whole appliance (`g:<group id>`, its phases summed; role audit 22.11 H-02). */
const TileW: React.FC<{ opts: WidgetOpts }> = ({ opts }) => {
  const m = useDashboardData();
  const { names: groupNames } = useDeviceGroups();
  const unit = String(opts.unit ?? '');
  if (unit.startsWith('g:')) {
    const gid = Number(unit.slice(2));
    const st = deviceSets(lineUnits(m.data.units, m.unitsCfg), m.unitsCfg).find((x) => x.groupId === gid && x.units.length > 1);
    if (!st) return null;
    return (
      <GroupTile name={groupNames.get(gid) || m.nameFor(st.lead)} units={st.units} stale={m.stale} generation={isGenerationUnit(st.lead, m.unitsCfg)}
        cfgOf={(g) => m.unitsCfg.forGroup(g)} partLabel={(g) => { const c = m.unitsCfg.forGroup(g); return c?.phase ? `L${c.phase} · ${m.nameFor(g)}` : m.nameFor(g); }}
        onOpen={m.openPoint} onOpenAppliance={() => m.navigate(`/appliance/${gid}`, { state: { from: 'dashboard' } })} />
    );
  }
  const g = m.data.units.find((u) => u.unitKey === unit || m.unitsCfg.forGroup(u)?.hex === unit);
  if (!g) return null;
  return <PointTile group={g} name={m.nameFor(g)} stale={m.stale} generation={isGenerationUnit(g, m.unitsCfg)} cfg={m.unitsCfg.forGroup(g)} onClick={() => m.openPoint(g.unitKey)} />;
};

const AlarmsW: React.FC = () => {
  const m = useDashboardData();
  return <AlarmsPanel groups={m.data.units} title={t('dashboard.attention')} nameOf={m.nameFor} ratedOf={ratedOf(m)} input={{ powerW: m.inputPowerW, limitW: m.unitsCfg.input.ratedPowerW }} />;
};

const TariffW: React.FC = () => {
  const m = useDashboardData();
  return <TariffRibbon currentZone={m.tariffActive ? m.liveTariff?.zone ?? null : null} gross={m.rate.gross} />;
};

const KpiW: React.FC = () => {
  const m = useDashboardData();
  const lens = useLensFormat();
  const ds = useDsTokens();
  const { main, totals, oss, inputPowerW, inputEstimated, frame } = m;
  const hasGen = frame.solar != null;
  const pf = main?.present ? unitMetricValue(main, 'power_factor') : null;
  const inNow = lens.power(main?.present ? inputPowerW : null);
  const took = lens.energy(totals?.e_day_in ?? null, false, oss ? null : totals?.cost_day ?? null);
  const gave = lens.energy(totals?.e_day_out ?? null, true, oss ? null : totals?.rev_day ?? null);
  const kpis: KpiItem[] = [
    { label: (inputPowerW ?? 0) < 0 && main?.present ? t('cards.heroExporting') : t('dashboard.kpiInputNow'), value: `${inputEstimated && main?.present ? '≈ ' : ''}${inNow.main}`, sub: main && !main.present ? t('dashboard.noLink') : inNow.sub ?? undefined },
    { label: t('dashboard.kpiImportedToday'), value: took.main, sub: m.timeSynced ? took.sub ?? undefined : t('dashboard.timeNotSynced') },
  ];
  if (hasGen) kpis.push({ label: t('dashboard.kpiExportedToday'), value: gave.main, sub: gave.sub ?? undefined, valueColor: gave.money ? 'var(--flow-export)' : undefined, subColor: 'var(--flow-export)' });
  const cost = oss ? null : totals?.cost_day ?? null;
  const rev = oss ? null : totals?.rev_day ?? null;
  if (lens.moneyMode && cost != null && hasGen && rev != null) {
    kpis.push({ label: t('dashboard.kpiNetToday'), value: m.mf.money(cost - rev), sub: t('dashboard.kpiNetSub') });
  } else if (!lens.moneyMode || !took.money) {
    kpis.push({ label: t('dashboard.kpiCostToday'), value: oss ? '—' : m.mf.money(cost) });
  }
  if (hasGen) kpis.push({ label: t('dashboard.kpiAutonomy'), value: `${Math.round(solarShare(frame) * 100)}%`, sub: t('dashboard.fromSolar'), valueColor: ds.solarInk });
  else if (pf != null) { const ch = qCharacter(main?.present ? unitMetricValue(main, 'reactive_power') : null, inputPowerW); kpis.push({ label: t('dashboard.kpiInputPf'), value: `${nf(pf, 2)}${ch ? ` ${ch}` : ''}` }); }
  return <KpiRow items={kpis} />;
};

const PointsTableW: React.FC = () => {
  const m = useDashboardData();
  return (
    <Titled title={t('dashboard.points')}>
      <PointsTable groups={m.data.units} join={m.unitsCfg} lost={m.stale}
        inputToday={{ wh: m.totals?.e_day_in ?? null, money: m.oss ? null : m.totals?.cost_day ?? null }} />
    </Titled>
  );
};

const LinesTableW: React.FC = () => {
  const m = useDashboardData();
  return (
    <Titled title={t('dashboard.loadLines')}>
      <LineLoadTable groups={m.units} join={m.unitsCfg} inputPowerW={m.inputPowerW} showRemainder={false} />
    </Titled>
  );
};

const InputLimitW: React.FC = () => {
  const m = useDashboardData();
  const { main, unitsCfg, grid } = m;
  const phase = unitsCfg.input.phase || (grid.kind === '3P' ? 4 : undefined);
  return (
    <InputLimitHero
      powerW={m.inputPowerW}
      estimated={m.inputEstimated}
      present={main?.present ?? false}
      // The input can't give more than its breaker passes (INP-R06) — the smaller of the two.
      limitW={effectiveInputLimitW(unitsCfg.input.ratedPowerW, unitsCfg.input.maxCurrentA, phase, grid.nominalV)}
      breakerA={unitsCfg.input.maxCurrentA}
      breakerW={unitsCfg.input.maxCurrentA ? effectiveInputLimitW(undefined, unitsCfg.input.maxCurrentA, phase, grid.nominalV) : undefined}
      configLoaded={unitsCfg.loaded}
      freq={m.freq}
      phases={phaseLoads(m.data.units, unitsCfg, grid)}
      unmeteredW={unmeteredW(m.data.units, unitsCfg, m.inputPowerW)}
      lost={m.stale}
      settingsHref={main ? `/point/${unitsCfg.forGroup(main)?.hex ?? apiUnitId(main.unitKey) ?? main.unitKey}?setup` : '/devices'}
    />
  );
};

/** «Left to set up» — not in the demo (its sites are set up). */
const SetupW: React.FC = () => (isDemo() ? null : <SetupChecklist />);

const EvNowW: React.FC = () => { const ev = useEv(); return ev ? <EvHeroBlock m={ev} /> : null; };
const EvKpiW: React.FC = () => { const ev = useEv(); return ev ? <EvKpiBlock m={ev} /> : null; };
const EvStationsW: React.FC<{ opts: WidgetOpts }> = ({ opts }) => {
  const ev = useEv(); const m = useDashboardData();
  return ev ? <EvStationsBlock m={ev} onOpenPoint={m.openPoint} opts={{ sort: opts.sort === 'name' || opts.sort === 'state' ? opts.sort : 'config', free: opts.free === 'hide' ? 'hide' : 'show' }} /> : null;
};
const EvPowerW: React.FC = () => { const ev = useEv(); return ev ? <EvPowerBlock m={ev} /> : null; };

// ---------- the registry ----------

const always = () => true;
/** Car-park blocks where there is a car park (2+ stations, or a hub set to «ev») — a home's one charger
 *  is not «parking load balance» (role audit 22.11). */
const evAvail = (s: SiteTraits): boolean => s.evStations >= 2 || (s.evStations > 0 && s.profileHint === 'ev');

export const WIDGETS: WidgetDef[] = [
  { id: 'setup', titleKey: 'board.w_setup', descKey: 'board.w_setup_d', sizes: ['half', 'full'], defaultSize: 'full', available: always, Render: SetupW },
  { id: 'now', titleKey: 'board.w_now', descKey: 'board.w_now_d', sizes: ['half', 'full'], defaultSize: 'half', available: (s) => s.hasInput, Render: NowW },
  { id: 'input-limit', titleKey: 'board.w_inputLimit', descKey: 'board.w_inputLimit_d', sizes: ['full'], defaultSize: 'full', available: (s) => s.hasInputLimit, Render: InputLimitW },
  { id: 'today', titleKey: 'board.w_today', descKey: 'board.w_today_d', sizes: ['third', 'half'], defaultSize: 'half', available: always, Render: TodayW },
  { id: 'insights', titleKey: 'board.w_insights', descKey: 'board.w_insights_d', sizes: ['half', 'full'], defaultSize: 'full', available: (s) => s.money,
    defaults: { collapsed: true },
    settings: [{ key: 'collapsed', labelKey: 'board.s_insights', kind: 'choice', options: [{ value: true, labelKey: 'board.s_collapsed' }, { value: false, labelKey: 'board.s_expanded' }] }],
    Render: InsightsW },
  { id: 'phases', titleKey: 'board.w_phases', descKey: 'board.w_phases_d', sizes: ['half', 'full'], defaultSize: 'full', available: (s) => s.phases > 1, Render: PhasesW },
  { id: 'flow', titleKey: 'board.w_flow', descKey: 'board.w_flow_d', sizes: ['full'], defaultSize: 'full', available: always,
    multiple: true, defaults: { view: 'home' },
    settings: [{ key: 'view', labelKey: 'board.s_flowView', kind: 'choice', options: [
      { value: 'home', labelKey: 'board.s_flowHome' }, { value: 'flows', labelKey: 'board.s_flowFlows' }, { value: 'tree', labelKey: 'board.s_flowTree' }] }],
    Render: FlowW },
  { id: 'hours', titleKey: 'board.w_hours', descKey: 'board.w_hours_d', sizes: ['half', 'full'], defaultSize: 'full', available: (s) => s.hasInput, Render: HoursW },
  { id: 'tiles', titleKey: 'board.w_tiles', descKey: 'board.w_tiles_d', sizes: ['full'], defaultSize: 'full', available: (s) => s.lineCount > 0,
    settings: [
      { key: 'sort', labelKey: 'board.s_sort', kind: 'choice', options: [{ value: 'manual', labelKey: 'board.s_sortManual' }, { value: 'name', labelKey: 'board.s_sortName' }] },
      { key: 'group', labelKey: 'board.s_group', kind: 'choice', options: [{ value: true, labelKey: 'board.s_groupOn' }, { value: false, labelKey: 'board.s_groupOff' }] },
      { key: 'idle', labelKey: 'board.s_idle', kind: 'choice', options: [{ value: 'show', labelKey: 'board.s_idleShow' }, { value: 'hide', labelKey: 'board.s_idleHide' }] },
    ],
    Render: TilesW },
  { id: 'tile', titleKey: 'board.w_tile', descKey: 'board.w_tile_d', sizes: ['third'], defaultSize: 'third', multiple: true, available: (s) => s.lineCount > 0,
    settings: [{ key: 'unit', labelKey: 'board.s_point', kind: 'point' }],
    Render: TileW },
  { id: 'alarms', titleKey: 'board.w_alarms', descKey: 'board.w_alarms_d', sizes: ['third', 'half'], defaultSize: 'third', available: always, Render: AlarmsW },
  { id: 'tariff', titleKey: 'board.w_tariff', descKey: 'board.w_tariff_d', sizes: ['third', 'half', 'full'], defaultSize: 'half', available: (s) => s.hasTariff, Render: TariffW },
  { id: 'kpi', titleKey: 'board.w_kpi', descKey: 'board.w_kpi_d', sizes: ['full'], defaultSize: 'full', available: always, Render: KpiW },
  { id: 'points-table', titleKey: 'board.w_pointsTable', descKey: 'board.w_pointsTable_d', sizes: ['full'], defaultSize: 'full', available: (s) => s.lineCount > 0, Render: PointsTableW },
  { id: 'lines-table', titleKey: 'board.w_linesTable', descKey: 'board.w_linesTable_d', sizes: ['full'], defaultSize: 'full', available: (s) => s.lineCount > 0, Render: LinesTableW },
  { id: 'ev-now', titleKey: 'board.w_evNow', descKey: 'board.w_evNow_d', sizes: ['full'], defaultSize: 'full', available: evAvail, Render: EvNowW },
  { id: 'ev-kpi', titleKey: 'board.w_evKpi', descKey: 'board.w_evKpi_d', sizes: ['full'], defaultSize: 'full', available: evAvail, Render: EvKpiW },
  { id: 'ev-stations', titleKey: 'board.w_evStations', descKey: 'board.w_evStations_d', sizes: ['full'], defaultSize: 'full', available: evAvail,
    settings: [
      { key: 'sort', labelKey: 'board.s_sort', kind: 'choice', options: [{ value: 'config', labelKey: 'board.s_sortConfig' }, { value: 'name', labelKey: 'board.s_sortName' }, { value: 'state', labelKey: 'board.s_sortState' }] },
      { key: 'free', labelKey: 'board.s_free', kind: 'choice', options: [{ value: 'show', labelKey: 'board.s_freeShow' }, { value: 'hide', labelKey: 'board.s_freeHide' }] },
    ],
    Render: EvStationsW },
  { id: 'ev-power', titleKey: 'board.w_evPower', descKey: 'board.w_evPower_d', sizes: ['half', 'full'], defaultSize: 'full', available: evAvail, Render: EvPowerW },
];

const BY_ID = new Map(WIDGETS.map((w) => [w.id, w]));
export const widgetDef = (id: string): WidgetDef | undefined => BY_ID.get(id);
export const knownWidget = (id: string): boolean => BY_ID.has(id);
export const widgetSizes = (id: string): Size[] => BY_ID.get(id)?.sizes ?? ['full'];
export const usesEv = (ids: string[]): boolean => ids.some((id) => id.startsWith('ev-'));
