#pragma once

///@file

namespace nix {

/**
 * Entry point of nix-sandbox-helper, reading its spec from the descriptors
 * the daemon passed.
 */
[[noreturn]] void sandboxHelperMain();

} // namespace nix
