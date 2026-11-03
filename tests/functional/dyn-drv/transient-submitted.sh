#!/usr/bin/env bash

source common.sh

# builder-rpc-v0
requireDaemonNewerThan "2.36pre"

TODO_NixOS # can't enable a sandbox feature easily

needLocalStore "transient-store-size is read by the garbage collector"

enableFeatures 'ca-derivations'
restartDaemon

NIX_BIN_DIR="$(dirname "$(type -p nix)")"
export NIX_BIN_DIR

nix-instantiate ./non-trivial-submitted.nix -A transientProducer --add-root "$TEST_ROOT/producer"
producer=$(readlink "$TEST_ROOT/producer")
out=$(nix build -L "${producer}^out^out" -o "$TEST_ROOT/transient" --print-out-paths)
builtDrv=$(nix build "${producer}^out" --no-link --print-out-paths)

# The derivations the producer wrote are evicted, the rooted output stays,
# and its build trace still finds it without them.
nix-store --gc --option transient-store-size 0
[[ ! -e "$builtDrv" && -e "$out" ]]

out2=$(nix build -L "${producer}^out^out" --no-link --print-out-paths)
[[ "$out" == "$out2" && ! -e "$builtDrv" ]]
