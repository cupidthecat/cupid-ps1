# Game database

Source: `src/core/game_database.c`, `src/core/game_database_apply.c`, `src/core/game_database_tracks.c`. Backing data: `data/resources/gamedb.yaml`, `discsets.yaml` and `discdb.yaml`. Database compatibility ratings are metadata, not validation results for cupid-ps1.

- YAML loaded at startup; a binary cache (`gamedb.cache`) is generated from the source and reloaded on subsequent runs when the source is unchanged. 
- Lookup is by disc serial (e.g. `SCUS-94900`) via binary search.
- Per-title traits drive runtime overrides such as `FORCE_INTERPRETER`, `FORCE_SOFTWARE_RENDERER`(_FOR_READBACKS), `FORCE_FULL_BOOT`, `DISABLE_AUTO_ANALOG_MODE`, `DISABLE_MULTITAP`, `DISABLE_PGXP` (+ subflags), `FORCE_RECOMPILER_ICACHE`, `FORCE_CDROM_SUBQ_SKEW`, `IS_LIBCRYPT_PROTECTED`, etc. Full list in `src/core/game_database.h`.
