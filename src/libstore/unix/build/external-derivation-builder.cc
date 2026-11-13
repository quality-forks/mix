#include "unix-derivation-builder-impl.hh"
#include "child-impl.hh"

namespace nix {

namespace {

struct ExternalDerivationBuilder : UnixDerivationBuilderImpl
{
    ExternalBuilder externalBuilder;

    ExternalDerivationBuilder(
        std::shared_ptr<BuildingStore> store,
        std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
        DerivationBuilderParams params,
        ExternalBuilder externalBuilder)
        : UnixDerivationBuilderImpl(store, miscMethods, std::move(params))
        , externalBuilder(std::move(externalBuilder))
    {
        experimentalFeatureSettings.require(Xp::ExternalBuilders);
    }

    std::filesystem::path tmpDirInSandbox() override
    {
        /* In a sandbox, for determinism, always use the same temporary
           directory. */
        return "/build";
    }

    void setBuildTmpDir() override
    {
        tmpDir = topTmpDir / "build";
        createDir(tmpDir, 0700);
    }

    void startChild() override
    {
        if (drvOptions.getRequiredSystemFeatures(drv).count("recursive-nix"))
            throw Error("'recursive-nix' is not supported yet by external derivation builders");

        auto json = nlohmann::json::object();

        json.emplace("version", 1);
        json.emplace("builder", drv.builder);
        StringRewriter rewrites(inputRewrites);
        {
            auto l = nlohmann::json::array();
            for (auto & i : drv.args)
                l.push_back(rewrites(i));
            json.emplace("args", std::move(l));
        }
        {
            auto j = nlohmann::json::object();
            for (auto & [name, value] : env)
                j.emplace(name, rewrites(value));
            json.emplace("env", std::move(j));
        }
        json.emplace("topTmpDir", topTmpDir.native());
        json.emplace("tmpDir", tmpDir.native());
        json.emplace("tmpDirInSandbox", tmpDirInSandbox().native());
        json.emplace("storeDir", store->storeDir);
        json.emplace("realStoreDir", store->getRealStoreDir().native());
        json.emplace("system", drv.platform);
        StorePathSet manifests = drvOptions.veneers;
        for (auto & [_, manifest] : drvOptions.writableVeneers)
            manifests.insert(manifest);
        {
            auto l = nlohmann::json::array();
            for (auto & i : inputPaths)
                if (!manifests.contains(i))
                    l.push_back(store->printStorePath(i));
            json.emplace("inputPaths", std::move(l));
        }
        {
            auto l = nlohmann::json::object();
            for (auto & i : scratchOutputs)
                l.emplace(i.first, store->printStorePath(i.second));
            json.emplace("outputs", std::move(l));
        }
        /* The builder writes each veneer itself, from the manifest's
           references listed in a file beside build.json. */
        if (!manifests.empty()) {
            createDir(topTmpDir / "veneers", 0755);
            auto l = nlohmann::json::array();
            auto add = [&](const StorePath & manifest, const std::filesystem::path & target, bool writable) {
                auto references = topTmpDir / "veneers" / std::to_string(l.size());
                /* The files of every layer below the manifest take bytes from
                   what that layer refers to. */
                auto & helper = store->getLocalSettings().veneerHelper.get();
                if (!helper)
                    throw Error(
                        "'%s' mounts veneers, but 'veneer-helper' is not set", store->printStorePath(drvPath));
                StorePathSet referenced;
                auto layers = runProgram(*helper, false, {"veneer", "layers", store->toRealPath(manifest).native()});
                for (auto & layer : tokenizeString<Strings>(layers, "\n"))
                    for (auto & reference :
                         store->queryReferences(StorePath(std::filesystem::path(layer).filename().string())))
                        referenced.insert(reference);
                std::string listed;
                for (auto & reference : referenced)
                    listed += store->printStorePath(reference) + "\n";
                writeFile(references, listed);
                l.push_back({
                    {"manifest", store->printStorePath(manifest)},
                    {"target", target.native()},
                    {"writable", writable},
                    {"references", references.native()},
                });
            };
            for (auto & manifest : drvOptions.veneers)
                add(manifest, store->printStorePath(manifest), false);
            for (auto & [dir, manifest] : drvOptions.writableVeneers)
                add(manifest, tmpDirInSandbox() / dir, true);
            json.emplace("veneers", std::move(l));
        }

        // TODO(cole-h): writing this to stdin is too much effort right now, if we want to revisit
        // that, see this comment by Eelco about how to make it not suck:
        // https://github.com/DeterminateSystems/nix-src/pull/141#discussion_r2205493257
        auto jsonFile = std::filesystem::path{topTmpDir} / "build.json";
        writeFile(jsonFile, json.dump());

        pid = startProcess([&]() {
            openSlave();
            try {
                commonChildInit();

                Strings args = {externalBuilder.program};

                if (!externalBuilder.args.empty()) {
                    args.insert(args.end(), externalBuilder.args.begin(), externalBuilder.args.end());
                }

                args.insert(args.end(), jsonFile);

                if (chdir(tmpDir.c_str()) == -1)
                    throw SysError("changing into %1%", PathFmt(tmpDir));

                chownToBuilder(topTmpDir);

                setUser();

                debug("executing external builder: %s", concatStringsSep(" ", args));
                execv(externalBuilder.program.c_str(), stringsToCharPtrs(args).data());

                throw SysError("executing %s", PathFmt(externalBuilder.program));
            } catch (...) {
                handleChildException(true);
                _exit(1);
            }
        });
    }
};

} // namespace

DerivationBuilderUnique makeExternalDerivationBuilder(
    std::shared_ptr<BuildingStore> store,
    std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
    DerivationBuilderParams params,
    const ExternalBuilder & handler)
{
    return DerivationBuilderUnique(new ExternalDerivationBuilder(store, miscMethods, std::move(params), handler));
}

} // namespace nix
