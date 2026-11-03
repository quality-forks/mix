#!/usr/bin/env bash

source common.sh

needLocalStore "transient-store-size is read by the garbage collector"

TODO_NixOS

clearStore

package=$(nix-build gc-transient.nix -A package -o "$TEST_ROOT/package")
collection=$(nix-build gc-transient.nix -A collection --no-out-link)
object=$(nix-build gc-transient.nix -A object --no-out-link)
source=$(nix-build gc-transient.nix -A source --no-out-link)

# Within the budget, transient paths stay without roots and keep what they
# refer to.
nix-store --gc
[[ -e "$collection" && -e "$object" && -e "$source" && -e "$package" ]]

# Over it, the collection goes first, then the object it held, and then the
# source is plain garbage.
nix-store --gc --option transient-store-size 0
[[ ! -e "$collection" && ! -e "$object" && ! -e "$source" ]]
[[ -e "$package" ]]

# A root keeps a transient path and what it refers to.
collection=$(nix-build gc-transient.nix -A collection -o "$TEST_ROOT/collection")
object=$(nix-build gc-transient.nix -A object --no-out-link)
nix-store --gc --option transient-store-size 0
[[ -e "$collection" && -e "$object" ]]

# Any build evicts in the background what the budget gives up, at least
# one batch of what nothing refers to before it ends.
rm "$TEST_ROOT/collection"
nix-build gc-transient.nix -A other --no-out-link --option transient-store-size 0
[[ ! -e "$collection" ]]
