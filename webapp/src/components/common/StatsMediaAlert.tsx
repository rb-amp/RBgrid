/**
 * StatsMediaAlert — the hub's statistics chip is missing (fw 1.3.x `stats_media: "none"`, root
 * stats-media-missing-chip 21.11). A hub that adopted an external flash chip and lost it keeps
 * statistics in RAM only: history reads empty while `storage_state` still says "ready", which looks
 * like lost history. Shown on the history and statistics screens; no button — the hub finds the chip
 * again on its next restart. `internal` / `external` / absent (older hubs) → nothing.
 * Source: schema `stats_media` at connect, then GET /api/system/version (public) every minute while
 * the tab is visible — the chip can drop out mid-session.
 */
import React, { useEffect, useState } from 'react';
import { Alert } from '@mui/material';
import { useCapabilities } from '../../core';
import { getApiClient } from '../../services/api';
import { isDemo } from '../../services/connection';
import { t } from '../../i18n';

const POLL_MS = 60_000;

export function useStatsMedia(): string | null {
  const caps = useCapabilities();
  const [media, setMedia] = useState<string | null>(caps.statsMedia);
  useEffect(() => { setMedia(caps.statsMedia); }, [caps.statsMedia]);
  useEffect(() => {
    if (isDemo()) return undefined;
    let alive = true;
    const read = () => {
      if (typeof document !== 'undefined' && document.visibilityState === 'hidden') return;
      Promise.resolve().then(() => getApiClient().getHubVersion())
        .then((v) => { if (alive && typeof v?.stats_media === 'string') setMedia(v.stats_media); })
        .catch(() => undefined);
    };
    read();
    const id = setInterval(read, POLL_MS);
    return () => { alive = false; clearInterval(id); };
  }, []);
  return media;
}

export const StatsMediaAlert: React.FC = () => {
  const media = useStatsMedia();
  if (media !== 'none') return null;
  return <Alert severity="warning" role="alert" sx={{ mb: 2 }}>{t('history.statsChipMissing')}</Alert>;
};
