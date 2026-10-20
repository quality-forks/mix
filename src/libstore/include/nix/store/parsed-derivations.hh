#pragma once
///@file

#include <memory>

#include <nlohmann/json.hpp>

#include "nix/util/types.hh"
#include "nix/store/path.hh"

namespace nix {

class Store;

namespace derivation {
template<typename Input>
struct Options;
struct Output;
} // namespace derivation

struct StructuredAttrs
{
    static constexpr std::string_view envVarName{"__json"};

    nlohmann::json::object_t structuredAttrs;

    /**
     * The encoding we parsed, kept verbatim because it was not the
     * canonical one `unparse` would produce. Derivations in the wild
     * predate us caring about this, and their encoding is part of their
     * identity, so we must be able to reproduce it exactly.
     */
    std::optional<std::string> verbatim;

    /**
     * Compares encodings, not just values, since that is what must
     * round-trip. Dumping is only needed when exactly one side has a
     * verbatim encoding to compare against.
     */
    bool operator==(const StructuredAttrs & other) const;

    /**
     * Unconditionally parse from a JSON string. Used by `tryExtract`.
     */
    static StructuredAttrs parse(std::string_view encoded);

    /**
     * Like `tryParse`, but removes the env var which encoded the structured
     * attrs from the map if one is found.
     */
    static std::optional<StructuredAttrs> tryExtract(StringPairs & env);

    /**
     * Opposite of `tryParse`, at least if one makes a map from this
     * single key-value PR.
     */
    std::pair<std::string_view, std::string> unparse() const;

    /**
     * Ensures that the structured attrs "env var" is not in used, so we
     * are free to use it instead.
     */
    static void checkKeyNotInUse(const StringPairs & env);

    nlohmann::json::object_t prepareStructuredAttrs(
        Store & store,
        const derivation::Options<StorePath> & drvOptions,
        const StorePathSet & inputPaths,
        const std::map<std::string, derivation::Output> & outputs) const;

    /**
     * As a convenience to bash scripts, write a shell file that
     * maps all attributes that are representable in bash -
     * namely, strings, integers, nulls, Booleans, and arrays and
     * objects consisting entirely of those values. (So nested
     * arrays or objects are not supported.)
     *
     * @param prepared This should be the result of
     * `prepareStructuredAttrs`, *not* the original `structuredAttrs`
     * field.
     */
    static std::string writeShell(const nlohmann::json::object_t & prepared);
};

/**
 * A derivation's structured attrs, shared by its copies since the attrs
 * are nearly all of its memory, often several times the size of the
 * derivation file.
 */
class SharedStructuredAttrs
{
    std::shared_ptr<StructuredAttrs> attrs;

public:
    SharedStructuredAttrs() = default;

    SharedStructuredAttrs(std::nullopt_t) {}

    SharedStructuredAttrs(StructuredAttrs attrs)
        : attrs{std::make_shared<StructuredAttrs>(std::move(attrs))}
    {
    }

    SharedStructuredAttrs(std::optional<StructuredAttrs> attrs)
        : attrs{attrs ? std::make_shared<StructuredAttrs>(std::move(*attrs)) : nullptr}
    {
    }

    explicit operator bool() const
    {
        return bool(attrs);
    }

    const StructuredAttrs & operator*() const
    {
        return *attrs;
    }

    const StructuredAttrs * operator->() const
    {
        return attrs.get();
    }

    /**
     * The attrs to change, copied first if another derivation shares them.
     */
    StructuredAttrs & unshare();

    bool operator==(const SharedStructuredAttrs & other) const;
};

/**
 * Like `get` on a `std::optional`.
 */
inline const StructuredAttrs * get(const SharedStructuredAttrs & attrs)
{
    return attrs ? &*attrs : nullptr;
}

} // namespace nix
