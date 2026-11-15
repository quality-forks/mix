#include "nix/store/restricted-store.hh"
#include "nix/store/build.hh"
#include "nix/util/finally.hh"
#include "nix/store/build-result.hh"
#include "nix/store/submit-store.hh"
#include "nix/store/build/worker.hh"
#include "nix/util/callback.hh"
#include "nix/store/realisation.hh"
#include "nix/store/local-store.hh"
#include "nix/util/error.hh"
#include "nix/util/repair-flag.hh"
#include "nix/util/archive.hh"
#include "nix/util/file-system-at.hh"
#include "nix/util/serialise.hh"

#include <fcntl.h>
#include <ranges>
#include <sys/stat.h>

#ifdef __linux__
#  include <linux/openat2.h>
#endif

namespace nix {

static StorePath pathPartOfReq(const SingleDerivedPath & req)
{
    return std::visit(
        overloaded{
            [&](const SingleDerivedPath::Opaque & bo) { return bo.path; },
            [&](const SingleDerivedPath::Built & bfd) { return pathPartOfReq(*bfd.drvPath); },
        },
        req.raw());
}

static StorePath pathPartOfReq(const DerivedPath & req)
{
    return std::visit(
        overloaded{
            [&](const DerivedPath::Opaque & bo) { return bo.path; },
            [&](const DerivedPath::Built & bfd) { return pathPartOfReq(*bfd.drvPath); },
        },
        req.raw());
}

void RestrictionContext::anchor() {}

bool RestrictionContext::isAllowed(const DerivedPath & req)
{
    return isAllowed(pathPartOfReq(req));
}

/**
 * A wrapper around LocalStore that only allows building/querying of
 * paths that are in the input closures of the build or were added via
 * recursive Nix calls.
 */
struct RestrictedStore : public virtual IndirectRootStore, public virtual GcStore, public virtual SubmitStore
{
private:
    void anchor() override;

public:
    ref<const LocalStore::Config> config;

    ref<LocalStore> next;

    RestrictionContext & goal;

    RestrictedStore(ref<LocalStore::Config> config, ref<LocalStore> next, RestrictionContext & goal)
        : Store{*config}
        , LocalFSStore{*config}
        , config{config}
        , next(next)
        , goal(goal)
    {
    }

    std::filesystem::path getRealStoreDir() override
    {
        return next->config->realStoreDir;
    }

    StorePathSet queryAllValidPaths() override;

    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

    void queryReferrers(const StorePath & path, StorePathSet & referrers) override;

    std::map<std::string, std::optional<StorePath>>
    queryPartialDerivationOutputMap(const StorePath & path, Store * evalStore = nullptr) override;

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
    {
        throw Error("queryPathFromHashPart");
    }

    StorePath addToStore(
        std::string_view name,
        const SourcePath & srcPath,
        ContentAddressMethod method,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        PathFilter & filter,
        RepairFlag repair) override
    {
        throw Error("addToStore");
    }

    void addToStore(
        const ValidPathInfo & info,
        Source & narSource,
        RepairFlag repair = NoRepair,
        CheckSigsFlag checkSigs = CheckSigs) override;

    void addInOneTransaction(const std::function<void()> & add) override;

    /**
     * The paths `addInOneTransaction` imports, which become the build's
     * dependencies once they are registered.
     */
    std::optional<StorePathSet> batched;

    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair) override;

    void narFromPath(const StorePath & path, Sink & sink) override;

    void registerDrvOutputUnchecked(const Realisation & info) override;

    void submitOutput(const SingleDerivedPath & path, const OutputName & output) override;

    void addBuildFiles(const std::vector<BuildFile> & files) override;

    AutoCloseFD openBuildFile(const std::filesystem::path & file);

    ref<const ValidPathInfo> addToStoreScanning(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo) override;

