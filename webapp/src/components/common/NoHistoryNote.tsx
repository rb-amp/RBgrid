/**
 * A point the hub keeps no history for (v1.1.2 units-config `history:false`, root 6e32d29): live values,
 * HA / MQTT and today's counters work, hour/day/month records aren't stored. Why: the hub's history
 * slots are full, or the point was bound after the last restart (slots are set up at boot).
 * Older hubs don't send the flag → nothing shown.
 */
import React from 'react';
import { Alert } from '@mui/material';
import { t } from '../../i18n';
import { useCapabilities } from '../../core';

/** History slots of a hub that doesn't say (v1.1.2 … 1.3.0-dev, operator 03.11 via root). */
export const HISTORY_SLOTS = 12;

/** The hub's own number of history slots (`history_slots`, root history-slots 23.11: 12 in its flash,
 *  49 on the S3 statistics chip, 0 when the chip is missing); 12 when the hub doesn't send it. */
export function useHistorySlots(): number {
  const n = useCapabilities().historySlots;
  return n ?? HISTORY_SLOTS;
}

export const NoHistoryNote: React.FC<{ history?: boolean; historyCount: number | null; name?: string }> = ({ history, historyCount, name }) => {
  const slots = useHistorySlots();
  if (history !== false) return null;
  // 0 slots = the statistics chip is missing: StatsMediaAlert says so; no «full» reason here.
  const why = slots === 0 ? '' : historyCount != null && historyCount >= slots ? t('point.historyFull', { n: slots }) : t('point.historyRestart');
  return (
    <Alert severity="info" sx={{ mb: 1.5 }}>
      {name ? t('point.liveOnlyNamed', { name }) : t('point.liveOnly')} {why}
    </Alert>
  );
};
