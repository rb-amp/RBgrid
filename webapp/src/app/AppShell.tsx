/**
 * AppShell — HA-style application layout: composes the @rbgrid/design-system <AppShell>
 * (brand + nav rail + content) with react-router nav state and the device/connection footer.
 */
import React, { useEffect } from 'react';
import { Outlet, useNavigate, useLocation } from 'react-router-dom';
import { Badge, Box, ButtonBase, Typography, Button, TextField, Menu, MenuItem, IconButton, Stack, useMediaQuery, useTheme } from '@mui/material';
import NotificationsIcon from '@mui/icons-material/NotificationsNone';
import ExpandLessIcon from '@mui/icons-material/ExpandLess';
import SpaceDashboardIcon from '@mui/icons-material/SpaceDashboard';
import RocketLaunchIcon from '@mui/icons-material/RocketLaunch';
import DeviceHubIcon from '@mui/icons-material/DeviceHub';
import ShowChartIcon from '@mui/icons-material/ShowChart';
import NotificationsActiveIcon from '@mui/icons-material/NotificationsActive';
import PaymentsIcon from '@mui/icons-material/Payments';
import RequestQuoteIcon from '@mui/icons-material/RequestQuote';
import TuneIcon from '@mui/icons-material/Tune';
import BoltIcon from '@mui/icons-material/Bolt';
import SwapHorizIcon from '@mui/icons-material/SwapHoriz';
import SmartToyIcon from '@mui/icons-material/SmartToy';
import { AppShell as DSAppShell, NavItem } from '@rbgrid/design-system';
import { useConnection } from '../hooks/useConnection';
import { disconnect, DEMO_ONLY } from '../services/connection';
import { ThemeSwitch } from '../theme/ThemeSwitch';
import { useLocalStorage } from '../hooks/useLocalStorage';
import { offlinePoints, pointUnits } from '../pages/overviewData';
import { t } from '../i18n';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { useCapabilities, CapabilityFlag } from '../core';
import { LinkStatus, DeviceUnreachable, DeviceNeedsToken, linkStateOf, LinkState } from './ConnectionStatus';
import { confirmLeave, setLeaveAsker } from './unsavedGuard';
import { useConfirm } from '../components/common/ConfirmDialog';
import { LlmPairBanner } from '../components/ai/LlmPairBanner';
import { QuickSearch } from './QuickSearch';
import SearchIcon from '@mui/icons-material/Search';
import { demoVariant, demoVariantIds, setDemoVariant, DemoVariantId } from '../mock/demoVariant';

// Menu by the user's questions (UI redesign 22.11, SPEC §4.1): Look (Overview, History, Economics) ·
// Watch (Events, with the active count) · Set up (Points, Tariffs & prices, AI, Settings). The wizard
// is listed only until the hub is set up (then: Settings → Device and /onboarding).
// `tab` = slot in the phone's bottom bar: Overview · Economics · History · Points · More (operator
// 22.11: Points instead of Events; events are the bell in the header).
// Labels are phrase keys (shell.*), resolved at render so a language switch applies.
const NAV: { to: string; label: string; icon: React.ReactNode; group: string; tab?: number; cap?: CapabilityFlag; key?: 'events' | 'wizard' }[] = [
  { to: 'dashboard', label: 'shell.navOverview', icon: <SpaceDashboardIcon />, group: 'shell.groupLook', tab: 0 },
  { to: 'history', label: 'shell.navHistory', icon: <ShowChartIcon />, group: 'shell.groupLook', tab: 2 },
  { to: 'economics', label: 'shell.navEconomics', icon: <PaymentsIcon />, group: 'shell.groupLook', tab: 1 },
  { to: 'events', label: 'shell.navEvents', icon: <NotificationsActiveIcon />, group: 'shell.groupWatch', key: 'events' },
  { to: 'devices', label: 'shell.navPoints', icon: <DeviceHubIcon />, group: 'shell.groupSetupNew', tab: 3 },
  { to: 'tariffs', label: 'shell.navTariffsPrices', icon: <RequestQuoteIcon />, group: 'shell.groupSetupNew' },
  // AI assistants (fw 1.3.0, LLM API): only where the hub reports it.
  { to: 'ai', label: 'shell.navAi', icon: <SmartToyIcon />, group: 'shell.groupSetupNew', cap: 'llm' },
  { to: 'onboarding', label: 'shell.navWizard', icon: <RocketLaunchIcon />, group: 'shell.groupSetupNew', key: 'wizard' },
  { to: 'config', label: 'shell.navSettings', icon: <TuneIcon />, group: 'shell.groupSetupNew' },
];

/** The public demo shows several sites (mock/demoVariant); switching reloads the page. Sits under the
 *  logo — at the foot of the rail it went unseen. */