    void queryRealisationUncached(
        const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override;

    void addTempRoots(const StorePathSet & paths) override {}

    void addIndirectRoot(const std::filesystem::path & path) override {}

    Roots findRoots(bool censor) override
    {
        return Roots();
    }

    void collectGarbage(const GCOptions & options, GCResults & results) override {}

    void deleteBuildTraces(const std::set<DrvOutput> & keys) override {}

    void addSignatures(const StorePath & storePath, const std::set<Signature> & sigs) override
    {
        unsupported("addSignatures");
    }

    MissingPaths queryMissing(const std::vector<DerivedPath> & targets) override;

    virtual std::optional<std::string> getBuildLogExact(const StorePath & path) override
    {
        return std::nullopt;
    }

    virtual void addBuildLog(const StorePath & path, std::string_view log) override
    {
        unsupported("addBuildLog");
    }

    std::optional<TrustedFlag> isTrustedClient() override
    {
        return NotTrusted;
    }

    ref<Builder> getBuilder(std::shared_ptr<Store> evalStore) override
    {
        unreachable();
    }
};

void RestrictedStore::anchor() {}

/**
 * A builder that wraps an inner builder, adding restriction checks
 * and dependency tracking for recursive Nix builds.
 */
struct RestrictedBuilder : Builder
{
    Worker & inner;
    RestrictionContext & goal;

    RestrictedBuilder(Worker & inner, RestrictionContext & goal)
        : inner(inner)
        , goal(goal)
    {
    }

    void buildPaths(const std::vector<DerivedPath> & paths, BuildMode buildMode) override;

    std::vector<KeyedBuildResult>
    buildPathsWithResults(const std::vector<DerivedPath> & paths, BuildMode buildMode) override;

    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode) override;

    void ensurePath(const StorePath & path) override;

    void repairPath(const StorePath & path) override;
};

ref<Store> makeRestrictedStore(ref<LocalStore::Config> config, ref<LocalStore> next, RestrictionContext & context)
{
    return make_ref<RestrictedStore>(config, next, context);
}

StorePathSet RestrictedStore::queryAllValidPaths()
{
    StorePathSet paths;
    for (auto & p : goal.originalPaths())
        paths.insert(p);
    for (auto & [p, future] : goal.state_.lock()->addedPaths) {
        /* Only report the paths that have finished materialising in the sandbox. */
        if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            paths.insert(p);
    }
    return paths;
}

void RestrictedStore::queryPathInfoUncached(
    const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    if (goal.isAllowed(path)) {
        try {
            /* Censor impure information. */
            auto info = std::make_shared<ValidPathInfo>(*next->queryPathInfo(path));
            info->deriver.reset();
            info->registrationTime = 0;
            info->ultimate = false;
            info->sigs.clear();
            callback(info);
        } catch (InvalidPath &) {
            callback(nullptr);
        }
    } else
        callback(nullptr);
};

void RestrictedStore::queryReferrers(const StorePath & path, StorePathSet & referrers) {}

std::map<std::string, std::optional<StorePath>>
RestrictedStore::queryPartialDerivationOutputMap(const StorePath & path, Store * evalStore)
{
    if (!goal.isAllowed(path))
        throw InvalidPath("cannot query output map for unknown path '%s' in recursive Nix", printStorePath(path));
    return next->queryPartialDerivationOutputMap(path, evalStore);
}

void RestrictedStore::addToStore(
    const ValidPathInfo & info, Source & narSource, RepairFlag repair, CheckSigsFlag checkSigs)
{
    next->addToStore(info, narSource, repair, checkSigs);
    if (batched) {
        batched->insert(info.path);
        return;
    }
    if (goal.transient())
        next->markTransient({info.path});
    goal.addDependency(info.path);
}

void RestrictedStore::addInOneTransaction(const std::function<void()> & add)
{
    if (batched)
        return add();

    batched.emplace();
    Finally reset([&] { batched.reset(); });
    next->addInOneTransaction(add);
    auto paths = std::move(*batched);
    batched.reset();
    if (goal.transient())
        next->markTransient(paths);
    for (auto & path : paths)
        goal.addDependency(path);
}

StorePath RestrictedStore::addToStoreFromDump(
    Source & dump,
    std::string_view name,
    FileSerialisationMethod dumpMethod,
    ContentAddressMethod hashMethod,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    RepairFlag repair)
{
    auto path = next->addToStoreFromDump(dump, name, dumpMethod, hashMethod, hashAlgo, references, repair);
    if (goal.transient())
        next->markTransient({path});
    goal.addDependency(path);
    return path;
}

