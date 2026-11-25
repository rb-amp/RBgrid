/**
 * ConfigurationPage — settings as Device-Shadow reconciliation. Edit config domains freely
 * (each a generic ShadowConfigEditor), see the combined diff in the sticky reconcile bar,
 * and Apply once (desired → reported convergence; reboot-domains surface a warning first).
 * Capability-gated: a domain panel renders only when the device advertises it.
 * The single settings hub — reconciliation editors + the action panels (network/admin/
 * profile/security).
 */
import React, { useEffect } from 'react';
import { useLocation, useParams } from 'react-router-dom';
import { Box, Typography, Button, Stack, List, ListItemButton, ListItemIcon, ListItemText, useMediaQuery, useTheme } from '@mui/material';
import HomeWorkIcon from '@mui/icons-material/HomeWorkOutlined';
import PaletteIcon from '@mui/icons-material/PaletteOutlined';
import WifiIcon from '@mui/icons-material/Wifi';
import HubIcon from '@mui/icons-material/HubOutlined';
import LockIcon from '@mui/icons-material/LockOutlined';
import MemoryIcon from '@mui/icons-material/Memory';
import ChevronRightIcon from '@mui/icons-material/ChevronRight';
import ArrowBackIcon from '@mui/icons-material/ArrowBack';
import { confirmLeave } from '../app/unsavedGuard';
import { SectionFormsProvider } from '../app/sectionForms';
import { LedPanel } from '../components/settings/LedPanel';
import { RebootStrip } from '../components/common/RebootStrip';
import { useNavigate } from 'react-router-dom';
import { Card, PanelCard, PanelGroup } from '@rbgrid/design-system';
import { useCapabilities, ConfigDomain } from '../core';
import { ShadowConfigEditor } from '../components/settings/ShadowConfigEditor';
import { GroupsPanel } from '../components/settings/GroupsPanel';
import { GridPanel } from '../components/settings/GridPanel';
import { SettingsReconcileBar } from '../components/settings/SettingsReconcileBar';
import { TelemetryPanel } from '../components/settings/TelemetryPanel';
import { EvPanel } from '../components/settings/EvPanel';
import { BackupPanel } from '../components/settings/BackupPanel';
import { HubFirmwarePanel } from '../components/settings/HubFirmwarePanel';
import { SecurityPanel } from '../components/settings/SecurityPanel';
import { NetworkPanel } from '../components/settings/NetworkPanel';
import { HubAccessWarning } from '../components/common/HubAccessWarning';
import { ApPasswordPanel } from '../components/settings/ApPasswordPanel';
import { isDemo } from '../services/connection';
import { fwAtLeast } from '../entities/hubVersion';
import { useHubVersion } from '../hooks/useHubVersion';
import { RELEASE_HIDDEN } from '../constants/release';
import { HomeAssistantPanel } from '../components/settings/HomeAssistantPanel';
import { AdminPanel } from '../components/settings/AdminPanel';
import { CorsPanel } from '../components/settings/CorsPanel';
import { TimePanel } from '../components/settings/TimePanel';
import { InterfacePanel } from '../components/settings/InterfacePanel';
import { t } from '../i18n';

interface DomainPanel {
  domain: ConfigDomain;
  /** Phrase key of the panel title (resolved at render). */
  title: string;
  /** Capability flag gating this panel (undefined = always shown). */
  cap?: 'mqttConfig' | 'telemetryConfigPost';
  /** No working POST for this domain on the device (DirectTransport write table) → view only. */
  readOnly?: boolean;
}

/** Anchors land below the sticky «Нет несохранённых изменений» bar (≈ 60 px), not under it. */
const ANCHOR_MARGIN = 88;

const PANELS: DomainPanel[] = [
  { domain: 'system', title: 'config.panelSystem' },
];

/** Settings sections (UI redesign 22.11, SPEC §5.2): /config/:section, one card each. */
export type SectionId = 'site' | 'ui' | 'network' | 'integrations' | 'security' | 'device';
export const SECTIONS: { id: SectionId; label: string; icon: React.ReactNode }[] = [
  { id: 'site', label: 'config.secSite', icon: <HomeWorkIcon /> },
  { id: 'ui', label: 'config.secUi', icon: <PaletteIcon /> },
  { id: 'network', label: 'config.secNetwork', icon: <WifiIcon /> },
  { id: 'integrations', label: 'config.secIntegrations', icon: <HubIcon /> },
  { id: 'security', label: 'config.secSecurity', icon: <LockIcon /> },
  { id: 'device', label: 'config.secDevice', icon: <MemoryIcon /> },
];

