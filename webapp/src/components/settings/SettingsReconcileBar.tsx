/**
 * SettingsReconcileBar — the shadow reconciliation control. Reads the whole DeviceShadow
 * and shows the pending diff ("N changes · M need reboot"), with Apply-all and Reset-all.
 * This is the Device-Shadow desired→reported convergence made visible: edit freely across
 * domains, see the delta, apply once.
 */
import React, { useEffect, useRef, useState } from 'react';
import { Box, Stack, Typography, Button, Chip, Alert, CircularProgress } from '@mui/material';
import { useDeviceSession, ConfigDomain, UnsupportedDomainError } from '../../core';
import { ApplyAllError } from '../../core/session/DeviceSession';
import { getApiClient } from '../../services/api';
import { t } from '../../i18n';
import { savedText } from '../../services/savedText';
import { useUnsavedChanges } from '../../app/unsavedGuard';
import { useFieldErrorCount, clearFieldErrors } from '../../app/fieldErrors';
import { ApiRequestError } from '../../services/api';
import { humanize } from './ShadowConfigEditor';
import { useConfirm } from '../common/ConfirmDialog';
import { useSectionForms } from '../../app/sectionForms';
import { markRebootPending } from '../../app/rebootPending';

/** Domain → phrase key (resolved at render). */
const DOMAIN_LABEL: Partial<Record<ConfigDomain, string>> = {
  system: 'reconcile.domainSystem', time: 'reconcile.domainTime', hardware: 'reconcile.domainHardware', telemetry: 'reconcile.domainTelemetry',
  tariff: 'reconcile.domainTariff', units: 'reconcile.domainUnits', mqtt: 'reconcile.domainMqtt', wifi: 'reconcile.domainWifi',
};

/**
 * `shadowEditors`: the section edits device-shadow fields (else the bar shows only for the section's
 * own forms or pending edits). The section's panels with their own drafts register in
 * app/sectionForms — one «Save» writes them all (UI redesign SPEC §5.3).
 */
