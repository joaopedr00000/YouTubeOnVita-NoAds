# Experimental modified Cobalt packaging

The regular Full/Lite builds use the unmodified upstream Cobalt core.
The experimental workflow builds with the upstream single target toolchain
and, only after inspecting the exact ELF, calls the Vita packaging workflow
with that source-built runtime, matching installed content and generated loader
profile. Candidate VPKs use separate title IDs and distinct artifact names.
CI compares the embedded runtime SHA-256 to the compiled source artifact;
the candidate loader also verifies the exact SHA-256 before mapping the ELF.
The baseline continues to use the unchanged official runtime profile.

The candidate gate fails closed on unsupported imports, relocation types,
memory layout, ABI or unrecognized ARM cacheflush instructions. The candidate
loader excludes original-binary-only offset probes and diagnostic patches.
This is not proof that the candidate boots, plays videos, or blocks ads on
physical hardware. Candidate and baseline share the existing app data
directory; back up settings before hardware testing. No release is made.
