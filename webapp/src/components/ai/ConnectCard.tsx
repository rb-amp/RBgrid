/**
 * ConnectCard — how to connect an assistant on a computer to this hub (CONNECT-UX §2.2 a): the hub address from
 * GET /api/v1/llm `connect` (mDNS name first, the IP as a fallback), the remote path greyed (a later release) and a
 * note for Home Assistant users.
 *
 * The bridge isn't published yet (25.11: npm answers 404 for the package), so the card offers no command to copy —
 * an instruction that fails is worse than none — and says so. The client tabs live in BridgeConnect, not imported
 * until the bridge is published (the release check wants no bridge package name in the kit).
 */
import React, { useState } from 'react';
import { Alert, Box, FormControlLabel, Switch, Typography } from '@mui/material';
import { LlmConnect } from '../../core/llm/api';
import { hubUrls } from '../../core/llm/connect';
import { t } from '../../i18n';
import { Section } from './aiShared';

export const ConnectCard: React.FC<{ connect: LlmConnect | null; origin: string }> = ({ connect, origin }) => {
  const [useIp, setUseIp] = useState(false);
  const urls = hubUrls(connect, origin);
  const hub = useIp && urls.fallback ? urls.fallback : urls.primary;

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

      <Alert severity="info" sx={{ mt: 1 }}>
        <Typography variant="body2" fontWeight={600}>{t('ai.bridgeNotYetTitle')}</Typography>
        <Typography variant="body2">{t('ai.bridgeNotYet')}</Typography>
      </Alert>

      <Box sx={{ mt: 2, p: 1.5, border: 1, borderColor: 'divider', borderRadius: 1, opacity: 0.6 }} aria-disabled>
        <Typography variant="body2" fontWeight={700}>{t('ai.remoteTitle')}</Typography>
        <Typography variant="body2">{t('ai.remoteBody')}</Typography>
      </Box>
      <Alert severity="info" sx={{ mt: 2 }}>{t('ai.haNote')}</Alert>
    </Section>
  );
};
