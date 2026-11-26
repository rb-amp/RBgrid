/**
 * BridgeConnect — the client tabs with the exact command or config to copy for the local bridge (CONNECT-UX §2.2 a).
 * NOT IMPORTED while the bridge is unpublished (see core/llm/bridge.ts): ConnectCard shows «not published yet»
 * instead. The day it is published, ConnectCard renders <BridgeConnect hub={hub} /> in place of that note.
 */
import React, { useState } from 'react';
import { Box, Button, FormControlLabel, Stack, Switch, Tab, Tabs, Typography } from '@mui/material';
import { CLIENT_TABS, ClientTab, MCPB_URL, snippet } from '../../core/llm/bridge';
import { t, getLang } from '../../i18n';
import PHRASES from './bridgePhrases.json';
import { CopyButton } from './aiShared';

/** The bridge's own phrases live next to it (not in the app's catalogs), so an unpublished bridge ships no text. */
const bt = (key: string, vars?: Record<string, string>): string => {
  const all = PHRASES as Record<string, Record<string, string>>;
  const raw = (all[getLang()] ?? all.en)[key] ?? all.en[key] ?? key;
  return vars ? raw.replace(/\{(\w+)\}/g, (m, k: string) => vars[k] ?? m) : raw;
};

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
        {CLIENT_TABS.map((c) => <Tab key={c} value={c} label={bt(`client_${c}`)} />)}
      </Tabs>
      {tab === 'claude-desktop' ? (
        <Stack spacing={1}>
          <Typography variant="body2">{bt('desktop1')}</Typography>
          <Box>
            <Button variant="contained" href={MCPB_URL} target="_blank" rel="noopener noreferrer">{bt('desktopDownload')}</Button>
          </Box>
          <Typography variant="body2">{bt('desktop2', { url: hub })}</Typography>
          <Typography variant="body2">{bt('sayConnect')}</Typography>
        </Stack>
      ) : (
        <Stack spacing={1}>
          <Typography variant="body2">{bt(`where_${tab}`)}</Typography>
          {text && <Code text={text} label={t('ai.copyText')} />}
          <FormControlLabel
            control={<Switch size="small" checked={readOnly} onChange={(e) => setReadOnly(e.target.checked)} />}
            label={<Typography variant="body2">{bt('bridgeReadOnly')}</Typography>} />
          <Typography variant="body2" color="text.secondary">{bt('needsNode')}</Typography>
          <Typography variant="body2">{bt('sayConnect')}</Typography>
        </Stack>
      )}
    </>
  );
};
