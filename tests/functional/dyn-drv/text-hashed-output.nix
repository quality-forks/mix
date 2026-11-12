with import ./config.nix;

let
  # Writes the derivation of `drv` without referencing it, so GC can delete it alone
  detachedProducer = detachedProducerWith { };

  detachedProducerWith =
    extra: drv:
    let
      outPlaceholder = builtins.placeholder "out";
      outTail = builtins.substring 1 (-1) outPlaceholder;
    in
    mkDerivation (
      {
        inherit outTail;
        name = "${drv.name}.drv";
        drvText = builtins.replaceStrings [ outPlaceholder ] [ "%${outTail}" ] (
          builtins.readFile drv.drvPath
        );
        buildCommand = ''
          printf '%s' "''${drvText//"%$outTail"/"/$outTail"}" > $out
        '';
        __contentAddressed = true;
        outputHashMode = "text";
        outputHashAlgo = "sha256";
      }
      // extra
    );
in

# A simple content-addressing derivation.
# The derivation can be arbitrarily modified by passing a different `seed`,
# but the output will always be the same
rec {
  hello = mkDerivation {
    name = "hello";
    buildCommand = ''
      set -x
      echo "Building a CA derivation"
      mkdir -p $out
      echo "Hello World" > $out/hello
    '';
  };

  producingDrv = mkDerivation {
    name = "hello.drv";
    buildCommand = ''
      echo "Copying the derivation"
      cp ${builtins.unsafeDiscardOutputDependency hello.drvPath} $out
    '';
    __contentAddressed = true;
    outputHashMode = "text";
    outputHashAlgo = "sha256";
  };

  detachedProducingDrv = detachedProducer hello;

  caHello = mkDerivation {
    name = "ca-hello";
    buildCommand = ''
      mkdir -p $out
      echo "Hello World" > $out/hello
    '';
    __contentAddressed = true;
    outputHashMode = "recursive";
    outputHashAlgo = "sha256";
  };

  # An input every seed rebuilds into the same output
  stableInput =
    seed:
    mkDerivation {
      name = "stable-input";
      inherit seed;
      buildCommand = "echo stable > $out";
      __contentAddressed = true;
      outputHashMode = "recursive";
      outputHashAlgo = "sha256";
    };

  seededProducer = seed: detachedProducerWith { input = stableInput seed; } caHello;

  seededWrapper =
    seed:
    mkDerivation {
      name = "use-seeded-dynamic-drv";
      buildCommand = ''
        cp -r ${builtins.outputOf (seededProducer seed).outPath "out"} $out
      '';
      __contentAddressed = true;
      outputHashMode = "recursive";
      outputHashAlgo = "sha256";
    };

  detachedProducingProducingDrv = detachedProducer detachedProducingDrv;

  wrapper = mkDerivation {
    name = "use-dynamic-drv-in-non-dynamic-drv";
    buildCommand = ''
      echo "Copying the output of the dynamic derivation"
      cp -r ${builtins.outputOf producingDrv.outPath "out"} $out
    '';
  };

  detachedWrapper = mkDerivation {
    name = "use-detached-dynamic-drv";
    buildCommand = ''
      cp -r ${builtins.outputOf (builtins.outputOf detachedProducingProducingDrv.outPath "out") "out"} $out
    '';
  };

  failingWrapper = mkDerivation {
    name = "use-dynamic-drv-in-non-dynamic-drv-wrong";
    buildCommand = ''
      echo "Fail at copying the output of the dynamic derivation"
      fail ${builtins.outputOf producingDrv.outPath "out"} $out
    '';
  };
}
