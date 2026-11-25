#include <atomic>
#include <cerrno>
#include <optional>
#include <vector>
#include <pwd.h>
#include <grp.h>

#include "nix/store/user-lock.hh"
#include "nix/store/local-settings.hh"
#include "nix/util/file-system.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/users.hh"
#include "nix/util/logging.hh"

namespace nix {

UserLock::~UserLock() {}

#ifdef __linux__

static std::vector<gid_t> get_group_list(const char * username, gid_t group_id)
{
    std::vector<gid_t> gids;
    gids.resize(32); // Initial guess

    auto getgroupl_failed{[&] {
        int ngroups = gids.size();
        int err = getgrouplist(username, group_id, gids.data(), &ngroups);
        gids.resize(ngroups);
        return err == -1;
    }};

    // The first error means that the vector was not big enough.
    // If it happens again, there is some different problem.
    if (getgroupl_failed() && getgroupl_failed()) {
        throw SysError("failed to get list of supplementary groups for '%s'", username);
    }

    return gids;
}
#endif

namespace {

/* Goals prepare their sandboxes on helper threads at once, so these look users and groups up with the
   reentrant calls: getpwnam() and getgrnam() return a buffer every thread shares, and another goal's
   lookup between this one locking a user's file and reading its uid gave two builds the same uid, the
   first of which to finish SIGKILLed the other with killUser(). */

/**
 * Calls `lookup` with an entry and a buffer for its strings, again with a larger buffer while that one
 * is too small. Any other error is taken as no entry, as the non-reentrant calls returned none.
 */
template<typename T, typename Lookup>
static void lookupEntry(Lookup && lookup)
{
    std::vector<char> buf(16384);
    T entry;
    while (lookup(&entry, buf.data(), buf.size()) == ERANGE)
        buf.resize(buf.size() * 2);
}

struct GroupEntry
{
    gid_t gid;
    Strings members;
};

static std::optional<GroupEntry> lookupGroup(const std::string & name)
{
    std::optional<GroupEntry> result;
    lookupEntry<struct group>([&](struct group * gr, char * buf, size_t size) {
        struct group * found = nullptr;
        int err = getgrnam_r(name.c_str(), gr, buf, size, &found);
        /* The members point into `buf`, so they are copied before it is reused or freed. */
        if (err == 0 && found) {
            result.emplace(GroupEntry{.gid = gr->gr_gid});
            for (char ** p = gr->gr_mem; *p; ++p)
                result->members.push_back(*p);
        }
        return err;
    });
    return result;
}

struct UserEntry
{
    std::string name;
    uid_t uid;
    gid_t gid;
};

static std::optional<UserEntry> lookupUser(const std::string & name)
{
    std::optional<UserEntry> result;
    lookupEntry<struct passwd>([&](struct passwd * pw, char * buf, size_t size) {
        struct passwd * found = nullptr;
        int err = getpwnam_r(name.c_str(), pw, buf, size, &found);
        if (err == 0 && found)
            result.emplace(UserEntry{.name = pw->pw_name, .uid = pw->pw_uid, .gid = pw->pw_gid});
        return err;
    });
    return result;
}

static std::optional<std::string> userNameOf(uid_t uid)
{
    std::optional<std::string> result;
    lookupEntry<struct passwd>([&](struct passwd * pw, char * buf, size_t size) {
        struct passwd * found = nullptr;
        int err = getpwuid_r(uid, pw, buf, size, &found);
        if (err == 0 && found)
            result = pw->pw_name;
        return err;
    });
    return result;
}

struct SimpleUserLock : UserLock
{
    AutoCloseFD fdUserLock;
    uid_t uid;
    gid_t gid;
    std::vector<gid_t> supplementaryGIDs;

    uid_t getUID() override
    {
        assert(uid);
        return uid;
    }

    uid_t getUIDCount() override
    {
        return 1;
    }

    gid_t getGID() override
    {
        assert(gid);
        return gid;
    }

    std::vector<gid_t> getSupplementaryGIDs() override
    {
        return supplementaryGIDs;
    }

