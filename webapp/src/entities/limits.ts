/**
 * One rule for «approaching the limit» (role audit 22.11 B7, round 2 business N1/N2): the input's live
 * power from this % of its configured limit is a warning on the Overview's limit bar, in «Needs
 * attention» and in Events. The hub has no warning level of its own (root ui-round-data-asks).
 */
export const APPROACH_PCT = 80;

/** % of the limit, or null when there is no limit or no reading. */
export const limitPct = (powerW: number | null | undefined, limitW: number | null | undefined): number | null =>
  powerW != null && limitW && limitW > 0 ? (Math.max(0, powerW) / limitW) * 100 : null;
