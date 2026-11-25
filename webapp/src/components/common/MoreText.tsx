/**
 * MoreText — a panel's explanation in one line (UI redesign S4, SPEC §6.4): the first sentence, the
 * rest behind «More». Short texts show whole.
 */
import React, { useState } from 'react';
import { Link, Typography } from '@mui/material';
import { t } from '../../i18n';

export const MoreText: React.FC<{ text: string; sx?: object }> = ({ text, sx }) => {
  const [open, setOpen] = useState(false);
  const cut = text.search(/[.!?](\s|$)/);
  const first = cut > 0 && cut < text.length - 2 ? text.slice(0, cut + 1) : text;
  const rest = first.length < text.length ? text.slice(first.length).trim() : '';
  return (
    <Typography variant="body2" color="text.secondary" sx={sx}>
      {first}{' '}
      {rest && (open ? <>{rest} </> : null)}
      {rest && (
        <Link component="button" type="button" variant="body2" onClick={() => setOpen((v) => !v)} sx={{ verticalAlign: 'baseline' }}>
          {open ? t('common.less') : t('common.more')}
        </Link>
      )}
    </Typography>
  );
};
