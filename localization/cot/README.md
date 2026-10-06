# Crash of the Titans Localization Pack

Release output: `MojoRecomp-COT-Localization-Pack-<version>.zip`.

Language sources live under `languages/<locale>/`. A language becomes publishable
when `publish = true` in its `language.toml`, it provides a non-empty `CREDITS.txt`, and its `payload/` directory contains
the signed-release input generated from localization deltas.

Each published `language.toml` also records the original translation version and
the public credits source. Credits are shipped with and validated as part of the
language component so offline installations preserve attribution.
