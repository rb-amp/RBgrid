/**
 * ConnectCard — how to connect an assistant on a computer to this hub (CONNECT-UX §2.2 a):
 * a tab per client with the exact command or config to copy, the hub address filled in from
 * GET /api/v1/llm `connect` (mDNS name first, the IP as a fallback), the remote path greyed
 * (a later release) and a note for Home Assistant users.
 */
import React, { useState } from 'react';
import { Alert, Box, Button, FormControlLabel, Stack, Switch, Tab, Tabs, Typography } from '@mui/material';
import { LlmConnect } from '../../core/llm/api';
import { CLIENT_TABS, ClientTab, MCPB_URL, hubUrls, snippet } from '../../core/llm/connect';
import { t } from '../../i18n';
import { CopyButton, Section } from './aiShared';

const Code: React.FC<{ text: string; label: string }> = ({ text, label }) => (
  <Stack direction="row" alignItems="flex-start" spacing={0.5}>
    <Box component="pre" sx={{
      m: 0, p: 1.5, flex: 1, minWidth: 0, overflowX: 'auto', bgcolor: 'action.hover', borderRadius: 1,
      fontFamily: 'monospace', fontSize: 13, whiteSpace: 'pre',
    }}>{text}</Box>
    <CopyButton text={text} label={label} />
  </Stack>
);

export const ConnectCard: React.FC<{ connect: LlmConnect | null; origin: string }> = ({ connect, origin }) => {
  const [tab, setTab] = useState<ClientTab>('claude-desktop');
  const [useIp, setUseIp] = useState(false);
  const [readOnly, setReadOnly] = useState(false);
  const urls = hubUrls(connect, origin);
  const hub = useIp && urls.fallback ? urls.fallback : urls.primary;
  const text = snippet(tab, hub, readOnly);

  return (
    <Section title={t('ai.connectTitle')}>
      {connect && (
        <Typography variant="body2" sx={{ mb: 1 }}>
          {t('ai.thisHub')} <b>{connect.name || 'RBgrid'}</b> · {t('ai.hubId')} <Box component="code">{connect.hub_id}</Box>
        </Typography>
      )}
      <Typography variant="body2">
        {t('ai.hubAddress')} <Box component="code" sx={{ wordBreak: 'break-all' }}>{hub}</Box>
      </Typography>
      {urls.fallback && (
        <FormControlLabel sx={{ mb: 1 }}
          control={<Switch size="small" checked={useIp} onChange={(e) => setUseIp(e.target.checked)} />}
          label={<Typography variant="body2">{t('ai.useIp', { url: urls.fallback })}</Typography>} />
      )}

      <Tabs value={tab} onChange={(_, v) => setTab(v)} variant="scrollable" scrollButtons="auto" sx={{ borderBottom: 1, borderColor: 'divider', mb: 2 }}>
        {CLIENT_TABS.map((c) => <Tab key={c} value={c} label={t(`ai.client_${c}`)} />)}
      </Tabs>

      {tab === 'claude-desktop' ? (
        <Stack spacing={1}>
          <Typography variant="body2">{t('ai.desktop1')}</Typography>
          <Box>
            {MCPB_URL
              ? <Button variant="contained" href={MCPB_URL} target="_blank" rel="noopener noreferrer">{t('ai.desktopDownload')}</Button>
              : <><Button variant="contained" disabled>{t('ai.desktopDownload')}</Button>
                <Typography variant="caption" color="text.secondary" display="block" sx={{ mt: 0.5 }}>{t('ai.desktopSoon')}</Typography></>}
          </Box>
          <Typography variant="body2">{t('ai.desktop2', { url: hub })}</Typography>
          <Typography variant="body2">{t('ai.sayConnect')}</Typography>
        </Stack>
      ) : (
        <Stack spacing={1}>
          <Typography variant="body2">{t(`ai.where_${tab}`)}</Typography>
          {text && <Code text={text} label={t('ai.copyText')} />}
          <FormControlLabel
            control={<Switch size="small" checked={readOnly} onChange={(e) => setReadOnly(e.target.checked)} />}
            label={<Typography variant="body2">{t('ai.bridgeReadOnly')}</Typography>} />
          <Typography variant="body2" color="text.secondary">{t('ai.needsNode')}</Typography>
          <Typography variant="body2">{t('ai.sayConnect')}</Typography>
        </Stack>
      )}

      <Box sx={{ mt: 2, p: 1.5, border: 1, borderColor: 'divider', borderRadius: 1, opacity: 0.6 }} aria-disabled>
        <Typography variant="body2" fontWeight={700}>{t('ai.remoteTitle')}</Typography>
        <Typography variant="body2">{t('ai.remoteBody')}</Typography>
      </Box>
      <Alert severity="info" sx={{ mt: 2 }}>{t('ai.haNote')}</Alert>
    </Section>
  );
};
