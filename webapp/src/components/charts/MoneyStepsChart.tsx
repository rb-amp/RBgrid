/**
 * MoneyStepsChart — Economics' period as columns (UI redesign U8, SPEC §5.7): what was paid per step
 * of the window (hours of a day, days of a week / month, months of a year) above the line and what
 * the exports earned below it, on one scale. A column's value is on hover; the totals are below the
 * chart on the page. The hub's closed records only (the running step is in the totals' «today»).
 */
import React, { useMemo } from 'react';
import { Box, Typography } from '@mui/material';
import { Card, BlockHeader } from '@rbgrid/design-system';
import { useHistory } from '../../hooks/useHistory';
import { useElementWidth } from '../../hooks/useElementWidth';
import { useDeviceClock, formatDeviceTime } from '../../core/deviceTime';
import { useDsTokens } from '../../theme/useDsTokens';
import { windowSpanAt, HistWindow } from '../../pages/historyWindow';
import { ChartLegend } from '../common/ChartLegend';
import type { HistoryEconomicsRecord } from '../../types/history';
import { t, dateLocale } from '../../i18n';

const H = 220;
const PAD_T = 12;
const PAD_B = 22;
const PAD_X = 8;
/** Room for the € scale on the left (round 2 home H-08: the columns had no axis). */
const AXIS_W = 60;

/** A round scale top at or above v: 1, 2, 2.5, 5 × 10^n (uiux V09: «4,14 €» on the scale). */
export function niceTop(v: number): number {
  if (!(v > 0)) return 0;
  const p = 10 ** Math.floor(Math.log10(v));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p;
  return 10 * p;
}

export const MoneyStepsChart: React.FC<{ seriesId: string | null; win: HistWindow; back?: number; fmt: (v: number) => string }> = ({ seriesId, win, back = 0, fmt }) => {
  const ds = useDsTokens();
  const clock = useDeviceClock();
  const span = useMemo(() => windowSpanAt(win, back, Math.floor(Date.now() / 1000), clock), [win, back, clock]);
  const { records, loading } = useHistory(seriesId, span.step, 'economics', span.limit, { from: span.from, to: span.to });
  const [ref, measured] = useElementWidth<HTMLDivElement>(720);
  const W = Math.max(240, measured);
  // Oldest on the left (the hub answers newest first).
  const recs = (records as HistoryEconomicsRecord[]).filter((r) => typeof r.ts === 'number' && r.ts >= span.from && r.ts < span.to).sort((a, b) => a.ts - b.ts);
  const maxC = niceTop(Math.max(0, ...recs.map((r) => r.c ?? 0)));
  const maxR = niceTop(Math.max(0, ...recs.map((r) => r.r ?? 0)));
  const topBand = maxR > 0 ? (H - PAD_T - PAD_B) * 0.72 : H - PAD_T - PAD_B;
  const botBand = H - PAD_T - PAD_B - topBand;
  const per = Math.min(maxC > 0 ? topBand / maxC : Infinity, maxR > 0 ? botBand / maxR : Infinity);
  const baseY = PAD_T + topBand;
  const n = Math.max(1, recs.length);
  const X0 = PAD_X + AXIS_W;
  const slot = (W - X0 - PAD_X) / n;
  const bw = Math.max(2, Math.min(24, slot * 0.6));
  const stepFmt: Intl.DateTimeFormatOptions = span.step === 'hour' ? { hour: '2-digit', minute: '2-digit' } : span.step === 'month' ? { month: 'short', year: 'numeric' } : { day: 'numeric', month: 'short' };
  const labelEvery = Math.max(1, Math.ceil(52 / slot)); // «12 нояб.» at 12 px needs ~50 px

  return (
    <Card>
      <BlockHeader title={t('economics.chartTitle')} caption={t(`economics.chartBy_${span.step}`)}
        actions={maxR > 0 ? <ChartLegend items={[{ label: t('economics.chartPaid'), color: ds.flowImport }, { label: t('economics.chartEarned'), color: ds.flowExport }]} /> : undefined} />
      {!recs.length ? (
        <Typography variant="body2" color="text.secondary" sx={{ py: 3, textAlign: 'center' }}>{loading ? t('common.loading') : t('economics.chartEmpty')}</Typography>
      ) : (
        <Box ref={ref} sx={{ position: 'relative', width: '100%', height: H, overflow: 'hidden', '& > svg': { position: 'absolute', left: 0, top: 0 } }}>
          <svg viewBox={`0 0 ${W} ${H}`} width={W} height={H} role="img" aria-label={t('economics.chartAria')}>
            {/* The scale: the top of the paid band, 0 on the line, the top of the earned band below. */}
            {maxC > 0 && (
              <g>
                <line x1={X0} x2={W - PAD_X} y1={baseY - maxC * per} y2={baseY - maxC * per} stroke={ds.border} strokeDasharray="2 4" />
                <text x={X0 - 6} y={baseY - maxC * per + 4} textAnchor="end" fill={ds.inkMuted} fontSize="12">{fmt(maxC)}</text>
              </g>
            )}
            {maxR > 0 && (
              <g>
                <line x1={X0} x2={W - PAD_X} y1={baseY + maxR * per} y2={baseY + maxR * per} stroke={ds.border} strokeDasharray="2 4" />
                {/* Earnings below the line, signed in the export colour (walkthrough W6: «€0.08» read as a smaller cost). */}
                <text x={X0 - 6} y={baseY + maxR * per + 4} textAnchor="end" fill={ds.flowExport} fontSize="12">+{fmt(maxR)}</text>
              </g>
            )}
            <text x={X0 - 6} y={baseY + 4} textAnchor="end" fill={ds.inkMuted} fontSize="12">{fmt(0)}</text>
            <line x1={X0} x2={W - PAD_X} y1={baseY} y2={baseY} stroke={ds.border} />
            {recs.map((r, i) => {
              const cx = X0 + slot * i + slot / 2;
              const hc = Math.max(r.c > 0 ? 1 : 0, (r.c ?? 0) * per);
              const hr = Math.max(r.r > 0 ? 1 : 0, (r.r ?? 0) * per);
              const when = formatDeviceTime(clock, r.ts, stepFmt, dateLocale());
              return (
                <g key={r.ts}>
                  {hc > 0 && (
                    <rect x={cx - bw / 2} y={baseY - hc} width={bw} height={hc} rx={Math.min(4, bw / 2)} fill={ds.flowImport}>
                      <title>{`${when}: ${t('economics.chartPaid')} ${fmt(r.c)}`}</title>
                    </rect>
                  )}
                  {hr > 0 && (
                    <rect x={cx - bw / 2} y={baseY + 1} width={bw} height={hr} rx={Math.min(4, bw / 2)} fill={ds.flowExport}>
                      <title>{`${when}: ${t('economics.chartEarned')} ${fmt(r.r)}`}</title>
                    </rect>
                  )}
                  {i % labelEvery === 0 && (
                    <text x={cx} y={H - 6} textAnchor="middle" fill={ds.inkMuted} fontSize="12">
                      {span.step === 'hour' ? when.slice(0, 2) : when}
                    </text>
                  )}
                </g>
              );
            })}
          </svg>
        </Box>
      )}
    </Card>
  );
};
