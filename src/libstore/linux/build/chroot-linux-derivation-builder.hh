#pragma once

#include "chroot-derivation-builder.hh"
#include "linux-derivation-builder.hh"

namespace nix {

struct ChrootLinuxDerivationBuilder : ChrootDerivationBuilder, LinuxDerivationBuilder
{
    /**
     * Pipe for synchronising updates to the builder namespaces.
     */
    Pipe userNamespaceSync;

    /**
     * The process that clones the builder into its namespaces, and the
     * pipe it sends the builder's pid on.
     */
    Pid helper;
    Pipe sendPid;

    /**
     * The mount namespace and user namespace of the builder, used to add additional
     * paths to the sandbox as a result of recursive Nix calls.
     */
    AutoCloseFD sandboxMountNamespace;
    AutoCloseFD sandboxUserNamespace;

    /**
     * On Linux, whether we're doing the build in its own user
     * namespace.
     */
    bool usingUserNamespace = true;

    /**
     * The cgroup of the builder, if any.
     */
    std::optional<std::filesystem::path> cgroup;

    /**
     * A veneer of `__veneers`, the overlay the builder moves onto
     * `target`, and a shared lock on its metadata tree, which the veneer
     * cache evicts only once no build holds it. The overlay closes first.
     */
    struct Veneer
    {
        std::filesystem::path target;
        bool writable;
        std::vector<AutoCloseFD> locks;
        AutoCloseFD mount;
    };

    std::vector<Veneer> veneers;

    ChrootLinuxDerivationBuilder(
        std::shared_ptr<BuildingStore> store,
        std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
        DerivationBuilderParams params)
        : UnixDerivationBuilderImpl{store, miscMethods, params}
        , ChrootDerivationBuilder{store, miscMethods, params}
        , LinuxDerivationBuilder{store, miscMethods, params}
    {
    }

    uid_t sandboxUid();

    gid_t sandboxGid() override;

    std::unique_ptr<UserLock> getBuildUser() override;

    void prepareUser() override;

    void prepareSandbox() override;

    Veneer mountVeneer(const StorePath & manifest, const std::filesystem::path & target, bool writable);

    /**
     * What `prepareChild` hands the helper.
     */
    AutoCloseFD specFile;
    AutoCloseFD slave;
    AutoCloseFD cgroupFd;

    void prepareChild() override;

    void startChild() override;

    void awaitSetup() override;

    BuilderExit unprepareBuild() override;

    void killSandbox(bool getStats) override;

    void addDependencyImpl(const StorePath & path) override;
};

} // namespace nix
