/**
 * DashboardPage — the Overview as a layout of blocks (UI redesign 2026-11-22, SPEC §2, §5.1).
 * The page is thin: the header (title, board tabs, one «View» button), the status strips (link,
 * offline points, hub time, access) and the board. What each board shows is the layout
 * (dashboard/layout.ts): the user's saved one, or the default built from what is wired to the hub
 * (dashboard/siteTraits.ts) — no site scenario on a real hub (operator 22.11).
 *
 * Shared states: loading, empty (→ wizard), no-connection (stale banner), seq-gap hint.
 */
import { EmptyState } from '../components/common/EmptyState';
import { RebootStrip } from '../components/common/RebootStrip';
import { PageSkeleton } from '../components/common/PageSkeleton';
import React, { useCallback, useMemo, useState } from 'react';
import { Box, Typography, Alert, Button, Chip, Stack, Tab, Tabs, Menu, MenuItem, ListItemIcon, ListItemText, Divider, IconButton, Tooltip } from '@mui/material';
import TuneIcon from '@mui/icons-material/Tune';
import FullscreenIcon from '@mui/icons-material/Fullscreen';
import DashboardCustomizeIcon from '@mui/icons-material/DashboardCustomizeOutlined';
import AddIcon from '@mui/icons-material/Add';
import EditIcon from '@mui/icons-material/EditOutlined';
import ChevronLeftIcon from '@mui/icons-material/ChevronLeft';
import ChevronRightIcon from '@mui/icons-material/ChevronRight';
import DeleteIcon from '@mui/icons-material/DeleteOutline';
import CheckIcon from '@mui/icons-material/Check';
import { useNavigate, Link as RouterLink } from 'react-router-dom';
import { Card } from '@rbgrid/design-system';
import { useDeviceClock, formatDeviceTime } from '../core/deviceTime';
import { useLocalStorage, useSessionData } from '../hooks/useLocalStorage';
import { HubTimeNote } from '../components/dashboard/HubTimeNote';
import { HubAccessWarning } from '../components/common/HubAccessWarning';
import { isDemo } from '../services/connection';
import { useDisplayMode } from '../hooks/useDisplayMode';
import { MoneyLensProvider } from '../economics/MoneyLens';
import { dayAvgRate, solarDayOf } from '../economics/costRate';
import { offlinePoints, pointUnits, isGenerationUnit } from './overviewData';
import { unitMetricValue } from '../entities/model';
import { useDashboardInputs, buildDashboardModel, DashboardDataProvider, DashboardModel } from '../dashboard/DashboardData';
import { useLayout } from '../dashboard/useLayout';
import { MAIN_BOARD, DETAIL_BOARD, Board, LayoutDoc, layoutBytes, LIMITS } from '../dashboard/layout';
import { DashboardBoard as Grid12, BoardEditHandlers } from '../dashboard/DashboardBoard';
import { CatalogDialog, SettingsDialog, BoardNameDialog, EditBar } from '../dashboard/BoardEditor';
import { widgetDef, PointsPrefsProvider } from '../dashboard/registry';
import {
  moveBlock, setBlockSize, setBlockOpts, removeBlock, addBlock, addBoard, renameBoard, removeBoard, moveBoard, sameLayout,
} from '../dashboard/editLayout';
import { useUnsavedChanges } from '../app/unsavedGuard';
import { useCapabilities } from '../core';
import { AdminRequiredError, AuthRequiredError } from '../services/api';
import { Dialog, DialogActions, DialogContent, DialogTitle } from '@mui/material';
import { useConfirm } from '../components/common/ConfirmDialog';
import { lineUnits, deviceSets } from './overviewData';
import { useDeviceGroups } from '../hooks/useDeviceGroups';
import { t, dateLocale } from '../i18n';

/** A board's tab title: its own name, or the translation for the two standard boards. */
export const boardTitle = (b: Board): string =>
  b.name || (b.id === MAIN_BOARD ? t('dashboard.overview') : b.id === DETAIL_BOARD ? t('dashboard.analytics') : b.id);

