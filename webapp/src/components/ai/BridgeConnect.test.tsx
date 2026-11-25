import React from 'react';
import { render, screen, fireEvent } from '@testing-library/react';
import { BridgeConnect } from './BridgeConnect';

// Not imported by the app until the bridge is published — kept working so the day it is, it drops in.
it('BridgeConnect: a tab per client with the command to copy', () => {
  render(<BridgeConnect hub="http://rbgrid-bfac.local/" />);
  fireEvent.click(screen.getByRole('tab', { name: 'Claude Code' }));
  expect(screen.getByText(/claude mcp add rbgrid -e RBGRID_HUB_URL=http:\/\/rbgrid-bfac\.local\//)).toBeInTheDocument();
});