const DemoSitePicker: React.FC = () => (
  <TextField
    select size="small" fullWidth label={t('demo.variant')} value={demoVariant().id}
    // The rail is 220 px: a long site name wraps instead of ending in «…».
    sx={{ '& .MuiSelect-select.MuiSelect-select': { whiteSpace: 'normal', overflow: 'visible', height: 'auto', lineHeight: 1.3 } }}
    onChange={async (e) => { if (await confirmLeave()) setDemoVariant(e.target.value as DemoVariantId); }}
  >
    {demoVariantIds().map((id) => <MenuItem key={id} value={id}>{t(`demo.v_${id}`)}</MenuItem>)}
  </TextField>
);

/** The device block at the foot of the rail. Pressing it opens the device menu (SPEC §4.2 N3): the
 *  theme and «Switch device» — the theme no longer takes room in the rail all the time. */
const DeviceFooter: React.FC<{ state: LinkState; lastUpdate: number | null }> = ({ state, lastUpdate }) => {
  const { connection, demo } = useConnection();
  const [menu, setMenu] = React.useState<HTMLElement | null>(null);
  const label = demo ? t('shell.demoMode') : (connection?.baseUrl || t('shell.thisDevice'));
  return (
    <Box sx={{ p: 1 }}>
      <ButtonBase onClick={(e) => setMenu(e.currentTarget)} aria-haspopup="menu" aria-expanded={!!menu} aria-label={t('shell.deviceMenu')}
        sx={{ width: '100%', display: 'block', textAlign: 'left', borderRadius: 2, p: 1, '&:hover': { bgcolor: 'action.hover' } }}>
        <Stack direction="row" alignItems="center" spacing={1}>
          <Box sx={{ flex: 1, minWidth: 0 }}>
            <Typography variant="caption" color="text.secondary" display="block">
              {demo ? t('shell.demoSite') : t('shell.device')}
            </Typography>
            <Typography variant="body2" fontWeight={600} noWrap title={label}>{label}</Typography>
          </Box>
          <ExpandLessIcon fontSize="small" sx={{ color: 'text.secondary' }} />
        </Stack>
      </ButtonBase>
      <Box sx={{ px: 1, pb: 0.5 }}><LinkStatus state={state} lastUpdate={lastUpdate} demo={demo} /></Box>
      <Menu anchorEl={menu} open={!!menu} onClose={() => setMenu(null)} anchorOrigin={{ vertical: 'top', horizontal: 'left' }} transformOrigin={{ vertical: 'bottom', horizontal: 'left' }}>
        <Box sx={{ minWidth: 240 }}><ThemeSwitch /></Box>
        {!DEMO_ONLY && (
          <MenuItem onClick={async () => { setMenu(null); if (await confirmLeave()) disconnect(); }}>
            <SwapHorizIcon fontSize="small" sx={{ mr: 1.5 }} />{t('shell.switchDevice')}
          </MenuItem>
        )}
      </Menu>
    </Box>
  );
};

