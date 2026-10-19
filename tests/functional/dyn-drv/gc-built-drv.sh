#!/usr/bin/env bash

source common.sh

producingDrv=$(nix-instantiate ./text-hashed-output.nix -A detachedProducingProducingDrv)
wrapperDrv=$(nix-instantiate ./text-hashed-output.nix -A detachedWrapper)

out1=$(nix build "${wrapperDrv}^out" --no-link --print-out-paths)
builtDrv1=$(nix build "${producingDrv}^out" --no-link --print-out-paths)
builtDrv2=$(nix build "${producingDrv}^out^out" --no-link --print-out-paths)
helloOut=$(nix-store -q --outputs "$builtDrv2")

mapfile -t helloReferrers < <(nix-store -q --referrers-closure "$helloOut")

nix-store --delete "$out1" "${helloReferrers[@]}" "$builtDrv2" "$builtDrv1"

out2=$(nix build "${wrapperDrv}^out" --no-link --print-out-paths)

test "$out1" == "$out2"
