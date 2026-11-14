#pragma once
///@file

#include "nix/store/store-api.hh"

namespace nix {

struct SubmitStore : public virtual Store
{
private:
    void anchor() override;

public:
    inline static std::string operationName = "Submit outputs for a currently running derivation";

    /**
     * Submit an output for the current derivation.
     * Only makes sense when running within a recursive-nix derivation
     */
    virtual void submitOutput(const SingleDerivedPath & path, const OutputName & output) = 0;

    /**
     * Add to store, scanning references.
     * Only within a recursive-nix derivation, as there would otherwise be no known
     * set of valid store paths
     */
    virtual ref<const ValidPathInfo> addToStoreScanning(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod = FileSerialisationMethod::NixArchive,
        ContentAddressMethod hashMethod = ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm hashAlgo = HashAlgorithm::SHA256) = 0;

    struct BuildFile
    {
        ValidPathInfo info;
        std::filesystem::path file;
        bool executable;
    };

    /**
     * Add each `info.path` from the regular file the builder sees at
     * `file`, which the store reads itself only when it lacks the path, so
     * the builder cannot tell which paths the store held.
     */
    virtual void addBuildFiles(const std::vector<BuildFile> & files) = 0;

    static SubmitStore & require(Store & store);
};

} // namespace nix
