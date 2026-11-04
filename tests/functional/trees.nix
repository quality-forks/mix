with import ./config.nix;

let
  data = builtins.toFile "data" "hello";
  blob = builtins.unsafeDiscardStringContext data;
  manifest = builtins.toFile "listed.tree" (
    builtins.toJSON {
      "a/b" = {
        file = {
          inherit data;
          size = 5;
        };
      };
      "c" = "empty";
      "x" = {
        executable = {
          inherit data;
          size = 5;
        };
      };
      "d" = {
        link = "a/b";
      };
    }
  );
  read =
    name:
    mkDerivation {
      inherit name;
      __trees = [ manifest ];
      buildCommand = ''
        [[ $(cat ${manifest}/a/b) == hello ]]
        [[ $(cat ${manifest}/d) == hello ]]
        [[ -f ${manifest}/c && ! -s ${manifest}/c ]]
        [[ -x ${manifest}/x && ! -x ${manifest}/a/b ]]
        [[ ! -e ${blob} ]]
        ! touch ${manifest}/e
        echo ${manifest} > $out
      '';
    };
in
{
  listed = read "listed";
  again = read "again";

  unreferenced = mkDerivation {
    name = "unreferenced";
    __trees = [
      (builtins.toFile "unreferenced.tree" (
        builtins.toJSON {
          a.file = {
            data = blob;
            size = 5;
          };
        }
      ))
    ];
    buildCommand = "touch $out";
  };
}
