#!/usr/bin/env bash

source common.sh

needLocalStore "the sandbox only runs on the builder side, so it makes no sense to test it with the daemon"

TODO_NixOS

requireSandboxSupport
[[ $(id -u) == 0 ]] || skipTest "mounting a tree needs root"
[[ -n ${_NIX_TEST_TREE_HELPER-} ]] || skipTest "_NIX_TEST_TREE_HELPER names no tree helper"
if [[ ! $SHELL =~ /nix/store ]]; then skipTest "Shell is not from Nix store"; fi

export NIX_STORE_DIR=/my/store
export NIX_REMOTE=$TEST_ROOT/store0

trees=$TEST_ROOT/trees
export NIX_CONFIG="tree-helper = $_NIX_TEST_TREE_HELPER
tree-cache-dir = $trees"
build () {
    nix-build trees.nix --no-out-link --sandbox-paths /nix/store "$@"
}
entries () {
    find "$trees" -mindepth 1 -maxdepth 1 -not -name '.*' | wc -l
}

# A writable tree takes the build user's changes, and the tree and store the
# next build reads stay as they were.
build -A writable
build -A structured

# A build reads the files its manifest lists, and the store path of none.
build -A listed

# Another build mounts the same tree.
build -A again
[[ $(entries) == 1 ]]

expectStderr 1 build -A unreferenced | grepQuiet "in no store path the manifest refers to"

# Deleting a manifest evicts its tree.
nix-collect-garbage
[[ $(entries) == 0 ]]
