#!/usr/bin/env bash

source common.sh

TODO_NixOS

needLocalStore "'--external-builders' can’t be used with the daemon"

expr="$TEST_ROOT/expr.nix"
cat > "$expr" <<EOF
with import ${config_nix};
mkDerivation {
  name = "external";
  system = "x68_46-xunil";
  buildCommand = ''
    echo xyzzy
    printf foo > \$out
  '';
}
EOF

external_builder="$TEST_ROOT/external-builder.sh"
cat > "$external_builder" <<EOF
#! $SHELL -e

PATH=${PATH@Q}

[[ "\$1" = bla ]]

system="\$(jq -r .system < "\$2")"
builder="\$(jq -r .builder < "\$2")"
args="\$(jq -r '.args | join(" ")' < "\$2")"
export buildCommand="\$(jq -r .env.buildCommand < "\$2")"
export out="\$(jq -r .env.out < "\$2")"
[[ \$system = x68_46-xunil ]]

printf "\2\n"

# In a real external builder, we would now call something like qemu to emulate the system.
"\$builder" \$args

printf bar >> \$out
EOF
chmod +x "$external_builder"

nix build -L --file "$expr" --out-link "$TEST_ROOT/result" \
  --extra-experimental-features external-builders \
  --external-builders "[{\"systems\": [\"x68_46-xunil\"], \"args\": [\"bla\"], \"program\": \"$external_builder\"}]"

[[ $(cat "$TEST_ROOT/result") = foobar ]]

veneers_expr="$TEST_ROOT/veneers.nix"
cat > "$veneers_expr" <<EOF
with import ${config_nix};
let
  data = builtins.toFile "data" "hello";
  manifest = builtins.toFile "listed.veneer" (builtins.toJSON {
    a.file = {
      inherit data;
      size = 5;
    };
  });
in
mkDerivation {
  name = "external-veneers";
  system = "x68_46-xunil";
  __veneers = [ "source=\${manifest}" ];
  buildCommand = "";
}
EOF

veneers_builder="$TEST_ROOT/veneers-builder.sh"
{
  echo "#! $SHELL -e"
  echo "PATH=${PATH@Q}"
  cat <<'EOF'
manifest=$(jq -r '.veneers[0].manifest' < "$1")
[[ $manifest = */*-listed.veneer ]]
[[ $(jq -r '.veneers[0].writable' < "$1") = true ]]
[[ $(jq -r '.veneers[0].target' < "$1") = /build/source ]]
[[ $(jq --arg manifest "$manifest" '.inputPaths | index($manifest)' < "$1") = null ]]
[[ $(cat "$(jq -r '.veneers[0].references' < "$1")") = */*-data ]]
printf "\2\n"
printf foo > "$(jq -r .env.out < "$1")"
EOF
} > "$veneers_builder"
chmod +x "$veneers_builder"

nix build --file "$veneers_expr" --no-link \
  --extra-experimental-features external-builders \
  --external-builders "[{\"systems\": [\"x68_46-xunil\"], \"args\": [], \"program\": \"$veneers_builder\"}]"
