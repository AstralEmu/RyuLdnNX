
## Push (2026-11-23)

- Wiki push denied: `git -C ../RyuLdnNX.wiki push` -> `remote: Permission to AstralEmu/RyuLdnNX.wiki.git denied to REMOVED. ... 403` (wiki origin is https://github.com/AstralEmu/RyuLdnNX.wiki.git, code origin is git@github-ethiquema:Ethiquema/ryu_ldn_nx.git).
- Code not pushed either: the owner asked for wiki and code to be pushed together.
- Needs the owner: credentials or the correct wiki remote.

## Owner actions needed (2026-11-23)

- `_FORTIFY_SOURCE=3` dropped from tests/Makefile: GCC 12.2 raises a false `-Wstringop-overflow` (tests/protocol_tests.cpp:680, :1837 via sysmodule/source/protocol/packet_buffer.hpp:128). Real fix needs a newer GCC in the test image.
- `scripts/codeql-create.sh` and `scripts/codeql-analyze.sh` swallow command failures through forced-success constructs; protected path for the agent, owner must fix or delete them.
- Untracked stray `sysmodule/source/ldn/ldn_icommunication.cpp.bak`: owner to delete.
- `hooks/pre-commit` is modified in the working tree; the owner commits it.
- Untracked stray `tests/_probe_move.cpp` (my use-after-move probe): deleting it from the shell is refused by `.claude/hooks/guard-no-test-deletion.py`; owner to delete it, never commit it.
