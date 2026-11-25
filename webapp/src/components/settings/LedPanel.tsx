/**
 * LedPanel — the hub's two status lights (fw 1.3.0, root LED_INDICATION.md §5), in Settings → Device:
 *  1. what green / red / a process show now, in words from the registry id (the hub's English text as fallback);
 *  2. an event the red light reports (R7 unexpected restart, R8 rolled-back update) → «Acknowledge» (POST {ack:true});
 *  3. the mode: all / quiet / off (POST {mode}, applies at once);
 *  4. «Find this hub»: both lights blink 10 s (POST {identify:true}).
 * Shown only where the hub lists /api/v1/led. Re-read every 5 s while open (GET needs no token).
 */
import React, { useCallback, useEffect, useState } from 'react';
import { Alert, Box, Button, Stack, Typography } from '@mui/material';
import { PanelCard, SettingRow } from '@rbgrid/design-system';
import { getApiClient } from '../../services/api';
import { LedMode, LedShow, LedState } from '../../types/led';
import { t, tCode } from '../../i18n';

const REFRESH_MS = 5000;
const MODES: LedMode[] = ['full', 'quiet', 'off'];

/** «working normally», «a module is missing · 2 flashes». */
export function ledWords(s: LedShow | null): string {
  if (!s) return t('led.off');
  const known = tCode('led', s.id);
  const words = known !== s.id ? known : (s.state || s.id);
  const m = /^R(\d)$/.exec(s.id);
  return m ? `${words} · ${t('led.flashes', { n: Number(m[1]) })}` : words;
}

export const LedPanel: React.FC = () => {
  const [st, setSt] = useState<LedState | null>(null);
  const [failed, setFailed] = useState(false);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);

  const load = useCallback(async () => {
    try { setSt(await getApiClient().getLed()); setFailed(false); } catch { setFailed(true); }
  }, []);
  useEffect(() => {
    void load();
    const id = setInterval(() => { void load(); }, REFRESH_MS);
    return () => clearInterval(id);
  }, [load]);

  const post = async (patch: Parameters<ReturnType<typeof getApiClient>['setLed']>[0], ok: string) => {
    setBusy(true); setMsg(null);
    try {
      setSt(await getApiClient().setLed(patch));
      setMsg({ ok: true, text: ok });
    } catch (e) {
      setMsg({ ok: false, text: t('led.saveFailed', { why: e instanceof Error ? e.message : String(e) }) });
    } finally { setBusy(false); }
  };

  return (
    <PanelCard title={t('led.title')}>
      <Stack spacing={1.5}>
        <Typography variant="body2" color="text.secondary">{t('led.intro')}</Typography>
        {failed && !st && <Alert severity="warning">{t('led.loadFailed')}</Alert>}

        {st && <>
          <Typography variant="overline" color="text.secondary" sx={{ lineHeight: 1.6 }}>{t('led.now')}</Typography>
          {st.process
            ? <SettingRow label={t('led.both')} value={ledWords(st.process)} />
            : <>
              <SettingRow label={t('led.green')} value={ledWords(st.green)} />
              <SettingRow label={t('led.red')} value={ledWords(st.red)} />
            </>}

          {st.events_pending && (
            <Alert severity="warning" action={(
              <Button color="inherit" size="small" disabled={busy} onClick={() => post({ ack: true }, t('led.acked'))}>{t('led.ack')}</Button>
            )}>
              <Box sx={{ fontWeight: 600 }}>{t('led.eventTitle')}</Box>
              {t('led.eventText')}
            </Alert>
          )}

          <Typography variant="overline" color="text.secondary" sx={{ lineHeight: 1.6, pt: 0.5 }}>{t('led.mode')}</Typography>
          <Stack direction="row" spacing={1}>
            {MODES.map((m) => (
              <Button key={m} size="small" variant={st.mode === m ? 'contained' : 'outlined'} disabled={busy}
                aria-pressed={st.mode === m}
                onClick={() => { if (st.mode !== m) void post({ mode: m }, t('led.modeSaved', { mode: t(`led.mode_${m}`) })); }}>
                {t(`led.mode_${m}`)}
              </Button>
            ))}
          </Stack>
          {MODES.includes(st.mode as LedMode) && (
            <Typography variant="caption" color="text.secondary">{t(`led.modeHint_${st.mode}`)}</Typography>
          )}

          <Box>
            <Button size="small" variant="outlined" disabled={busy} onClick={() => post({ identify: true }, t('led.identifyOn'))}>{t('led.identify')}</Button>
            <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 0.5 }}>{t('led.identifyHint')}</Typography>
          </Box>
        </>}

        {msg && <Alert severity={msg.ok ? 'success' : 'error'}>{msg.text}</Alert>}
      </Stack>
    </PanelCard>
  );
};
