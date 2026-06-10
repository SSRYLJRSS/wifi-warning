# WiFi Warning Optimization Report

Generated: 2026-06-09

## Current Status

- Build and smoke validation passed after the shortcut restore and settings changes.
- The app is still distributed as a portable zip only.
- Shortcut backups now live under `%APPDATA%\WiFiWarning\shortcut-backups` instead of next to the Desktop or Start Menu `.lnk` file, reducing Explorer refresh flicker and avoiding visible `.lnk.backup` side files.
- Restore operations now keep failed shortcut records so the user can retry one-click restore later.
- Local acceptance previously measured the tray/runtime working set below the 5 MiB target; the runtime smoke path trims the working set after heavier icon and tray operations.

## Suggested Optimizations

These items are recommendations only. They were not implemented in this pass.

1. Replace the detached thread per HTTP request with a tiny bounded worker pool.
   - Expected benefit: more predictable memory use under repeated settings-page polling or accidental refresh storms.
   - Risk: medium, because request shutdown and server stop logic need careful testing.

2. Add a small in-memory icon cache with a fixed entry limit.
   - Expected benefit: lower CPU and GDI churn when settings or warning pages request the same application icons repeatedly.
   - Risk: low to medium. Cache must destroy GDI handles promptly and avoid unbounded growth.

3. Reduce repeated config disk reads inside API handlers.
   - Expected benefit: less file I/O and fewer chances of read/write races while the UI saves rules and reloads statuses.
   - Risk: medium. The existing `ConfigManager::get()` cache can be used, but mutation points must invalidate cleanly.

4. Batch shortcut COM operations.
   - Expected benefit: less repeated `CoInitializeEx` and ShellLink object setup when replacing or restoring a whole software group.
   - Risk: medium. COM apartment lifetime and error handling should be covered by real `.lnk` tests.

5. Serialize shortcut replacement and restore requests.
   - Expected benefit: avoids duplicate writes if the user clicks replace/restore repeatedly or toggles protection during a replace.
   - Risk: low. A narrow process-level mutex around shortcut mutation would likely be enough.

6. Add backup directory cleanup.
   - Expected benefit: removes orphaned backup files after successful restore, config repair, or deleted stale rules.
   - Risk: low. Cleanup should only delete files whose hash is no longer referenced by config.

7. Harden localhost API request validation.
   - Expected benefit: improves stability and safety against malformed large requests or unexpected cross-origin calls.
   - Risk: medium. Add body-size limits, strict JSON parse errors, and Origin/Host checks for mutating endpoints.

8. Tighten static file path checks.
   - Expected benefit: protects against edge-case path prefix confusion in local file serving.
   - Risk: low. Canonical path comparisons should enforce a directory separator boundary.

9. Add adaptive WiFi polling.
   - Expected benefit: lower idle wakeups when WiFi state is stable; faster checks only around user actions.
   - Risk: low. Keep the current 5 second interval as fallback.

10. Make log writes buffered only if volume grows.
    - Expected benefit: fewer small disk writes during repeated blocks.
    - Risk: medium. A bounded queue must flush on exit and never lose restore or failure records.

## Recommended Order

1. Serialize shortcut mutation first, because it protects user data.
2. Add icon caching next, because it is low-risk and likely improves UI responsiveness.
3. Move HTTP serving to a bounded worker pool if profiling shows request-thread growth.
4. Add API hardening and static path boundary checks before any wider distribution.
