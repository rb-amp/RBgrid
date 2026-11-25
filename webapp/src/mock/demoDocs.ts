/**
 * The documentation build of the demo (`REACT_APP_DEMO_DOCS=true`, `npm run build:demo-docs`): the
 * screenshots for the app's docs need screens of hub firmware the public demo may not show yet (the
 * public demo names published firmware only). Never deployed: it only feeds the docs shots.
 *
 * On the «home» site it adds: two remote nodes (one online with two points, one offline), the car
 * charger measured inside the garage sub-panel, a broker CA on mqtts, and an hour yesterday with
 * energy from a break in measurement (quality bit 12). As hub 1.2.0 it adds dynamic prices.
 */
export const demoDocs = (): boolean => process.env.REACT_APP_DEMO_DOCS === 'true';

/** The hub firmware the docs build stands for: 1.2.0 (dynamic prices, mock/demoPrices); `?hub=1.1.8`
 *  gives the 1.1.8 hub the earlier frames were shot on. */
export const docsHubVersion = (): string =>
  (typeof window !== 'undefined' && /[?&]hub=1\.1\.8\b/.test(window.location.search) ? '1.1.8' : '1.2.0');
export const docsDynPrices = (): boolean => demoDocs() && docsHubVersion() === '1.2.0';

/** Hours back of the closed hour that carries the break in measurement (yesterday evening). */
export const DOCS_OUTAGE_HOURS_BACK = 20;

/** Pairing window state (the «pair» action opens it for 120 s, as on the hub). */
let pairUntil = 0;
export const docsPairing = {
  open(): void { pairUntil = Date.now() + 120_000; },
  close(): void { pairUntil = 0; },
  leftS(): number { return Math.max(0, Math.round((pairUntil - Date.now()) / 1000)); },
};
