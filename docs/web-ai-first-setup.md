# Set up a hub by talking to it

A hub as it comes has no modules, one point — the main supply — waiting for a clamp, and a sample tariff that
is not yours. You can do all of that from a chat with a paired AI
assistant: you work with your hands, it works with the hub.

**What you do:** connect one module at a time, clip the clamps on, say which clamp is which, answer the
assistant's questions and agree to its proposals.
**What it does:** finds each module, gives it a free address, creates the points, sets the names, roles and
tariff, and checks the result.

> ⚠️ **For any work inside an electrical panel, call a qualified electrician.**

**Before you start you need two things.** An AI assistant already paired with the hub →
[Connect an AI assistant](web-ai-connect.md). And the wiring itself: which four wires go from a module to
the hub and to which pins, how a clamp goes round a single conductor and which way it faces, and where the
voltage input connects. None of that is in this chat, and none of it is on this page →
[Sensors and modules](web-sensors.md)

## Why one module at a time

Every rbAmp module leaves the factory at the same address. Two of them on one bus at the same address cannot be
told apart, so you connect one, let the AI assistant move it to a free address, and only then connect the next.

An address has to be unique on its bus — the I²C line the modules share with the hub — not on the hub as a
whole: the same address can sit once on each of the two buses.

## The main supply point is already there

A hub starts with one main supply point, and it stays: an AI assistant cannot delete it, and it cannot make any
other point into a supply or take that role away. Setting a hub up does not mean creating it — it means
**moving** it onto the input where your grid clamp actually is. Every other point you add is a load.

**If your supply is three-phase,** set that up in the web app before you start here: the supply is measured by
one rbAmp STD-U3I3, and the web app is where you set the supply to three phase and mark that input. Set up as a
point per phase, it gives one main-supply point per phase. →
[Scenario: one main meter](web-scenario-main-meter.md)

**If your only clamp is not on the supply at all,** put it where you want to measure and let the assistant create
an ordinary point for it. The supply point simply stays where it is, without a clamp of its own.

## The clamp is the one thing the hub cannot know

The hub reads which module it is talking to. It cannot read which clamp you put on it, and a clamp of the wrong
rating gives wrong readings with no error anywhere — nothing will warn you later. The assistant will ask; answer
with what is printed on the clamp.

## The order it goes in

1. Pair an AI assistant with `config`. `read` is not enough to set anything up, and the scope cannot be changed
   afterwards without pairing again.
2. Connect the first module, with the hub powered down, and clip its clamp on. → [Sensors and modules](web-sensors.md)
3. Tell the assistant it is connected. It finds the module and moves it to a free address.
4. Tell it which clamp is on which input. For your grid clamp it **moves the hub's main supply point** onto that
   input; for everything else it creates a new point and names it.
5. Repeat 2–4 for each module.
6. Set the tariff and the hub's time zone.
7. Ask for an installation check, then ask your first question about consumption.

## A worked session

> **You:** Connect to my RBgrid hub at http://192.168.1.50. I want you to be able to change settings too.
>
> **Assistant:** I've started a pairing and asked for read and config access. The hub is now showing a 6-digit
> code on its AI assistants page. It is good for 10 minutes. What is the code?
>
> **You:** 504453
>
> **Assistant:** Paired. Please quit the app completely and open it again, so the hub's tools load.
>
> **You:** Done, restarted.
>
> **Assistant:** I can see the hub: firmware v1.3.0, no modules, no points yet. Let's add modules one at a
> time. Every module ships with the same address, so connect only the first one and tell me when it's in.
>
> **You:** First one is connected.
>
> **Assistant:** Found it on bus 0 at the factory address 0x50. I've moved it to 0x51, so the next module can use
> 0x50 without a conflict. It has one current input (I0) and a voltage input. The hub can't detect the clamp, so I
> need it from you: which clamp is on I0, and what is it measuring?
>
> **You:** SCT-013-100, on the incoming line from the meter.
>
> **Assistant:** That's your supply, so I've moved the hub's main supply point onto this input rather than making
> a new one — a hub has exactly one and it was waiting for a clamp. Set its clamp to SCT-013-100 and named it
> "Main supply". Live reading: 229.8 V, 0.94 A, 198 W — that is active power; 229.8 × 0.94 is apparent power, and
> the two differ on any load that is not purely resistive. Connect the next module when you're ready.
>
> **You:** Second one is in, SCT-013-030, on the water heater.
>
> **Assistant:** Found it at 0x50, moved it to 0x52, created the point "Water heater" as a load. It reads 0 A
> right now.
>
> **You:** It's off. Now set my tariff: 0.35 by day, 0.15 at night from 23:00 to 07:00.
>
> **Assistant:** Here is the exact change I would make:
>     day price    0.30 -> 0.35 EUR/kWh
>     night price  0.12 -> 0.15 EUR/kWh
>     night window 23:00-07:00 (unchanged)
> One warning from the hub: its time zone is not set, so "night" is counted in UTC. Apply the tariff?
>
> **You:** Yes. And fix the time zone too — Europe/Berlin.
>
> **Assistant:** Tariff applied; the hub kept the previous version, so you can restore it. For the time zone,
> here is the change: time zone (not set) -> Europe/Berlin. Apply?
>
> **You:** Yes.
>
> **Assistant:** Applied. Installation check: two modules, two points, no address conflicts, clock and time zone
> set. Nothing left open.
>
> **You:** How much did the house use in the last hour?
>
> **Assistant:** 0.21 kWh, which is 0.07 EUR at your day price. Energy for the current hour is updated every
> 15 minutes, so the last few minutes may not be in yet.

