with import ./config.nix;

let
  data = builtins.toFile "data" "hello";
  blob = builtins.unsafeDiscardStringContext data;
  manifest = builtins.toFile "listed.veneer" (
    builtins.toJSON {
      paths."a/b" = {
        file = {
          inherit data;
          size = 5;
        };
      };
      paths."c" = "empty";
      paths."x" = {
        executable = {
          inherit data;
          size = 5;
        };
      };
      paths."d" = {
        link = "a/b";
      };
    }
  );
  read =
    name:
    mkDerivation {
      inherit name;
      __veneers = [ manifest ];
      buildCommand = ''
        [[ $(cat ${manifest}/a/b) == hello ]]
        [[ $(cat ${manifest}/d) == hello ]]
        [[ -f ${manifest}/c && ! -s ${manifest}/c ]]
        [[ -x ${manifest}/x && ! -x ${manifest}/a/b ]]
        [[ ! -e ${blob} ]]
        if touch ${manifest}/e; then exit 1; fi
        echo ${manifest} > $out
      '';
    };
in
{
  listed = read "listed";
  again = read "again";

  writable = mkDerivation {
    name = "writable";
    __veneers = [
      manifest
      "source=${manifest}"
    ];
    buildCommand = ''
      [[ $(stat -c %u $NIX_BUILD_TOP/source/a/b) == $(id -u) ]]
      echo more >> $NIX_BUILD_TOP/source/a/b
      [[ $(cat $NIX_BUILD_TOP/source/a/b) == hellomore ]]
      [[ $(cat ${manifest}/a/b) == hello ]]
      echo new > $NIX_BUILD_TOP/source/e
      mkdir $NIX_BUILD_TOP/source/f
      touch $out
    '';
  };

  structured = mkDerivation {
    name = "structured";
    __structuredAttrs = true;
    __veneers = [
      {
        veneer = manifest;
        writableAt = "source";
      }
    ];
    buildCommand = ''
      echo more >> $NIX_BUILD_TOP/source/a/b
      [[ $(cat $NIX_BUILD_TOP/source/a/b) == hellomore ]]
      touch "''${outputs[out]}"
    '';
  };

  unreferenced = mkDerivation {
    name = "unreferenced";
    __veneers = [
      (builtins.toFile "unreferenced.veneer" (
        builtins.toJSON {
          paths.a.file = {
            data = blob;
            size = 5;
          };
        }
      ))
    ];
    buildCommand = "touch $out";
  };
}
