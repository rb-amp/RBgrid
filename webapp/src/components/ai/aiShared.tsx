/** Small pieces the AI assistants page, its cards and the app-wide pairing banner share. */
import React, { useState } from 'react';
import { Box, Chip, IconButton, Stack, Tooltip, Typography } from '@mui/material';
import ContentCopyIcon from '@mui/icons-material/ContentCopy';
import { Card } from '@rbgrid/design-system';
import { DeviceClock, formatDeviceTime } from '../../core/deviceTime';
import { t, intlLocale } from '../../i18n';

export const Section: React.FC<{ title: string; children: React.ReactNode; right?: React.ReactNode; id?: string }> = ({ title, children, right, id }) => (
  <Box sx={{ mb: 2 }} id={id}>
    <Card>
      <Stack direction="row" alignItems="center" sx={{ mb: 1.5 }}>
        <Typography variant="h6" fontWeight={700}>{title}</Typography>
        {right && <Box sx={{ ml: 'auto' }}>{right}</Box>}
      </Stack>
      {children}
    </Card>
  </Box>
);

export const scopeLabel = (s: string): string => { const k = `ai.scope_${s}`; const v = t(k); return v === k ? s : v; };
/** «read data» / «read data + propose changes». */
export const scopesText = (scopes: string[]): string => scopes.map(scopeLabel).join(' + ');
export const Scopes: React.FC<{ scopes: string[] }> = ({ scopes }) => (
  <Stack direction="row" spacing={0.5} flexWrap="wrap" useFlexGap>
    {scopes.map((s) => <Chip key={s} size="small" variant="outlined" label={scopeLabel(s)} />)}
  </Stack>
);

export const mmss = (s: number) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
/** «482 913» — easier to read aloud than «482913». */
export const spacedCode = (c: string) => (c.length === 6 ? `${c.slice(0, 3)} ${c.slice(3)}` : c);

/** Before the hub's clock syncs it stamps time from 1970 (seen on the S3 after a restart: «1 Jan, 01:00»). */
export const SYNCED = 1_600_000_000;
export const when = (clock: DeviceClock, ts: number | null, seconds = false): string => {
  if (!ts) return '—';
  if (ts < SYNCED) return t('ai.noClock');
  return formatDeviceTime(clock, ts, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit', ...(seconds ? { second: '2-digit' } : {}) }, intlLocale());
};
/** The tools answer ISO 8601 with the hub's offset; null before clock sync. */
export const whenIso = (clock: DeviceClock, iso: string | null): string => {
  if (!iso) return t('ai.noClock');
  const ms = Date.parse(iso);
  return Number.isFinite(ms) ? when(clock, Math.floor(ms / 1000)) : iso;
};

/** Who made a config version: `web` (this app), `hub` (noticed: the hub page, REST or HA), `llm:<client>`. */
export const sourceText = (src: string): string => {
  if (src.startsWith('llm:')) return t('ai.src_llm', { name: src.slice(4) || t('ai.unnamed') });
  const k = `ai.src_${src}`; const v = t(k);
  return v === k ? src : v;
};

export const CopyButton: React.FC<{ text: string; label?: string }> = ({ text, label }) => {
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try { await navigator.clipboard.writeText(text); setCopied(true); setTimeout(() => setCopied(false), 1500); } catch { /* no clipboard: the text is on screen */ }
  };
  return (
    <Tooltip title={copied ? t('ai.copied') : (label ?? t('ai.copyText'))}>
      <IconButton onClick={copy} aria-label={label ?? t('ai.copyText')}><ContentCopyIcon /></IconButton>
    </Tooltip>
  );
};
