/**
 * QuickSearch — go anywhere by name (UI redesign N9, SPEC §4.2): Ctrl+K / ⌘K or the search button.
 * Finds pages, metering points (name, short name) and settings — sections and the panels inside
 * them by their titles and a few keywords. Enter opens the first match; ↑ ↓ move.
 */
import React, { useEffect, useMemo, useState } from 'react';
import { Dialog, InputAdornment, List, ListItemButton, ListItemText, TextField, Typography } from '@mui/material';
import SearchIcon from '@mui/icons-material/Search';
import { useNavigate } from 'react-router-dom';
import { useCapabilities, useConfigShadow } from '../core';
import { hexKey } from '../entities/siteModel';
import { roleLabel } from '../pages/overviewData';
import { confirmLeave } from './unsavedGuard';
import { t } from '../i18n';

interface Hit { label: string; kind: string; to: string; words?: string; tab?: string }

/** Settings panels by section (their titles; keywords in the user's words). */
/**
 * Settings panels by section: their titles and the words a user would type — the words are phrases
 * (search.w_<key>, both languages), so «часовой пояс» finds the time panel as «time zone» does
 * (role audit 22.11: English-only keywords found nothing in Russian).
 */
const SETTINGS: { key: string; section: string; anchor?: string; title: string; to?: string; where?: string; tab?: string }[] = [
  { key: 'site', section: 'site', title: 'config.panelSystem' },
  { key: 'grid', section: 'site', anchor: 'grid', title: 'gridPanel.title' },
  { key: 'groups', section: 'site', anchor: 'groups', title: 'groups.title' },
  { key: 'ev', section: 'site', anchor: 'ev', title: 'evPanel.title' },
  { key: 'ui', section: 'ui', anchor: 'interface', title: 'ui.title' },
  { key: 'wifi', section: 'network', anchor: 'network', title: 'search.wifi' },
  { key: 'time', section: 'network', anchor: 'time', title: 'time.title' },
  { key: 'mqtt', section: 'integrations', anchor: 'mqtt', title: 'ha.title' },
  { key: 'telemetry', section: 'integrations', anchor: 'telemetry', title: 'telemetry.title' },
  { key: 'token', section: 'security', anchor: 'security', title: 'security.title' },
  { key: 'cors', section: 'security', anchor: 'cors', title: 'cors.title' },
  { key: 'firmware', section: 'device', anchor: 'firmware', title: 'hubFw.title' },
  { key: 'backup', section: 'device', anchor: 'backup', title: 'backup.title' },
  { key: 'admin', section: 'device', anchor: 'admin', title: 'admin.title' },
  { key: 'leds', section: 'device', anchor: 'leds', title: 'led.title' },
  { key: 'tariff', section: '', title: 'tariffs.title', to: '/tariffs' },
  { key: 'prices', section: '', title: 'prices.title', to: '/tariffs/dynamic' },
  // The input limit is the input point's own setting (round 2 N04: it led to the point list); its
  // address is filled from the configured input below.
  { key: 'limits', section: '', title: 'search.limits', to: '/devices', where: 'search.whereInput' },
  { key: 'modules', section: '', title: 'search.modules', to: '/devices', where: 'search.whereHardware', tab: 'hardware' },
  // «датчик», «ct», «калибровка» found nothing (round 3 installer N14): the sensors are on Hardware.
  { key: 'sensors', section: '', title: 'search.sensors', to: '/devices', where: 'search.whereHardware', tab: 'hardware' },
  { key: 'groups2', section: '', title: 'groups.title', to: '/config/site#groups', where: 'search.whereSite' },
];

const WORDS_PAGE: Record<string, string> = {
  '/dashboard': 'search.wp_dashboard', '/history': 'search.wp_history', '/economics': 'search.wp_economics', '/events': 'search.wp_events',
  '/devices': 'search.wp_devices', '/tariffs': 'search.wp_tariffs', '/config': 'search.wp_config', '/onboarding': 'search.wp_onboarding',
};
const ROLE_WORDS = new Set(['HEATING_COOLING', 'EV_CHARGING', 'RENEWABLE_SOURCE', 'MAIN_SUPPLY']);

/** Every word of the query is somewhere in the hit (any order): «пояс часовой», «mqtt брокер». */
const matches = (hay: string, needle: string): boolean => needle.split(/\s+/).filter(Boolean).every((w) => hay.includes(w));

const PAGES: { to: string; label: string }[] = [
  { to: '/dashboard', label: 'shell.navOverview' }, { to: '/history', label: 'shell.navHistory' },
  { to: '/economics', label: 'shell.navEconomics' }, { to: '/events', label: 'shell.navEvents' },
  { to: '/devices', label: 'shell.navPoints' }, { to: '/tariffs', label: 'shell.navTariffsPrices' },
  { to: '/config', label: 'shell.navSettings' }, { to: '/onboarding', label: 'shell.navWizard' },
];

