#!/usr/bin/env bash

source common.sh

producingDrv=$(nix-instantiate ./text-hashed-output.nix -A detachedProducingProducingDrv)
wrapperDrv=$(nix-instantiate ./text-hashed-output.nix -A detachedWrapper)

out1=$(nix build "${wrapperDrv}^out" --no-link --print-out-paths)
builtDrv2=$(nix build "${producingDrv}^out^out" --no-link --print-out-paths)

nix-store --delete "$out1" "$builtDrv2"

# The build trace of the deleted derivation resolves the wrapper's input,
# so nothing writes the derivation again.
out2=$(nix build "${wrapperDrv}^out" --no-link --print-out-paths)

test "$out1" == "$out2"
[[ ! -e "$builtDrv2" ]]
