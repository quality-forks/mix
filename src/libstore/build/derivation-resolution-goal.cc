#include "goal-impl.hh"

#include "nix/store/build/derivation-resolution-goal.hh"
#include "nix/store/build/worker.hh"
#include "nix/util/callback.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"
#include "nix/store/derivation/resolution.hh"

#include <nlohmann/json.hpp>

#include <array>
#include <thread>

namespace nix {

DerivationResolutionGoal::DerivationResolutionGoal(
    const StorePath & drvPath, ref<const Derivation> drv, Worker & worker, BuildMode buildMode)
    : Goal(worker, init(std::move(drv), buildMode))
    , drvPath(drvPath)
{
    name = fmt("resolving derivation '%s'", worker.store.printStorePath(drvPath));
    trace("created");
}

std::string DerivationResolutionGoal::key()
{
    return "dc$" + std::string(drvPath.name()) + "$" + worker.store.printStorePath(drvPath);
}

Goal::Co<DerivationResolutionGoal::InputsGoalMap>
DerivationResolutionGoal::realiseInputs(const Derivation & drv, BuildMode buildMode)
{
    Goals waitees;
    InputsGoalMap inputGoals;

    /* Ensure that pure, non-fixed-output derivations don't depend on
       impure derivations. Only worth checking each input derivation
       once, however many of its outputs we depend on. */
    bool checkImpureInputs =
        experimentalFeatureSettings.isEnabled(Xp::ImpureDerivations) && !type(drv).isImpure() && !type(drv).isFixed();

    StorePathSet checkedInputDrvs;

    for (const auto & input : drv.inputs) {
        std::visit(
            overloaded{
                [&](const SingleDerivedPath::Opaque &) {
                    /* Source paths don't need to be waited on */
                },
                [&](const SingleDerivedPath::Built & built) {
                    /* For a dynamic derivation input, this checks the
                       root derivation the chain is ultimately built
                       from. */
                    auto & inputDrvPath = built.drvPath->getBaseStorePath();
                    if (checkImpureInputs && checkedInputDrvs.insert(inputDrvPath).second) {
                        auto inputDrv = worker.evalStore.readDerivation(inputDrvPath);
                        if (type(inputDrv).isImpure())
                            throw Error(
                                "pure derivation '%s' depends on impure derivation '%s'",
                                worker.store.printStorePath(drvPath),
                                worker.store.printStorePath(inputDrvPath));
                    }

                    auto inputDrvRef = make_ref<SingleDerivedPath>(input);
                    auto g = worker.makeGoal(
                        DerivedPath::Built{
                            .drvPath = built.drvPath,
                            .outputs = OutputsSpec::Names{built.output},
                        },
                        buildMode == bmRepair ? bmRepair : bmNormal);
                    inputGoals.insert_or_assign(inputDrvRef, g);
                    waitees.insert(std::move(g));
                }},
            input.raw());
    }

    co_await await(std::move(waitees));

    trace("all inputs realised");

    co_return inputGoals;
}

Goal::Co<decltype(DerivationResolutionGoal::resolvedDrv)>
DerivationResolutionGoal::resolveDerivation(const Derivation & drv, const InputsGoalMap & inputGoals)
{
    experimentalFeatureSettings.require(Xp::CaDerivations);

    /* We are be able to resolve this derivation based on the
       now-known results of dependencies. If so, we become a
       stub goal aliasing that resolved derivation goal. */

    /* Resolving rewrites the derivation's structured attrs and hashes the
       result, which for a unit derivation held the goal thread for a fifth
       of a units build, so it runs on a thread from what the inputs built. */
    std::map<SingleDerivedPath::Built, StorePath> built;
    for (auto & [input, goal] : inputGoals) {
        auto * edge = std::get_if<SingleDerivedPath::Built>(&input->raw());
        auto * success = goal->buildResult.tryGetSuccess();
        if (!edge || !success)
            continue;
        if (auto * output = get(success->builtOutputs, edge->output))
            built.insert_or_assign(*edge, output->outPath);
    }

    std::jthread resolving;
    auto resolved = co_await AsyncCallback<std::shared_ptr<std::pair<StorePath, BasicDerivation>>>{
        [&](Callback<std::shared_ptr<std::pair<StorePath, BasicDerivation>>> callback) {
            resolving = std::jthread([callback = std::move(callback),
                                      &drv,
                                      &built,
                                      &store = worker.store,
                                      &evalStore = worker.evalStore]() mutable {
                try {
                    ReceiveInterrupts receiveInterrupts;
                    auto attempt = tryResolve(
                        drv,
                        store,
                        [&](ref<const SingleDerivedPath> inputDrvPath,
                            const std::string & outputName) -> std::optional<StorePath> {
                            if (auto * path = get(built, SingleDerivedPath::Built{inputDrvPath, outputName}))
                                return *path;
                            return std::nullopt;
                        });

                    /* TODO (impure derivations-induced tech debt) (see below):
                       The above attempt should have found it, but because we manage
                       inputDrvOutputs statefully, sometimes it gets out of sync with
                       the real source of truth (store). So we query the store
                       directly if there's a problem. */
                    if (!attempt)
                        attempt = tryResolve(drv, store, &evalStore);
                    assert(attempt);

                    auto path = computeStorePath(store, unresolve(*attempt));
                    callback(std::make_shared<std::pair<StorePath, BasicDerivation>>(
                        std::move(path), std::move(*attempt)));
                } catch (...) {
                    callback.rethrow();
                }
            });
        }};
    auto & pathResolved = resolved->first;

    Activity act(
        *logger,
        lvlInfo,
        actBuildWaiting, /* TODO: Is this the right type of activity? */
        fmt("resolved derivation: '%s' -> '%s'",
            worker.store.printStorePath(drvPath),
            worker.store.printStorePath(pathResolved)),
        std::to_array<Logger::Field>({
            worker.store.printStorePath(drvPath),
            worker.store.printStorePath(pathResolved),
        }));

    co_return std::make_unique<std::pair<StorePath, BasicDerivation>>(std::move(*resolved));
}

Goal::Co<Goal::ExitCode> DerivationResolutionGoal::init(ref<const Derivation> drv, BuildMode buildMode)
{
    buildResult = co_await resolve(std::move(drv), buildMode);
    co_return buildResult.tryGetSuccess() ? ecSuccess : ecFailed;
}

Goal::Co<BuildResult> DerivationResolutionGoal::resolve(ref<const Derivation> drv, BuildMode buildMode)
{
    auto inputGoals = co_await realiseInputs(*drv, buildMode);

    if (nrFailed != 0) {
        auto msg =
            fmt("Cannot build '%s'.\n"
                "Reason: " ANSI_RED "%d %s failed" ANSI_NORMAL ".",
                Magenta(worker.store.printStorePath(drvPath)),
                nrFailed,
                nrFailed == 1 ? "dependency" : "dependencies");
        msg += showKnownOutputs(worker.store, *drv);
        co_return BuildResult{
            .inner = BuildResult::Failure{{
                .status = BuildResult::Failure::DependencyFailed,
                .msg = HintFmt(msg),
            }},
        };
    }

    if (shouldResolve(*drv))
        resolvedDrv = co_await resolveDerivation(*drv, inputGoals);
    co_return BuildResult{.inner = BuildResult::Success{.status = BuildResult::Success::AlreadyValid}};
}

} // namespace nix
