/**
 * Point nesting (hub fw 1.1.8, root point-nesting): units-config `inside` names the point a load is
 * measured inside — an EV charger metered on its own inside a metered garage sub-panel. Its power is
 * already in the parent's reading, so a sum of lines counts it once: through the parent. One rule
 * for every sum of lines as a share of the input (the remainder, coverage, phases, the flow, the
 * balance, bill shares, History «Total and parts»).
 */
import { hexKey } from './siteModel';

export interface NestingConfig {
  hex?: string;
  inside?: string;
}

/**
 * The keys among `keys` measured inside another of `keys` — directly or through points outside the
 * set (A ⊃ B ⊃ C with only A and C selected: C is still in A's reading). The chain is followed up
 * through `cfgOf(parentHex)` (unit keys are HEX4); a point with no ancestor in the set stays top level.
 */
export function nestedKeys(keys: string[], cfgOf: (key: string) => NestingConfig | undefined): Set<string> {
  const hexOf = (k: string) => cfgOf(k)?.hex ?? hexKey(k);
  const present = new Set(keys.map(hexOf).filter((h): h is string => !!h));
  return new Set(keys.filter((k) => {
    const self = hexOf(k);
    const seen = new Set<string>([self ?? k]);
    let parent = cfgOf(k)?.inside;
    // The hub refuses loops; the guard keeps a stale or hand-made config from spinning.
    for (let depth = 0; parent && depth < 16 && !seen.has(parent); depth++) {
      if (present.has(parent)) return true;
      seen.add(parent);
      parent = cfgOf(parent)?.inside;
    }
    return false;
  }));
}

/** `keys` without the ones nested in another of them: the set whose sum counts every watt once. */
export function topLevelKeys(keys: string[], cfgOf: (key: string) => NestingConfig | undefined): string[] {
  const nested = nestedKeys(keys, cfgOf);
  return keys.filter((k) => !nested.has(k));
}
