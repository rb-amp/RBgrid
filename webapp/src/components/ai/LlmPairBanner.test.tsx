import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { llmApi, LlmDoc, LlmError } from '../../core/llm/api';
import { LlmPairBanner } from './LlmPairBanner';

const DOC: LlmDoc = {
  enabled: true, read_only: false, proto: '1.0', tokens: [], log: [], log_total: 0, connect: null,
  pairing: [{ pair_id: 'p9', client_name: 'Claude Desktop', scopes: ['read'], code: '482913', expires_in: 521 }],
};
afterEach(() => jest.restoreAllMocks());
const show = (n: number) => render(<MemoryRouter><LlmPairBanner pending={n} /></MemoryRouter>);

it('nothing pending: no banner and no request', () => {
  const get = jest.spyOn(llmApi, 'get');
  const { container } = show(0);
  expect(container).toBeEmptyDOMElement();
  expect(get).not.toHaveBeenCalled();
});

it('shows who, the code spaced, the time left; Deny cancels by pair_id', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue(DOC);
  const set = jest.spyOn(llmApi, 'set').mockResolvedValue({ ...DOC, pairing: [] });
  show(1);
  expect(await screen.findByText('482 913')).toBeInTheDocument();
  expect(screen.getByText(/«Claude Desktop» хочет подключиться/)).toBeInTheDocument();
  expect(screen.getByText(/осталось 8:4\d/)).toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'Отклонить' }));
  await waitFor(() => expect(set).toHaveBeenCalledWith({ pair_cancel: 'p9' }));
});

it('without the admin token it still says an assistant waits', async () => {
  jest.spyOn(llmApi, 'get').mockRejectedValue(new LlmError('HTTP 401', 401));
  show(1);
  expect(await screen.findByText(/Откройте «ИИ-ассистенты», чтобы увидеть код/)).toBeInTheDocument();
});
