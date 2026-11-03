with import ./config.nix;

rec {
  source = mkDerivation {
    name = "source";
    buildCommand = "echo source > $out";
  };

  object = mkDerivation {
    name = "object";
    __transient = true;
    buildCommand = "echo ${source} > $out";
  };

  collection = mkDerivation {
    name = "collection";
    __transient = true;
    buildCommand = "echo ${object} > $out";
  };

  # reads the collection without referring to it, as an assembly copies objects
  package = mkDerivation {
    name = "package";
    buildCommand = "wc -c < ${collection} > $out";
  };

  other = mkDerivation {
    name = "other";
    buildCommand = "touch $out";
  };
}
