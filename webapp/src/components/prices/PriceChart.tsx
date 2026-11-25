/**
 * PriceChart — the hub's price per 15-min period, today and tomorrow (fw 1.2.0 `resolved[]`):
 * buy (all-in) and sell as steps, a zero line when a price goes negative, «now», and periods the
 * hub prices from its backup or the plan marked under the axis. One hover card per period.
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { HoverLayer } from '../charts/HoverLayer';
import { linePath } from '../charts/chartPath';
import { useDsTokens } from '../../theme/useDsTokens';
import { DeviceClock, formatDeviceTime } from '../../core/deviceTime';
import { ResolvedPrice } from '../../core/dynprice/types';
import { t, intlLocale } from '../../i18n';

const H = 240, M = { t: 12, r: 12, b: 40, l: 64 };

/** 3–6 round ticks covering [lo, hi]. */
export function priceTicks(lo: number, hi: number): number[] {
  if (!(hi > lo)) { hi = lo + 0.1; }
  const raw = (hi - lo) / 4;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? 10 * mag;
  const a = Math.floor(lo / step) * step, b = Math.ceil(hi / step) * step;
  const out: number[] = [];
  for (let v = a; v <= b + step / 2; v += step) out.push(Math.round(v / step) * step);
  return out;
}

export const PriceChart: React.FC<{
  periods: ResolvedPrice[];
  from: number;
  until: number;
  now: number;
  clock: DeviceClock;
  fmt: (v: number) => string;
  /** Short form for the y axis (no «/kWh»), with as many decimals as the tick step needs. */
  fmtAxis?: (v: number, decimals: number) => string;
  /** The primary provider: with Home Assistant as the primary its `ha_push` periods are not «backup». */
  primary?: string;
  showSell: boolean;
}> = ({ periods, from, until, now, clock, fmt, fmtAxis, primary, showSell }) => {
  const ds = useDsTokens();
  // Drawn at the box's own width, so a phone gets readable labels rather than a shrunk desktop chart.
  const box = useRef<HTMLDivElement>(null);
  const [W, setW] = useState(860);
  useEffect(() => {
    const el = box.current;
    if (!el || typeof ResizeObserver === 'undefined') return undefined;
    const ro = new ResizeObserver(([e]) => setW(Math.max(320, Math.round(e.contentRect.width))));
    ro.observe(el);
    return () => ro.disconnect();
  }, [periods.length > 0]); // eslint-disable-line react-hooks/exhaustive-deps
  const geo = useMemo(() => {
    const vals = periods.flatMap((p) => [p.buy, ...(showSell && p.sell != null ? [p.sell] : [])]);
    const lo = Math.min(0, ...vals), hi = Math.max(0, ...vals);
    const ticks = priceTicks(lo, hi);
    const yMin = ticks[0], yMax = ticks[ticks.length - 1];
    const PW = W - M.l - M.r, PH = H - M.t - M.b;
    const xOf = (ts: number) => M.l + ((ts - from) / (until - from || 1)) * PW;
    const yOf = (v: number) => M.t + (1 - (v - yMin) / (yMax - yMin || 1)) * PH;
    const every = W < 600 ? 12 : 3;
    const hours = Array.from({ length: Math.floor((until - from) / 3600) + 1 }, (_, i) => from + i * 3600).filter((ts) => clock.parts(ts).h % every === 0);
    const step = ticks.length > 1 ? ticks[1] - ticks[0] : 0.1;
    const decimals = Math.min(5, Math.max(2, Math.ceil(-Math.log10(step) - 1e-9)));
    return { ticks, xOf, yOf, PW, PH, hours, neg: lo < 0, decimals };
  }, [periods, from, until, clock, showSell, W]);

  if (!periods.length) {
    return <Typography ref={box} color="text.secondary" sx={{ py: 4, textAlign: 'center' }}>{t('prices.noPrices')}</Typography>;
  }
  const { ticks, xOf, yOf, hours, neg, decimals } = geo;
  const fromPrimary = (p: ResolvedPrice) => p.prov === 'source' || (primary === 'ha' && p.prov === 'ha_push');
  // Contiguous runs only: a missing period is a gap, not a line across it.
  const runs: ResolvedPrice[][] = [];
  periods.forEach((p) => { const r = runs[runs.length - 1]; if (r && r[r.length - 1].until === p.from) r.push(p); else runs.push([p]); });
  const path = (rs: ResolvedPrice[], v: (p: ResolvedPrice) => number) => linePath(rs.map((p) => ({ x: xOf(p.from), y: yOf(v(p)) })), 'step', xOf(rs[rs.length - 1].until));
  const sellRuns = showSell ? runs.flatMap((rs) => {
    const out: ResolvedPrice[][] = [];
    rs.forEach((p) => { if (p.sell == null) { out.push([]); return; } const r = out[out.length - 1]; if (r && r.length && r[r.length - 1].until === p.from) r.push(p); else out.push([p]); });
    return out.filter((r) => r.length);
  }) : [];
  const nowX = now >= from && now <= until ? xOf(now) : null;
  const hhmm = (ts: number) => formatDeviceTime(clock, ts, { hour: '2-digit', minute: '2-digit' }, intlLocale());
  const day = (ts: number) => formatDeviceTime(clock, ts, { weekday: 'short', day: 'numeric', month: 'short' }, intlLocale());
  const byFrom = new Map(periods.map((p) => [p.from, p]));
  const baseY = H - M.b;

  return (
    <Box ref={box}>
      <Box component="svg" viewBox={`0 0 ${W} ${H}`} sx={{ width: '100%', height: 'auto', display: 'block' }} role="img" aria-label={t('prices.chartAria')}>
        {ticks.map((v) => (
          <g key={v}>
            <line x1={M.l} x2={W - M.r} y1={yOf(v)} y2={yOf(v)} stroke={ds.chartGrid} strokeWidth={1} />
            <text x={M.l - 6} y={yOf(v) + 4} textAnchor="end" fontSize={11} fill={ds.inkMuted}>{fmtAxis ? fmtAxis(v, decimals) : fmt(v)}</text>
          </g>
        ))}
        {neg && <line x1={M.l} x2={W - M.r} y1={yOf(0)} y2={yOf(0)} stroke={ds.inkMuted} strokeWidth={1} />}
        {hours.map((ts) => {
          const p = clock.parts(ts);
          return (
            <g key={ts}>
              {p.h === 0 && <line x1={xOf(ts)} x2={xOf(ts)} y1={M.t} y2={baseY} stroke={ds.borderStrong} strokeWidth={1} strokeDasharray="3 3" />}
              <text x={xOf(ts)} y={baseY + 14} textAnchor={ts >= until ? 'end' : ts <= from ? 'start' : 'middle'} fontSize={11} fill={ds.inkMuted}>{p.h === 0 ? day(ts) : String(p.h).padStart(2, '0')}</text>
            </g>
          );
        })}
        {/* periods not priced by the primary: a strip under the axis (backup / HA / the plan) */}
        {periods.filter((p) => !fromPrimary(p)).map((p) => (
          <rect key={`v${p.from}`} x={xOf(p.from)} width={Math.max(1, xOf(p.until) - xOf(p.from))} y={baseY + 20} height={5}
            fill={p.prov === 'plan' ? ds.inkMuted : ds.sevWarning} opacity={0.7} />
        ))}
        {runs.map((rs, i) => <path key={`b${i}`} d={path(rs, (p) => p.buy)} fill="none" stroke={ds.primary} strokeWidth={2} strokeLinejoin="round" />)}
        {sellRuns.map((rs, i) => <path key={`s${i}`} d={path(rs, (p) => p.sell as number)} fill="none" stroke={ds.flowExport} strokeWidth={1.5} strokeDasharray="5 3" />)}
        {nowX != null && (
          <g>
            <line x1={nowX} x2={nowX} y1={M.t} y2={baseY} stroke={ds.ink} strokeWidth={1.5} />
            <text x={nowX + 4} y={M.t + 10} fontSize={11} fill={ds.ink}>{t('prices.now')}</text>
          </g>
        )}
        <HoverLayer
          ts={periods.map((p) => p.from)}
          xOf={(ts) => xOf(ts + ((byFrom.get(ts)?.until ?? ts + 900) - ts) / 2)}
          title={(ts) => `${day(ts)} ${hhmm(ts)}–${hhmm(byFrom.get(ts)?.until ?? ts + 900)}`}
          rows={(ts) => {
            const p = byFrom.get(ts);
            if (!p) return [];
            return [
              { label: t('prices.buy'), value: fmt(p.buy), color: ds.primary },
              ...(showSell && p.sell != null ? [{ label: t('prices.sell'), value: fmt(p.sell), color: ds.flowExport }] : []),
              ...(p.raw != null ? [{ label: t('prices.raw'), value: fmt(p.raw), muted: true }] : []),
              { label: t('prices.provLabel'), value: t(`prices.prov.${String(p.prov).split('+')[0]}`), muted: true },
            ];
          }}
          left={M.l} right={W - M.r} top={M.t} bottom={baseY}
          ink={ds.ink} muted={ds.inkMuted} surface={ds.surfaceRaised} border={ds.border}
          ariaLabel={t('prices.chartAria')}
        />
      </Box>
      <Stack direction="row" spacing={2} flexWrap="wrap" useFlexGap sx={{ mt: 0.5 }}>
        <Legend color={ds.primary} label={t('prices.buyLegend')} />
        {showSell && <Legend color={ds.flowExport} label={t('prices.sellLegend')} dashed />}
        {periods.some((p) => !fromPrimary(p) && p.prov !== 'plan') && <Legend color={ds.sevWarning} label={t('prices.stripBackup')} bar />}
        {periods.some((p) => p.prov === 'plan') && <Legend color={ds.inkMuted} label={t('prices.stripPlan')} bar />}
      </Stack>
    </Box>
  );
};

const Legend: React.FC<{ color: string; label: string; dashed?: boolean; bar?: boolean }> = ({ color, label, dashed, bar }) => (
  <Stack direction="row" spacing={0.75} alignItems="center">
    <Box component="svg" width={22} height={10} aria-hidden>
      {bar ? <rect x={0} y={3} width={22} height={5} fill={color} opacity={0.7} />
        : <line x1={0} x2={22} y1={5} y2={5} stroke={color} strokeWidth={2} strokeDasharray={dashed ? '5 3' : undefined} />}
    </Box>
    <Typography variant="caption" color="text.secondary">{label}</Typography>
  </Stack>
);
