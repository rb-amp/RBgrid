# Claude Desktop

This is the first app we have installed the bridge into ourselves, start to finish, and this page is that walk
written down. Everything on the other pages of this section — what an assistant reads, what it may change,
where your readings go — is the same whichever app you use. What is here is only what is specific to this one.

**What was checked, and when.** Install, a full restart, pairing by code and the first answers with numbers were
walked by hand on **Windows**, on 23 and 25 November 2026. **macOS was not walked by us.** The bridge should
behave the same there, and we are not going to tell you it does until someone has done it.

## What you need

| | |
|---|---|
| The bridge, downloaded | One file. → [Where an AI assistant runs](web-ai-clients.md#getting-the-bridge) |
| Node | The bridge runs on **Node 20.3.0 or newer**. This app's own documentation says it brings a Node with it, so there is usually nothing for you to install — that is their statement about their app, not a measurement of ours, and app makers change their rules. |
| The hub's AI interface switched on | It is off until you switch it on, and that needs the admin password. → [Connect an AI assistant](web-ai-connect.md) |

## Installing it

**One door works, and three look like they should.** Save yourself the detour:

| What you might try | What happens |
|---|---|
| Double-clicking the downloaded file | Nothing. |
| **Load plugin** | The format is not accepted. |
| **Add connector** | It asks for a URL, and the bridge is a file, not an address. |

**The door that works:** **Settings → Desktop app → Extensions → Manage → Advanced settings → Install
Extension…**, then pick the file you downloaded.

*You should see:* the extension listed as installed.

## Restarting it — not optional

**Quit the app completely and open it again, every time you pair.** Three times out of three, without a full
restart, the bridge was paired and the assistant still could not see the hub.

**Closing the window is not quitting.** On Windows, close it from the notification area as well.

The reason is not a fault in the bridge or in the hub: the bridge announces that the hub's tools have arrived,
and this app does not re-read that list while a session is running. The bridge says so itself if you ask it —
*"their app has not picked up the change — restarting it will"*.

## Pairing

The order is the same as on the main page, and the part worth repeating is the part people get backwards: **you
do not start the pairing.** You ask the assistant to connect, it asks the hub, the hub shows a six-digit code,
and you type that code into the chat as an ordinary message. Typing it **is** the approval — there is no second
"allow" step.

The code is good for **ten minutes** and works once. → [Connect an AI assistant](web-ai-connect.md)

## A check that works even on an empty hub

Ask *"What do you see on my hub?"* You should get the firmware version and that there are no modules and no
points. That is a complete, correct answer on a hub with nothing on it, and it proves the whole chain — app,
bridge, hub — without needing a single clamp fitted.

On a hub that is already measuring, ask *"Which circuits do you see on my hub?"* and the answer comes back with
your own point names.

→ Set the hub up by talking to it: [Set up a hub by talking to it](web-ai-first-setup.md)

## If something is not working

**The assistant says it has no such tool.** If you paired read-only, that is the expected answer: the changing
tools are not in the list it was given at all. Widening what it may do needs a second pairing code, not a
retry. → [What an AI assistant reads and changes](web-ai-tools.md#what-it-changes)

**The assistant cannot see the hub at all after pairing.** The restart above, done properly — from the
notification area too.

**A terminal says "node is not recognized" when you run the bridge's self-check.** That says nothing about the
bridge. The self-check starts Node yourself and so needs Node on your PATH; the Node this app brings is its
own. → [Where an AI assistant runs](web-ai-clients.md#when-something-is-not-working)

## Where your readings go

The hub answers on your own network and sends nothing out of it. Your AI assistant is a separate program: what
it reads from the hub goes wherever that assistant sends it. If it runs in the cloud, your readings go to its
service — the hub has no say in that.
