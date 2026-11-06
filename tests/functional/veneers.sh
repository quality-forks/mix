#!/usr/bin/env bash

source common.sh

needLocalStore "the sandbox only runs on the builder side, so it makes no sense to test it with the daemon"

TODO_NixOS

requireSandboxSupport
[[ $(id -u) == 0 ]] || skipTest "mounting a veneer needs root"
[[ -n ${_NIX_TEST_VENEER_HELPER-} ]] || skipTest "_NIX_TEST_VENEER_HELPER names no veneer helper"
if [[ ! $SHELL =~ /nix/store ]]; then skipTest "Shell is not from Nix store"; fi

export NIX_STORE_DIR=/my/store
export NIX_REMOTE=$TEST_ROOT/store0

veneers=$TEST_ROOT/veneers
export NIX_CONFIG="veneer-helper = $_NIX_TEST_VENEER_HELPER
veneer-cache-dir = $veneers"
build () {
    nix-build veneers.nix --no-out-link --sandbox-paths /nix/store "$@"
}
entries () {
    find "$veneers" -mindepth 1 -maxdepth 1 -not -name '.*' | wc -l
}

# A writable veneer takes the build user's changes, and the veneer and store
# the next build reads stay as they were.
build -A writable
build -A structured

# A build reads the files its manifest lists, and the store path of none.
build -A listed

# Another build mounts the same veneer.
build -A again
[[ $(entries) == 1 ]]

expectStderr 1 build -A unreferenced | grepQuiet "in no store path the manifest refers to"

# Deleting a manifest evicts its veneer.
nix-collect-garbage
[[ $(entries) == 0 ]]

# The cache is a tmpfs of its own, whose inodes no other user of its
# filesystem takes.
[[ $(stat -f -c %T "$veneers") == tmpfs ]]
umount "$veneers"
