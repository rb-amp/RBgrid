# What an AI assistant reads and changes

## The manifest is the list

The hub publishes what it offers in its MCP manifest (`GET /api/llm/v1/manifest`). That list grows with firmware
versions, so the hub itself — not a document — is the answer to *what can my assistant do*. The manifest is read
when a connection is made, so a restarted bridge reads the current list.

This page describes the **kinds of thing** that are there, which do not change when a new one is added.

## What it reads

| Family | What is in it |
|---|---|
| State and discovery | The hub itself, its health, its channels, live readings, events, the configuration and how that configuration is organised. |
| Analytics | Energy over a period, cost split by your own tariff, one period against another, the load profile, and what you import against what you export. Answers break down to the hour, and the hour in progress is included. |
| Context | The ready-made lines of enquiry the hub suggests for an analysis, and the price context for your region — the currency, and the day-ahead price sources and zones it knows about. That reference list is built into the firmware, not fetched. |

A cost is computed on the hub, from the tariff you set there — zones, taxes and the price of what you export
included. If you have turned a day-ahead price source on, the hub fetches those prices itself and counts with
them instead — that is the one thing the hub fetches from the internet, and it sends none of your readings when it does. → [Prices](web-prices.md) · The assistant reads the figure the hub arrived at.

## What it checks

These do not change anything. They look at what is there and tell you what they found.

| | What it does |
|---|---|
| Installation check | Checks the set-up itself: that every configured module answered, that points are attached to inputs, that no two modules share an address on a bus, and that the clock and time zone are set. Run it after any change you made by hand. It checks the set-up, not your figures. |
| Anomalies | Readings that stand out from the rest. |
| Find the channel | You switch a load on and off; the hub watches its channels once a second and names the one whose current moved with your steps. Useful when you do not know which clamp went on which circuit. The main supply is not one of the candidates — it is the reference, and the hub reports its step separately. If the supply saw your load and no other channel did, the answer says so: that circuit has no clamp of its own. |
| Identify the hub | The hub blinks its own status lights, so you can tell which hub you are talking to. Ten seconds by default; you can ask for longer, or stop it at once. A hub without status lights says so instead. |

## What it changes

The two steps below are what protects you from an assistant that got something wrong — and an assistant can get
something wrong. But they do not cover everything, and the difference matters.

**In read mode your assistant does not have these tools at all.** They are not offered to it and then refused —
they are not in the list it was given, so it answers that it has no such tool. That is the expected answer, not
a fault: widening what an assistant may do needs a second pairing code, not a retry. →
[Connect an AI assistant](web-ai-connect.md)

### Settings — proposed first, written after you agree

Channel names and roles, the clamp model **of a point that already exists**, the tariff plan with its zone
prices and time windows, the day the monthly peak-demand period starts, the hub's time zone, the hub's own name
and location.

A point's name, role and clamp are also given when the point is first created, and creating a point is a setup
action — so they are set immediately that first time, and change in two steps afterwards.

A change arrives in two steps, and the first one is the one that protects you. The assistant proposes, and the hub
answers with the **exact change** it would make — the fields, the values, and any warning that comes with them —
together with a one-time confirmation that is good for ten minutes and is tied to that change and to the version
it was based on. Once you agree, the assistant applies it with that confirmation. So you see what will happen
before it happens, in the same conversation; and a proposal never used simply expires.

### Setup actions — done when you ask

Adding a module on either bus, moving a module to another address, creating, moving or deleting a point, setting
the pins of the second bus, restarting the hub.

**These take effect at once.** There is no diff and no confirmation from the hub: you asked, and it was done. Some
apps will ask you first on their own, because the hub marks these as actions that change things — but that is
your app's behaviour, not the hub's.

Most are undone by the opposite action: set the address back, move the point to the right input. A point's name,
role and clamp are versioned and come back with a restore; modules, addresses and bus pins are not.

The **main supply point** is the exception to all of this: a hub starts with one, an assistant cannot delete it,
and it cannot move the supply role onto or off any point. Setting a hub up means **moving** the supply point to
the input your grid clamp is on. A three-phase supply set up as a point per phase has one supply point per
phase, and that arrangement is made in the web app, not from a chat.

### Undoing a change

The hub keeps **eight** versions of its settings, and every applied change is listed on the AI assistants page
with a **Restore** button next to it. Restore previews exactly what it would undo before it does it.

> ⚠️ **A restore brings back the whole version, not the one field you are thinking of.** If you set the tariff,
> then set the time zone, then restore to the version before the tariff, you lose the time zone as well. Go back
> one step at a time and read the preview each time.

