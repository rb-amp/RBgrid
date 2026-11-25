/**
 * KioskPage — «Wall display» (UI redesign §7, SPEC «Табло»): one Overview board on the whole
 * screen, no menu, bigger figures, the screen kept on (Screen Wake Lock where the browser has it),
 * boards changing on a timer if asked (off / 15 / 30 / 60 s). The bar at the top shows on a tap or a
 * mouse move and hides again; «Close» goes back to the Overview. Same data and layout as the
 * Overview (its boards, the hub's or this device's view).
 */
import React, { useEffect, useRef, useState } from 'react';
import { Alert, Box, Button, MenuItem, Stack, TextField, Typography } from '@mui/material';
import { offlinePoints } from './overviewData';
import CloseIcon from '@mui/icons-material/Close';
import { useNavigate, useParams } from 'react-router-dom';
import { useDashboardInputs, buildDashboardModel, DashboardDataProvider, useDashboardData } from '../dashboard/DashboardData';
import { DashboardBoard } from '../dashboard/DashboardBoard';
import { useLayout } from '../dashboard/useLayout';
import { useCapabilities } from '../core';
import { useDisplayMode } from '../hooks/useDisplayMode';
import { MoneyLensProvider } from '../economics/MoneyLens';
import { dayAvgRate } from '../economics/costRate';
import { PageSkeleton } from '../components/common/PageSkeleton';
import { useLocalStorage } from '../hooks/useLocalStorage';
import { boardTitle } from './DashboardPage';
import { useDeviceClock, formatDeviceTime } from '../core/deviceTime';
import { t, dateLocale } from '../i18n';

const ROTATE = [0, 15, 30, 60];

export const KioskPage: React.FC = () => {
  const inputs = useDashboardInputs();
  const navigate = useNavigate();
  const display = useDisplayMode(inputs.moneyAvailable);
  if (!inputs.data || !inputs.frame) return <Box sx={{ p: 3 }}><PageSkeleton kind="dashboard" /></Box>;
  const m = buildDashboardModel({ ...inputs, data: inputs.data, frame: inputs.frame }, navigate);
  return (
    <MoneyLensProvider mode={display.mode} rate={m.rate} fmt={m.mf.money} gridW={m.main?.present && m.frame.gridHasDirection ? m.frame.grid * 1000 : null}
      dayRate={dayAvgRate(m.oss ? null : m.totals)}>
      <DashboardDataProvider model={m}><Kiosk /></DashboardDataProvider>
    </MoneyLensProvider>
  );
};

