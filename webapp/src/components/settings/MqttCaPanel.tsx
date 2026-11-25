/**
 * MqttCaPanel — the broker CA for mqtts:// (hub 1.1.8, root mqtt-ca-ui): upload a PEM (≤ 8 KB,
 * certificates only), see each certificate's subject / issuer / dates, remove it (back to the public
 * CA bundle). Both apply at the next restart → «Restart now». Shown only where the hub reports `tls`
 * in mqtt-config; the TLS state line sits next to the broker (MqttBrokerForm).
 */
import { MoreText } from '../common/MoreText';
import React, { useEffect, useRef, useState } from 'react';
import { Alert, Box, Button, Stack, Typography } from '@mui/material';
import { useDeviceSession, MqttCaStatus } from '../../core';
import { useMarkReboot } from '../../app/rebootPending';
import { t, dateLocale } from '../../i18n';
import { useConfirm } from '../common/ConfirmDialog';

/** The hub's own cap on the upload (POST /api/system/mqtt-ca). */
export const MQTT_CA_MAX_BYTES = 8192;

const day = (iso?: string): string => {
  const d = iso ? new Date(iso) : null;
  return d && !Number.isNaN(d.getTime()) ? d.toLocaleDateString(dateLocale(), { day: 'numeric', month: 'short', year: 'numeric', timeZone: 'UTC' }) : '—';
};

const readText = (f: File): Promise<string> => new Promise((ok, fail) => {
  const r = new FileReader();
  r.onload = () => ok(String(r.result ?? ''));
  r.onerror = () => fail(r.error);
  r.readAsText(f);
});

/** A refusal in plain words: the hub's 400 field=pem, the admin token, or the transport's message. */
export function mqttCaError(e: unknown): string {
  const status = (e as { response?: { status?: number } })?.response?.status;
  if (status === 400) return t('mqttCa.rejected');
  if (status === 401 || status === 403) return t('common.adminTokenRequired');
  return t('mqttCa.failed', { reason: e instanceof Error ? e.message : String(e) });
}

export const MqttCaPanel: React.FC = () => {
  const session = useDeviceSession();
  const confirm = useConfirm();
  const tr = session.transport;
  const [ca, setCa] = useState<MqttCaStatus | null>(null);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'warning' | 'error'; text: string; reboot?: boolean } | null>(null);
  // «Saved, after a restart» goes to the one «Waits for a restart» strip (SPEC §5.3), not a button here.
  useMarkReboot(!!msg?.reboot, t('mqttCa.title'));
  const file = useRef<HTMLInputElement>(null);
  const canWrite = typeof tr.setMqttCa === 'function';

  useEffect(() => {
    let on = true;
    tr.getMqttCa?.().then((c) => { if (on) setCa(c); }).catch(() => undefined);
    return () => { on = false; };
  }, [tr]);

  const done = (c: MqttCaStatus, text: string) => {
    setCa(c);
    setMsg(c.warning ? { sev: 'warning', text: t('mqttCa.notCa'), reboot: true } : { sev: 'warning', text, reboot: true });
  };

  const upload = async (f: File) => {
    setMsg(null);
    if (f.size > MQTT_CA_MAX_BYTES) { setMsg({ sev: 'error', text: t('mqttCa.tooBig') }); return; }
    const pem = await readText(f);
    if (!/-----BEGIN CERTIFICATE-----/.test(pem)) { setMsg({ sev: 'error', text: t('mqttCa.notPem') }); return; }
    if (/PRIVATE KEY-----/.test(pem)) { setMsg({ sev: 'error', text: t('mqttCa.privateKey') }); return; }
    setBusy(true);
    try { done(await tr.setMqttCa!(pem), t('mqttCa.uploaded')); } catch (e) { setMsg({ sev: 'error', text: mqttCaError(e) }); } finally { setBusy(false); }
  };

  const remove = async () => {
    if (!tr.deleteMqttCa) return;
    if (!(await confirm({ title: t('mqttCa.removeTitle'), body: t('mqttCa.removeBody'), confirmLabel: t('mqttCa.remove'), danger: true }))) return;
    setBusy(true); setMsg(null);
    try { done(await tr.deleteMqttCa(), t('mqttCa.removed')); } catch (e) { setMsg({ sev: 'error', text: mqttCaError(e) }); } finally { setBusy(false); }
  };

  return (
    <Box>
      <Typography variant="subtitle2" fontWeight={700}>{t('mqttCa.title')}</Typography>
      <MoreText text={t('mqttCa.hint')} sx={{ mb: 1 }} />
      {ca?.present && ca.certs.length ? (
        <Stack spacing={0.75} sx={{ mb: 1 }}>
          {ca.certs.map((c, i) => (
            <Box key={i} sx={{ borderLeft: 3, borderColor: c.ca === false ? 'warning.main' : 'divider', pl: 1 }}>
              <Typography variant="body2" sx={{ overflowWrap: 'anywhere' }}>{c.subject || '—'}</Typography>
              <Typography variant="caption" color="text.secondary" component="div" sx={{ overflowWrap: 'anywhere' }}>
                {t('mqttCa.issuer', { issuer: c.issuer || '—' })} · {t('mqttCa.valid', { from: day(c.valid_from), to: day(c.valid_to) })}
                {c.ca === false ? ` · ${t('mqttCa.notCaShort')}` : ''}
              </Typography>
            </Box>
          ))}
        </Stack>
      ) : (
        <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>{t('mqttCa.none')}</Typography>
      )}
      <Stack direction="row" spacing={1}>
        <input ref={file} type="file" accept=".pem,.crt,.cer,application/x-pem-file,application/x-x509-ca-cert" hidden
          onChange={(e) => { const f = e.target.files?.[0]; e.target.value = ''; if (f) void upload(f); }} />
        <Button size="small" variant="outlined" disabled={!canWrite || busy} onClick={() => file.current?.click()}>
          {ca?.present ? t('mqttCa.replace') : t('mqttCa.upload')}
        </Button>
        {ca?.present && (
          <Button size="small" color="error" disabled={!canWrite || busy} onClick={remove}>{t('mqttCa.remove')}</Button>
        )}
      </Stack>
      {msg && <Alert severity={msg.sev} sx={{ mt: 1 }}>{msg.text}</Alert>}
    </Box>
  );
};

/** mqtt-config `tls.state` → the line next to the broker; null for `off` (plain mqtt://) or an older hub. */
export function tlsStateLine(state: unknown): { sev: 'success' | 'info' | 'warning' | 'error'; text: string } | null {
  switch (state) {
    case 'ok': return { sev: 'success', text: t('mqttCa.state.ok') };
    case 'connecting': return { sev: 'info', text: t('mqttCa.state.connecting') };
    case 'waiting_clock': return { sev: 'info', text: t('mqttCa.state.waiting_clock') };
    case 'expired': return { sev: 'error', text: t('mqttCa.state.expired') };
    case 'not_yet_valid': return { sev: 'error', text: t('mqttCa.state.not_yet_valid') };
    case 'name_mismatch': return { sev: 'error', text: t('mqttCa.state.name_mismatch') };
    case 'unknown_ca': return { sev: 'error', text: t('mqttCa.state.unknown_ca') };
    case 'tls_error': return { sev: 'error', text: t('mqttCa.state.tls_error') };
    default: return null;
  }
}
