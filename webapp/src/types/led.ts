/**
 * Hub status LEDs (fw 1.3.0, root LED_INDICATION.md §5): GET /api/v1/led, POST {mode?, identify?, ack?}.
 * Measured on S3 .157 25.11:
 * {"mode":"full","leds":{"green_gpio":4,"red_gpio":5},"green":{"id":"G7","state":"normal operation","pattern":"heartbeat"},
 *  "red":null,"process":null,"events_pending":false}
 */
export type LedMode = 'full' | 'quiet' | 'off';
/** What one layer shows now: a registry id (G1…G7, R1…R8, P1…P4), the hub's English words, the pattern. */
export interface LedShow { id: string; state: string; pattern: string }
export interface LedState {
  mode: LedMode | string;
  leds: { green_gpio: number; red_gpio: number } | null;
  green: LedShow | null;
  red: LedShow | null;
  process: LedShow | null;
  /** An event (unexpected restart R7, firmware rolled back R8) waits for an acknowledgement. */
  events_pending: boolean;
}
export interface LedPatch { mode?: LedMode; identify?: number | boolean; ack?: true }

const show = (v: unknown): LedShow | null => {
  if (!v || typeof v !== 'object') return null;
  const o = v as Record<string, unknown>;
  const id = typeof o.id === 'string' ? o.id : '';
  return id ? { id, state: typeof o.state === 'string' ? o.state : '', pattern: typeof o.pattern === 'string' ? o.pattern : '' } : null;
};

export function parseLed(raw: unknown): LedState {
  const b = (raw && typeof raw === 'object' ? raw : {}) as Record<string, unknown>;
  const l = b.leds && typeof b.leds === 'object' ? (b.leds as Record<string, unknown>) : null;
  return {
    mode: typeof b.mode === 'string' ? b.mode : 'full',
    leds: l ? { green_gpio: typeof l.green_gpio === 'number' ? l.green_gpio : -1, red_gpio: typeof l.red_gpio === 'number' ? l.red_gpio : -1 } : null,
    green: show(b.green),
    red: show(b.red),
    process: show(b.process),
    events_pending: b.events_pending === true,
  };
}
