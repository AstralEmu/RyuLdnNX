---
name: sourced-and-verified
description: ABSOLUTE, ALWAYS-ON rule. Nothing may be answered, claimed or produced without a real source or a verification you actually ran — never from memory or plausibility. Every action report ends with a "Sources & proofs" section. Apply on EVERY task; unsourced or unverified work is sent back.
---

# sourced-and-verified — no unsourced, no unverified work

## The rule

Do not answer, claim, decide or produce anything without a real source or a
verification you actually ran. Memory, plausibility and "I'm pretty sure" are
forbidden as a basis. If you cannot source or verify it, research or test it first.

A file read in this context is a verification you ran; knowledge from training or a
previous session is memory.

## Two kinds of backing

1. **External facts** (a library's behaviour, the Switch/libnx/Atmosphère API, the
   Ryujinx LDN protocol, a tool's flags) → cite an authoritative source fetched this
   session: switchbrew.org, the Atmosphère / libnx sources, the Ryujinx and LdnServer
   sources under `~/GIT`, the tool's own docs.
2. **Claims about this code** ("it passes", "the packet is 0x14 bytes") → prove it by
   running a command and showing the real output.

Before writing a parser, a generator or a client, check whether a maintained tool
already does it, and state why you build in-house if you do.

## Every action report ends with "Sources & proofs"

```
## Sources & proofs
- Source: <file:line or URL> — confirms <the fact relied on>.
- Verified: `<command>` → <result / exit code / key output line>.
```

Anything still unverified is listed as an open risk, never presented as established.
