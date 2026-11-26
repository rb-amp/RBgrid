# Connect an AI assistant

This is the only numbered route in this section. It takes a hub that is already on your Wi-Fi and ends with an
answer you can check.

> You need a **bridge** running on the computer where your AI assistant runs — a small local program that carries
> the conversation to the hub. Your assistant's own documentation may call it an *MCP server*: MCP is the agreed
> way AI assistants talk to tools like this hub. → [Where an AI assistant runs](ai-clients.md)

## From nothing to the first answer

Seven steps, in the order they happen. Step 1 is on the hub, and skipping it is the usual reason nothing works
later.

**1. Switch the AI interface on.** It is off until you do, and the hub will not switch it on without an admin
password: that password is what keeps anyone else on your network from switching it on for you. You can do this
in either place below; both change the same setting.

| | Where |
|---|---|
| On the hub's own pages | Open `http://<your hub>/ai`. Served by the hub, so it works whether or not the web app is installed. If it asks for the **Admin password** and the hub has none, set one on the hub's **WiFi** page. Then under **Status** choose **Turn on**. |
| In the app | **Set up → AI assistants**, section **Access**, switch on **Allow AI assistants**. If a red notice asks for an admin token first, its **Security** button takes you there. |

The admin password and the app's "API token" are one value: set it in either place and it is set for both.

*You should see:* the interface on, and no assistant paired yet.

**2. Install the bridge** on the computer that will run your AI assistant — it is one file:
**[rbgrid-mcp.mcpb](https://github.com/rb-amp/RBgrid/releases/download/mcp-v0.2.0/rbgrid-mcp.mcpb)**. Add that file in your AI assistant's app; what it
needs on the computer, and the one app we have walked ourselves, are on the pages below. Do this **before** step 3: the pairing
code appears only once an assistant asks for it, it works once, and it expires in ten minutes — so installing
the bridge while a code is counting down means starting over. → [Where an AI assistant runs](ai-clients.md)

**3. Ask your AI assistant to connect**, naming your hub's address and how much you want it to be able to do.
Something like *"Connect to my RBgrid hub at http://192.168.1.50 — read only"*, or *"…and I want you to be able
to change settings too."*

The address is the one you used in step 1 to open the hub's page. If you do not have it yet, that is where to
start: a hub fresh out of its box makes its own Wi-Fi network first, and you set it up from there.
→ [Quick start](web-quick-start.md)

*You should see:* the assistant say it has asked to pair.

**4. Read the code off the hub.** The assistant starts the pairing — there is no button here to start it. When it
asks, the hub shows a six-digit code together with **who is asking and what it is asking for**, for example
*"<name> asks to connect (read, config)"*.

| | What you see |
|---|---|
| The hub's page `/ai` | A **Pairing code** box: the code, the name of the client, and the scopes it asked for. |
| The app | A bar on top of whatever page you are on: the client's name, the code, and the time left. **Show** opens the AI assistants page; **Deny** refuses. |
| The app's AI assistants page | A **Pairing** card: the client's name, **Asks for: read data / propose changes**, the code, **Copy the code** and the time left. |

> **"Propose changes" is the narrower half of what you are agreeing to.** With it, an assistant also adds and
> deletes points, readdresses modules and restarts the hub, and those happen as soon as it asks — no proposal,
> no confirmation. → [What an AI assistant reads and changes](ai-tools.md)

> ⚠️ **The bar on top of the app does not show the scopes.** They are on the hub's page and on the AI assistants
> page. If you are about to type a code you read off the bar, open the AI assistants page first and see what you
> are agreeing to.

**5. Type the code into your AI assistant.** That is the whole approval — there is no second step, and no separate
"allow" button anywhere. **Deny** next to the request refuses it instead.

Only pair an assistant whose pairing you started yourself.

**6. Quit your AI assistant's app completely and open it again.** Do this every time you pair. The bridge tells
the app that the hub's tools have arrived, and some apps do not re-read that list while they are running — so
until you restart, the hub is paired and the assistant still answers as though it were not there. Closing the
window is not always enough: on Windows close it from the notification area as well, and on a Mac quit it from
the menu bar.

*You should see:* the assistant listing the hub's tools, or simply answering the next question about the hub.

**7. Ask something you can check.** On a hub that is already set up: *"Which circuits do you see on my hub?"* —
the answer comes back with your own point names.

On a hub with nothing on it yet, that question has no answer, and that is not a failure. Ask *"What do you see on
my hub?"* instead: you should get the firmware version and that there are no modules and no points. From there,
→ [Set up a hub by talking to it](ai-first-setup.md)

## How many at the same time

| | |
|---|---|
| Pairing requests waiting at the same time | 2 |
| Assistants paired at the same time | 8 |
| Wrong codes before a request is cancelled | 5 |
| Wrong codes in ten minutes before the hub stops accepting pairings for ten minutes | 20 |

## Two AI assistants, two pairings

Each assistant pairs on its own and gets its own token and its own scope. Pairing a second one does not disturb
the first, and revoking one does not touch the other. The app's **Paired assistants** table shows each one with
its access, when it was paired, when it was last used and how many calls it made today.

## Take access away

On the hub's page, under **Connected assistants**, choose **Revoke**; in the app the section is called **Paired
assistants** and the button is the same. The assistant loses access at once, and to use it again you pair it
again.

Revoking takes away access from that moment on. It does not undo anything the assistant already did: changes it
applied stay applied, and they stay in the hub's list of versions, where **Restore** can still take them back.

A factory reset removes every pairing along with everything else.

## Where your readings go

The hub answers on your own network and sends nothing out of it. Your AI assistant is a separate program: what it
reads from the hub goes wherever that assistant sends it. If it runs in the cloud, your readings go to its
service — the hub has no say in that. Pick the assistant you are willing to show your data to.

## Troubleshooting

**The assistant says it is paired, but it cannot see the hub.**
Quit its app completely — on Windows also from the notification area, on a Mac from the menu bar — and open it
again. The bridge tells the app that the
tool list changed, but some apps do not re-read it inside a running session. This is the first thing to try, and
it is not a sign that anything went wrong.

**No code appears.**
The interface is off, or the hub has no admin password yet. The AI assistants page shows which of the two it is:
it will either offer you **Turn on**, or tell you to set the password first.

**The bar in the app says an AI assistant wants to connect but shows no code.**
This browser has not been given the hub's admin token yet, and the code is not shown without it. Open the AI
assistants page: it will ask for the token, and after that the code is there. The hub's own page `/ai` asks the
same way.

**The hub refuses the pairing — `llm_disabled`.**
The hub has been reset to factory settings. Pairing needs both an admin password and the interface switched on;
set them again, then pair again.

**Codes keep being rejected.**
After five wrong codes the request dies and the assistant has to ask again. After twenty wrong codes in ten
minutes the hub stops accepting pairings for ten minutes.

**There is more than one RBgrid hub on the network.**
Your AI assistant can list the hubs it finds, with a name, an address and an id for each. Choose by **address** —
open each one's page in a browser and you will see which is which. On firmware older than v1.3.0 the name
announced on the network was not kept up to date, so two hubs could announce the same one; from v1.3.0 the
announced name follows the hub's own name.

**The hub answered yesterday and does not answer today.**
Its address has probably changed. Ask your assistant to look for the hub again, or give it the new address.
