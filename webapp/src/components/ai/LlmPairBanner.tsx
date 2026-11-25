/**
 * LlmPairBanner — on every page, while an assistant waits for its pairing code (CONNECT-UX §2.2 b):
 * «<client> wants to connect — code 482 913 · 8:41 left · [Show] [Deny]». The chat tells the user
 * to look at the hub, so the code must be visible wherever they are in the app.
 * Trigger: telemetry `system.llm_pair_pending` (fw 1.3.0-dev, present while the API is on) — no
 * polling while nothing is pending. The code itself needs GET /api/v1/llm (admin token); without
 * it the banner still says an assistant is waiting and leads to the page.
 */
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { Alert, Box, Button, Stack } from '@mui/material';
import { useNavigate } from 'react-router-dom';
import { llmApi, LlmPairing } from '../../core/llm/api';
import { t } from '../../i18n';
import { mmss, spacedCode } from './aiShared';

const REFRESH_MS = 5000;

export const LlmPairBanner: React.FC<{ pending: number }> = ({ pending }) => {
  const navigate = useNavigate();
  const [pairs, setPairs] = useState<LlmPairing[] | null>(null);
  const [failed, setFailed] = useState(false);
  const [busy, setBusy] = useState(false);
  const [now, setNow] = useState(() => Date.now());
  const deadlines = useRef(new Map<string, number>());

  const load = useCallback(async () => {
    try {
      const d = await llmApi.get();
      const at = Date.now();
      for (const p of d.pairing) if (!deadlines.current.has(p.code)) deadlines.current.set(p.code, at + p.expires_in * 1000);
      setPairs(d.pairing); setFailed(false);
    } catch { setFailed(true); }
  }, []);

  useEffect(() => {
    if (pending <= 0) { setPairs(null); deadlines.current.clear(); return undefined; }
    load();
    const id = setInterval(() => { load(); }, REFRESH_MS);
    const tick = setInterval(() => setNow(Date.now()), 1000);
    return () => { clearInterval(id); clearInterval(tick); };
  }, [pending, load]);

  if (pending <= 0) return null;
  const p = pairs?.find((x) => (deadlines.current.get(x.code) ?? 0) > now) ?? null;
  if (!p && !failed && pairs) return null; // the count lags the hub by one frame: nothing left to show
  const left = p ? Math.max(0, Math.round(((deadlines.current.get(p.code) ?? now) - now) / 1000)) : 0;
  const more = (pairs?.length ?? 0) - 1;

  const deny = async () => {
    if (!p) return;
    setBusy(true);
    try { const d = await llmApi.set({ pair_cancel: p.pair_id }); setPairs(d.pairing); } catch { /* the page has the details */ } finally { setBusy(false); }
  };

  return (
    <Alert severity="info" variant="filled" sx={{ mb: 2, alignItems: 'center' }} role="status"
      action={(
        <Stack direction="row" spacing={1}>
          <Button color="inherit" size="small" variant="outlined" onClick={() => navigate('/ai')}>{t('ai.bannerShow')}</Button>
          {p?.pair_id && <Button color="inherit" size="small" disabled={busy} onClick={deny}>{t('ai.deny')}</Button>}
        </Stack>
      )}>
      {p ? (
        <>
          {t('ai.bannerWants', { name: p.client_name || t('ai.unnamed') })}{' '}
          <Box component="span" sx={{ fontFamily: 'monospace', fontWeight: 800, fontSize: '1.15em', whiteSpace: 'nowrap' }}>{spacedCode(p.code)}</Box>
          {' · '}{t('ai.bannerLeft', { time: mmss(left) })}
          {more > 0 && <> · {t('ai.bannerMore', { n: more })}</>}
        </>
      ) : t('ai.bannerNoCode')}
    </Alert>
  );
};