    static std::unique_ptr<UserLock>
    acquire(const std::filesystem::path & userPoolDir, const std::string & buildUsersGroup)
    {
        assert(buildUsersGroup != "");

        /* Get the members of the build-users-group. */
        auto gr = lookupGroup(buildUsersGroup);
        if (!gr)
            throw Error("the group '%s' specified in 'build-users-group' does not exist", buildUsersGroup);

        auto & users = gr->members;
        for (auto & user : users)
            debug("found build user '%s'", user);

        if (users.empty())
            throw Error("the build users group '%s' has no members", buildUsersGroup);

        /* Find a user account that isn't currently in use for another
           build. */
        for (auto & i : users) {
            debug("trying user '%s'", i);

            auto pw = lookupUser(i);
            if (!pw)
                throw Error("the user '%s' in the group '%s' does not exist", i, buildUsersGroup);

            auto fnUserLock = userPoolDir / std::to_string(pw->uid);

            AutoCloseFD fd = open(fnUserLock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (!fd)
                throw SysError("opening user lock %s", PathFmt(fnUserLock));

            if (lockFile(fd.get(), ltWrite, false)) {
                auto lock = std::make_unique<SimpleUserLock>();

                lock->fdUserLock = std::move(fd);
                lock->uid = pw->uid;
                lock->gid = gr->gid;

                /* Sanity check... */
                if (lock->uid == getuid() || lock->uid == geteuid())
                    throw Error("the Nix user should not be a member of '%s'", buildUsersGroup);

#ifdef __linux__
                /* Get the list of supplementary groups of this user. This is
                 * usually either empty or contains a group such as "kvm". */

                // Finally, trim back the GID list to its real size.
                for (auto gid : get_group_list(pw->name.c_str(), pw->gid)) {
                    if (gid != lock->gid)
                        lock->supplementaryGIDs.push_back(gid);
                }
#endif

                return lock;
            }
        }

        return nullptr;
    }
};

struct AutoUserLock : UserLock
{
    AutoCloseFD fdUserLock;
    uid_t firstUid = 0;
    gid_t firstGid = 0;
    uid_t nrIds = 1;

    uid_t getUID() override
    {
        assert(firstUid);
        return firstUid;
    }

    gid_t getUIDCount() override
    {
        return nrIds;
    }

    gid_t getGID() override
    {
        assert(firstGid);
        return firstGid;
    }

    std::vector<gid_t> getSupplementaryGIDs() override
    {
        return {};
    }

    static std::unique_ptr<UserLock> acquire(
        const std::filesystem::path & userPoolDir,
        const std::string & buildUsersGroup,
        uid_t nrIds,
        bool useUserNamespace,
        const AutoAllocateUidSettings & uidSettings)
    {
#if !defined(__linux__)
        useUserNamespace = false;
#endif

        experimentalFeatureSettings.require(Xp::AutoAllocateUids);
        assert(uidSettings.startId > 0);
        assert(uidSettings.uidCount % maxIdsPerBuild == 0);
        assert((uint64_t) uidSettings.startId + (uint64_t) uidSettings.uidCount <= std::numeric_limits<uid_t>::max());
        assert(nrIds <= maxIdsPerBuild);

        size_t nrSlots = uidSettings.uidCount / maxIdsPerBuild;

        /* Builds take and release slots roughly in order, so the slot after the last one this
           process took is usually free, where a scan from slot 0 opens and locks a file for
           every running build first. */
        static std::atomic<size_t> nextSlot = 0;
        size_t firstTried = nextSlot.load() % nrSlots;

        for (size_t tried = 0; tried < nrSlots; tried++) {
            size_t i = (firstTried + tried) % nrSlots;
            debug("trying user slot '%d'", i);

            auto fnUserLock = userPoolDir / fmt("slot-%d", i);

            AutoCloseFD fd = open(fnUserLock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (!fd)
                throw SysError("opening user lock %s", PathFmt(fnUserLock));

            if (lockFile(fd.get(), ltWrite, false)) {
                nextSlot = i + 1;

                auto firstUid = uidSettings.startId + i * maxIdsPerBuild;

                if (auto name = userNameOf(firstUid))
                    throw Error("auto-allocated UID %d clashes with existing user account '%s'", firstUid, *name);

                auto lock = std::make_unique<AutoUserLock>();
                lock->fdUserLock = std::move(fd);
                lock->firstUid = firstUid;
                if (useUserNamespace)
                    lock->firstGid = firstUid;
                else {
                    auto gr = lookupGroup(buildUsersGroup);
                    if (!gr)
                        throw Error("the group '%s' specified in 'build-users-group' does not exist", buildUsersGroup);
                    lock->firstGid = gr->gid;
                }
                lock->nrIds = nrIds;
                return lock;
            }
        }

        return nullptr;
    }
};

} // namespace

std::unique_ptr<UserLock> acquireUserLock(
    const std::filesystem::path & stateDir, const LocalSettings & localSettings, uid_t nrIds, bool useUserNamespace)
{
    if (auto * uidSettings = localSettings.getAutoAllocateUidSettings()) {
        auto userPoolDir = stateDir / "userpool2";
        createDirs(userPoolDir);
        return AutoUserLock::acquire(userPoolDir, localSettings.buildUsersGroup, nrIds, useUserNamespace, *uidSettings);
    } else {
        auto userPoolDir = stateDir / "userpool";
        createDirs(userPoolDir);
        return SimpleUserLock::acquire(userPoolDir, localSettings.buildUsersGroup);
    }
}

bool useBuildUsers(const LocalSettings & localSettings)
{
#ifdef __linux__
    static bool b = (localSettings.buildUsersGroup != "" || localSettings.autoAllocateUids) && isRootUser();
    return b;
#elif defined(__APPLE__) || defined(__FreeBSD__)
    static bool b = localSettings.buildUsersGroup != "" && isRootUser();
    return b;
#else
    return false;
#endif
}

} // namespace nix