void RestrictedStore::narFromPath(const StorePath & path, Sink & sink)
{
    if (!goal.isAllowed(path))
        throw InvalidPath("cannot dump unknown path '%s' in recursive Nix", printStorePath(path));
    Store::narFromPath(path, sink);
}

void RestrictedBuilder::ensurePath(const StorePath & path)
{
    if (!goal.isAllowed(path))
        throw InvalidPath("cannot substitute unknown path '%s' in recursive Nix", inner.store.printStorePath(path));
    /* Nothing to be done; 'path' must already be valid. */
}

void RestrictedStore::registerDrvOutputUnchecked(const Realisation & info)
// XXX: This should probably be allowed as a no-op if the realisation
// corresponds to an allowed derivation
{
    throw Error("registerDrvOutput");
}

void RestrictedStore::submitOutput(const SingleDerivedPath & path, const OutputName & output)
{
    goal.submitOutput(path, output);
}

/* `relative` beneath `dir`, a regular file reached without a symlink. */
static AutoCloseFD
openBeneath(Descriptor dir, const std::filesystem::path & relative, const std::filesystem::path & shown)
{
#ifdef __linux__
    auto fd = linux::openat2(
        dir,
        relative.c_str(),
        O_RDONLY | O_CLOEXEC | O_NONBLOCK,
        0,
        RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS);
    if (!fd)
        throw Error("adding build files needs openat2");
    if (!*fd)
        throw SysError("opening build file %s", PathFmt(shown));
    struct stat st;
    if (fstat(fd->get(), &st) == -1)
        throw SysError("statting build file %s", PathFmt(shown));
    if (!S_ISREG(st.st_mode))
        throw Error("build file %s is not a regular file", PathFmt(shown));
    return std::move(*fd);
#else
    throw Error("adding build files needs Linux");
#endif
}

AutoCloseFD RestrictedStore::openBuildFile(const std::filesystem::path & file)
{
    if (auto beneath = goal.inVeneer(file))
        return openBeneath(beneath->second, beneath->first, file);

    if (!isInStore(file.native())) {
        auto [inSandbox, dir] = goal.buildDir();
        auto relative = file.lexically_relative(inSandbox);
        if (relative.empty() || *relative.begin() == "..")
            throw Error("build file %s is in neither the store nor the build directory", PathFmt(file));
        return openBeneath(dir, relative, file);
    }

    auto [storePath, rest] = toStorePath(file.native());
    if (!goal.isAllowed(storePath))
        throw InvalidPath("build file %s is not in an input of the build", PathFmt(file));
    auto real = next->toRealPath(storePath);
    auto [base, relative] = rest.isRoot() ? std::pair{real.parent_path(), real.filename()}
                                          : std::pair{real, std::filesystem::path(rest.rel())};
    auto root = openDirectory(base, FinalSymlink::DontFollow);
    if (!root)
        throw SysError("opening %s", PathFmt(base));
    return openBeneath(root.get(), relative, file);
}

/* A NAR of one regular file read from `fd`. */
static void dumpRegular(Descriptor fd, bool executable, Sink & sink)
{
    struct stat st;
    if (fstat(fd, &st) == -1)
        throw SysError("statting a build file");
    uint64_t size = st.st_size;
    sink << narVersionMagic1 << "(" << "type" << "regular";
    if (executable)
        sink << "executable" << "";
    sink << "contents" << size;
    std::array<char, 64 * 1024> buffer;
    for (uint64_t left = size; left > 0;) {
        auto count = ::read(fd, buffer.data(), std::min<uint64_t>(left, buffer.size()));
        if (count == -1)
            throw SysError("reading a build file");
        if (count == 0)
            throw EndOfFile("a build file shrank while the store read it");
        sink(std::string_view{buffer.data(), static_cast<size_t>(count)});
        left -= count;
    }
    writePadding(size, sink);
    sink << ")";
}