export const DashboardPage: React.FC = () => {
  const inputs = useDashboardInputs();
  const navigate = useNavigate();
  const hubClock = useDeviceClock();
  const display = useDisplayMode(inputs.moneyAvailable);
  const { data, frame } = inputs;

  if (!data || !frame) {
    return (
      <PageSkeleton kind="dashboard" message={inputs.error ? t('dashboard.connecting') : t('dashboard.loadingTelemetry')} />
    );
  }
  const m = buildDashboardModel({ ...inputs, data, frame }, navigate);
  return <Ready m={m} display={display} hubClock={hubClock} />;
};

const Ready: React.FC<{ m: DashboardModel; display: ReturnType<typeof useDisplayMode>; hubClock: ReturnType<typeof useDeviceClock> }> = ({ m, display, hubClock }) => {
  const caps = useCapabilities();
  const layout = useLayout(m.traits, caps.flags.uiState);
  const confirm = useConfirm();
  // Kept for the tab's session: a remount (language, reconnect) or Back keeps the board (I3P-26).
  const [boardId, setBoardId] = useSessionData<string>('rbgrid.dashboard_board', MAIN_BOARD);
  const [viewMenu, setViewMenu] = useState<HTMLElement | null>(null);
  // «Customise» (SPEC §2.7): a draft of the whole layout; «Done» saves it.
  const [draft, setDraft] = useState<LayoutDoc | null>(null);
  const [busy, setBusy] = useState(false);
  const [saveErr, setSaveErr] = useState<string | null>(null);
  const [catalog, setCatalog] = useState(false);
  const [settingsAt, setSettingsAt] = useState<number | null>(null);
  const [nameDlg, setNameDlg] = useState<'new' | 'rename' | null>(null);
  const editing = draft != null;
  const doc = draft ?? layout.doc;
  const dirty = !!draft && !sameLayout(draft, layout.doc);
  const discard = useCallback(() => setDraft(null), []);
  useUnsavedChanges('dashboard-layout', dirty, discard);
  const boards = doc.boards;
  const board = boards.find((b) => b.id === boardId) ?? boards[0];
  const { names: groupNames } = useDeviceGroups();
  // Points for a single-point block — appliances first, as one (`g:<id>`), then every point.
  const points = useMemo(() => [
    ...deviceSets(lineUnits(m.data.units, m.unitsCfg), m.unitsCfg).filter((st) => st.units.length > 1 && st.groupId != null)
      .map((st) => ({ id: `g:${st.groupId}`, name: `${groupNames.get(st.groupId as number) || m.nameFor(st.lead)} · ${t('pointTile.phasesN', { n: st.units.length })}` })),
    ...lineUnits(m.units, m.unitsCfg).map((g) => ({ id: m.unitsCfg.forGroup(g)?.hex ?? g.unitKey, name: m.nameFor(g) })),
  ], [m, groupNames]);

  const edit: BoardEditHandlers | undefined = draft ? {
    move: (from, to) => setDraft((d) => d && moveBlock(d, board.id, from, to)),
    size: (i, sz) => setDraft((d) => d && setBlockSize(d, board.id, i, sz)),
    remove: (i) => setDraft((d) => d && removeBlock(d, board.id, i)),
    settings: (i) => setSettingsAt(i),
    add: () => setCatalog(true),
  } : undefined;

  const finish = async () => {
    if (!draft) return;
    if (!dirty) { setDraft(null); return; }
    if (layoutBytes(draft) > LIMITS.bytes) { setSaveErr(t('board.tooBig')); return; }
    setBusy(true); setSaveErr(null);
    try {
      // The standard boards are not stored as such: they keep following what is wired (`std`).
      const std = JSON.stringify(draft.boards) === JSON.stringify(layout.defaults.boards);
      if (std && !draft.pins?.length && !draft.hide?.length && !draft.order?.length) await layout.reset();
      else await layout.save(std ? { ...draft, std: true } : draft);
      setDraft(null);
    } catch (e) {
      setSaveErr(e instanceof AdminRequiredError || e instanceof AuthRequiredError ? t('board.saveNeedsToken')
        : t('board.saveFailed', { why: e instanceof Error ? e.message : String(e) }));
    } finally { setBusy(false); }
  };
  const startEdit = () => { setDraft(layout.doc); setSaveErr(null); dismissHint(); };
  // A one-time note that the Overview can be arranged (the entry was too easy to miss).
  const [hintSeen, setHintSeen] = useLocalStorage<boolean>('rbgrid.board_hint_seen', false);
  const dismissHint = () => setHintSeen(true);
  const settingsDef = settingsAt != null ? widgetDef(board.w[settingsAt]?.[0] ?? '') ?? null : null;
  const { data, stale, totals, oss, rate, main, frame, unitsCfg } = m;
  const offlineNow = offlinePoints(data.units);

  const header = (
    <>
      <Stack direction="row" alignItems="center" justifyContent="space-between" spacing={1.5} sx={{ mb: 1 }}>
        <Stack direction="row" alignItems="center" spacing={1.5} sx={{ minWidth: 0 }}>
          {/* The page title = its menu item (HEU-15). */}
          <Typography variant="h4" fontWeight={700}>{t('shell.navOverview')}</Typography>
          {m.seqGap && <Chip size="small" color="warning" label={t('dashboard.dataDelayed')} />}
        </Stack>
        <Stack direction="row" spacing={1} sx={{ flexShrink: 0 }}>
          {/* «Customise» is a button of its own on wider screens (SPEC §2.7); on phones it is in «View». */}
          {!editing && (
            <Button variant="outlined" startIcon={<DashboardCustomizeIcon />} onClick={startEdit}
              sx={{ display: { xs: 'none', md: 'inline-flex' } }}>{t('board.customise')}</Button>
          )}
          <Button variant="outlined" startIcon={<TuneIcon />} onClick={(e) => setViewMenu(e.currentTarget)}
            aria-haspopup="menu" aria-expanded={!!viewMenu}>{t('board.view')}</Button>
        </Stack>
        <Menu anchorEl={viewMenu} open={!!viewMenu} onClose={() => setViewMenu(null)}>
          {display.moneyAvailable && (['money', 'power'] as const).map((mode) => (
            <MenuItem key={mode} selected={display.mode === mode} onClick={() => { display.setMode(mode); setViewMenu(null); }}>
              <ListItemIcon>{display.mode === mode ? <CheckIcon fontSize="small" /> : null}</ListItemIcon>
              <ListItemText>{t(mode === 'money' ? 'board.showMoney' : 'board.showPower')}</ListItemText>
            </MenuItem>
          ))}
          {display.moneyAvailable && <Divider />}
          <MenuItem disabled={editing} onClick={() => { startEdit(); setViewMenu(null); }}>
            <ListItemIcon><DashboardCustomizeIcon fontSize="small" /></ListItemIcon>
            <ListItemText>{t('board.customise')}</ListItemText>
          </MenuItem>
          <MenuItem onClick={() => { setViewMenu(null); void document.documentElement.requestFullscreen?.().catch(() => undefined); m.navigate(`/board/${board.id}`); }}>
            <ListItemIcon><FullscreenIcon fontSize="small" /></ListItemIcon>
            <ListItemText>{t('kiosk.open')}</ListItemText>
          </MenuItem>
          {layout.custom && !editing && (
            <MenuItem onClick={async () => {
              setViewMenu(null);
              if (await confirm({ title: t('board.resetConfirm'), body: t('board.resetBody'), confirmLabel: t('board.reset') })) void layout.reset();
            }}>
              <ListItemIcon />
              <ListItemText>{t('board.reset')}</ListItemText>
            </MenuItem>
          )}
        </Menu>
      </Stack>
      {(boards.length > 1 || editing) && (
        <Stack direction="row" alignItems="center" spacing={0.5} sx={{ mb: 2, minWidth: 0 }}>
          <Tabs value={board.id} onChange={(_, v) => setBoardId(v)} variant="scrollable" allowScrollButtonsMobile sx={{ minHeight: 40, minWidth: 0, flex: '0 1 auto' }}
            aria-label={t('board.tabsAria')}>
            {boards.map((b) => <Tab key={b.id} value={b.id} label={boardTitle(b)} sx={{ minHeight: 40, textTransform: 'none', fontWeight: 600 }} />)}
          </Tabs>
          {draft && (
            <Stack direction="row" spacing={0} sx={{ flexShrink: 0 }}>
              <Tooltip title={t('board.renameBoard')}><IconButton size="small" onClick={() => setNameDlg('rename')} aria-label={t('board.renameBoard')}><EditIcon fontSize="small" /></IconButton></Tooltip>
              <Tooltip title={t('board.boardLeft')}><span><IconButton size="small" disabled={boards[0].id === board.id} onClick={() => setDraft(moveBoard(draft, board.id, -1))} aria-label={t('board.boardLeft')}><ChevronLeftIcon fontSize="small" /></IconButton></span></Tooltip>
              <Tooltip title={t('board.boardRight')}><span><IconButton size="small" disabled={boards[boards.length - 1].id === board.id} onClick={() => setDraft(moveBoard(draft, board.id, 1))} aria-label={t('board.boardRight')}><ChevronRightIcon fontSize="small" /></IconButton></span></Tooltip>
              <Tooltip title={t('board.deleteBoard')}><span><IconButton size="small" disabled={boards.length <= 1} aria-label={t('board.deleteBoard')}
                onClick={async () => { if (await confirm({ title: t('board.deleteBoardConfirm', { name: boardTitle(board) }), confirmLabel: t('board.deleteBoard'), danger: true })) { setDraft(removeBoard(draft, board.id)); setBoardId(boards.find((b) => b.id !== board.id)!.id); } }}>
                <DeleteIcon fontSize="small" /></IconButton></span></Tooltip>
              <Tooltip title={t('board.addBoard')}><span><IconButton size="small" disabled={boards.length >= LIMITS.boards} onClick={() => setNameDlg('new')} aria-label={t('board.addBoard')}><AddIcon fontSize="small" /></IconButton></span></Tooltip>
            </Stack>
          )}
        </Stack>
      )}
      {!hintSeen && !editing && !layout.custom && (
        <Alert severity="info" icon={<DashboardCustomizeIcon />} onClose={dismissHint} sx={{ mb: 2 }}
          action={<Button color="inherit" size="small" onClick={startEdit} sx={{ whiteSpace: 'nowrap' }}>{t('board.customise')}</Button>}>
          <Box component="span" sx={{ display: { xs: 'none', md: 'inline' } }}>{t('board.hint')}</Box>
          <Box component="span" sx={{ display: { xs: 'inline', md: 'none' } }}>{t('board.hintShort')}</Box>
        </Alert>
      )}
      {stale && (
        <Alert severity="warning" sx={{ mb: 2 }}>
          {m.authRejected ? t('dashboard.readTokenBanner') : t('dashboard.staleBanner', { time: m.lastUpdate ? formatDeviceTime(hubClock, Math.floor(m.lastUpdate / 1000), { hour: '2-digit', minute: '2-digit', second: '2-digit' }, dateLocale()) : '' })}
        </Alert>
      )}
      {!stale && <HubAccessWarning />}
      <RebootStrip />
      {!stale && !isDemo() && <HubTimeNote />}
      {/* System health on the first screen (UX audit HOME-06). */}
      {!stale && offlineNow.length > 0 && (
        <Alert severity="warning" sx={{ mb: 2 }}
          action={<Button color="inherit" size="small" sx={{ whiteSpace: 'nowrap' }} onClick={() => m.navigate('/events')}>{t('dashboard.offlineMore')}</Button>}>
          {t('dashboard.offlineStrip', { n: offlineNow.length, m: pointUnits(data.units).filter((g) => !g.noChannel).length })}
        </Alert>
      )}
    </>
  );

  if (m.units.length === 0) {
    return (
      <Box>
        {header}
        <EmptyState title={t('dashboard.emptyTitle')} why={t('dashboard.emptyText')} action={{ label: t('dashboard.addSensor'), to: '/devices' }} />
      </Box>
    );
  }

  return (
    <MoneyLensProvider mode={display.mode} rate={rate} fmt={m.mf.money} gridW={main?.present && frame.gridHasDirection ? frame.grid * 1000 : null}
      dayRate={dayAvgRate(oss ? null : totals)}
      solarDay={solarDayOf(data.units.filter((g) => isGenerationUnit(g, unitsCfg)).reduce((s, g) => s + (unitMetricValue(g, 'energy_day_out') ?? 0), 0),
        oss ? null : totals, dayAvgRate(oss ? null : totals).buy ?? rate.buy, rate.sell)}>
      <DashboardDataProvider model={m}>
        <Box>
          {header}
          {editing && <Alert severity="info" sx={{ mb: 2 }}>{t('board.editHint')}</Alert>}
          <PointsPrefsProvider value={{
            pins: doc.pins ?? [], hide: doc.hide ?? [], order: doc.order ?? [], editing,
            set: (p) => { if (draft) setDraft((d) => (d ? { ...d, ...p } : d)); else void layout.setPoints({ pins: doc.pins, hide: doc.hide, order: doc.order, ...p }); },
          }}>
            <Grid12 board={board} edit={edit} />
          </PointsPrefsProvider>
          {!editing && board.w.length === 0 && (
            <Typography color="text.secondary" sx={{ py: 4, textAlign: 'center' }}>{t('board.emptyBoardView')}</Typography>
          )}
          {draft && (
            <>
              {saveErr && <Alert severity="error" sx={{ mt: 2 }} onClose={() => setSaveErr(null)}>{saveErr}</Alert>}
              <EditBar where={t(layout.where === 'hub' ? 'board.whereHub' : layout.where === 'device' ? 'board.whereDevice' : 'board.whereBrowser')} dirty={dirty} busy={busy}
                localOnly={layout.hubCapable ? { value: layout.localOnly, onChange: (v) => { layout.setLocalOnly(v); setDraft(null); } } : null}
                onCancel={() => setDraft(null)} onReset={() => setDraft(layout.defaults)} onDone={() => void finish()} />
              <CatalogDialog open={catalog} traits={m.traits} present={new Set(board.w.map((p) => p[0]))} onClose={() => setCatalog(false)}
                onAdd={(def) => {
                  const opts = def.id === 'tile' ? { unit: points[0]?.id } : def.defaults;
                  setDraft(addBlock(draft, board.id, opts ? [def.id, def.defaultSize, opts] : [def.id, def.defaultSize]));
                  setCatalog(false);
                  if (def.settings?.some((x) => x.kind === 'point')) setSettingsAt(board.w.length);
                  // To the new block at the end (the list is long on a plant's board).
                  setTimeout(() => window.scrollTo({ top: document.documentElement.scrollHeight, behavior: 'smooth' }), 50);
                }} />
              {settingsDef && settingsAt != null && (
                <SettingsDialog def={settingsDef} value={board.w[settingsAt]?.[2] ?? settingsDef.defaults ?? {}} points={points}
                  onClose={() => setSettingsAt(null)} onSave={(o) => { setDraft(setBlockOpts(draft, board.id, settingsAt, o)); setSettingsAt(null); }} />
              )}
              <BoardNameDialog open={nameDlg != null} initial={nameDlg === 'rename' ? boardTitle(board) : ''}
                title={t(nameDlg === 'rename' ? 'board.renameBoard' : 'board.addBoard')} onClose={() => setNameDlg(null)}
                onSave={(name) => {
                  if (nameDlg === 'rename') setDraft(renameBoard(draft, board.id, name));
                  else { const r = addBoard(draft, name); setDraft(r.doc); setBoardId(r.id); }
                  setNameDlg(null);
                }} />
            </>
          )}
          {/* Another device saved first (SPEC §2.6 rule 2): nothing is lost — the user picks. */}
          <Dialog open={!!layout.conflict} maxWidth="xs" fullWidth>
            <DialogTitle>{t('board.conflictTitle')}</DialogTitle>
            <DialogContent><Typography variant="body2">{t('board.conflictBody')}</Typography></DialogContent>
            <DialogActions>
              <Button onClick={() => void layout.takeHubs()}>{t('board.takeHubs')}</Button>
              <Button variant="contained" onClick={() => void layout.keepMine()}>{t('board.keepMine')}</Button>
            </DialogActions>
          </Dialog>
        </Box>
      </DashboardDataProvider>
    </MoneyLensProvider>
  );
};