export const AppShell: React.FC = () => {
  const navigate = useNavigate();
  const { pathname, state: navState } = useLocation();
  const fromOverview = (navState as { from?: string } | null)?.from === 'dashboard';
  const { demo } = useConnection();
  const confirm = useConfirm();
  // One «Уйти без сохранения?» for the menu, the browser's Back and «Сменить» (rerun 16.10
  // HEU-R03/R05: «Остаться» / «Уйти без сохранения» — «Отменить» read as «undo my edits»).
  useEffect(() => {
    setLeaveAsker(() => confirm({ title: t('form.leaveTitle'), body: t('form.leaveBody'), confirmLabel: t('form.leave'), cancelLabel: t('form.stay'), danger: true }));
    return () => setLeaveAsker(null);
  }, [confirm]);
  const theme = useTheme();
  const mobile = useMediaQuery(theme.breakpoints.down('md'));
  // One shell-level view of the link: status line + «hub unreachable» screen (UX audit T2).
  const tel = useTelemetryEntities();
  const state = linkStateOf(tel.conn, !!tel.data);
  const unreachable = !demo && !tel.data && tel.conn === 'error';
  // Refused for want of a read token: say so, and keep Settings open to enter it.
  const needsToken = unreachable && tel.authRejected;

  // Quick search (UI redesign N9): Ctrl+K / ⌘K anywhere, or the search field in the rail / header.
  const [searchOpen, setSearchOpen] = React.useState(false);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => { if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') { e.preventDefault(); setSearchOpen(true); } };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
  const searchField = (
    <ButtonBase onClick={() => setSearchOpen(true)} aria-label={t('search.open')}
      sx={{ width: '100%', justifyContent: 'flex-start', gap: 1, px: 1.25, py: 0.75, borderRadius: 2, border: 1, borderColor: 'divider', color: 'text.secondary', mb: demo ? 1.5 : 0 }}>
      <SearchIcon fontSize="small" />
      <Typography variant="body2" sx={{ flex: 1, textAlign: 'left' }}>{t('search.short')}</Typography>
      <Typography variant="caption" sx={{ border: 1, borderColor: 'divider', borderRadius: 1, px: 0.5, '@media (pointer: coarse)': { display: 'none' } }}>Ctrl K</Typography>
    </ButtonBase>
  );
  // A new page opens at its top (UI/UX audit U04: Settings opened scrolled by 207 px, its title hidden).
  // An #anchor inside the page still scrolls to its target afterwards.
  useEffect(() => { window.scrollTo(0, 0); document.querySelector('main')?.scrollTo?.(0, 0); }, [pathname]);
  const caps = useCapabilities();
  // Active events, as on the Events page: alarms + points without a link.
  const eventsN = tel.data ? tel.data.units.filter((g) => g.attributes?.['alarm'] === true).length + offlinePoints(tel.data.units).length : 0;
  // The wizard until the hub is set up: points measured and the clock synced — or the wizard was run to its end.
  const [wizardDone] = useLocalStorage<number>('rbgrid.onboarding.reached', 0);
  const setUp = demo || wizardDone >= 6 || (!!tel.data && pointUnits(tel.data.units).length > 0 && (tel.data.system.tq === undefined || tel.data.system.tq === 1));
  // The rail folds to icons (64 px) — by default on 820–1099 px, by choice anywhere (SPEC §4.2 N6).
  const narrow = useMediaQuery('(max-width:1099px)');
  const [railPref, setRailPref] = useLocalStorage<'' | 'open' | 'folded'>('rbgrid.rail', '');
  const collapsed = railPref ? railPref === 'folded' : narrow;
  const nav: NavItem[] = NAV.filter((item) => (!item.cap || caps.flags[item.cap]) && !(item.key === 'wizard' && setUp && !pathname.startsWith('/onboarding'))).map((item) => ({
    label: t(item.label),
    icon: item.icon,
    group: t(item.group),
    tab: item.tab !== undefined,
    tabOrder: item.tab,
    badge: item.key === 'events' ? eventsN : undefined,
    // A point page belongs to «Точки» (HEU-22: nothing was highlighted on /point/*); Prices is a Tariffs tab.
    active: pathname.startsWith(`/${item.to}`) || (item.to === 'devices' && /^\/(point|appliance)\//.test(pathname) && !fromOverview)
      || (item.to === 'dashboard' && /^\/(point|appliance)\//.test(pathname) && fromOverview) || (item.to === 'tariffs' && pathname.startsWith('/prices')),
    onClick: async () => {
      if (!(await confirmLeave())) return;
      navigate(item.to);
    },
  }));

  return (
    <DSAppShell
      brand="RBgrid"
      brandIcon={<BoltIcon color="primary" />}
      nav={nav}
      moreLabel={t('shell.more')}
      navLabel={t('shell.navAria')}
      moreNavLabel={t('shell.moreNavAria')}
      railTop={<>{searchField}{demo && <DemoSitePicker />}</>}
      headerRight={mobile ? (
        <Stack direction="row" alignItems="center" spacing={1}>
          <LinkStatus state={state} lastUpdate={tel.lastUpdate} demo={demo} compact />
          <IconButton size="small" aria-label={t('search.open')} onClick={() => setSearchOpen(true)}><SearchIcon /></IconButton>
          {/* Events on a phone: the bell with the active count (SPEC §4.2 N8). */}
          <IconButton size="small" aria-label={t('shell.eventsBell', { n: eventsN })} onClick={async () => { if (await confirmLeave()) navigate('events'); }}>
            <Badge color="error" badgeContent={eventsN} max={99}><NotificationsIcon /></Badge>
          </IconButton>
        </Stack>
      ) : undefined}
      footer={<DeviceFooter state={state} lastUpdate={tel.lastUpdate} />}
      footerCollapsed={(
        <Box sx={{ display: 'flex', justifyContent: 'center', py: 1 }}>
          <LinkStatus state={state} lastUpdate={tel.lastUpdate} demo={demo} compact />
        </Box>
      )}
      collapsed={!mobile && collapsed}
      onToggleCollapsed={mobile ? undefined : () => setRailPref(collapsed ? 'open' : 'folded')}
      collapseLabel={t('shell.collapse')}
      expandLabel={t('shell.expand')}
    >
      <QuickSearch open={searchOpen} onClose={() => setSearchOpen(false)} />
      {!demo && caps.flags.llm && !pathname.startsWith('/ai') && <LlmPairBanner pending={tel.data?.system.totals?.llm_pair_pending ?? 0} />}
      {needsToken ? (pathname.startsWith('/config') ? <Outlet /> : <DeviceNeedsToken />) : unreachable ? <DeviceUnreachable /> : <Outlet />}
    </DSAppShell>
  );
};
