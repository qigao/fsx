# FileSearch build policy

- The user requires **no install/consumer approaches** for this project.
- Do not add or run project install rules, install presets, exports, consumer
  projects, or install/consumer verification through scripts, CMake, CI or tools.
- Use the versioned configure/build presets and formal source-owned tests.
  Do not add synthetic consumers, smoke scripts, configuration self-tests or
  placeholder targets.
- Keep production dependencies in the manifest and explicit Core/CMeta/host-tool
  roots. Preserve private `.env` credentials and existing shared checkout edits.
- Reuse `cmake/CmakeUtils.cmake` and `cmake/Sanitizers.cmake`; keep project policy
  in the root options/presets and module source lists in their owning directory.
- Keep `CMakeUserPresets.json` versioned. The user instruction above takes
  precedence over general skill guidance that would add installation presets.
