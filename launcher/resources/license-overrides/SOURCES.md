# License Override Provenance

Updated: 2026-09-22

These files are source inputs for `scripts/generate-third-party-notices.mjs`.
They are used only when an installed Cargo/npm package declares a license but
does not carry a usable root license file in the audited package tree.

Every override below was compared with the indicated upstream or canonical text
after normalizing line endings and the final newline. The normalized content
matched exactly. SHA-256 values below are for the normalized UTF-8 text used by
the notice generator.

| Local file | Used for locked package(s) | Verified source | SHA-256 |
| --- | --- | --- | --- |
| `alloc-stdlib-BSD-3-Clause.txt` | `alloc-stdlib 0.2.4` | `https://raw.githubusercontent.com/dropbox/rust-alloc-no-stdlib/ae42d22078b98549e987d2f03d12df7b984fde47/LICENSE` | `C0C56F26D9C051CAC4D200C34C84E7AE9AAA853E01A982A1DF08B09931E518AE` |
| `defmt-MIT.txt` | `defmt-parser 1.0.0` | `https://raw.githubusercontent.com/knurling-rs/defmt/4a8cdb44891ed57b8ff5a023b6bec7137c48708f/LICENSE-MIT` | `0D17B75C1867FD568BCBB735F329D0D4253846C4B756A65E4D440C1E4BD59187` |
| `defmt-Apache-2.0.txt` | `defmt-parser 1.0.0` | `https://raw.githubusercontent.com/knurling-rs/defmt/4a8cdb44891ed57b8ff5a023b6bec7137c48708f/LICENSE-APACHE` | `8173D5C29B4F956D532781D2B86E4E30F83E6B7878DCE18C919451D6BA707C90` |
| `MPL-2.0.txt` | `selectors 0.36.1` | SPDX canonical MPL-2.0 text: `https://raw.githubusercontent.com/spdx/license-list-data/main/text/MPL-2.0.txt` | `66A3107D5AD6A058AAB753EAAC2047CCB2ED0E39465DD0FE5844DA3E300D5172` |
| `rolldown-MIT.txt` | `@rolldown/binding-win32-x64-msvc 1.2.9` | `https://raw.githubusercontent.com/rolldown/rolldown/v1.2.9/LICENSE` | `23ECFFF35A5A2E80D92142F75228912C3B1ABC4B5A8337A821FF4397E2F9F734` |
| `rust-unic-MIT.txt` | `unic-char-property 0.9.0`, `unic-char-range 0.9.0`, `unic-common 0.9.0`, `unic-ucd-ident 0.9.0`, `unic-ucd-version 0.9.0` | `https://raw.githubusercontent.com/open-i18n/rust-unic/5878605364af97a3358368a6eaef02104af2e016/LICENSE-MIT` | `23F18E03DC49DF91622FE2A76176497404E46CED8A715D9D2B67A7446571CCA3` |
| `rust-unic-Apache-2.0.txt` | same rust-unic packages | `https://raw.githubusercontent.com/open-i18n/rust-unic/5878605364af97a3358368a6eaef02104af2e016/LICENSE-APACHE` | `A60EEA817514531668D7E00765731449FE14D059D3249E0BC93B36DE45F759F2` |
| `webview2-rs-MIT.txt` | `webview2-com 0.38.2`, `webview2-com-macros 0.8.1`, `webview2-com-sys 0.38.2` | `https://raw.githubusercontent.com/wravery/webview2-rs/b74dc5e2b394044bea5191052868ce7a106c202c/LICENSE` | `0DCF41516E608BBCB6CDC5229FEB7B86FE4A643B85E7DF251133C93408FDAC73` |

The rust-unic crates are locked to package metadata originating from rust-unic
commits recorded in their Cargo package metadata; the shared license files above
were verified against the common upstream repository commit used by the locked
0.9.0 package family. `unic-ucd-ident 0.9.0` records a different packaging
commit, but declares the same `MIT/Apache-2.0` licensing and comes from the same
upstream repository.

`tauri-plugin 2.6.3` and `@tauri-apps/cli-win32-x64-msvc 2.11.5` use the
already-audited `resources/licenses/Tauri-API-MIT.txt` and
`resources/licenses/Tauri-API-APACHE-2.0.txt` rather than duplicate override
files. Their package metadata declares `Apache-2.0 OR MIT` and points to the
Tauri upstream repository.

Changing an override body, its package mapping, or its upstream provenance
requires updating this record and the hashes enforced by
`scripts/validate-release.mjs`.
