---
synopsis: "New derivation attribute `hiddenInputs`"
prs: []
---

The [`hiddenInputs`](@docroot@/language/advanced-attributes.md#adv-attr-hiddenInputs) attribute lists inputs whose closures are realised and may be referenced by the outputs, but are not made available inside the sandbox.
A builder that only names a large number of store paths no longer needs one bind mount per path, which could exceed the kernel's `fs.mount-max` limit.
