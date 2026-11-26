# AI assistants

From hub firmware **v1.3.0** you can work with the hub by asking. Ask what the water heater cost last week
and the hub answers with its own numbers. Ask to rename a channel or change a tariff zone and the hub works out
the exact change first, hands it back, and writes nothing until it is confirmed — keeping the version it
replaced. Some other things an assistant does, such as adding a module or creating a point, happen as soon as it
asks; the difference is set out below.

The hub answers on your own network. Today this is the path for managing and configuring your hub on your local
network: an AI assistant running on a computer on the same network reads what the hub has recorded and changes the
hub's own settings.

> **Home Assistant and an AI assistant do different jobs, and they run side by side.** Home Assistant shows your
> circuits as sensors on a dashboard; an AI assistant answers questions about them in words and changes the hub's
> settings when you ask. If you already run Home Assistant, its own MCP
> server reads the RBgrid entities, so you do not need to pair an assistant here just to read them. Cost by your
> tariff, the hub's own checks and changing the hub's settings do not come that way — those need a pairing here.

## What an AI assistant is, and is not

The hub is an instrument: the same question gives it the same answer, and the figures it reports come from its own
measurements.

An AI assistant is not an instrument. It is a language model, and it can misread a figure, miss part of what you
asked, or answer with confidence about something it got wrong. Two assistants will not behave the same way on the
same request, and neither will the same assistant on another day: the model behind it is not ours, it changes when
its maker changes it, and it is the one part of this that we cannot test for you.

The hub is built around that, and it is worth knowing exactly how far the protection goes — because it is not the
same for everything an assistant can do.

**Settings** — names, roles, clamp models, the tariff, the time zone, the hub's name — are changed in two steps.
The hub answers a proposal with the exact fields and values it would alter, hands back a confirmation that is
good for ten minutes and tied to that one change, and writes nothing until that confirmation comes back.

**Setup actions** — adding a module, moving it to another address, creating, moving or deleting a point, turning
the second bus on, restarting the hub — happen when they are asked for. The hub does not show a diff first and
does not wait for a second call. → [What an AI assistant reads and changes](web-ai-tools.md)

| What the hub guarantees | What depends on the assistant and its app |
|---|---|
| Two steps for settings, and a confirmation tied to one change | Whether it waits for your "yes" before making the second call |
| The list of applied changes on the AI assistants page, and Restore, which previews what it would undo | Whether the change it showed you before you agreed matches the one it proposed |
| Read-only mode, which refuses writes whatever scope was given | Whether it asks you before a setup action |
| The call log, which records what was actually asked and done, until the hub restarts | |

**What you should do.** The change you are shown before you agree is computed by the hub, but it reaches you
through your assistant — the hub has no page where a pending proposal waits to be read. Once a change is applied
it is on the hub: the AI assistants page lists it as a version, and **Restore** there shows exactly what it would
undo. So the checks worth doing are the ones the hub can confirm: on a hub that is already working, keep
read-only on while you see what a new assistant is like; read the applied changes and the call log on the AI
assistants page afterwards; and check a number you mean to act on against the hub's own pages.

## One name per thing

Some words in this section mean something narrower than they do elsewhere. And the same setting can be reached
in two places — on the hub's own pages and in the RBgrid app — where it carries different labels. Both are worth
knowing before you start.

| In these pages | What it means |
|---|---|
| **AI assistant** | The program you talk to. Never «assistant» on its own: on this site that word means Home Assistant. |
| **The hub's page** | `http://<your hub>/ai`, served by the hub itself. It works whether or not the web app is installed. |
| **The app** | The RBgrid web app, at `http://<your hub>/#/ai`. Where a page says "your app", it means the program your AI assistant runs in, which is a different thing. |
| **The bridge** | A small **program** on your own computer that connects your AI assistant to the hub. Not a wire and not a link between terminals. → [Where an AI assistant runs](web-ai-clients.md) |
| **MCP** | The agreed way AI assistants talk to tools like this hub. You never type it anywhere; it is the word to look for in your own app's documentation. |

Four more are ordinary English words that mean something narrow here, and three of them mean something else
again in an electrical panel.

| Word | Here it means | Not to be read as |
|---|---|---|
| **Clamp** | The clip-on current sensor that closes round **one** conductor and measures the current in it. Its **rating** is the current it is made for, such as 30 A or 100 A. | A terminal or a screw connector |
| **Bus** | The **I²C** connection between the hub and its measurement modules — four wires, carrying data and power to the modules. A hub has two of them. | A busbar |
| **Module** · **input** · **point** | A **module** is the rbAmp board on the bus. Each module has one or more **inputs** — the sockets a clamp plugs into, numbered from `I0`. A **point** is what you create on an input and give a name to: the thing that appears in the app and keeps history. One input, one point. | — |
| **Scope** | What a paired AI assistant is allowed to do: `read`, or `read` and `config`. | — |

The same switch and the same list carry different labels in the two places. The setting behind them is one and
the same, and you can use either.

| What it is | On the hub's page | In the app |
|---|---|---|
| Turning the interface on | **Status** → **Turn on** | **Access** → **Allow AI assistants** |
| The list of assistants | **Connected assistants** | **Paired assistants** |
| The hub's own secret | **Admin password** | **API token** |

## Four secrets, and you choose only one of them

