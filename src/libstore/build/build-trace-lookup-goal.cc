#include "goal-impl.hh"

#include "nix/store/build/build-trace-lookup-goal.hh"
#include "nix/store/build/drv-output-substitution-goal.hh"
#include "nix/store/build/substitution-goal.hh"
#include "nix/store/build/worker.hh"
#include "nix/store/binary-cache-store.hh"
#include "nix/store/derivation/aterm.hh"
#include "nix/store/derivation/resolution.hh"
#include "nix/store/local-store.hh"
#include "nix/store/nar-info.hh"
#include "nix/util/callback.hh"
#include "nix/util/compression.hh"
#include "nix/util/nar-accessor.hh"

namespace nix {

BuildTraceLookupGoal::BuildTraceLookupGoal(
    const SingleDerivedPath::Built & req, std::shared_ptr<const Derivation> drv, Worker & worker)
    : Goal(worker, init(std::move(drv)))
    , req(req)
{
    name = fmt("looking up the build trace of '%s'", req.to_string(worker.store));
    trace("created");
}

std::string BuildTraceLookupGoal::key()
{
    return "ab$" + std::string(req.getBaseStorePath().name()) + "$" + req.to_string(worker.store);
}

Goal::Co<Goal::ExitCode> BuildTraceLookupGoal::init(std::shared_ptr<const Derivation> knownDrv)
{
    auto drvPath = co_await obtainDrvPath();
    if (!drvPath)
        co_return ecNoSubstituters;
    concreteDrvPath = *drvPath;

    /* A build trace entry's output stands without the derivation, which a
       planned build drops once built and whose unit derivations run to
       megabytes. Parsing one for every lookup kept the worker's goal thread
       too busy to start builds. Impure derivations get no entry. */
    if (!knownDrv)
        if (auto local = queryLocalTrace(*drvPath)) {
            traceKey = local->id;
            outputInfo = std::make_shared<const UnkeyedRealisation>(std::move(*local));
            co_return ecSuccess;
        }

    drv = knownDrv ? std::move(knownDrv) : readLocalDerivation(*drvPath);

    if (!drv)
        drv = co_await readDerivation(*drvPath);

    if (!drv || type(*drv).isImpure() || !drv->outputs.contains(req.output))
        co_return ecNoSubstituters;

    if (auto local = queryLocalRecord(*drvPath, *drv)) {
        traceKey = local->id;
        outputInfo = std::make_shared<const UnkeyedRealisation>(std::move(*local));
        co_return ecSuccess;
    }

    if (worker.buildTraceMisses.contains(req))
        co_return ecNoSubstituters;

    auto result = co_await queryRemote(*drvPath, *drv);
    if (result != ecSuccess)
        worker.buildTraceMisses.insert(req);
    co_return result;
}

Goal::Co<std::optional<StorePath>> BuildTraceLookupGoal::obtainDrvPath()
{
    auto * producer = std::get_if<SingleDerivedPath::Built>(&req.drvPath->raw());
    if (!producer)
        co_return std::get<SingleDerivedPath::Opaque>(req.drvPath->raw()).path;

    producerGoal = worker.makeBuildTraceLookupGoal(*producer);
    co_await await({producerGoal});
    if (nrFailed != 0)
        co_return std::nullopt;

    co_return producerGoal->outputInfo->outPath;
}

std::shared_ptr<const Derivation> BuildTraceLookupGoal::readLocalDerivation(const StorePath & drvPath)
{
    for (auto * drvStore : {&worker.evalStore, &worker.store})
        if (drvStore->isValidPath(drvPath))
            return std::make_shared<const Derivation>(drvStore->readDerivation(drvPath));
    return nullptr;
}

Goal::Co<std::shared_ptr<const Derivation>> BuildTraceLookupGoal::readDerivation(const StorePath & drvPath)
{
    /* The unresolved key needs only the path, so it is asked for during
       the narinfo and NAR round trips below. */
    if (!worker.buildTraceMisses.contains(req))
        unresolvedGoal = worker.makeDrvOutputSubstitutionGoal(DrvOutput{drvPath, req.output});

    for (const auto & sub : worker.getSubstituters()) {
        auto * cache = dynamic_cast<BinaryCacheStore *>(&*sub);
        if (!cache)
            continue;

        try {
            auto info = co_await AsyncCallback<ref<const ValidPathInfo>>(
                [sub, drvPath](Callback<ref<const ValidPathInfo>> callback) {
                    sub->queryPathInfo(drvPath, std::move(callback));
                });
            auto narInfo = std::dynamic_pointer_cast<const NarInfo>(info.get_ptr());
            if (!narInfo)
                continue;

            auto nar = co_await AsyncCallback<std::optional<std::string>>(
                [sub, cache, narInfo](Callback<std::optional<std::string>> callback) {
                    cache->getFile(narInfo->url, std::move(callback));
                });
            if (!nar)
                continue;

            if (narInfo->compression)
                nar = decompress(*narInfo->compression, *nar);
            auto drv = derivation::parse(
                worker.store,
                makeNarAccessor(std::move(*nar))->readFile(CanonPath::root),
                Derivation::nameFromPath(drvPath));
            if (computeStorePath(worker.store, drv) == drvPath)
                co_return std::make_shared<const Derivation>(std::move(drv));
        } catch (Error & e) {
            debug("cannot read '%s' from '%s': %s", worker.store.printStorePath(drvPath), sub->config.getHumanReadableURI(), e.msg());
        }
    }

    co_return nullptr;
}

std::optional<Realisation> BuildTraceLookupGoal::queryLocalTrace(const StorePath & drvPath)
{
    DrvOutput id{drvPath, req.output};

    for (auto * drvStore : {&worker.evalStore, &worker.store})
        if (auto real = drvStore->queryRealisation(id))
            return Realisation{*real, id};

    if (auto * localStore = dynamic_cast<LocalStore *>(&worker.store))
        if (auto resolution = localStore->querySettledResolution(drvPath)) {
            DrvOutput resolvedId{resolution->path, req.output};
            if (auto real = localStore->queryRealisation(resolvedId))
                return Realisation{*real, resolvedId};
        }

    return std::nullopt;
}

std::optional<Realisation> BuildTraceLookupGoal::queryLocalRecord(const StorePath & drvPath, const Derivation & drv)
{
    if (auto local = queryLocalTrace(drvPath))
        return local;

    DrvOutput id{drvPath, req.output};
    if (auto path = drv.outputs.at(req.output).path(worker.store, drv.name, req.output))
        return Realisation{{.outPath = std::move(*path)}, id};

    return std::nullopt;
}

bool BuildTraceLookupGoal::hasLocalRecordsBelow(const Derivation & drv)
{
    if (!dynamic_cast<LocalStore *>(&worker.store))
        return false;

    StorePathSet closure;
    for (auto & input : drv.inputs)
        if (auto * built = std::get_if<SingleDerivedPath::Built>(&input.raw())) {
            closure.insert(built->getBaseStorePath());
            for (auto * drvStore : {&worker.evalStore, &worker.store})
                if (drvStore->isValidPath(built->getBaseStorePath())) {
                    drvStore->computeFSClosure(built->getBaseStorePath(), closure);
                    break;
                }
        }

    std::erase_if(closure, [](const StorePath & path) { return !path.isDerivation(); });
    return worker.anyRecorded(closure);
}

Goal::Co<Goal::ExitCode> BuildTraceLookupGoal::queryRemote(const StorePath & drvPath, const Derivation & drv)
{
    DrvOutput id{drvPath, req.output};

    bool resolve = shouldResolve(drv);

    auto inputsRecorded = [&]() {
        StorePathSet bases;
        for (auto & input : drv.inputs)
            if (auto * built = std::get_if<SingleDerivedPath::Built>(&input.raw()))
                bases.insert(built->getBaseStorePath());
        return worker.anyRecorded(bases);
    };

    bool askUnresolved = !resolve || !inputsRecorded();
    if (askUnresolved && !unresolvedGoal)
        unresolvedGoal = worker.makeDrvOutputSubstitutionGoal(id);

    /* The inputs are walked while the unresolved key is in flight, but
       only once it has been out longer than a hit takes (0.23 s through
       ncro), since every input below a hit would be asked for in vain. A
       local record of an input already rules out taking a hit, so the key
       is then not asked for. A goal that finishes first has no waiters
       left to wake, so only unfinished ones are awaited. */
    if (askUnresolved && resolve && !unresolvedGoal->exitCode)
        co_await awaitUntil(unresolvedGoal, std::chrono::steady_clock::now() + std::chrono::milliseconds(300));

    std::map<SingleDerivedPath::Built, std::shared_ptr<BuildTraceLookupGoal>> inputGoals;
    if (resolve && !(askUnresolved && unresolvedGoal->exitCode == ecSuccess && !hasLocalRecordsBelow(drv)))
        for (auto & input : drv.inputs)
            if (auto * built = std::get_if<SingleDerivedPath::Built>(&input.raw()))
                inputGoals.insert_or_assign(*built, worker.makeBuildTraceLookupGoal(*built));

    if (askUnresolved && !unresolvedGoal->exitCode)
        co_await await({unresolvedGoal});

    if (askUnresolved && unresolvedGoal->exitCode == ecSuccess && (!resolve || !hasLocalRecordsBelow(drv))) {
        substituted = true;
        traceKey = id;
        outputInfo = unresolvedGoal->outputInfo;
        co_return ecSuccess;
    }

    if (!resolve)
        co_return ecNoSubstituters;

    Goals waitees;
    for (auto & [_, goal] : inputGoals)
        if (!goal->exitCode)
            waitees.insert(goal);

    co_await await(std::move(waitees));
    nrFailed = nrNoSubstituters = 0;
    for (auto & [_, goal] : inputGoals)
        if (goal->exitCode != ecSuccess)
            co_return ecNoSubstituters;

    auto resolved = tryResolve(
        drv,
        worker.store,
        [&](ref<const SingleDerivedPath> inputDrv, const std::string & outputName) -> std::optional<StorePath> {
            auto goal = get(inputGoals, SingleDerivedPath::Built{inputDrv, outputName});
            if (!goal || !(*goal)->outputInfo)
                return std::nullopt;
            return (*goal)->outputInfo->outPath;
        });
    if (!resolved)
        co_return ecNoSubstituters;

    DrvOutput resolvedId{computeStorePath(worker.store, unresolve(*resolved)), req.output};

    auto resolvedGoal = worker.makeDrvOutputSubstitutionGoal(resolvedId);
    co_await await({resolvedGoal});
    if (nrFailed != 0)
        co_return ecNoSubstituters;

    trace(fmt("resolved to '%s'", resolvedId.render(worker.store)));

    substituted = true;
    traceKey = resolvedId;
    outputInfo = resolvedGoal->outputInfo;
    resolvedDrv = std::make_shared<const BasicDerivation>(std::move(*resolved));
    co_return ecSuccess;
}

void BuildTraceLookupGoal::registerBuildTrace()
{
    if (producerGoal)
        producerGoal->registerBuildTrace();

    if (!substituted)
        return;

    auto & key = *traceKey;

    if (!worker.store.queryRealisation(key))
        worker.store.registerDrvOutput({*outputInfo, key}, CheckSigs);

    /* Not a build trace entry for the unresolved key, since a store serving
       its build trace would hand that to clients that cannot check it
       against their own records. */
    if (auto * localStore = dynamic_cast<LocalStore *>(&worker.store); localStore && resolvedDrv) {
        SettledResolution settled{key.drvPath, {}};
        for (auto & [outputName, _] : resolvedDrv->outputs)
            settled.outputs.push_back(outputName);
        localStore->registerSettledResolution(*concreteDrvPath, settled);
    }
}

} // namespace nix
