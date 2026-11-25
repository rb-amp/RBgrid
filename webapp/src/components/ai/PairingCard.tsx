/**
 * PairingCard — one assistant waiting for its code: who asks, what for, the code big and copyable,
 * the time left, and Deny (POST /api/v1/llm {pair_cancel}). The code is never logged.
 */
import React from 'react';
import { Box, Button, Stack, Typography } from '@mui/material';
import { LlmPairing } from '../../core/llm/api';
import { t } from '../../i18n';
import { CopyButton, mmss, scopesText } from './aiShared';

export const PairingCard: React.FC<{ p: LlmPairing; deadline: number; now: number; onDeny: () => void; busy?: boolean }> = ({ p, deadline, now, onDeny, busy }) => {
  const left = Math.max(0, Math.round((deadline - now) / 1000));
  return (
    <Box sx={{ p: 2, border: 2, borderColor: 'primary.main', borderRadius: 2, mb: 1 }}>
      <Typography variant="body2" color="text.secondary">{t('ai.pairFrom')}</Typography>
      <Typography fontWeight={700} sx={{ wordBreak: 'break-word' }}>{p.client_name || t('ai.unnamed')}</Typography>
      <Typography variant="body2">{t('ai.pairWants', { scopes: scopesText(p.scopes) })}</Typography>
      <Stack direction="row" alignItems="center" spacing={1} sx={{ mt: 1 }}>
        <Typography component="div" aria-label={t('ai.codeAria')}
          sx={{ fontFamily: 'monospace', fontSize: { xs: 36, sm: 48 }, fontWeight: 800, letterSpacing: '0.15em', userSelect: 'all' }}>
          {p.code}
        </Typography>
        <CopyButton text={p.code} label={t('ai.copy')} />
      </Stack>
      <Stack direction="row" alignItems="center" spacing={2} sx={{ mt: 0.5 }}>
        <Typography variant="body2" color={left < 60 ? 'warning.main' : 'text.secondary'}>
          {left > 0 ? t('ai.validFor', { time: mmss(left) }) : t('ai.expired')}
        </Typography>
        {p.pair_id && <Button size="small" color="error" variant="outlined" disabled={busy} onClick={onDeny}>{t('ai.deny')}</Button>}
      </Stack>
    </Box>
  );
};
