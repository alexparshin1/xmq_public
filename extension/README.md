# Writing an XMQ extension

An extension is a shared library the broker loads at startup. It sees what the broker is doing and
can act on it, without being part of the broker: it is built separately, licensed separately, and
a failure in it is not a failure of the broker.

The interface is C. That is not nostalgia — an extension is compiled by someone else, with another
compiler and another standard library, and loaded into the broker's process. A C++ interface across
that line breaks silently when any of those differ. You still write C++: `XmqExtension.h` wraps the
C in a base class, and the boundary stays C underneath.

## Start with the generator

`new-extension.py`, installed beside these headers, writes a project that builds and loads before
you have written a line of it — the CMake, the class, the entry point, the configuration entry to
paste, and a README of its own. Every decision that is yours is a `TODO` saying what the choice is
between.

```sh
./new-extension.py audit-trail --capability observer
./new-extension.py device-auth --capability authenticator --database
```

Build it as it comes out and let the broker load it before changing anything: that separates "my
code is wrong" from "my build is wrong", and the second is much harder to see. A generated extension
answers `NOT_HANDLED` to everything — no opinion — so it changes nothing while you fill it in, and
it says so in the log at every start until you delete the line that does.

## The smallest extension

```cpp
#include "extension/XmqExtension.h"

namespace {
class Counter : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    void onEvent(const xmq_event& event) override
    {
        if (event.type == XMQ_EVENT_PUBLISHED)
        {
            ++m_published;
        }
    }

    bool stop() override
    {
        log(XMQ_LOG_INFO, "saw " + std::to_string(m_published) + " messages");
        return true;
    }

private:
    uint64_t m_published {0};
};
}

XMQ_DEFINE_EXTENSION(Counter, "counter", "1.0", XMQ_CAP_OBSERVER)
```

Build it as a plain shared library. It includes two headers from the broker and links nothing of
it — check with `ldd`: an extension that has picked up a dependency on the broker's libraries has
been built wrong.

```cmake
ADD_LIBRARY(counter SHARED Counter.cpp)
TARGET_INCLUDE_DIRECTORIES(counter PRIVATE /usr/local/include/xmq)
```

Or through the helper the broker installs to `share/xmq/cmake`, which gives an extension the ABI
headers and nothing else of the broker, and is what XMQ builds its own samples with:

```cmake
INCLUDE(/usr/local/share/xmq/cmake/XmqExtension.cmake)
XMQ_ADD_EXTENSION(counter SOURCES Counter.cpp)
```

Install it somewhere that is not `lib/xmq` — `lib/xmq-extensions` is the convention these samples use.
`lib/xmq` is where the web interface is served from, so a library left there can be fetched over HTTP
by anyone who can reach the control service.

Three complete samples are in `examples/extensions`, all in the public domain — copy them.
`event-log` writes every broker event to a file and declares `XMQ_CAP_OBSERVER`; `allow-list`
admits the client ids in its configuration and declares `XMQ_CAP_AUTHENTICATOR`; `topic-guard`
lets a group use the topics under its own name and declares `XMQ_CAP_AUTHORIZER`. They are
separate on purpose: one extension, one job, and between them they show that an extension is
handed only what it asked for.

The broker also ships one, in `extension/user-database` — the authenticator a standalone broker
runs, answering CONNECT from its SQL user database. It is worth reading after the samples, because
it is the one with a store: a connection made once and reused, a lookup on the broker's thread,
settings that are validated before they are accepted, and an outage told apart from a wrong
password. It is built through `XMQ_ADD_EXTENSION` like everything else and reaches no more of the
broker than your own extension does.

## Configuring one

Extensions are named in `xmq_extensions.conf`, beside `xmq_server.conf`. Nothing is discovered by
scanning a directory: a broker that loads whatever is in a folder is a broker whose behaviour
depends on what someone left there.

```json
{
  "extensions": [
    {
      "name": "event-log",
      "library": "/usr/local/lib/xmq-extensions/libxmq_event_log.so",
      "enabled": true,
      "settings": { "file": "/var/log/xmq/events.log" }
    }
  ]
}
```

`name` must match the name the library declares, or the broker refuses to load it — otherwise the
file names one extension and the broker runs another. Everything under `settings` is the
extension's own, reachable through `setting("key")`.

An extension that will not load, refuses the ABI version, or fails to start is reported in the
broker's log and skipped. The broker starts either way. An extension is an addition to a broker,
never a condition for one.

## Capabilities