/** Sections with device-shadow fields (the bar shows there even with nothing changed). */
const SHADOW_SECTIONS = new Set<SectionId>(['site']);

/** Old anchors (/config#security, links across the app) → their section. */
const ANCHOR_SECTION: Record<string, SectionId> = {
  system: 'site', grid: 'site', groups: 'site', ev: 'site', tariff: 'site', units: 'site',
  interface: 'ui',
  network: 'network', time: 'network', 'hub-ap': 'network', wifi: 'network',
  telemetry: 'integrations', mqtt: 'integrations', ha: 'integrations',
  security: 'security', cors: 'security',
  backup: 'device', firmware: 'device', admin: 'device', leds: 'device',
};

export const ConfigurationPage: React.FC = () => {
  const caps = useCapabilities();
  const hubVersion = useHubVersion();
  // v1.1.2 (operator 03.11 via root): hub ≥ 1.1.2 that lists the endpoint; hidden until root's .139 run.
  const showAp = !RELEASE_HIDDEN.apPassword && (isDemo() || (caps.endpoints.some((e) => e.path === '/api/system/ap') && fwAtLeast(hubVersion, [1, 1, 2]) === true));
  const { hash } = useLocation();
  const { section: param } = useParams();
  const navigate = useNavigate();
  const theme = useTheme();
  const phone = useMediaQuery(theme.breakpoints.down('md'));
  const anchor = (hash || '').slice(1) || window.location.hash.split('#')[2] || '';
  const sections = SECTIONS.filter((x) => x.id !== 'security' || caps.flags.auth);
  // /config: the section an old anchor points to, else the first one (on a phone: the list).
  const section: SectionId | null = (sections.find((x) => x.id === param)?.id)
    ?? (anchor ? ANCHOR_SECTION[anchor] : undefined) ?? (phone ? null : 'site');
  // Panels load after the first paint and push each other down: retry until the target settles.
  useEffect(() => {
    if (!anchor) return undefined;
    const go = () => document.getElementById(anchor)?.scrollIntoView({ block: 'start' });
    const timers = [50, 500, 1200, 2500].map((ms) => setTimeout(go, ms));
    return () => timers.forEach(clearTimeout);
  }, [anchor, section]);
  const panels = PANELS.filter((p) => !p.cap || caps.flags[p.cap]);
  const open = async (id: SectionId) => { if (await confirmLeave()) navigate(`/config/${id}`); };
  const anchored = (id: string, el: React.ReactNode) => <Box id={id} sx={{ scrollMarginTop: ANCHOR_MARGIN }}>{el}</Box>;

  const body = (id: SectionId) => {
    switch (id) {
      case 'site': return (
        <PanelGroup>
          {panels.map((p) => anchored(p.domain, (
            <PanelCard key={p.domain} title={t(p.title)}><ShadowConfigEditor domain={p.domain} readOnly={p.readOnly} /></PanelCard>
          )))}
          {anchored('grid', <GridPanel />)}
          {anchored('groups', <GroupsPanel />)}
          <EvPanel anchorMargin={ANCHOR_MARGIN} />
          {anchored('tariff', (
            <PanelCard title={t('config.panelTariff')}>
              <Typography variant="body2" color="text.secondary" sx={{ mb: 1.5 }}>{t('config.tariffElsewhere')}</Typography>
              <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
                <Button variant="outlined" size="small" onClick={() => navigate('/tariffs')}>{t('config.openTariffs')}</Button>
                <Button variant="outlined" size="small" onClick={() => navigate('/devices')}>{t('config.openPoints')}</Button>
              </Stack>
            </PanelCard>
          ))}
        </PanelGroup>
      );
      case 'ui': return <PanelGroup>{anchored('interface', <InterfacePanel />)}</PanelGroup>;
      case 'network': return (
        <>
          {showAp && <HubAccessWarning sx={{ mb: 2 }} />}
          <PanelGroup>
            {anchored('network', <NetworkPanel />)}
            {showAp && anchored('hub-ap', <ApPasswordPanel />)}
            {anchored('time', <TimePanel />)}
          </PanelGroup>
        </>
      );
      case 'integrations': return (
        <PanelGroup>
          {caps.flags.mqttConfig && anchored('mqtt', <HomeAssistantPanel />)}
          {anchored('telemetry', <TelemetryPanel />)}
          {caps.flags.llm && (
            <PanelCard title={t('shell.navAi')}>
              <Typography variant="body2" color="text.secondary" sx={{ mb: 1.5 }}>{t('config.aiElsewhere')}</Typography>
              <Button variant="outlined" size="small" onClick={() => navigate('/ai')}>{t('config.openAi')}</Button>
            </PanelCard>
          )}
        </PanelGroup>
      );
      case 'security': return (
        <PanelGroup>
          {anchored('security', <SecurityPanel />)}
          {anchored('cors', <CorsPanel />)}
        </PanelGroup>
      );
      case 'device': return (
        <PanelGroup>
          {anchored('firmware', <HubFirmwarePanel />)}
          {anchored('backup', <BackupPanel />)}
          {caps.flags.leds && anchored('leds', <LedPanel />)}
          <PanelCard title={t('config.panelWizard')}>
            <Typography variant="body2" color="text.secondary" sx={{ mb: 1.5 }}>{t('config.wizardText')}</Typography>
            <Button variant="outlined" size="small" onClick={() => navigate('/onboarding')}>{t('config.openWizard')}</Button>
          </PanelCard>
          {anchored('admin', <AdminPanel />)}
        </PanelGroup>
      );
      default: return null;
    }
  };

  const title = section ? t(SECTIONS.find((x) => x.id === section)!.label) : t('config.title');
  const nav = (
    <List component="nav" aria-label={t('config.sectionsAria')} sx={{ p: 0 }}>
      {sections.map((x) => (
        <ListItemButton key={x.id} selected={x.id === section} onClick={() => void open(x.id)} sx={{ borderRadius: 2, mb: 0.5 }}
          aria-current={x.id === section ? 'page' : undefined}>
          <ListItemIcon sx={{ minWidth: 36 }}>{x.icon}</ListItemIcon>
          <ListItemText primary={t(x.label)} primaryTypographyProps={{ fontWeight: 600, fontSize: 14 }} />
          {phone && <ChevronRightIcon fontSize="small" sx={{ color: 'text.secondary' }} />}
        </ListItemButton>
      ))}
    </List>
  );

  // Phone: the list of sections, then a section with «‹ Settings» back.
  if (phone) {
    return (
      <Box>
        {section ? (
          <>
            <Button size="small" startIcon={<ArrowBackIcon />} sx={{ mb: 1, ml: -1 }} onClick={async () => { if (await confirmLeave()) navigate('/config'); }}>{t('config.title')}</Button>
            <Typography variant="h4" fontWeight={800} sx={{ mb: 2 }}>{title}</Typography>
            <RebootStrip />
            <SectionFormsProvider key={section}>
              <SettingsReconcileBar shadowEditors={SHADOW_SECTIONS.has(section)} />
              {body(section)}
            </SectionFormsProvider>
          </>
        ) : (
          <>
            <Typography variant="h4" fontWeight={800} sx={{ mb: 2 }}>{t('config.title')}</Typography>
            <Card>{nav}</Card>
          </>
        )}
      </Box>
    );
  }

  return (
    <Box>
      <Typography variant="h4" fontWeight={800} sx={{ mb: 2.5 }}>{t('config.title')}</Typography>
      <RebootStrip />
      <Box sx={{ display: 'grid', gridTemplateColumns: '220px minmax(0, 1fr)', gap: 3, alignItems: 'start' }}>
        <Box sx={{ position: 'sticky', top: 16 }}>{nav}</Box>
        <Box sx={{ maxWidth: 880, minWidth: 0 }}>
          {/* The section's name is the selected item on the left (a heading here repeated the first panel's). */}
          {section && (
            <SectionFormsProvider key={section}>
              <SettingsReconcileBar shadowEditors={SHADOW_SECTIONS.has(section)} />
              {body(section)}
            </SectionFormsProvider>
          )}
        </Box>
      </Box>
    </Box>
  );
};
