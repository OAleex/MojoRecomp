# Localization

Localization Packs are versioned separately from the launcher and game runtimes.
Each game owns a pack, and each custom language inside the pack is an independent
`language.<game>.<locale>` component.

Only localization deltas and MojoRecomp metadata belong here. Original game files
must not be committed.
