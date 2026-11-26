/**
 * DesktopBridge — connecting Claude Desktop through the bridge's extension file (.mcpb, published 26.11). The npm
 * package is not published, so no command to paste for other clients (BridgeConnect waits for it, not imported).
 * Steps as the bridge's maintainers measured them: download → open (Claude Desktop installs it; the bundle needs Node ≥ 20.3.0 on the host — Claude Desktop supplies one on
 * macOS and Windows, correction 26.11) → leave «Hub
 * address» empty unless discovery can't reach the hub → «connect to my RBgrid» → read the code from the top of this
 * app → restart Claude Desktop (a permanent step: the client doesn't re-list tools within a session).
 */
import React from 'react';
import { Alert, Box, Button, Stack, Typography } from '@mui/material';
import DownloadIcon from '@mui/icons-material/Download';
import { MCPB_URL, MCPB_VERSION } from '../../core/llm/mcpb';
import { getLang } from '../../i18n';
import PHRASES from './desktopPhrases.json';

const bt = (key: string, vars?: Record<string, string>): string => {
  const all = PHRASES as Record<string, Record<string, string>>;
  const raw = (all[getLang()] ?? all.en)[key] ?? all.en[key] ?? key;
  return vars ? raw.replace(/\{(\w+)\}/g, (m, k: string) => vars[k] ?? m) : raw;
};

export const DesktopBridge: React.FC<{ hub: string }> = ({ hub }) => (
  <Stack spacing={1} sx={{ mt: 1.5 }}>
    <Typography variant="subtitle2" fontWeight={700}>{bt('desktopTitle')}</Typography>
    <Typography variant="body2">{bt('desktop1')}</Typography>
    <Box>
      <Button variant="contained" startIcon={<DownloadIcon />} href={MCPB_URL} target="_blank" rel="noopener noreferrer">{bt('desktopDownload')}</Button>
      <Typography variant="caption" color="text.secondary" display="block" sx={{ mt: 0.5 }}>{bt('fileNote', { version: `v${MCPB_VERSION}` })}</Typography>
    </Box>
    <Typography variant="body2">{bt('desktop2', { url: hub })}</Typography>
    <Typography variant="body2">{bt('sayConnect')}</Typography>
    <Alert severity="warning">{bt('restartAfter')}</Alert>
    <Typography variant="body2" color="text.secondary">{bt('othersLater')}</Typography>
  </Stack>
);
