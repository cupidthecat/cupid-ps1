# Game database

The database uses `data/resources/gamedb.yaml`, `discsets.yaml` and
`discdb.yaml`. Compatibility ratings describe that data; they are not
validation results for cupid-ps1.

At startup, the loader reads the YAML and generates a binary cache,
`gamedb.cache`. Later runs reuse the cache while the source is unchanged.
Disc lookup uses a binary search by serial, such as `SCUS-94900`.

Per-title traits select runtime overrides. Examples include `FORCE_INTERPRETER`,
`FORCE_SOFTWARE_RENDERER`, `FORCE_SOFTWARE_RENDERER_FOR_READBACKS`,
`FORCE_FULL_BOOT`, `DISABLE_AUTO_ANALOG_MODE`, `DISABLE_MULTITAP`,
`DISABLE_PGXP` and its subflags, `FORCE_RECOMPILER_ICACHE`,
`FORCE_CDROM_SUBQ_SKEW` and `IS_LIBCRYPT_PROTECTED`. The full list is in
`src/core/database/game_database.h`.

The implementation is split across `src/core/database/game_database.c`,
`src/core/database/game_database_apply.c` and
`src/core/database/game_database_tracks.c`.
