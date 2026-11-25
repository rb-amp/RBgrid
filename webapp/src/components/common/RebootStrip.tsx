/**
 * RebootStrip — the one «Waits for a restart» line (SPEC §5.3), on Settings and the Overview:
 * what was stored that the hub reads only at start, and «Restart now».
 */
import React from 'react';
import { Alert } from '@mui/material';
import { useRebootPending, clearRebootPending } from '../../app/rebootPending';
import { RebootNowButton } from './RebootNowButton';
import { t } from '../../i18n';

export const RebootStrip: React.FC = () => {
  const items = useRebootPending();
  if (!items.length) return null;
  return (
    <Alert severity="warning" sx={{ mb: 2 }} action={<RebootNowButton onDone={(ok) => { if (ok) clearRebootPending(); }} />}>
      {t('reconcile.waitsRestart', { n: items.length, list: items.join(', ') })}
    </Alert>
  );
};
