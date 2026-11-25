/**
 * BridgeConnect — the client tabs with the exact command or config to copy for the local bridge (CONNECT-UX §2.2 a).
 * NOT IMPORTED while the bridge is unpublished (see core/llm/bridge.ts): ConnectCard shows «not published yet»
 * instead. The day it is published, ConnectCard renders <BridgeConnect hub={hub} /> in place of that note.
 */
import React, { useState } from 'react';
import { Box, Button, FormControlLabel, Stack, Switch, Tab, Tabs, Typography } from '@mui/material';
import { CLIENT_TABS, ClientTab, MCPB_URL, snippet } from '../../core/llm/bridge';
import { t } from '../../i18n';
import { CopyButton } from './aiShared';

const Code: React.FC<{ text: string; label: string }> = ({ text, label }) => (
  <Stack direction="row" alignItems="flex-start" spacing={0.5}>
    <Box component="pre" sx={{
      m: 0, p: 1.5, flex: 1, minWidth: 0, overflowX: 'auto', bgcolor: 'action.hover', borderRadius: 1,
      fontFamily: 'monospace', fontSize: 13, whiteSpace: 'pre',
    }}>{text}</Box>
    <CopyButton text={text} label={label} />
  </Stack>
);

export const BridgeConnect: React.FC<{ hub: string }> = ({ hub }) => {
  const [tab, setTab] = useState<ClientTab>('claude-desktop');
  const [readOnly, setReadOnly] = useState(false);
  const text = snippet(tab, hub, readOnly);
  return (
    <>
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
    </>
  );
};
