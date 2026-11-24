#!/bin/sh
# Shared helpers for the tracked git hooks. Sourced, never executed.

# `git commit`/`git push` run from a linked worktree export GIT_DIR and friends
# (githooks(5)); they override every `git -C <dir>` below and would make the
# wiki calls act on the code repository. Drop them before any git call.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE

repo_root() {
	git rev-parse --show-toplevel
}

# Absolute path of the wiki clone. The repo reaches it through the untracked
# `docs` symlink (the wiki is a separate git repository, not a submodule).
# Prints nothing when the link is absent or does not point to a git clone.
wiki_dir() {
	root=$(repo_root) || return 0
	[ -e "$root/docs/.git" ] || return 0
	(cd "$root/docs" && pwd -P)
}

wiki_git() {
	git -C "$(wiki_dir)" "$@"
}

# The wiki's default branch as the remote names it (GitHub wikis are `master`
# or `main` depending on age). Never hardcoded.
wiki_branch() {
	wd=$(wiki_dir)
	ref=$(git -C "$wd" symbolic-ref --quiet --short refs/remotes/origin/HEAD || [ $? -eq 1 ])
	if [ -n "$ref" ]; then
		echo "${ref#origin/}"
		return
	fi
	git -C "$wd" config --get init.defaultBranch 2>/dev/null \
		|| git -C "$wd" for-each-ref --format='%(refname:short)' refs/heads | head -n 1
}
