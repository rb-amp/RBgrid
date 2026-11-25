/**
 * «Tariffs & prices» (UI redesign 22.11, SPEC §4.1): what a kWh costs, in one place — the tariff plan
 * and, where the hub has dynamic prices (fw 1.2.0), their page as a second tab. `/prices` leads here.
 */
import React from 'react';
import { Box, Tab, Tabs, Typography } from '@mui/material';
import { useNavigate, useParams } from 'react-router-dom';
import { useCapabilities } from '../core';
import { confirmLeave } from '../app/unsavedGuard';
import { TariffsPage } from './TariffsPage';
import { PricesPage } from './PricesPage';
import { t } from '../i18n';

export const TariffsPricesPage: React.FC = () => {
  const { tab } = useParams();
  const navigate = useNavigate();
  const caps = useCapabilities();
  const dyn = caps.flags.dynamicTariffs;
  const current = dyn && tab === 'dynamic' ? 'dynamic' : 'plan';
  return (
    <Box>
      <Typography variant="h4" fontWeight={800} sx={{ mb: 1 }}>{t('shell.navTariffsPrices')}</Typography>
      {dyn && (
        <Tabs value={current} sx={{ mb: 2.5, minHeight: 40 }} aria-label={t('shell.navTariffsPrices')}
          onChange={async (_, v) => { if (await confirmLeave()) navigate(v === 'dynamic' ? '/tariffs/dynamic' : '/tariffs'); }}>
          <Tab value="plan" label={t('tariffs.title')} sx={{ minHeight: 40, textTransform: 'none', fontWeight: 600 }} />
          <Tab value="dynamic" label={t('prices.title')} sx={{ minHeight: 40, textTransform: 'none', fontWeight: 600 }} />
        </Tabs>
      )}
      {current === 'dynamic' ? <PricesPage embedded /> : <TariffsPage embedded />}
    </Box>
  );
};