void RestrictedStore::addBuildFiles(const std::vector<BuildFile> & files)
{
    /* Every file opens before any import, so a bad name fails alike
       whether or not the store holds its path. */
    std::vector<AutoCloseFD> opened;
    opened.reserve(files.size());
    for (auto & file : files)
        opened.push_back(openBuildFile(file.file));

    addInOneTransaction([&] {
        for (auto && [file, fd] : std::views::zip(files, opened)) {
            auto nar = sinkToSource([&](Sink & sink) { dumpRegular(fd.get(), file.executable, sink); });
            addToStore(file.info, *nar, NoRepair, CheckSigs);
        }
    });
}

ref<const ValidPathInfo> RestrictedStore::addToStoreScanning(
    Source & dump,
    std::string_view name,
    FileSerialisationMethod dumpMethod,
    ContentAddressMethod hashMethod,
    HashAlgorithm hashAlgo)
{
    auto path = next->addToStoreFromDump(
        dump, name, dumpMethod, hashMethod, hashAlgo, queryAllValidPaths(), RepairFlag::NoRepair, true);
    if (goal.transient())
        next->markTransient({path});
    goal.addDependency(path);
    return queryPathInfo(path);
}

void RestrictedStore::queryRealisationUncached(
    const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept
// XXX: This should probably be allowed if the realisation corresponds to
// an allowed derivation
{
    if (!goal.isAllowed(id)) {
        callback(nullptr);
        return;
    }
    next->queryRealisation(id, std::move(callback));
}

void RestrictedBuilder::buildPaths(const std::vector<DerivedPath> & paths, BuildMode buildMode)
{
    for (auto & result : buildPathsWithResults(paths, buildMode))
        result.tryThrowBuildError();
}

std::vector<KeyedBuildResult>
RestrictedBuilder::buildPathsWithResults(const std::vector<DerivedPath> & paths, BuildMode buildMode)
{
    if (buildMode != bmNormal)
        throw Error("unsupported build mode");

    for (auto & req : paths) {
        if (!goal.isAllowed(req))
            throw InvalidPath("cannot build '%s' in recursive Nix because path is unknown", req.to_string(inner.store));
    }

    auto results = inner.buildPathsWithResults(paths, buildMode);

    StorePathSet newPaths;
    std::set<Realisation> newRealisations;

    for (auto & result : results) {
        if (auto * successP = result.tryGetSuccess()) {
            if (auto * pathBuilt = std::get_if<DerivedPath::Built>(&result.path)) {
                // TODO ugly extra IO
                auto drvPath = resolveDerivedPath(inner.store, *pathBuilt->drvPath);
                for (auto & [outputName, output] : successP->builtOutputs) {
                    newPaths.insert(output.outPath);
                    newRealisations.insert(
                        {output,
                         {
                             .drvPath = drvPath,
                             .outputName = outputName,
                         }});
                }
            }
        }
    }

    StorePathSet closure;
    inner.store.computeFSClosure(newPaths, closure);
    for (auto & path : closure)
        goal.addDependency(path);

    {
        auto state(goal.state_.lock());
        for (auto & real : newRealisations)
            state->addedDrvOutputs.insert(real.id);
    }

    return results;
}

MissingPaths RestrictedStore::queryMissing(const std::vector<DerivedPath> & targets)
{
    /* This is slightly impure since it leaks information to the
       client about what paths will be built/substituted or are
       already present. Probably not a big deal. */

    std::vector<DerivedPath> allowed;
    StorePathSet unknown;
    for (auto & req : targets) {
        if (goal.isAllowed(req))
            allowed.emplace_back(req);
        else
            unknown.insert(pathPartOfReq(req));
    }

    auto res = next->queryMissing(allowed);

    for (auto & p : unknown)
        res.unknown.insert(p);

    return res;
}

ref<Builder> makeRestrictedBuilder(Worker & inner, RestrictionContext & context)
{
    return make_ref<RestrictedBuilder>(inner, context);
}

BuildResult
RestrictedBuilder::buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode)
{
    throw Unsupported("buildDerivation");
}

void RestrictedBuilder::repairPath(const StorePath & path)
{
    throw Unsupported("repairPath");
}

} // namespace nix
