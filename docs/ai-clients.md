# Where an AI assistant runs

An AI assistant does not talk to your hub directly. Between them runs a small program on your own computer — the
**bridge**. Your AI assistant starts it, the bridge holds the hub's address and the key the hub issued when you paired, and
it makes the calls to the hub from the computer in front of you.

Because it works that way, the next few things are true.

## What it needs

| | |
|---|---|
| A computer that can run a local program | A desktop or a laptop. Your assistant has to be able to start a program on it. |
| On the same network as the hub | The bridge reaches the hub at a local address. |
| Switched on while you are asking | Close the laptop and the bridge stops with it; the hub is still fine, but nothing is carrying your questions to it. |

**From a phone or a browser alone, this does not work today.** Neither can start a local program, and the hub is
not on the internet.

## Getting the bridge

The bridge comes as one file, and that file is the whole of it: nothing is fetched from a package registry and
there is no installer to run. What it does need is Node on the computer — see the table.

**Download:** [rbgrid-mcp.mcpb](https://github.com/rb-amp/RBgrid/releases/download/mcp-v0.2.0/rbgrid-mcp.mcpb)
· about 2.7 MB · bridge **0.2.0**

| | |
|---|---|
| **Nothing to fetch from a package registry** | The file is the whole package: there is no `install` step that downloads anything. |
| **It runs on Node 20.3.0 or newer** | Some AI assistant apps ship Node inside themselves, and then there is nothing for you to install. Whether yours does is a question for that app's own documentation — and app makers change their rules. **If the bridge does not start, install Node 20.3.0 or newer on that computer yourself**; that is the whole fix, and it is the same fix whichever app you use. |
| **Its own version line** | The bridge is versioned apart from the hub's firmware. What has to match is the hub's **protocol v1**, not a firmware number. |
| **Licence** | The bridge is MIT. That covers this package, not the hub firmware, which has its own licence. |

**If you check downloads,** this file's SHA-256 is
`c979c255de52baf8e6a15d020de730f16051e92900cb2ea539c3a63e351054e4`, and the release page carries the sums too.
It is a binary from the internet; checking it is reasonable.

Keep the link above: it names its version and will go on pointing at this exact file. A "latest release" address
would follow whatever came out last in the repository — a firmware release today, and your download gone
tomorrow.

## Which app you use

Any app that can start a local MCP server can run the bridge — *MCP server* is also the name to look for in your
own app's documentation. What differs between apps is only two things: how you install the bridge, and where you
restart the app afterwards. Everything else on these pages — pairing, scopes, what changes how, where your
readings go — is the same whichever app you choose.

**The apps we have walked ourselves.** An app we have not installed the bridge into, start to finish, does not
get a row here — not even when we are fairly sure it works.

| App | What was walked, and when | |
|---|---|---|
| **Claude Desktop** | Install, a full restart, pairing by code and the first answers with numbers — by hand, on **Windows**, 23 and 25 November 2026. **macOS was not walked.** | → [Claude Desktop](ai-client-claude-desktop.md) |

Every client page carries the same things in the same order, so that once you have read one you know where to
look on the next: what was checked and when, what you need, where your readings go, the exact install path, the
the places that look right and are not, the pairing, the restart, a check that works even on an empty hub, and the
problems specific to that app.

## When something is not working

The bridge can check itself. It runs one command and prints a list: whether your computer can run it, whether
the hub answers, whether your pairing is still good, and how fast the network between them is. The last line
says whether it found a problem.

It tells apart two things that look the same from the chat — a pairing that was revoked, and a hub that is not
answering at all — and they need different fixes. It never prints your token.

**The command.** From a terminal:

```
node "<path to the extension>\server\dist\cli.js" doctor
```

You can give it the hub's address too, if the bridge has not been paired yet:

```
node "<path to the extension>\server\dist\cli.js" doctor http://<the hub's address>
```

🔸 This command starts Node yourself, so it needs Node **on your PATH**. If the only Node you have is the one
inside your assistant's app, a terminal answers *"node is not recognized"* — and that says nothing about the
bridge, which may be working perfectly. The self-check is for a reader who has Node installed.

**The last line is the answer.** When nothing is wrong it reads:

```
doctor: no problem found
```

Above it, each check is its own line, marked **ok** or **FAIL**, and a failing one says what to do about it. A
bridge that is installed but not yet paired is the common case, and it is not a fault — it reads *"a hub is
paired — none yet"* and tells you to pair one. The Node line names the version it is running and the version it
needs, so it settles that question for you rather than leaving you to check.

**The assistant cannot see the hub right after pairing.** Quit the app completely and open it again. The bridge
tells the app that its tool list changed; some apps do not re-read it inside a running session. →
[Connect an AI assistant](ai-connect.md)

**More than one hub answers.** Your AI assistant can list what it found — a name, an address and an id for each —
and ask which one you mean. On firmware **v1.3.0** the announced name follows the name you gave the hub, so the
name is enough to tell them apart. On older firmware the announced name could go stale and two hubs could answer with
the same one; there, choose by address and open each one's page in a browser to see which is which.

## Once it is running

Pair your AI assistant with the hub → [Connect an AI assistant](ai-connect.md), then set the hub up or
start asking it questions → [Set up a hub by talking to it](ai-first-setup.md)

## Where your readings go

The hub answers on your own network and sends nothing out of it. Your AI assistant is a separate program: what it
reads from the hub goes wherever that assistant sends it. If it runs in the cloud, your readings go to its
service — the hub has no say in that. Pick the assistant you are willing to show your data to.
