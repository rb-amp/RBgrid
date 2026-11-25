import React from 'react';
import { Stack, Typography } from '@mui/material';

export interface BlockHeaderProps {
  /** The block's name (16, bold). */
  title: React.ReactNode;
  /** An optional short line under or beside the title (what, for which period). */
  caption?: React.ReactNode;
  /** Actions on the right («As a table», «More», «⋯»). */
  actions?: React.ReactNode;
  /** Heading level for screen readers (default h3). */
  component?: 'h2' | 'h3' | 'h4';
}

/**
 * One pattern for every block and panel heading (UI redesign S2): the name on the left, a quiet
 * caption, the block's own actions on the right. Spaced overlines stay for group labels only.
 *
 * @category Surfaces
 */
export const BlockHeader: React.FC<BlockHeaderProps> = ({ title, caption, actions, component = 'h3' }) => (
  <Stack direction="row" alignItems="flex-start" justifyContent="space-between" flexWrap="wrap" useFlexGap gap={1} sx={{ mb: 1.5 }}>
    <Stack spacing={0.25} sx={{ minWidth: 0 }}>
      <Typography component={component} sx={{ fontSize: 16, lineHeight: '22px', fontWeight: 700, m: 0 }}>{title}</Typography>
      {caption && <Typography sx={{ fontSize: 13, lineHeight: '18px', color: 'text.secondary' }}>{caption}</Typography>}
    </Stack>
    {actions && <Stack direction="row" alignItems="center" spacing={1} sx={{ flexShrink: 0 }}>{actions}</Stack>}
  </Stack>
);
