#include "nix/store/outputs-query.hh"
#include "nix/store/derivations.hh"
#include "nix/store/derivation/resolution.hh"
#include "nix/store/realisation.hh"
#include "nix/store/local-store.hh"
#include "nix/store/remote-store.hh"
#include "nix/util/util.hh"
#include "nix/util/sync.hh"

#include <boost/unordered/unordered_flat_map.hpp>

namespace nix {

namespace {

/**
 * A derivation's resolved path and its output names, all a lookup needs
 * of the derivation once it is resolved.
 */
struct Resolved
{
    StorePath path;
    std::vector<std::string> outputs;
};

/**
 * Cache mapping an unresolved drv path to its resolution. Shared across a
 * single top-level resolution call to prevent exponential re-traversal of
 * the closure when many derivations share the same dependencies.
 */
using ResolveCache = boost::unordered_flat_map<StorePath, Resolved>;

/**
 * Cache mapping a resolved derivation output to its realisation output path.
 */
using RealisationCache = boost::unordered_flat_map<DrvOutput, std::optional<StorePath>>;

/**
 * Resolutions that found every input realised, per store. A realisation
 * never changes once recorded, so these hold for the life of the process,
 * while a worker resolves each dynamic derivation's whole input chain
 * again for every goal that loads it.
 */
Sync<boost::unordered_flat_map<std::string, ResolveCache>> settled;

struct Caches
{
    ResolveCache resolved = {};
    RealisationCache realisations = {};
    /**
     * The key into `settled`, absent when a caller's own realisation
     * query may answer differently.
     */
    std::optional<std::string> store;
};

/**
 * A daemon that resolves the chain itself, against the resolutions its
 * store keeps, rather than this process reading and rewriting every input.
 */
RemoteStore * resolvingDaemon(Store & store, Store * evalStore, const QueryRealisationFun & queryRealisation)
{
    auto * remote = dynamic_cast<RemoteStore *>(&store);
    if ((evalStore && evalStore != &store) || queryRealisation || !remote || !remote->resolvesDerivationOutputs())
        return nullptr;
    return remote;
}

std::optional<std::string> settledKey(Store & store, const QueryRealisationFun & queryRealisation)
{
    if (queryRealisation)
        return std::nullopt;
    return store.config.getReference().render(/*withParams=*/true);
}

/* Forward declaration so resolveSingleDerivedPath can call it. */
static std::optional<StorePath> deepQueryPartialDerivationOutputImpl(
    Store & store,
    const StorePath & drvPath,
    const std::string & outputName,
    Store * evalStore_,
    QueryRealisationFun & queryRealisation,
    Caches & caches);

static std::optional<StorePath> resolveDerivationPath(
    Store & store,
    const SingleDerivedPath & drvPath,
    Store * evalStore_,
    QueryRealisationFun & queryRealisation,
    Caches & caches);

/**
 * Resolve a `SingleDerivedPath` to a concrete store path.
 *
 * For `Opaque` paths this is trivial. For `Built` paths (dynamic
 * derivations), recursively resolve the inner path and query its
 * output.
 *
 * @param queryRealisation must already be initialized (not empty)
 */
static std::optional<StorePath> resolveSingleDerivedPath(
    Store & store,
    const SingleDerivedPath & path,
    Store * evalStore_,
    QueryRealisationFun & queryRealisation,
    Caches & caches)
{
    return std::visit(
        overloaded{
            [](const SingleDerivedPath::Opaque & opaque) -> std::optional<StorePath> { return opaque.path; },
            [&](const SingleDerivedPath::Built & built) -> std::optional<StorePath> {
                auto innerPath =
                    resolveDerivationPath(store, *built.drvPath, evalStore_, queryRealisation, caches);
                if (!innerPath)
                    return std::nullopt;
                return deepQueryPartialDerivationOutputImpl(
                    store, *innerPath, built.output, evalStore_, queryRealisation, caches);
            },
        },
        path.raw());
}

static std::optional<StorePath> resolveDerivationPath(
    Store & store,
    const SingleDerivedPath & drvPath,
    Store * evalStore_,
    QueryRealisationFun & queryRealisation,
    Caches & caches)
{
    auto resolved = resolveSingleDerivedPath(store, drvPath, evalStore_, queryRealisation, caches);
    auto & evalStore = evalStore_ ? *evalStore_ : store;
    if (resolved && std::holds_alternative<SingleDerivedPath::Built>(drvPath.raw()) && !evalStore.isValidPath(*resolved)
        && !store.isValidPath(*resolved))
        return std::nullopt;
    return resolved;
}

/**
 * Resolve a derivation and compute its store path, with memoization.
 *
 * Results are stored in `caches` (keyed on the unresolved `drvPath`) so that
 * each derivation in the closure is resolved at most once per top-level call,
 * preventing the exponential re-traversal that would otherwise occur for
 * content-addressed derivation closures.
 *
 * @param queryRealisation must already be initialized (not empty)
 */
static Resolved resolveDerivation(
    Store & store,
    const StorePath & drvPath,
    Store * evalStore_,
    QueryRealisationFun & queryRealisation,
    Caches & caches)
{
    if (auto it = caches.resolved.find(drvPath); it != caches.resolved.end())
        return it->second;

    auto * localStore = caches.store ? dynamic_cast<LocalStore *>(&store) : nullptr;
    if (caches.store) {
        auto settled_(settled.lock());
        if (auto known = settled_->find(*caches.store); known != settled_->end())
            if (auto it = known->second.find(drvPath); it != known->second.end())
                return caches.resolved.emplace(drvPath, it->second).first->second;
    }
    if (localStore)
        if (auto kept = localStore->querySettledResolution(drvPath)) {
            Resolved result{std::move(kept->path), std::move(kept->outputs)};
            (*settled.lock())[*caches.store].emplace(drvPath, result);
            return caches.resolved.emplace(drvPath, std::move(result)).first->second;
        }

    auto & evalStore = evalStore_ ? *evalStore_ : store;

    Derivation drv = evalStore.readInvalidDerivation(drvPath);
    bool complete = true;
    if (shouldResolve(drv)) {
        /* Without a custom queryRealisation, we could just use
           tryResolve(drv, store, &evalStore). But we need to use the
           callback variant to ensure all realisation queries go
           through queryRealisation. */
        auto resolvedDrv = tryResolve(
            drv,
            store,
            [&](ref<const SingleDerivedPath> depDrvPath,
                const std::string & depOutputName) -> std::optional<StorePath> {
                auto concreteDrvPath =
                    resolveDerivationPath(store, *depDrvPath, evalStore_, queryRealisation, caches);
                if (!concreteDrvPath)
                    return std::nullopt;
                return deepQueryPartialDerivationOutputImpl(
                    store, *concreteDrvPath, depOutputName, evalStore_, queryRealisation, caches);
            });
        if (resolvedDrv)
            drv = unresolve(*resolvedDrv);
        else
            complete = false;
    }

    Resolved result{computeStorePath(store, drv), {}};
    for (auto & [outputName, _] : drv.outputs)
        result.outputs.push_back(outputName);
    if (complete && caches.store) {
        (*settled.lock())[*caches.store].emplace(drvPath, result);
        if (localStore)
            localStore->registerSettledResolution(drvPath, {result.path, result.outputs});
    }
    return caches.resolved.emplace(drvPath, std::move(result)).first->second;
}

void queryPartialDerivationOutputMapCA(
    Store & store,
    const StorePath & drvPath,
    const std::vector<std::string> & outputNames,
    std::map<std::string, std::optional<StorePath>> & outputs,
    QueryRealisationFun queryRealisation,
    RealisationCache & resCache)
{
    if (!queryRealisation)
        queryRealisation = [&store](const DrvOutput & o) { return store.queryRealisation(o); };

    for (auto & outputName : outputNames) {
        DrvOutput id{drvPath, outputName};
        auto it = resCache.find(id);
        if (it != resCache.end()) {
            outputs.insert_or_assign(outputName, it->second);
            continue;
        }

        auto realisation = queryRealisation(id);
        std::optional<StorePath> outPath = realisation ? std::optional{realisation->outPath} : std::nullopt;
        resCache.emplace(id, outPath);

        if (outPath) {
            outputs.insert_or_assign(outputName, *outPath);
        } else {
            outputs.insert({outputName, std::nullopt});
        }
    }
}

/**
 * Internal implementation of deepQueryPartialDerivationOutput that accepts a
 * shared ResolveCache and RealisationCache, allowing memoization across recursive calls.
 */
static std::optional<StorePath> deepQueryPartialDerivationOutputImpl(
    Store & store,
    const StorePath & drvPath,
    const std::string & outputName,
    Store * evalStore_,
    QueryRealisationFun & queryRealisation,
    Caches & caches)
{
    auto & evalStore = evalStore_ ? *evalStore_ : store;

    auto staticResult = evalStore.queryStaticPartialDerivationOutput(drvPath, outputName);
    if (staticResult || !experimentalFeatureSettings.isEnabled(Xp::CaDerivations))
        return staticResult;

    auto resolved = resolveDerivation(store, drvPath, evalStore_, queryRealisation, caches);

    if (std::ranges::find(resolved.outputs, outputName) == resolved.outputs.end())
        throw Error("derivation '%s' does not have an output named '%s'", store.printStorePath(drvPath), outputName);

    DrvOutput id{resolved.path, outputName};
    auto it = caches.realisations.find(id);
    if (it != caches.realisations.end())
        return it->second;

    auto realisation = queryRealisation(id);
    std::optional<StorePath> outPath = realisation ? std::optional{realisation->outPath} : std::nullopt;
    caches.realisations.emplace(id, outPath);
    return outPath;
}

} // namespace

void queryPartialDerivationOutputMapCA(
    Store & store,
    const StorePath & drvPath,
    const Derivation & drv,
    std::map<std::string, std::optional<StorePath>> & outputs,
    QueryRealisationFun queryRealisation)
{
    std::vector<std::string> outputNames;
    for (auto & [outputName, _] : drv.outputs)
        outputNames.push_back(outputName);
    RealisationCache resCache;
    queryPartialDerivationOutputMapCA(store, drvPath, outputNames, outputs, queryRealisation, resCache);
}

std::map<std::string, std::optional<StorePath>> deepQueryPartialDerivationOutputMap(
    Store & store, const StorePath & drvPath, Store * evalStore_, QueryRealisationFun queryRealisation)
{
    if (auto * daemon = resolvingDaemon(store, evalStore_, queryRealisation))
        return daemon->queryPartialDerivationOutputMap(drvPath);

    auto & evalStore = evalStore_ ? *evalStore_ : store;

    Caches caches{.store = settledKey(store, queryRealisation)};
    if (!queryRealisation)
        queryRealisation = [&store](const DrvOutput & o) { return store.queryRealisation(o); };

    auto outputs = evalStore.queryStaticPartialDerivationOutputMap(drvPath);

    if (!experimentalFeatureSettings.isEnabled(Xp::CaDerivations))
        return outputs;

    auto resolved = resolveDerivation(store, drvPath, evalStore_, queryRealisation, caches);
    queryPartialDerivationOutputMapCA(
        store, resolved.path, resolved.outputs, outputs, queryRealisation, caches.realisations);

    return outputs;
}

OutputPathMap deepQueryDerivationOutputMap(
    Store & store, const StorePath & drvPath, Store * evalStore, QueryRealisationFun queryRealisation)
{
    auto resp = deepQueryPartialDerivationOutputMap(store, drvPath, evalStore, std::move(queryRealisation));
    OutputPathMap result;
    for (auto & [outName, optOutPath] : resp) {
        if (!optOutPath)
            throw MissingRealisation(store, drvPath, outName);
        result.insert_or_assign(outName, *optOutPath);
    }
    return result;
}

std::optional<StorePath> deepQueryPartialDerivationOutput(
    Store & store,
    const StorePath & drvPath,
    const std::string & outputName,
    Store * evalStore_,
    QueryRealisationFun queryRealisation)
{
    if (auto * daemon = resolvingDaemon(store, evalStore_, queryRealisation)) {
        auto outputs = daemon->queryPartialDerivationOutputMap(drvPath);
        auto output = outputs.find(outputName);
        if (output == outputs.end())
            throw Error("derivation '%s' does not have an output named '%s'", store.printStorePath(drvPath), outputName);
        return output->second;
    }

    Caches caches{.store = settledKey(store, queryRealisation)};
    if (!queryRealisation)
        queryRealisation = [&store](const DrvOutput & o) { return store.queryRealisation(o); };

    return deepQueryPartialDerivationOutputImpl(
        store, drvPath, outputName, evalStore_, queryRealisation, caches);
}

} // namespace nix