export const QuickSearch: React.FC<{ open: boolean; onClose: () => void }> = ({ open, onClose }) => {
  const navigate = useNavigate();
  const units = useConfigShadow('units');
  const list = Array.isArray(units.reported.units) ? (units.reported.units as { unit_id?: unknown; name?: unknown; short_name?: unknown; role?: unknown }[]) : [];
  const caps = useCapabilities();
  const [q, setQ] = useState('');
  const [sel, setSel] = useState(0);
  useEffect(() => { if (open) { setQ(''); setSel(0); } }, [open]);

  const all = useMemo<Hit[]>(() => {
    const pages: Hit[] = [...PAGES, ...(caps.flags.llm ? [{ to: '/ai', label: 'shell.navAi' }] : [])]
      // Words a user types for a page (round 3: «выручка», «нет связи», «линии», «тревога» found nothing).
      .map((p) => ({ label: t(p.label), kind: t('search.page'), to: p.to, words: WORDS_PAGE[p.to] ? t(WORDS_PAGE[p.to]) : undefined }));
    const points: Hit[] = list.map((u) => {
      const hex = hexKey(u.unit_id as never) ?? String(u.unit_id ?? '');
      const name = String(u.name || u.short_name || hex);
      // The role in words too: «тепловой насос», «зарядка» find the demo's «Heat pump», «EV-1».
      const role = typeof u.role === 'string' ? u.role : '';
      const roleWords = role ? `${roleLabel(role)} ${ROLE_WORDS.has(role) ? t(`search.r_${role}`) : ''}` : '';
      return { label: name, kind: t('search.point'), to: `/point/${hex}`, words: `${String(u.short_name ?? '')} ${hex} ${roleWords}` };
    }).filter((h) => !!h.to.split('/')[2]);
    const mainHex = (() => { const m = list.find((u) => u.role === 'MAIN_SUPPLY'); return m ? hexKey(m.unit_id as never) : null; })();
    const settings: Hit[] = SETTINGS.filter((x) => x.key !== 'groups2' && (x.key !== 'leds' || caps.flags.leds)).map((x) => ({
      label: t(x.title),
      // Where it is: the section's name under the hit («Settings · Network & time»), or the place.
      kind: x.section ? `${t('shell.navSettings')} · ${t(`config.sec${x.section[0].toUpperCase()}${x.section.slice(1)}`)}` : x.where ? t(x.where) : t('search.page'),
      to: x.key === 'limits' && mainHex ? `/point/${mainHex}?setup` : x.to ?? `/config/${x.section}${x.anchor ? `#${x.anchor}` : ''}`,
      words: t(`search.w_${x.key}`),
      tab: x.tab,
    }));
    return [...pages, ...points, ...settings];
  }, [list, caps.flags.llm, caps.flags.leds]);

  const needle = q.trim().toLowerCase();
  const hits = needle ? all.filter((h) => matches(`${h.label} ${h.words ?? ''} ${h.kind}`.toLowerCase(), needle)).slice(0, 12) : all.slice(0, 8);
  const go = async (h: Hit | undefined) => {
    if (!h) return;
    onClose();
    if (!(await confirmLeave())) return;
    if (h.tab) { try { window.sessionStorage.setItem('rbgrid.devices_tab', JSON.stringify(h.tab)); } catch { /* private mode */ } }
    navigate(h.to);
  };

  return (
    <Dialog open={open} onClose={onClose} maxWidth="sm" fullWidth PaperProps={{ sx: { alignSelf: 'flex-start', mt: '12vh' } }}>
      <TextField autoFocus fullWidth placeholder={t('search.placeholder')} value={q}
        onChange={(e) => { setQ(e.target.value); setSel(0); }}
        onKeyDown={(e) => {
          if (e.key === 'ArrowDown') { e.preventDefault(); setSel((s) => Math.min(hits.length - 1, s + 1)); }
          if (e.key === 'ArrowUp') { e.preventDefault(); setSel((s) => Math.max(0, s - 1)); }
          if (e.key === 'Enter') { e.preventDefault(); void go(hits[sel]); }
        }}
        inputProps={{ 'aria-label': t('search.placeholder') }}
        InputProps={{ startAdornment: <InputAdornment position="start"><SearchIcon /></InputAdornment>, sx: { borderRadius: 0, '& fieldset': { border: 'none' } } }}
        sx={{ borderBottom: 1, borderColor: 'divider' }} />
      {hits.length ? (
        <List dense sx={{ maxHeight: 420, overflowY: 'auto' }}>
          {hits.map((h, i) => (
            <ListItemButton key={`${h.to}-${i}`} selected={i === sel} onClick={() => void go(h)}>
              <ListItemText primary={h.label} secondary={h.kind} primaryTypographyProps={{ fontWeight: 600 }} />
            </ListItemButton>
          ))}
        </List>
      ) : (
        <Typography color="text.secondary" sx={{ p: 2 }}>{t('search.nothing')}</Typography>
      )}
    </Dialog>
  );
};
