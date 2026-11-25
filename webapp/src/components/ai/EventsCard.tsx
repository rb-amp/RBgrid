/**
 * EventsCard — the hub's typed event ring, newest 20 (tool get_events via POST /api/v1/llm/call):
 * the same list an assistant sees. Texts are ours per type (the hub's `message` is English and the
 * fallback for a type we don't know).
 */
import React from 'react';
import { Alert, Box, Stack, Typography } from '@mui/material';
import InfoOutlinedIcon from '@mui/icons-material/InfoOutlined';
import WarningAmberIcon from '@mui/icons-material/WarningAmber';
import ErrorOutlineIcon from '@mui/icons-material/ErrorOutline';
import { LlmEvent } from '../../core/llm/api';
import { DeviceClock } from '../../core/deviceTime';
import { t } from '../../i18n';
import { Section, sourceText, whenIso } from './aiShared';

const s = (v: unknown) => (typeof v === 'string' || typeof v === 'number' ? String(v) : '');

export function eventText(e: LlmEvent): string {
  switch (e.type) {
    case 'reboot': return t('ai.ev_reboot', { reason: s(e.data.reason) || '?' });
    case 'config_changed': return e.data.version != null
      ? t('ai.ev_config_changed', { n: s(e.data.version), by: sourceText(s(e.data.source)) }) : e.message;
    case 'module_lost': return t('ai.ev_module_lost', { name: s(e.data.name) || e.channel || '?' });
    case 'module_back': return t('ai.ev_module_back', { name: s(e.data.name) || e.channel || '?' });
    case 'clock_unsynced': return t('ai.ev_clock_unsynced');
    case 'price_missing': return t('ai.ev_price_missing');
    default: return e.message || e.type;
  }
}

const Icon: React.FC<{ sev: LlmEvent['severity'] }> = ({ sev }) => (sev === 'critical'
  ? <ErrorOutlineIcon fontSize="small" color="error" aria-label={t('ai.sev_critical')} />
  : sev === 'warning' ? <WarningAmberIcon fontSize="small" color="warning" aria-label={t('ai.sev_warning')} />
    : <InfoOutlinedIcon fontSize="small" color="action" aria-label={t('ai.sev_info')} />);

export const EventsCard: React.FC<{ events: LlmEvent[] | null; error: string | null; clock: DeviceClock }> = ({ events, error, clock }) => (
  <Section title={t('ai.eventsTitle')}>
    <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>{t('ai.eventsLead')}</Typography>
    {error && <Alert severity="warning" sx={{ mb: 1 }}>{t('ai.eventsFailed', { why: error })}</Alert>}
    {events && events.length === 0 && <Typography color="text.secondary">{t('ai.eventsNone')}</Typography>}
    {events && events.length > 0 && (
      <Stack spacing={0.75}>
        {events.map((e) => (
          <Stack key={e.seq} direction="row" spacing={1} alignItems="flex-start">
            <Box sx={{ pt: '2px' }}><Icon sev={e.severity} /></Box>
            <Typography variant="body2" color="text.secondary" sx={{ minWidth: 120, whiteSpace: 'nowrap' }}>{whenIso(clock, e.time)}</Typography>
            <Typography variant="body2" sx={{ wordBreak: 'break-word' }}>{eventText(e)}</Typography>
          </Stack>
        ))}
      </Stack>
    )}
  </Section>
);
