/**
 * PeriodPicker — one way to pick a period (UI redesign N4, SPEC §4.3): the windows «Today ·
 * Yesterday · 7 days · This month · Last month · Year», and ‹ › to step back and forth where the page
 * can. History and Economics share the chosen window for the tab's session (`usePeriodWindow`), so
 * switching between them keeps the period.
 */
import React, { useCallback, useEffect, useMemo } from 'react';
import { useLocation } from 'react-router-dom';
import { IconButton, MenuItem, Stack, TextField, ToggleButton, ToggleButtonGroup, Typography, useMediaQuery, useTheme } from '@mui/material';
import ChevronLeftIcon from '@mui/icons-material/ChevronLeft';
import ChevronRightIcon from '@mui/icons-material/ChevronRight';
import { HIST_WINDOWS, HistWindow, windowSpanAt } from '../../pages/historyWindow';
import { useSessionData } from '../../hooks/useLocalStorage';
import { useDeviceClock, formatDeviceTime, DeviceClock } from '../../core/deviceTime';
import { t, dateLocale } from '../../i18n';

/** «16 окт.», «10–16 окт.», «сентябрь 2026», «2025» — the period on display, in the hub's time. */
export function periodLabel(l: { from: number; to: number; kind: 'day' | 'week' | 'month' | 'year' }, clock: DeviceClock): string {
  const f = (ts: number, o: Intl.DateTimeFormatOptions) => formatDeviceTime(clock, ts, o, dateLocale());
  if (l.kind === 'day') return f(l.from, { day: 'numeric', month: 'short' });
  if (l.kind === 'week') return `${f(l.from, { day: 'numeric', month: 'short' })} – ${f(Math.max(l.from, l.to - 1), { day: 'numeric', month: 'short' })}`;
  if (l.kind === 'month') return f(l.from + 3600, { month: 'long', year: 'numeric' });
  return f(l.from + 3600, { year: 'numeric' });
}

/**
 * ‹ › over the shared window (round 2 uiux U12: the arrows and the date were in History only): how many
 * whole periods back, the span it stands for and its label.
 *
 * The step belongs to THIS page (round 3 business R1, uiux V02: one ‹ stuck — «Last month» showed
 * September in History, Economics and on every point opened later). It is kept for the tab's session
 * only so that a re-mount of the same page (a resize across a layout) does not drop it; another page,
 * another window, or the window's own button starts at the current period again.
 */
export function usePeriodBack(win: HistWindow) {
  const clock = useDeviceClock();
  const { pathname } = useLocation();
  const [st, setSt] = useSessionData<{ path?: string; win: HistWindow; back: number }>('rbgrid.period_back', { path: pathname, win, back: 0 });
  const mine = !!st && st.path === pathname && st.win === win;
  const back = mine && Number.isFinite(st.back) ? Math.max(0, st.back) : 0;
  // Leaving the step behind: a stale one is cleared, so coming back to this window starts at «now».
  useEffect(() => { if (!mine && st?.back) setSt({ path: pathname, win, back: 0 }); }, [mine]); // eslint-disable-line react-hooks/exhaustive-deps
  const setBack = useCallback((b: number) => setSt({ path: pathname, win, back: Math.max(0, b) }), [pathname, win, setSt]);
  const span = useMemo(() => windowSpanAt(win, back, Math.floor(Date.now() / 1000), clock), [win, back, clock]);
  return { back, setBack, span, label: periodLabel(span.label, clock) };
}

/** The window shared by History and Economics (the key History has used since 16.10). */
export function usePeriodWindow(): [HistWindow, (w: HistWindow) => void] {
  const [w, set] = useSessionData<HistWindow>('rbgrid.history_win', 'today');
  return [w, set];
}

const segmentSx = {
  maxWidth: '100%',
  overflowX: 'auto',
  width: { xs: '100%', sm: 'auto' },
  '& .MuiToggleButton-root': { flex: { xs: '1 0 auto', sm: '0 0 auto' }, whiteSpace: 'nowrap' },
} as const;

export interface PeriodPickerProps {
  win: HistWindow;
  onWin: (w: HistWindow) => void;
  /** Steps back (0 = the current one); with `onBack` the arrows are shown. */
  back?: number;
  onBack?: (back: number) => void;
  /** What the stepped window is («March 2026»…), between the arrows. */
  label?: string;
  /** false = the period on display has no records: ‹ goes no further back into empty ones (round 3 R4). */
  canBack?: boolean;
}

export const PeriodPicker: React.FC<PeriodPickerProps> = ({ win, onWin, back = 0, onBack, label, canBack = true }) => {
  // Six buttons were cut off on a phone (UI/UX audit U14): a list there.
  const phone = useMediaQuery(useTheme().breakpoints.down('sm'));
  return (
  <>
    {phone ? (
      <TextField select size="small" label={t('history.periodAria')} value={win} onChange={(e) => onWin(e.target.value as HistWindow)} sx={{ minWidth: 160 }}>
        {HIST_WINDOWS.map((w) => <MenuItem key={w.value} value={w.value}>{t(w.label)}</MenuItem>)}
      </TextField>
    ) : (
    // A stepped-back period is not «This month» any more (round 3 ev R3-03): no button lit then, and a
    // tap on the window's button returns to its current period.
    <ToggleButtonGroup size="small" exclusive value={back > 0 ? null : win} sx={segmentSx} aria-label={t('history.periodAria')}
      onChange={(_, v) => { if (!v) return; if (v === win && back > 0) onBack?.(0); else onWin(v as HistWindow); }}>
      {HIST_WINDOWS.map((w) => <ToggleButton key={w.value} value={w.value}>{t(w.label)}</ToggleButton>)}
    </ToggleButtonGroup>
    )}
    {onBack && (
      <Stack direction="row" alignItems="center" spacing={0.5}>
        <IconButton size="small" aria-label={t('history.periodBack')} disabled={!canBack} onClick={() => onBack(back + 1)} sx={{ minWidth: 44, minHeight: 44 }}><ChevronLeftIcon /></IconButton>
        <Typography sx={{ fontSize: 13, minWidth: 120, textAlign: 'center', fontVariantNumeric: 'tabular-nums' }}>{label}</Typography>
        <IconButton size="small" aria-label={t('history.periodForward')} disabled={back === 0} onClick={() => onBack(Math.max(0, back - 1))} sx={{ minWidth: 44, minHeight: 44 }}><ChevronRightIcon /></IconButton>
      </Stack>
    )}
  </>
  );
};
