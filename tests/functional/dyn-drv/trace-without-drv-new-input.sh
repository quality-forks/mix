#!/usr/bin/env bash

source common.sh

instantiate() {
    nix-instantiate --expr "(import ./text-hashed-output.nix).$1 \"$2\""
}

# Evaluation writes the derivation the producer writes too, so all of it
# happens before that derivation is deleted.
wrapperDrv1=$(instantiate seededWrapper 1)
wrapperDrv2=$(instantiate seededWrapper 2)
producerDrv1=$(instantiate seededProducer 1)
[[ "$wrapperDrv1" != "$wrapperDrv2" ]]

out1=$(nix build "${wrapperDrv1}^out" --no-link --print-out-paths)
builtDrv=$(nix build "${producerDrv1}^out" --no-link --print-out-paths)

nix-store --delete "$builtDrv"
[[ ! -e "$builtDrv" ]]

# The new seed rebuilds the producer's input into the same output, so the
# producer resolves as before and its build trace stands for the deleted
# derivation, which nothing writes again.
out2=$(nix build "${wrapperDrv2}^out" --no-link --print-out-paths)

test "$out1" == "$out2"
[[ ! -e "$builtDrv" ]]
