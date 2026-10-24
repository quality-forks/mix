#pragma once
///@file

#include "nix/store/derivations.hh"
#include "nix/store/derived-path.hh"
#include "nix/store/build/goal.hh"
#include "nix/store/realisation.hh"

namespace nix {

/**
 * Find the build trace entry of one derivation output without making any
 * output path valid, so a floating content-addressed derivation can be
 * substituted by its resolved key alone.
 *
 * Local records always win. A substituter's entry for the unresolved key
 * is taken only if no derivation below has a local record, otherwise every
 * input is looked up the same way and the resolved key is asked for.
 */
struct BuildTraceLookupGoal : public Goal
{
    friend class Worker;

    BuildTraceLookupGoal(const SingleDerivedPath::Built & req, std::shared_ptr<const Derivation> drv, Worker & worker);

    std::shared_ptr<const UnkeyedRealisation> outputInfo;

    /**
     * The key `outputInfo` was found under, the resolved key if the
     * derivation was resolved here or by a settled resolution.
     */
    std::optional<DrvOutput> traceKey;

    /**
     * The derivation resolved from the inputs' build trace entries, if
     * `outputInfo` was found by resolving.
     */
    std::shared_ptr<const BasicDerivation> resolvedDrv;

    /**
     * Record how the wanted output and the derivations producing it were
     * found, after its output path was substituted.
     */
    void registerBuildTrace();

private:

    /**
     * The derivation `req` names once its producers are looked up.
     */
    std::optional<StorePath> concreteDrvPath;

    std::shared_ptr<BuildTraceLookupGoal> producerGoal;

    bool substituted = false;

    SingleDerivedPath::Built req;

    Co<ExitCode> init(std::shared_ptr<const Derivation> drv);
    Co<std::optional<StorePath>> obtainDrvPath();
    Co<std::shared_ptr<const Derivation>> readDerivation(const StorePath & drvPath);
    std::optional<Realisation> queryLocalRecord(const StorePath & drvPath, const Derivation & drv);
    Co<ExitCode> queryRemote(const StorePath & drvPath, const Derivation & drv);

    /**
     * Whether the local store has a build trace entry or settled resolution
     * for any derivation below this one, whose inputs a substituter's entry
     * for the unresolved key may disagree with.
     */
    bool hasLocalRecordsBelow(const Derivation & drv);

    std::string key() override;

    JobCategory jobCategory() const override
    {
        return JobCategory::Administration;
    };
};

} // namespace nix
