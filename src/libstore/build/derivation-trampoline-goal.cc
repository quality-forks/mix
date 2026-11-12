#include "nix/store/build/derivation-trampoline-goal.hh"
#include "nix/store/build/build-trace-lookup-goal.hh"
#include "nix/store/build/substitution-goal.hh"
#include "nix/store/build/worker.hh"
#include "nix/store/derivations.hh"
#include "nix/store/worker-settings.hh"

#include <ranges>
#include <algorithm>

namespace nix {

DerivationTrampolineGoal::DerivationTrampolineGoal(
    ref<const SingleDerivedPath> drvReq, const OutputsSpec & wantedOutputs, Worker & worker, BuildMode buildMode)
    : Goal(worker, init(haveToLoadFromStore()))
    , drvReq(drvReq)
    , wantedOutputs(wantedOutputs)
    , buildMode(buildMode)
{
    commonInit();
}

DerivationTrampolineGoal::DerivationTrampolineGoal(
    const StorePath & drvPath,
    const OutputsSpec & wantedOutputs,
    ref<const Derivation> drv,
    Worker & worker,
    BuildMode buildMode)
    : Goal(worker, init(haveDerivation(drvPath, std::move(drv))))
    , drvReq(makeConstantStorePathRef(drvPath))
    , wantedOutputs(wantedOutputs)
    , buildMode(buildMode)
{
    commonInit();
}

void DerivationTrampolineGoal::commonInit()
{
    name =
        fmt("obtaining derivation from '%s' and then building outputs %s",
            drvReq->to_string(worker.store),
            std::visit(
                overloaded{
                    [&](const OutputsSpec::All) -> std::string { return "* (all of them)"; },
                    [&](const OutputsSpec::Names os) { return concatStringsSep(", ", quoteStrings(os)); },
                },
                wantedOutputs.raw));
    trace("created outer");

    worker.updateProgress();
}

DerivationTrampolineGoal::~DerivationTrampolineGoal() {}

std::string DerivationTrampolineGoal::key()
{
    auto pathPartOfReq = [](this const auto & self, const SingleDerivedPath & req) -> StorePath {
        return std::visit(
            overloaded{
                [&](const SingleDerivedPath::Opaque & bo) { return bo.path; },
                [&](const SingleDerivedPath::Built & bfd) { return self(*bfd.drvPath); },
            },
            req.raw());
    };

    return "da$" + std::string(pathPartOfReq(*drvReq).name()) + "$" + DerivedPath::Built{
        .drvPath = drvReq,
        .outputs = wantedOutputs,
    }.to_string(worker.store);
}

Goal::Co<Goal::ExitCode> DerivationTrampolineGoal::init(Co<Result> work)
{
    auto result = co_await std::move(work);
    buildResult = std::move(result.second);
    co_return result.first;
}

Goal::Co<DerivationTrampolineGoal::Result> DerivationTrampolineGoal::haveToLoadFromStore()
{
    std::optional<LoadedDerivation> lookedUp, producer;
    if (auto substituted = co_await substituteFromBuildTrace(lookedUp, producer))
        co_return std::move(*substituted);

    if (co_await realiseProducerInputs(producer)) {
        lookedUp.reset();
        producer.reset();
        if (auto substituted = co_await substituteFromBuildTrace(lookedUp, producer))
            co_return std::move(*substituted);
    }

    auto loadResult = co_await loadDerivation(std::move(lookedUp), std::move(producer));
    if (auto * failure = std::get_if<BuildResult::Failure>(&loadResult))
        co_return Result{ecFailed, BuildResult{.inner = std::move(*failure)}};

    auto & [drvPath, drv] = std::get<LoadedDerivation>(loadResult);
    co_return co_await haveDerivation(std::move(drvPath), std::move(drv));
}

/* Obtaining a dynamic derivation makes it valid along with every path it
   references, which for a planned build are all its tools and objects, so
   outputs found in the build trace are substituted without it. */
Goal::Co<std::optional<DerivationTrampolineGoal::Result>> DerivationTrampolineGoal::substituteFromBuildTrace(
    std::optional<LoadedDerivation> & lookedUp, std::optional<LoadedDerivation> & producer)
{
    auto * outputNames = std::get_if<OutputsSpec::Names>(&wantedOutputs.raw);
    if (buildMode != bmNormal || !outputNames || !std::holds_alternative<SingleDerivedPath::Built>(drvReq->raw())
        || !experimentalFeatureSettings.isEnabled(Xp::CaDerivations))
        co_return std::nullopt;

    std::map<std::string, std::shared_ptr<BuildTraceLookupGoal>> lookups;
    Goals waitees;
    for (auto & output : *outputNames) {
        auto lookup = worker.makeBuildTraceLookupGoal(SingleDerivedPath::Built{drvReq, output});
        waitees.insert(lookup);
        lookups.insert_or_assign(output, std::move(lookup));
    }

    co_await await(std::move(waitees));

    for (auto & [_, lookup] : lookups) {
        if (!lookedUp && lookup->concreteDrvPath && lookup->drv)
            lookedUp.emplace(*lookup->concreteDrvPath, ref<const Derivation>(lookup->drv));
        if (auto & above = lookup->producerGoal; !producer && above && above->concreteDrvPath && above->drv)
            producer.emplace(*above->concreteDrvPath, ref<const Derivation>(above->drv));
    }

    Goals substitutions;
    if (nrFailed == 0)
        for (auto & [_, lookup] : lookups)
            if (!worker.store.isValidPath(lookup->outputInfo->outPath))
                substitutions.insert(upcast_goal(worker.makePathSubstitutionGoal(lookup->outputInfo->outPath)));

    bool substituted = !substitutions.empty();
    co_await await(std::move(substitutions));

    if (nrFailed != 0) {
        nrFailed = nrNoSubstituters = 0;
        co_return std::nullopt;
    }

    SingleDrvOutputs outputs;
    for (auto & [output, lookup] : lookups) {
        lookup->registerBuildTrace();
        if (lookup->outputInfo->outPath.isDerivation())
            worker.store.addTempRoot(lookup->outputInfo->outPath);
        outputs.insert_or_assign(output, *lookup->outputInfo);
    }

    co_return Result{
        ecSuccess,
        BuildResult{
            .inner =
                BuildResult::Success{
                    .status = substituted ? BuildResult::Success::Substituted : BuildResult::Success::AlreadyValid,
                    .builtOutputs = std::move(outputs),
                },
        },
    };
}

/* A producer whose inputs were rebuilt into the same outputs resolves as
   before, but the lookup finds that only once the inputs have build trace
   entries of their own. Realising them first lets the producer's entry stand
   for a produced derivation that was evicted, which obtaining it would
   otherwise write again by running the producer. */
Goal::Co<bool> DerivationTrampolineGoal::realiseProducerInputs(const std::optional<LoadedDerivation> & producer)
{
    auto * built = std::get_if<SingleDerivedPath::Built>(&drvReq->raw());
    auto * base = built ? std::get_if<SingleDerivedPath::Opaque>(&built->drvPath->raw()) : nullptr;
    if (buildMode != bmNormal || !base || !producer || producer->first != base->path)
        co_return false;

    try {
        auto produced = resolveDerivedPath(worker.store, *drvReq);
        if (worker.evalStore.isValidPath(produced) || worker.store.isValidPath(produced))
            co_return false;
    } catch (MissingRealisation &) {
    }

    Goals waitees;
    for (auto & input : producer->second->inputs)
        if (std::holds_alternative<SingleDerivedPath::Built>(input.raw()))
            waitees.insert(worker.makeGoal(DerivedPath::fromSingle(input)));
    if (waitees.empty())
        co_return false;

    co_await await(std::move(waitees));
    if (nrFailed != 0) {
        nrFailed = nrNoSubstituters = 0;
        co_return false;
    }

    worker.buildTraceMisses.erase(*built);
    if (auto * outputNames = std::get_if<OutputsSpec::Names>(&wantedOutputs.raw))
        for (auto & output : *outputNames)
            worker.buildTraceMisses.erase(SingleDerivedPath::Built{drvReq, output});
    co_return true;
}

/* The lookup that found no build trace entry already parsed the derivation
   whose output is the one wanted, which saves parsing it again to build it. */
GoalPtr DerivationTrampolineGoal::obtainGoal(std::optional<LoadedDerivation> producer)
{
    auto * built = std::get_if<SingleDerivedPath::Built>(&drvReq->raw());
    auto * base = built ? std::get_if<SingleDerivedPath::Opaque>(&built->drvPath->raw()) : nullptr;
    if (!producer || !base || base->path != producer->first
        || !(worker.evalStore.isValidPath(base->path) || worker.store.isValidPath(base->path)))
        return worker.makeGoal(DerivedPath::fromSingle(*drvReq));

    worker.evalStore.addTempRoot(base->path);
    return worker.makeDerivationTrampolineGoal(
        base->path, OutputsSpec::Names{built->output}, std::move(producer->second), bmNormal);
}

Goal::Co<DerivationTrampolineGoal::LoadResult> DerivationTrampolineGoal::loadDerivation(
    std::optional<LoadedDerivation> lookedUp, std::optional<LoadedDerivation> producer)
{
    trace("need to load derivation from file");

    /* The first thing to do is to make sure that the derivation
       exists.  If it doesn't, it may be built from another derivation,
       or merely substituted. We can make goal to get it and not worry
       about which method it takes to get the derivation. */
    if (auto optDrvPath = [this]() -> std::optional<StorePath> {
            if (buildMode != bmNormal)
                return std::nullopt;

            auto drvPath = StorePath::dummy;
            try {
                drvPath = resolveDerivedPath(worker.store, *drvReq);
            } catch (MissingRealisation &) {
                return std::nullopt;
            }
            auto cond = worker.evalStore.isValidPath(drvPath) || worker.store.isValidPath(drvPath);
            return cond ? std::optional{drvPath} : std::nullopt;
        }()) {
        trace(
            fmt("already have drv '%s' for '%s', can go straight to building",
                worker.store.printStorePath(*optDrvPath),
                drvReq->to_string(worker.store)));
    } else {
        trace("need to obtain drv we want to build");
        Goals waitees{obtainGoal(std::move(producer))};
        co_await await(std::move(waitees));
    }

    trace("outer load and build derivation");

    if (nrFailed != 0) {
        co_return BuildResult::Failure{{
            .status = BuildResult::Failure::DependencyFailed,
            .msg = HintFmt("failed to obtain derivation of '%s'", drvReq->to_string(worker.store)),
        }};
    }

    StorePath drvPath = resolveDerivedPath(worker.store, *drvReq);

    /* `drvPath' should already be a root, but let's be on the safe
       side: if the user forgot to make it a root, we wouldn't want
       things being garbage collected while we're busy. */
    worker.evalStore.addTempRoot(drvPath);

    if (lookedUp && lookedUp->first == drvPath)
        co_return std::move(*lookedUp);

    auto haveDrv = [&] { return worker.evalStore.isValidPath(drvPath) || worker.store.isValidPath(drvPath); };

    /* Eviction drops a transient derivation unused for ten minutes, which
       can land between obtaining it above and rooting it, so a built one is
       obtained once more now that it is rooted. */
    if (!haveDrv() && std::holds_alternative<SingleDerivedPath::Built>(drvReq->raw())) {
        Goals waitees{obtainGoal(std::nullopt)};
        co_await await(std::move(waitees));
        if (nrFailed == 0) {
            drvPath = resolveDerivedPath(worker.store, *drvReq);
            worker.evalStore.addTempRoot(drvPath);
        }
    }

    if (nrFailed != 0 || !haveDrv())
        co_return BuildResult::Failure{{
            .status = BuildResult::Failure::DependencyFailed,
            .msg = HintFmt("failed to obtain derivation of '%s'", drvReq->to_string(worker.store)),
        }};

    /* Get the derivation. It is probably in the eval store, but it might be in the main store:

         - Resolved derivation are resolved against main store realisations, and so must be stored there.

         - Dynamic derivations are built, and so are found in the main store.
     */
    auto drv = [&] {
        for (auto * drvStore : {&worker.evalStore, &worker.store})
            if (drvStore->isValidPath(drvPath))
                return make_ref<const Derivation>(drvStore->readDerivation(drvPath));
        assert(false);
    }();

    co_return std::pair{std::move(drvPath), std::move(drv)};
}

Goal::Co<DerivationTrampolineGoal::Result>
DerivationTrampolineGoal::haveDerivation(StorePath drvPath, ref<const Derivation> drv)
{
    trace("have derivation, will kick off derivations goals per wanted output");

    auto resolvedWantedOutputs = std::visit(
        overloaded{
            [&](const OutputsSpec::Names & names) -> OutputsSpec::Names { return names; },
            [&](const OutputsSpec::All &) -> OutputsSpec::Names {
                StringSet outputs;
                for (auto & [outputName, _] : drv->outputs)
                    outputs.insert(outputName);
                return outputs;
            },
        },
        wantedOutputs.raw);

    /* Must have at least one wanted output. This is assumed below. */
    assert(!resolvedWantedOutputs.empty());

    Goals concreteDrvGoals;

    /* Build this step! */

    for (auto & output : resolvedWantedOutputs) {
        auto g = upcast_goal(worker.makeDerivationGoal(drvPath, drv, output, buildMode));
        g->preserveFailure = true;
        /* We will finish with it ourselves, as if we were the derivational goal. */
        concreteDrvGoals.insert(std::move(g));
    }

    co_await await(concreteDrvGoals);

    trace("outer build done");

    if (nrFailed != 0) {
        auto gi = std::ranges::find_if(concreteDrvGoals, [](const GoalPtr & goal) -> bool {
            auto exitCode = goal->exitCode;
            /* Note that without --keep-going waitees might be cancelled before
               we are woken up. */
            return exitCode && exitCode != ecSuccess;
        });

        const Goal * g = gi->get();
        assert(gi != concreteDrvGoals.end() && "expected a failing goal");
        auto exitCode = *g->exitCode;
        const auto * failure = g->buildResult.tryGetFailure();
        assert(failure && "failing goal does not report a failed build result");

        /* Report the exit status of *some* failing goal. This might not be strictly
           correct, since multiple subgoals can fail independently, but this should be
           a good enough heuristic without --keep-going. */
        co_return Result{exitCode, BuildResult{.inner = *failure}};
    }

    SingleDrvOutputs outputs;

    auto successes = std::views::transform(concreteDrvGoals, [](const GoalPtr & a) -> const BuildResult::Success & {
        auto * success = a->buildResult.tryGetSuccess();
        assert(success && "goal succeeded, but some waitees do not report a successful status");
        return *success;
    });

    for (const auto & success : successes)
        std::ranges::copy(success.builtOutputs, std::inserter(outputs, outputs.end()));

    auto statuses = successes | std::views::transform(&BuildResult::Success::status);

    /* Aggregate the status code. If some outputs we already valid, but we had
       to build/substitute the other ones, report it as the smallest common
       denominator. */
    auto compareSuccesses = [](auto a, auto b) {
        /* This is technically an identity mapping of the underlying values, but
           it would be worse to rely on the enum ordering here. */
        auto toPriority = [](auto st) {
            using enum BuildResult::Success::Status;
            switch (st) {
            case Built:
                return 0;
            case Substituted:
                return 1;
            case AlreadyValid:
                return 2;
            case ResolvesToAlreadyValid:
                return 3;
            default:
                unreachable();
            }
        };
        return toPriority(a) < toPriority(b);
    };

    co_return Result{
        ecSuccess,
        BuildResult{
            .inner =
                BuildResult::Success{
                    .status = std::ranges::min(statuses, compareSuccesses),
                    .builtOutputs = std::move(outputs),
                },
        },
    };
}

} // namespace nix