export const SettingsReconcileBar: React.FC<{ shadowEditors?: boolean }> = ({ shadowEditors = true }) => {
  const session = useDeviceSession();
  const shadow = session.current;
  const dirty = shadow.dirtyDomains;
  // Two ways to save (operator 18.10): what the hub applies at once goes with «Применить»;
  // what it reads only at start goes with «Сохранить и перезагрузить», never silently together.
  const split = session.pendingSplit();
  const [applying, setApplying] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error' | 'warning' | 'info'; text: string } | null>(null);
  // A reboot-domain write is stored but only takes effect after a restart; nothing reboots
  // on its own (review 02 B-8) → offer it explicitly instead of claiming it happens.
  const [rebootPending, setRebootPending] = useState(false);
  const [rebooting, setRebooting] = useState(false);
  // From the reboot request until the hub is back and re-read (round 2 F05/N09: «Restart now» and
  // «Cancel» stayed live while it restarted, and the «restarting» line was wiped).
  const [restarting, setRestarting] = useState(false);
  const fieldErrors = useFieldErrorCount();
  const confirm = useConfirm();
  const forms = useSectionForms();
  const formsDirty = forms.filter((f) => f.dirty);
  const formsInvalid = formsDirty.some((f) => f.invalid);

  const apply = async () => {
    setApplying(true);
    setMsg(null);
    try {
      let needsReboot = false;
      if (split.live.length) {
        const jobs = await session.applyLive();
        // The hub's answer to each write decides: a «live» write it still wants a restart for
        // is said so, not hidden.
        needsReboot = session.awaitingReboot(jobs);
      }
      // The section's own forms: each writes and shows its outcome in its panel.
      const failed: string[] = [];
      for (const f of formsDirty) if (!(await f.save())) failed.push(f.label);
      if (failed.length) setMsg({ sev: 'warning', text: t('reconcile.formsFailed', { list: failed.join(', ') }) });
      else if (needsReboot) {
        setRebootPending(true);
        split.live.forEach((d) => markRebootPending(t(DOMAIN_LABEL[d] ?? d)));
        setMsg({ sev: 'warning', text: savedText('reconcile.writtenNeedsReboot') });
      } else {
        setMsg({ sev: 'success', text: savedText('reconcile.applied') });
      }
    } catch (err) {
      showError(err);
    } finally {
      setApplying(false);
    }
  };

  const saveAndReboot = async () => {
    const names = split.reboot.map((d) => t(DOMAIN_LABEL[d] ?? d)).join(', ');
    // Everything that will be written is named (round 2 N02: the dialog said the rest needed «Save»).
    const others = [...split.live.map((d) => t(DOMAIN_LABEL[d] ?? d)), ...formsDirty.map((f) => f.label)];
    if (!(await confirm({
      title: t('reconcile.rebootSaveTitle'),
      body: t('reconcile.rebootSaveBody', { list: names }) + (others.length ? ` ${t('reconcile.rebootSaveAlso', { list: others.join(', ') })}` : ''),
      confirmLabel: t('reconcile.saveAndReboot'),
    }))) return;
    setApplying(true);
    setMsg(null);
    try {
      // Everything changed in the section goes with it (role audit 22.11 F03: a renamed group was
      // left unsaved and lost): the live fields and the section's forms first, then the on-start part.
      if (split.live.length) await session.applyLive();
      const failed: string[] = [];
      for (const f of formsDirty) if (!(await f.save())) failed.push(f.label);
      if (failed.length) {
        setMsg({ sev: 'warning', text: t('reconcile.formsFailed', { list: failed.join(', ') }) });
        setApplying(false);
        return;
      }
      await session.applyRebootPart();
    } catch (err) {
      showError(err);
      setApplying(false);
      return;
    }
    setApplying(false);
    setRebootPending(true);
    await reboot();
  };

  const showError = (err: unknown) => {
    // Each domain applies on its own (W191-02): name the ones that didn't, and why.
    let e: unknown = err;
    if (err instanceof ApplyAllError) {
      const why = (f: { domain: ConfigDomain; error: unknown }) => (f.error instanceof UnsupportedDomainError
        ? t(f.domain === 'time' ? 'reconcile.timeOwnPanel' : 'reconcile.domainReadOnly')
        : t('reconcile.domainRefused'));
      const list = err.failures.map((f) => `${t(DOMAIN_LABEL[f.domain] ?? f.domain)} — ${why(f)}`).join('; ');
      if (err.applied.length || err.failures.every((f) => f.error instanceof UnsupportedDomainError)) {
        setMsg({ sev: 'warning', text: t('reconcile.partlyApplied', { list }) });
        return;
      }
      e = err.failures[0].error;
    }
    // The hub names the field it refused (canon 2d2653ee: 400 OUT_OF_RANGE + `field` path).
    const data = e instanceof ApiRequestError ? (e.data as Record<string, unknown> | undefined) : undefined;
    const field = typeof data?.field === 'string' ? data.field : null;
    setMsg({ sev: 'error', text: e instanceof ApiRequestError && e.code === 'OUT_OF_RANGE' && field
      ? t('reconcile.outOfRange', { field: humanize(field.split('.').pop() ?? field) })
      : t('reconcile.applyFailed') });
  };

  const reboot = async () => {
    setRebooting(true);
    try {
      await getApiClient().rebootDevice();
      setRestarting(true);
      setMsg({ sev: 'info', text: t('reconcile.rebooting') });
      // Once the hub is back: re-read what was written and say whether it took (Kimi #14).
      void session.reconcileAfterReboot().then(({ applied, notApplied }) => {
        setRebootPending(false);
        setRestarting(false);
        const names = (ds: ConfigDomain[]) => ds.map((d) => t(DOMAIN_LABEL[d] ?? d)).join(', ');
        if (notApplied.length) setMsg({ sev: 'warning', text: t('reconcile.notAppliedAfterReboot', { list: names(notApplied) }) });
        else if (applied.length) setMsg({ sev: 'success', text: t('reconcile.appliedAfterReboot') });
        else setMsg(null);
      });
    } catch {
      setMsg({ sev: 'error', text: t('reconcile.rebootFailed') });
    } finally {
      setRebooting(false);
    }
  };

  const reset = () => {
    dirty.forEach((d) => session.resetDomain(d));
    formsDirty.forEach((f) => f.reset());
    clearFieldErrors();
    setMsg(null);
  };
  // Settings drafts live in the device shadow (they survive the page) — leaving asks, and a
  // confirmed leave drops them, so nothing lingers silently (HEU-21). Written-awaiting-reboot
  // edits are not «unsaved».
  useUnsavedChanges('settings', dirty.length > 0 && !rebootPending, reset);

  const count = dirty.length + formsDirty.length;
  // A NEW edit after «Saved»: the old message goes (role audit 22.11 F04) — only when the count grows;
  // a write that leaves the on-start part counted must not wipe «restarting» (round 2 N01).
  const prevCount = useRef(count);
  useEffect(() => {
    if (count > prevCount.current && msg?.sev === 'success' && !restarting) setMsg(null);
    prevCount.current = count;
  }, [count]); // eslint-disable-line react-hooks/exhaustive-deps
  // Nothing this section saves with the bar (Interface, Device): no bar at all.
  // Nothing to save and nothing to say: no bar (UI/UX audit U15 — «No unsaved changes» stood there always).
  if (!count && !fieldErrors && !msg && !rebootPending && !split.reboot.length) return null;
  const canSave = (split.live.length > 0 || formsDirty.length > 0) && !applying && fieldErrors === 0 && !formsInvalid;

  return (
    <Box
      sx={{
        // Below the phone's sticky header (48 px) — it went under it (round 2 installer N05).
        position: 'sticky', top: { xs: 52, md: 0 }, zIndex: 5, mb: 2, p: 1.5, borderRadius: 3,
        bgcolor: 'background.paper', border: 1, borderColor: count ? 'primary.main' : 'divider',
      }}
    >
      <Stack direction="row" alignItems="center" spacing={1.5} flexWrap="wrap" useFlexGap>
        <Typography variant="subtitle2" fontWeight={700}>
          {/* After a write that needs a reboot the delta stays until the restart — it is written,
              not unsaved (HEU-06: «Записано» next to «Несохранённых изменений: 1»). */}
          {restarting ? t('reconcile.rebootingShort')
            : rebootPending && dirty.length ? t('reconcile.writtenPending', { n: dirty.length })
            : count ? t('reconcile.unsavedCount', { n: count })
              // A red field is an edit too, just not an acceptable one (HEU-R04: «Нет несохранённых
              // изменений» next to a field in error).
              : fieldErrors > 0 ? t('reconcile.invalidEdit') : t('reconcile.noUnsaved')}
        </Typography>
        {split.live.map((d) => <Chip key={`l-${d}`} size="small" label={t(DOMAIN_LABEL[d] ?? d)} variant="outlined" />)}
        {formsDirty.map((f) => <Chip key={`f-${f.id}`} size="small" label={f.label} variant="outlined" color={f.invalid ? 'error' : 'default'} />)}
        {split.reboot.map((d) => (
          <Chip key={`r-${d}`} size="small" color="warning" variant="outlined"
            label={t('reconcile.afterReboot', { domain: t(DOMAIN_LABEL[d] ?? d) })} />
        ))}
        {fieldErrors > 0 && <Typography variant="caption" color="error.main">{t('form.fixErrors')}</Typography>}
        <Box sx={{ ml: 'auto', width: { xs: '100%', sm: 'auto' } }}>
          <Stack direction="row" spacing={1} justifyContent="flex-end">
            {!restarting && <Button size="small" onClick={reset} disabled={(!count && !fieldErrors) || applying}>{t('reconcile.reset')}</Button>}
            {split.reboot.length > 0 && !rebootPending && !restarting && (
              <Button size="small" variant="outlined" color="warning" onClick={saveAndReboot}
                disabled={applying || fieldErrors > 0}>
                {t('reconcile.saveAndReboot')}
              </Button>
            )}
            <Button
              size="small" variant="contained" onClick={apply} disabled={!canSave || restarting}
              startIcon={applying ? <CircularProgress size={16} aria-label={t('common.loading')} /> : undefined}
            >
              {applying ? t('reconcile.applying') : t('reconcile.apply')}
            </Button>
          </Stack>
        </Box>
      </Stack>
      {msg && (
        <Alert
          severity={msg.sev}
          sx={{ mt: 1.5 }}
          action={rebootPending && !restarting ? (
            <Button color="inherit" size="small" onClick={reboot} disabled={rebooting}>
              {rebooting ? t('reconcile.rebootingShort') : t('reconcile.rebootNow')}
            </Button>
          ) : undefined}
        >
          {msg.text}
        </Alert>
      )}
    </Box>
  );
};
