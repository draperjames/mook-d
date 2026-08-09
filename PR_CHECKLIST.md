# Submitting Mook D to Schwung

Mook D ships as its own repo (like `schwung-dx7`, `schwung-sf2`, `aphex-move`)
and is registered in the host via a one-entry PR to `module-catalog.json`.

## A. Your module repo (draperjames/mook-d)
- [ ] Push this tree to `main`.
- [ ] Tag a release: `git tag v0.1.0 && git push --tags`.
      The Release workflow cross-compiles the ARM64 `dsp.so`, verifies it's
      `aarch64`, packages `mook-d-module.tar.gz`, publishes the GitHub Release,
      and commits `release.json`.
- [ ] Confirm the release asset exists at the `download_url` in `release.json`.
- [ ] Verify `module.json` `version` == the tag (the workflow enforces this).

## B. Upstream PR (charlesvestal/schwung)
- [ ] Fork, branch: `git switch -c add-mook-d`.
- [ ] Add the object in `catalog-entry.json` to the `modules` array in
      `module-catalog.json`.
- [ ] Open the PR against `main`. Three required checks must pass
      (`host-tests`, `go`, `cross-compile`) — none are affected by a
      catalog-only change, but a maintainer must click "Approve and run"
      for a first-time contributor.

## C. Required PR-description text (contribution provenance)
Schwung requires disclosing AI tooling, and does **not** accept Grok-developed
work. Paste something like:

> Built with AI assistance (Anthropic Claude). No Grok used.
> New standalone sound-generator module: Mook D, a Model D-style mono synth.
> Repo: https://github.com/draperjames/mook-d — release v0.1.0 asset attached
> via the catalog entry.

## D. Before you tag: confirm on device
CI proves it compiles for ARM64; it does not run on Move hardware. Load the
module on a Move and check audio/MIDI/UI once (the `module.json` UI hierarchy
renders, notes sound, presets switch). See `scripts/install.sh` for scp deploy.

## Open items a maintainer may raise
- **`module.json` size:** docs mention an 8 KB loader cap; this manifest is
  ~14 KB (the accepted `aphex-move` module is ~18 KB, so it loads in practice,
  but confirm on-device or compact the JSON if asked).
- **`min_host_version`:** set to `0.3.0` to match the other api_version-2
  sound generators (dexed/sf2). Bump if a maintainer specifies a higher floor.
- **Trademark:** "Model D" / "Minimoog" are Moog Music marks. The product name
  is Mook D and code is original; remaining references are descriptive
  (nominative) — reword further if the maintainer prefers.
- **Screen reader / ui.js:** not shipped (matches `aphex-move`); the host
  auto-generates UI from `module.json`. Add `ui.js` only if requested.