| | What it is | Who makes it | Where you see it | How long it lives | Do you type it? |
|---|---|---|---|---|---|
| **Admin password** — the app calls it **API token**, and some screens say **admin token** | The hub's own secret, without which the interface cannot be switched on | you | you set it | until you change it | you choose it, and type it again when a page asks |
| **Pairing code** | Six digits that tie one AI assistant to the hub | the hub | the hub's page and the app | 10 minutes | **yes — the only one you ever pass to an assistant** |
| **The assistant's token** | What that assistant uses from then on | the hub | never shown | until you revoke it | no |
| **Change confirmation** | A one-time key tied to one proposed change | the hub | never shown | 10 minutes | no |

Two of those are ten minutes, and **they are counted separately**: the code counts down from the moment an AI
assistant asks to pair, the confirmation from the moment it proposes a change.

## What an AI assistant may do

You choose this once, when you pair. The choice is fixed for that assistant: to give it a different one, pair it
again and revoke the old pairing. Setting a hub up with nothing on it needs `config`; `read` is enough for questions.

| Scope | What it allows |
|---|---|
| `read` | Reads your installation, live values, history and cost. Changes nothing. |
| `config` | Everything `read` allows, **and two different kinds of change**: it may propose a settings change, which the hub shows you in full before anything is written — and it may carry out setup actions, which happen when asked. Adding or deleting a point and restarting the hub are setup actions. |

### Keep the hub read-only

**Read only** is a separate switch on the hub, and it overrides `config` for every paired assistant —
whatever they were paired with.

It refuses **everything an assistant would write**: settings changes, setup actions such as adding a module or
creating a point, restarting the hub, and the assistant's own rollback. Reading still works, and so does the
preview — ask for a change and the hub still answers with the exact diff, marked as refused.

**Restore in the app keeps working**, because there it is you making the change, not an assistant.

Use it when you want answers and no changes: when someone else is using the assistant, or when you are seeing
what a new one is like on a hub that is already working.

> **Not while you are setting a hub up.** Connecting modules and creating points are exactly what read-only
> refuses, so a first setup is done with it off. → [Set up a hub by talking to it](web-ai-first-setup.md)

## What stays on the hub's own pages

Some things an AI assistant cannot touch at all, whatever it was paired with. The hub reports this list itself,
so the hub, not this page, is what finally decides what is possible.

Wi-Fi · the admin password · assistant access itself · firmware updates · factory reset · backup and restore ·
the language pack · the certificate the hub uses to trust an MQTT broker · read-only mode.

The **main supply point** is a special case. A hub starts with one, and an AI assistant cannot delete it or
change a role to or from supply. What it can do is **move** it to the input your grid clamp is actually on, which
is the normal thing to do when you set a hub up.

**Three-phase supplies are supported.** Set the supply to three phase and choose the three-phase input in the web
app → [Scenario: one main meter](web-scenario-main-meter.md). A three-phase supply is measured by one
rbAmp STD-U3I3 — one module for the three phases — and when it is set up as a point per phase, there is one
main-supply point per phase. Do that part in the web app; an AI assistant then reads and analyses a three-phase
supply like any other.

Everything else about your installation — names, roles, clamp models, the tariff, the time zone, the hub's own
name, modules and points — an assistant can change, under the rules on
[What an AI assistant reads and changes](web-ai-tools.md).

## Where your readings go

The hub answers on your own network and sends nothing out of it. Your AI assistant is a separate program: what it
reads from the hub goes wherever that assistant sends it. If it runs in the cloud, your readings go to its
service — the hub has no say in that. Pick the assistant you are willing to show your data to.

One thing the hub does reach out for, and only if you switch it on: day-ahead electricity prices, so it can cost
your energy with them. It fetches prices and sends no readings. → [Prices](web-prices.md)

## The interface itself

The hub serves the AI interface under `/api/llm/v1/…`, and it is off until you turn it on.

> ⚠️ **Two paths that look alike, and they are different things.** `/api/llm/v1/…` is what AI assistants use.
> `/api/v1/llm` also exists, but it belongs to the hub's own AI assistants page — switching the interface on,
> showing pairing codes, revoking, the call log — and it is behind the admin password. An assistant pointed at
> the second one will not reach the AI interface.

The list of what the hub offers is not in this document: the hub publishes it as an MCP manifest, and the manifest
grows with firmware. → [What an AI assistant reads and changes](web-ai-tools.md)

## Where to go next

- Set up a hub that has nothing on it yet, by talking to it → [Set up a hub by talking to it](web-ai-first-setup.md)
- Pair an AI assistant, step by step → [Connect an AI assistant](web-ai-connect.md)
- What must run on your own computer → [Where an AI assistant runs](web-ai-clients.md)
- What the hub offers and what it will let an AI assistant change → [What an AI assistant reads and changes](web-ai-tools.md)
- Installing the bridge in one particular app, as we walked it → [Claude Desktop](web-ai-client-claude-desktop.md) — so far the only app we have walked ourselves, on Windows

## Troubleshooting

**The AI assistants page says to set an admin token first.**
The hub will not switch the interface on without one: until it has one, anyone on your network could read a
pairing code. It is one value with one place to set it in each door — in the app, **Set up → Settings → Security**;
on the hub's own pages, the **WiFi** page. Setting it in either place sets it for both.

**An AI assistant with `config` is told a change cannot be made.**
The hub is in read-only mode, which keeps every assistant from writing, whatever it was paired with. Turn it off
on the AI assistants page.

**Prices in the answers look wrong, or a tariff zone looks wrong.**
Check the hub's time zone. A hub as it comes runs on UTC, so a night window of 23:00–07:00 is counted in UTC
until you set the zone, and the hub will not put a price on a reading when it does not know when it was taken. → [Time](web-time.md)

**The hub asks the assistant to wait.**
The hub limits how often it answers. The reply carries `retry_after` — the time after which the same call is
accepted again.