> ⚠️ **On a hub you set up today, money comes later than power.** Live power answers straight away, but a
> point's energy and cost are read out of stored hourly records — and a point whose history has not started yet
> answers **empty, not zero**. Two things have to happen first: the hub has to write its first hourly record,
> and a point you created while the hub was running starts keeping history only after the next restart. Ask for
> watts on your first evening; ask for money the next day. → [Points](web-points.md)

## What that session shows

| In the session | The rule behind it |
|---|---|
| The assistant started the pairing; you read the code off the hub and typed it in | You never start a pairing on the hub — the AI assistant asks, and typing the code is how you agree |
| "Quit the app completely and open it again" | After pairing, your app has to be restarted before it sees the hub's tools |
| One module, then the next | They all leave the factory at the same address |
| "Which clamp is on I0, and what is it measuring?" | The hub reads the module, never the clamp. A wrong rating gives wrong readings with no warning |
| The supply point was **moved**, not created | Every hub starts with exactly one, and it cannot be deleted or given another role |
| Modules and points appeared at once | Setup actions take effect immediately. Most are undone by the opposite action — but deleting a point and restarting the hub have no opposite, and a deleted point's history is gone |
| The tariff came back as an exact change first | Settings arrive in two steps: you see the fields and values before anything is written |
| "The hub kept the previous version" | Settings changes are versioned, and Restore puts the earlier one back |
| "Its time zone is not set, so night is counted in UTC" | A hub as it comes runs on UTC; tariff windows and "today" follow the hub's clock |
| "Updated every 15 minutes" | Energy and cost for the hour in progress are behind by up to that much, and there is no breakdown finer than an hour. Live readings do not lag |
| "0.21 kWh, which is 0.07 EUR" | Both came out of stored records. That session is on a hub that has been running; a point with no history yet answers empty, not zero |

→ The rules in full: [What an AI assistant reads and changes](web-ai-tools.md)

## The second bus

A hub has two I²C buses — the four-wire connections the modules sit on. Eight modules fit in all, on one bus or
split between the two. The second bus is not on until its pins are set and
the hub has restarted, and you can ask an AI assistant to do both.

Two things to know before you do. Setting the bus pins and restarting are setup actions: the hub carries them out
when asked, without stopping to confirm. And the restart is a real one: the hub shuts down and starts again. Your pairing survives it.

## Where your readings go

The hub answers on your own network and sends nothing out of it. Your AI assistant is a separate program: what it
reads from the hub goes wherever that assistant sends it. If it runs in the cloud, your readings go to its
service — the hub has no say in that. Pick the assistant you are willing to show your data to.

## Troubleshooting

**The assistant cannot find a module you just connected.**
The hub re-reads the bus every ten seconds, so give it a moment. If it still does not appear, check the four
wires and that nothing else on that bus already sits at the factory address.

**Two modules were connected before anything was said.**
Both are at the factory address and cannot be told apart. Disconnect one, let the assistant readdress the other,
then connect the second again.

**The readings look wrong from the first day.**
Check the clamp rating you gave against what is printed on the clamp. Nothing else in the chain can catch that
mistake, and correcting it later applies from that moment on — what was already recorded is not recalculated.

**A point ended up on the wrong input.**
Ask to move it: the point keeps its name, its role and everything it has recorded, and only the input changes.
Deleting it instead would erase its history. → [What an AI assistant reads and changes](web-ai-tools.md)