const Kiosk: React.FC = () => {
  const { id } = useParams();
  const navigate = useNavigate();
  const caps = useCapabilities();
  const m = useDashboardData();
  const layout = useLayout(m.traits, caps.flags.uiState);
  const boards = layout.doc.boards;
  const [rotate, setRotate] = useLocalStorage<number>('rbgrid.kiosk_rotate', 0);
  const [idx, setIdx] = useState(() => Math.max(0, boards.findIndex((b) => b.id === id)));
  const [bar, setBar] = useState(true);
  const hideTimer = useRef<ReturnType<typeof setTimeout>>();
  const clock = useDeviceClock();
  const [, tick] = useState(0);

  // Keep the screen on while the display is open (where the browser allows).
  useEffect(() => {
    let lock: { release: () => Promise<void> } | null = null;
    const nav = navigator as Navigator & { wakeLock?: { request: (t: 'screen') => Promise<{ release: () => Promise<void> }> } };
    const take = () => { nav.wakeLock?.request('screen').then((l) => { lock = l; }).catch(() => undefined); };
    take();
    const onVis = () => { if (document.visibilityState === 'visible') take(); };
    document.addEventListener('visibilitychange', onVis);
    return () => { document.removeEventListener('visibilitychange', onVis); void lock?.release().catch(() => undefined); };
  }, []);
  // Boards in turn.
  useEffect(() => {
    if (!rotate || boards.length < 2) return undefined;
    const h = setInterval(() => setIdx((i) => (i + 1) % boards.length), rotate * 1000);
    return () => clearInterval(h);
  }, [rotate, boards.length]);
  // The clock in the corner.
  useEffect(() => { const h = setInterval(() => tick((n) => n + 1), 30_000); return () => clearInterval(h); }, []);
  // The bar shows on a move / tap and hides after 4 s.
  const wake = () => {
    setBar(true);
    if (hideTimer.current) clearTimeout(hideTimer.current);
    hideTimer.current = setTimeout(() => setBar(false), 4000);
  };
  useEffect(() => { wake(); return () => { if (hideTimer.current) clearTimeout(hideTimer.current); }; }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const board = boards[Math.min(idx, boards.length - 1)];
  // Fit: measure the board at zoom 1 and scale it to the window (the bar stays on top).
  const fitRef = useRef<HTMLDivElement>(null);
  const [fit, setFit] = useState(1);
  useEffect(() => {
    const el = fitRef.current;
    if (!el) return undefined;
    const measure = () => {
      const natural = el.scrollHeight / (fit || 1);
      const free = window.innerHeight - el.getBoundingClientRect().top - 16;
      // Never below ×1 (round 2 uiux N02: at ×0.9 it was smaller than the Overview): squeezing a long board onto one screen made it unreadable (it is better kept
      // short — a board of its own for the wall); up to ×1.5 where it has room.
      const next = Math.max(1, Math.min(1.5, free / Math.max(1, natural)));
      if (Math.abs(next - fit) > 0.02) setFit(next);
    };
    const ro = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(measure);
    ro?.observe(el);
    window.addEventListener('resize', measure);
    const h = setTimeout(measure, 1500);
    return () => { ro?.disconnect(); window.removeEventListener('resize', measure); clearTimeout(h); };
  }, [fit, board.id]);
  // A board taller than the wall (round 3 business B2, ev R3-04, uiux N02: 1865 px on a 1080 screen, and
  // nobody scrolls a wall): it scrolls itself — down, a pause, back to the top — while nobody touches it.
  useEffect(() => {
    if (bar) return undefined;
    const h = setInterval(() => {
      const el = document.scrollingElement ?? document.documentElement;
      if (el.scrollHeight <= window.innerHeight + 24) return;
      const atBottom = el.scrollTop + window.innerHeight >= el.scrollHeight - 24;
      window.scrollTo({ top: atBottom ? 0 : el.scrollHeight, behavior: 'smooth' });
    }, 15_000);
    return () => clearInterval(h);
  }, [bar, board.id]);
  const offline = offlinePoints(m.data.units);
  const offlineN = offline.length;
  const offlineNames = offline.map((g) => m.nameFor(g)).join(', ');
  const alarmN = m.data.units.filter((g) => g.present && g.attributes?.['alarm'] === true).length;
  const close = () => { if (document.fullscreenElement) void document.exitFullscreen().catch(() => undefined); navigate('/dashboard'); };

  return (
    <Box onMouseMove={wake} onClick={wake} sx={{ minHeight: '100vh', bgcolor: 'background.default', p: { xs: 2, md: 3 } }}>
      {/* Over the board, not above it: hidden it left an empty ~80 px band (uiux N02). */}
      <Stack direction="row" alignItems="center" spacing={2} sx={{ position: 'fixed', top: 0, left: 0, right: 0, zIndex: 10, px: { xs: 2, md: 3 }, py: 1.5,
        bgcolor: 'background.paper', borderBottom: 1, borderColor: 'divider', opacity: bar ? 1 : 0, transition: 'opacity .3s', pointerEvents: bar ? 'auto' : 'none' }}>
        <Typography variant="h4" fontWeight={800} sx={{ flex: 1 }}>{boardTitle(board)}</Typography>
        <Typography sx={{ fontSize: 22, fontVariantNumeric: 'tabular-nums' }}>
          {formatDeviceTime(clock, Math.floor(Date.now() / 1000), { hour: '2-digit', minute: '2-digit' }, dateLocale())}
        </Typography>
        {boards.length > 1 && (
          <TextField select size="small" label={t('kiosk.rotate')} value={rotate} onChange={(e) => setRotate(Number(e.target.value))} sx={{ minWidth: 150 }}>
            {ROTATE.map((s) => <MenuItem key={s} value={s}>{s ? t('kiosk.every', { s }) : t('kiosk.off')}</MenuItem>)}
          </TextField>
        )}
        <Button variant="outlined" startIcon={<CloseIcon />} onClick={close}>{t('common.close')}</Button>
      </Stack>
      {/* As big as the screen allows (role audit B2: too small to read from across the room): scaled to
          the free height between ×1 and ×1.5; a short board of its own fits the wall best. */}
      <Box ref={fitRef} sx={{ zoom: fit, transformOrigin: 'top left' }}>
        {/* A wall must show trouble even on a board without «Needs attention» (round 2 ev N-06: a station
            without link was invisible from the wall). */}
        {!board.w.some((p) => p[0] === 'alarms') && (alarmN + offlineN) > 0 && (
          <Alert severity={alarmN ? 'error' : 'warning'} sx={{ mb: 2, fontSize: 18 }}>
            {[alarmN ? t('kiosk.alarms', { n: alarmN }) : null, offlineN ? t('kiosk.offline', { names: offlineNames }) : null].filter(Boolean).join(' · ')}
          </Alert>
        )}
        <DashboardBoard board={board} />
      </Box>
    </Box>
  );
};