An extension declares what it implements, and is given nothing else. An authenticator is never
handed an event; an observer is never asked about a connection. That is not filtering — the
capability set decides which pointers the broker gets at all, so there is nothing to call.

```cpp
XMQ_DEFINE_EXTENSION(LdapAuth,  "ldap",      "1.0", XMQ_CAP_AUTHENTICATOR)
XMQ_DEFINE_EXTENSION(SnmpAgent, "snmp",      "1.0", XMQ_CAP_OBSERVER)
XMQ_DEFINE_EXTENSION(TopicAcl,  "acl",       "1.0", XMQ_CAP_AUTHORIZER)
XMQ_DEFINE_EXTENSION(Both,      "both",      "1.0", XMQ_CAP_OBSERVER | XMQ_CAP_AUTHENTICATOR)
```

It is declared rather than inferred because the base class carries both methods with harmless
defaults: were the broker to guess from what is overridable, every extension would look like it
implemented everything. And if nothing loaded declares a capability, the machinery behind it is not
built at all — no event thread, no authentication pool, and the CONNECT path keeps its original
shape.

**Observe.** `onEvent()` is called for connects, disconnects, subscribes, unsubscribes and
publishes. This is enough to build an SNMP agent, an alerting bridge, or an audit log.

Observers never run on a thread that carries messages. The broker copies each event into a bounded
queue and delivers from a thread of its own, so an observer that blocks costs dropped events rather
than delivery latency — and the drops are counted and logged. Take your time in `onEvent()` if you
must, but know what you are trading.

Message payloads are not passed to observers, only their size. Copying every body through the queue
would make watching the broker cost more than running it.

**Authenticate.** `authenticate()` decides whether a client may connect. It is given the client id,
username, password and peer address, and answers `ALLOW`, `DENY`, or `NOT_HANDLED`.

`NOT_HANDLED` is the one to reach for by default. The broker asks each authenticator in
configuration order, stops at the first `ALLOW` or `DENY`, and falls back to its own accounts when
all of them abstain. An extension that denies every client it does not recognise locks out
everyone the broker would otherwise have admitted — which is how an authentication extension takes
a broker down on the day it is installed.

**This call may block, and is expected to.** Real authentication is a network round trip to a
directory server. It is made on a thread the broker keeps for authentication, never on one carrying
messages: the receive worker that read the CONNECT is released immediately, and nothing more is
read from that client until the answer arrives. Four concurrent connections against an
authenticator that takes 700 ms complete in 700 ms, not in 2.8 seconds, and traffic from every
other client is untouched throughout.

What it costs is a thread from a small pool for the duration. An authenticator that never returns
holds one forever; when the pool is exhausted the broker refuses further connections rather than
admitting them unchecked, and says which extension it is waiting for. "We could not check" has only
one safe reading.

**Authorize.** `authorize()` decides what a connected client may publish and subscribe to. Unlike
the other two, it sits on a path where the broker's own work is measured in tens of microseconds,
and that shapes the whole capability.

It comes in two halves. `resolveGroup()` runs **once per connection**, on the same authentication
thread that already asks the authenticators, and **may block** — it is where a directory is asked
which groups a client belongs to. It returns a name the extension chooses; the broker keeps it on
the session and never interprets it.

`authorize()` is then asked about **a group and a topic, never about a client**, and the broker
caches the answer under exactly that pair. A thousand clients in one group publishing to one topic
ask once between them, and every message after that is a lookup with the extension nowhere near it
— about 8 ns, scaling with cores rather than contending for a lock. Write rules that depend on the
individual client and that sharing is gone: the cache key becomes the client, and the extension is
back on the hot path for every message.

`authorize()` **must not block** and must not call back into the broker. Read a table prepared in
`start()` and return. When the rules change, call `invalidateAcl()` on the host and the broker asks
again.

The same abstention rule applies as for authentication: answer `NOT_HANDLED` about groups and
topics the extension has no rules for, rather than denying them.

## What is coming, and why it is not here yet

Dashboard routes are planned next.

Every capability added to this ABI is permanent: once an extension in production uses it, it is
supported for years. That is why it grows slowly.

## Versions

`xmq_extension_describe()` is handed the broker's ABI version and returns a table, or `NULL` to
decline. The generated implementation accepts a broker whose major version matches and whose minor
is at least the one it was built against — a newer broker runs an older extension, because
everything added since is additive; an older broker refuses a newer extension, because it may not
have what the extension expects.

Current version: **1.2** — 1.1 added authentication, 1.2 authorisation; 1.0 had observation only.
