# Source SBOM

Source repository inventory only; completeness is UNKNOWN. Syft inventories tracked source/package manifests. Gitlinks and declared vendored/model evidence are inspected at immutable pins. CMake declarations are conditional requirements, not resolved installations. This is NOT proof of operating-system packages, static-link closure, build-time resolution, or the contents of release binaries.

Generation: **success**

## Source identity

wave-owl/fldigi-mfsk-core `784861c3bec5316147533d834542168156dde2cb`

- sbom:role: source-root
- sbom:git-commit: 784861c3bec5316147533d834542168156dde2cb
- sbom:git-tree: b86715074ddfb5f971431cd4c915ed4e31aa2722
- sbom:source-repository: https://github.com/wave-owl/fldigi-mfsk-core
- sbom:release-tags: ["v0.1.1"]
- sbom:license-evidence: [{"path": "LICENSE", "sha256": "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"}]
- sbom:license-resolution: unresolved; manual review required

## Generation errors

None.

## Components and unresolved licenses

| Component | Version | Role | License |
| --- | --- | --- | --- |
| wave-owl/fldigi-mfsk-core | 784861c3bec5316147533d834542168156dde2cb | source-root | unresolved; manual review required |
| wave-owl/fldigi-dsp-core | b368f5ea24c180c9d43db2e2670ebac7330e01c8 | gitlink | unresolved; manual review required |
| src/mfsk_common.c | e7c3ab3709ad62a7d91829364eb48a05fa325c0d | declared-vendored-source | unresolved; manual review required |
| src/mfsk_rx.c | e7c3ab3709ad62a7d91829364eb48a05fa325c0d | declared-vendored-source | unresolved; manual review required |
| src/mfsk_tx.c | e7c3ab3709ad62a7d91829364eb48a05fa325c0d | declared-vendored-source | unresolved; manual review required |
| actions/checkout | v4 | Syft source/package discovery | unresolved |
| actions/checkout | v4 | Syft source/package discovery | unresolved |
| tracked-source/.github/workflows/ci.yml | unresolved | Syft source/package discovery | unresolved |
| tracked-source/dsp/.github/workflows/ci.yml | unresolved | Syft source/package discovery | unresolved |

License evidence paths/hashes, source manifests, model hashes and conditional
CMake declarations are recorded in SBOM.cdx.json. Declared upstream pins do not
prove vendored equivalence. No dependency installation or build was performed.
