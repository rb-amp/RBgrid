/**
 * SetupChecklist — «Left to set up» (UI redesign U2, SPEC §5.9): the hub's setup as a checklist on
 * the Overview, each step leading to ITS place in the app (not the whole wizard). Required: site &
 * time, metering points; optional: security, tariff, integrations — marked, each can be dismissed
 * («not needed», per hub in this browser). Gone once the required ones are done and the optional
 * ones are done or dismissed. Replaces the factory-defaults banner (SetupBanner).
 */
import React from 'react';
import { Box, Button, ButtonBase, Chip, IconButton, Stack, Tooltip, Typography } from '@mui/material';
import CheckCircleIcon from '@mui/icons-material/CheckCircle';
import RadioButtonUncheckedIcon from '@mui/icons-material/RadioButtonUnchecked';
import CloseIcon from '@mui/icons-material/Close';
import ChevronRightIcon from '@mui/icons-material/ChevronRight';
import { useNavigate } from 'react-router-dom';
import { Card, BlockHeader } from '@rbgrid/design-system';
import { useOnboarding, OnboardingContext } from '../../onboarding/useOnboarding';
import { useLocalStorage } from '../../hooks/useLocalStorage';
import { getBaseUrl } from '../../services/connection';
import { t } from '../../i18n';

interface Item { key: string; label: string; hint: string; to: string; optional: boolean; done: (c: OnboardingContext) => boolean; visible?: (c: OnboardingContext) => boolean }

const ITEMS: Item[] = [
  { key: 'site', label: 'setup.site', hint: 'setup.siteHint', to: '/config/site', optional: false, done: (c) => c.siteNamed },
  { key: 'time', label: 'setup.time', hint: 'setup.timeHint', to: '/config/network#time', optional: false, done: (c) => c.timeSynced },
  { key: 'points', label: 'setup.points', hint: 'setup.pointsHint', to: '/devices', optional: false, done: (c) => c.pointCount > 0 },
  { key: 'tariff', label: 'setup.tariff', hint: 'setup.tariffHint', to: '/tariffs', optional: true, done: (c) => c.tariffSet, visible: (c) => c.isFull },
  { key: 'security', label: 'setup.security', hint: 'setup.securityHint', to: '/config/security', optional: true, done: (c) => c.adminTokenSet },
];

export const SetupChecklist: React.FC = () => {
  const ob = useOnboarding();
  const navigate = useNavigate();
  const [dismissed, setDismissed] = useLocalStorage<string[]>(`rbgrid.setup.dismissed.${getBaseUrl() || 'local'}`, []);
  if (!ob.loaded) return null;
  const c = ob.ctx;
  const items = ITEMS.filter((i) => !i.visible || i.visible(c));
  const open = items.filter((i) => !i.done(c) && !(i.optional && dismissed.includes(i.key)));
  if (!open.length) return null;
  const doneN = items.filter((i) => i.done(c)).length;
  return (
    <Card>
      <BlockHeader title={t('setup.title')} caption={t('setup.progress', { done: doneN, of: items.length })}
        actions={<Button size="small" onClick={() => navigate('/onboarding')}>{t('setup.wizard')}</Button>} />
      <Stack spacing={0.5}>
        {items.filter((i) => !(i.optional && dismissed.includes(i.key) && !i.done(c))).map((i) => {
          const done = i.done(c);
          return (
            <Stack key={i.key} direction="row" alignItems="center" spacing={1}>
              <ButtonBase onClick={() => navigate(i.to)} disabled={done}
                sx={{ flex: 1, justifyContent: 'flex-start', textAlign: 'left', gap: 1.25, py: 0.75, px: 1, borderRadius: 2, '&:hover': { bgcolor: 'action.hover' } }}>
                {done ? <CheckCircleIcon fontSize="small" color="success" /> : <RadioButtonUncheckedIcon fontSize="small" sx={{ color: 'text.secondary' }} />}
                <Box sx={{ flex: 1, minWidth: 0 }}>
                  <Typography sx={{ fontSize: 14, fontWeight: 600, color: done ? 'text.secondary' : 'text.primary', textDecoration: done ? 'line-through' : 'none' }}>
                    {t(i.label)}
                    {i.optional && !done && <Chip size="small" variant="outlined" label={t('setup.optional')} sx={{ ml: 1, height: 20 }} />}
                  </Typography>
                  {!done && <Typography sx={{ fontSize: 13, color: 'text.secondary' }}>{t(i.hint)}</Typography>}
                </Box>
                {!done && <ChevronRightIcon fontSize="small" sx={{ color: 'text.secondary' }} />}
              </ButtonBase>
              {i.optional && !done && (
                <Tooltip title={t('setup.notNeeded')}>
                  <IconButton size="small" aria-label={t('setup.notNeededFor', { what: t(i.label) })} onClick={() => setDismissed([...dismissed, i.key])}>
                    <CloseIcon fontSize="small" />
                  </IconButton>
                </Tooltip>
              )}
            </Stack>
          );
        })}
      </Stack>
    </Card>
  );
};
