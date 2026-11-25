/**
 * Site traits (SPEC §2.2) — what is wired to the hub, in place of the site scenario (operator
 * 22.11: the scenario is a thing of the demo; a real hub is defined by its connections). They pick
 * the default layout and which blocks the catalogue offers.
 */
import type { UnitEntityGroup } from '../entities/model';
import type { UnitsConfigJoin } from '../hooks/useUnitsConfig';
import type { GridSystem } from '../entities/gridSystem';
import { isStorageRole } from '../entities/siteModel';
import { lineUnits, isGenerationUnit, stationSets, mainUnit } from '../pages/overviewData';

export interface SiteTraits {
  /** 1 · 2 (split) · 3: declared by the hub or measured per phase on the input. */
  phases: 1 | 2 | 3;
  hasInput: boolean;
  hasGeneration: boolean;
  hasStorage: boolean;
  /** EV charging stations (a three-phase charger is one). */
  evStations: number;
  hasInputLimit: boolean;
  /** Metered lines (the input and supply/pseudo points aside). */
  lineCount: number;
  hasTariff: boolean;
  hasDynamicPrice: boolean;
  /** Money can be shown: a tariff and not the OSS build. */
  money: boolean;
  /**
   * The hub's old `system.site_profile`, read only as a hint for the default layout (SPEC §3): the
   * firmware keeps the field and uses it for nothing; hubs that chose «industrial» / «ev» before
   * keep that look. Not offered for change any more.
   */
  profileHint?: 'home' | 'industrial' | 'ev';
}

export interface TraitsInput {
  units: UnitEntityGroup[];
  join: UnitsConfigJoin;
  grid: Pick<GridSystem, 'kind'> & { fromDevice?: boolean };
  measuredLegs: number[];
  moneyAvailable: boolean;
  tariffActive: boolean;
  dynamicPrice: boolean;
  profileHint?: unknown;
}

export function deriveSiteTraits(i: TraitsInput): SiteTraits {
  const lines = lineUnits(i.units, i.join);
  const declared = i.grid.fromDevice ? (i.grid.kind === '3P' ? 3 : i.grid.kind === 'SPLIT' ? 2 : 1) : 1;
  const measured = i.measuredLegs.length;
  const phases = Math.max(declared, measured >= 3 ? 3 : measured === 2 ? 2 : 1) as 1 | 2 | 3;
  return {
    phases,
    hasInput: !!mainUnit(i.units, i.join),
    hasGeneration: lines.some((g) => isGenerationUnit(g, i.join)),
    hasStorage: lines.some((g) => isStorageRole(i.join.forGroup(g)?.role)),
    evStations: stationSets(i.units, i.join).length,
    hasInputLimit: !!(i.join.input.ratedPowerW || i.join.input.maxCurrentA),
    lineCount: lines.length,
    hasTariff: i.tariffActive,
    hasDynamicPrice: i.dynamicPrice,
    money: i.moneyAvailable,
    profileHint: i.profileHint === 'home' || i.profileHint === 'industrial' || i.profileHint === 'ev' ? i.profileHint : undefined,
  };
}

/**
 * The car-park look (SPEC §2.5, condition 2): stations make up at least half of the lines — or the
 * hub was set to «ev» before. A hub hinted «home» / «industrial» keeps its look.
 */
export const evLed = (t: SiteTraits): boolean =>
  t.evStations > 0 && (t.profileHint ? t.profileHint === 'ev' : t.evStations >= t.lineCount / 2);
/**
 * The plant look (SPEC §2.5, condition 3): the hub was set to «industrial» before, or — with no hint —
 * an input limit and many lines. 8, not 5: homes set an input limit too and often meter 5–7 lines
 * (the demo homes did and drew the plant view at 5).
 */
export const PLANT_LINES = 8;
export const plantLed = (t: SiteTraits): boolean =>
  !evLed(t) && (t.profileHint ? t.profileHint === 'industrial' : t.hasInputLimit && t.lineCount >= PLANT_LINES);
