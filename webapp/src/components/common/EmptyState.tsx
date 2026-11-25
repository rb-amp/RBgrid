/**
 * EmptyState — one pattern for an empty page or block (UI redesign U9, SPEC §5.10): what will be
 * here · why it is empty now · one button that does something about it.
 */
import React from 'react';
import { Box, Button, Stack, Typography } from '@mui/material';
import { Link as RouterLink } from 'react-router-dom';
import { Card } from '@rbgrid/design-system';

export interface EmptyStateProps {
  /** What this place shows once there is data. */
  title: string;
  /** Why it is empty now. */
  why?: string;
  /** The one thing to do: a route or a handler. */
  action?: { label: string; to?: string; onClick?: () => void };
  icon?: React.ReactNode;
  /** Inside another card: no card of its own. */
  bare?: boolean;
}

export const EmptyState: React.FC<EmptyStateProps> = ({ title, why, action, icon, bare = false }) => {
  const body = (
    <Stack spacing={1} alignItems="flex-start" sx={{ py: 2 }}>
      {icon && <Box sx={{ color: 'text.secondary', '& svg': { fontSize: 32 } }}>{icon}</Box>}
      <Typography sx={{ fontSize: 16, fontWeight: 700 }}>{title}</Typography>
      {why && <Typography variant="body2" color="text.secondary">{why}</Typography>}
      {action && (action.to
        ? <Button variant="contained" component={RouterLink} to={action.to} sx={{ mt: 0.5 }}>{action.label}</Button>
        : <Button variant="contained" onClick={action.onClick} sx={{ mt: 0.5 }}>{action.label}</Button>)}
    </Stack>
  );
  return bare ? body : <Card>{body}</Card>;
};