Both paths work. In the app you press **Restore**; from the chat an AI assistant can do it, in the same two steps
as any settings change — it proposes the rollback and applies it after you agree. The chat path needs `config`,
and it is refused while the hub is read-only. The button in the app is not: there it is you making the change.

One of the eight versions is treated differently: the state the hub was in before any AI assistant changed
anything. On a hub that already had its settings when it came to v1.3.0, that state is kept and later
changes do not push it out. On a hub whose settings were made after the AI interface was already in use, there is
no such state to keep, and all eight are ordinary.

A restore never creates, deletes or moves points.

### Correcting a mistake

| What went wrong | What to do | What happens to the history |
|---|---|---|
| The point is on the wrong input | Ask to **move** it | Kept. The id, name, role and recorded history stay; only the input changes |
| The point should not exist | Ask to **delete** it | 🔴 **Erased.** The hub frees the history slot, and what that point recorded is gone |
| The clamp rating was wrong | Ask to set the right one | Applies from that moment. Records already written are **not** recalculated, so the old readings stay as they were measured |
| A settings change was wrong | Restore, one step at a time | Settings only; the recorded measurements are untouched |

Deleting a point and restarting the hub are the two setup actions with no opposite action to undo them. Both are
carried out as soon as they are asked for.

**Clamp models come from a dated list inside the bridge.** If your clamp is newer than the bridge you installed,
the answer says the list is older than that model and asks you to update the bridge — it does not pick the
nearest-looking one. That is on purpose: a nearest guess would be the wrong current rating on a real circuit in
your home.

## How to read the numbers

Five things look like mistakes and are not.

- **Energy and cost for the current hour are behind.** They are updated every fifteen minutes, so the last few minutes
  may not be in the answer yet. Live readings are not behind — they are the current moment.
- **There is nothing finer than an hour.** Ask for a shorter period and you still get hours.
- **Cost rounds to the cent.** Over a short period that is 0.00 or 0.01, and nothing is wrong.
- **Anomaly detection stays quiet on a new installation.** It needs about ten full days of recorded history. Silence then means *not enough history*, not *nothing is wrong*.
- **In live values, power is not voltage times current.** Two reasons, and both are normal. A module averages
  current, voltage and power separately, so the three figures in one answer are not three parts of one
  multiplication. And on a load that is not purely resistive, active power is below apparent power anyway — that
  ratio is the power factor. Multiply the volts by the amps and you get apparent power (V·A); what the hub reports
  as watts is active power.

## The call log

The hub records every call: when, which assistant, which tool, the result and how long it took, with the applied
changes marked. You read it on the AI assistants page — the hub's own page and the app both show it. It holds the
newest fifty of the calls made since the hub last started, so a restart clears it, and an assistant with `config`
can restart the hub. The app's table of
paired assistants also shows, for each, when it was last used and how many calls it made today.

## Things to ask it

Phrasing that works, grouped the way the tools are.

**Reading**
*"What is connected to my hub?"* · *"How much did the house use today, hour by hour?"* · *"Compare this hour with
the last one."* · *"What did the water heater cost last week?"*

**Checking**
*"Check my installation."* · *"I have just connected a module — find it."* · *"I am switching the kettle on and
off now; tell me which channel sees it."*

**Changing**
*"Name the point on I0 'Water heater', clamp SCT-013-030."* · *"Set the tariff: 0.35 by day, 0.15 at night from
23:00 to 07:00."* · *"Set the time zone to Europe/Berlin."*

An assistant with `read` will answer the first two groups and tell you it cannot do the third.

## Not paired yet?

Nothing on this page works until an AI assistant is paired with your hub.
→ [Connect an AI assistant](web-ai-connect.md)

## Where your readings go

The hub answers on your own network and sends nothing out of it. Your AI assistant is a separate program: what it
reads from the hub goes wherever that assistant sends it. If it runs in the cloud, your readings go to its
service — the hub has no say in that. Pick the assistant you are willing to show your data to.

## Troubleshooting

**The assistant says a proposal has expired.**
A confirmation is good for ten minutes from the moment the change was proposed. Ask again and agree to the new
one.

**It says a change cannot be made, although it was paired with `config`.**
The hub is in read-only mode, which overrides the scope of every paired assistant and covers everything an
assistant would write — settings, setup actions, a restart and its own rollback. It still answers with the diff,
marked as refused, so you can see what the change would have been. →
[AI assistants](web-ai-assistants.md)

**It says a proposal would change nothing.**
The value is already what you asked for. That is an answer, not an error.

**Finding a channel does not name one.**
More than one clamp is seeing the same load. Switch something that only one channel can see.

**Costs are there but the figures look wrong for the time of day.**
Check the hub's time zone: tariff windows follow the hub's clock, and a hub as it comes runs on UTC. →
[Time](web-time.md)
