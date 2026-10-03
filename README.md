# fbs-factions

A C99 faction registry with stable ids and directed friendly, neutral or hostile attitudes between factions.

## What it does

You register factions under a byte-string key and get back a permanent id. You then set directed relations between them and ask "how does faction A feel about faction B?".

- `fbs_factions_create` builds a table from a `fbs_faction_config` (capacities and a default attitude). `fbs_faction_add` registers a key with a `fbs_faction_desc` (attitude toward itself, attitude toward everyone else) and returns an id. Ids start at 1 and are never reused or renumbered; `FBS_FACTION_NONE` is 0.
- `fbs_relation_set` writes one directed edge `from -> to`. `fbs_relation_set_symmetric` writes both directions, or neither if there is no room.
- `fbs_attitude_of(f, from, to)` resolves an attitude in a fixed order: the explicit edge, then the source faction's descriptor (`self_attitude` when `from == to`, otherwise `external_attitude`), then the table default. The target's descriptor is never consulted, so A and B can disagree about each other without any edges. `fbs_attitude_of_checked` returns `FBS_FACTION_E_NOT_FOUND` for unknown or retired ids instead of silently answering with the default. `fbs_relation_get` also tells you whether an explicit edge decided the result.
- `fbs_factions_query` lists the live factions whose resolved attitude equals a given value, either toward a faction (`FBS_FACTION_TOWARD`) or from them toward it (`FBS_FACTION_FROM`), in ascending id order.
- `fbs_faction_retire` tombstones an id and drops every edge that touches it. The key can then be registered again under a new id.
- `fbs_faction_to_team` and `fbs_faction_from_team` map ids 1..255 to 8-bit team ids 0..254 (the Unreal `FGenericTeamId` convention). Team 255 (`FBS_TEAM_NONE`) is reserved.
- `fbs_factions_serialize` and `fbs_factions_deserialize` save and load a table as a little-endian binary blob.

## When to use it

You need a small, explicit "who is hostile to whom" table for AI targeting, team checks or diplomacy, keyed by stable ids you can store in save files and network messages. Relations are one-way unless you ask for both directions, which suits cases like "the guards tolerate the player, the player distrusts the guards".

## When not to use it

- Attitudes are exactly three values: `FBS_ATTITUDE_HOSTILE` (-1), `FBS_ATTITUDE_NEUTRAL` (0) and `FBS_ATTITUDE_FRIENDLY` (1). Anything else is rejected with `FBS_FACTION_E_INVALID`. There are no numeric reputation scores or decay.
- Capacities are fixed when the table is created. Defaults are 64 factions, 512 relations and 4096 key bytes; hard upper limits are 2^20 factions, 2^22 relations and 2^26 key bytes. Keys are 1 to 255 bytes.
- Retired ids still count against `max_factions`, and retired keys still occupy key storage, until `fbs_factions_clear`. A table with heavy add/retire churn eventually returns `FBS_FACTION_E_FULL` (tests/test_factions.c, `test_capacities`, checks the key storage case).
- Only ids 1..255 have a team id; `fbs_faction_to_team` returns `FBS_FACTION_E_RANGE` above that.
- There are no change callbacks or events. `fbs_factions_query` scans every issued id, and adding an edge shifts a sorted array, so the cost grows with table size.
- There is no internal locking.

## Example

```c
#include <fbs/factions.h>
#include <stdio.h>
#include <string.h>

static fbs_faction_id add(fbs_factions *f, const char *key, int self_att, int ext_att) {
  fbs_faction_desc d;
  fbs_faction_id id = FBS_FACTION_NONE;
  d.self_attitude = self_att;
  d.external_attitude = ext_att;
  if (fbs_faction_add(f, key, strlen(key), &d, &id) != FBS_FACTION_OK) return FBS_FACTION_NONE;
  return id;
}

int main(void) {
  fbs_faction_config cfg = fbs_faction_config_default();
  fbs_factions *f = NULL;
  fbs_faction_id player, guards, bandits, hostile[8];
  size_t n = 0, i;
  if (fbs_factions_create(&cfg, NULL, &f) != FBS_FACTION_OK) return 1;
  player = add(f, "player", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  guards = add(f, "guards", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  bandits = add(f, "bandits", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  if (!player || !guards || !bandits ||
      fbs_relation_set_symmetric(f, guards, bandits, FBS_ATTITUDE_HOSTILE) != FBS_FACTION_OK ||
      fbs_relation_set(f, guards, player, FBS_ATTITUDE_FRIENDLY) != FBS_FACTION_OK ||
      fbs_factions_query(f, player, FBS_FACTION_FROM, FBS_ATTITUDE_HOSTILE, hostile, 8, &n) != FBS_FACTION_OK) {
    fbs_factions_destroy(f);
    return 1;
  }
  /* 1 = friendly, 0 = neutral, -1 = hostile. Prints 1 then 0: edges are one-way. */
  printf("guards -> player: %d\n", fbs_attitude_of(f, guards, player));
  printf("player -> guards: %d\n", fbs_attitude_of(f, player, guards));
  for (i = 0; i < n; ++i) {
    const char *key = NULL;
    size_t len = 0;
    if (fbs_faction_key(f, hostile[i], &key, &len) == FBS_FACTION_OK)
      printf("hostile to player: %.*s\n", (int)len, key); /* bandits */
  }
  fbs_factions_destroy(f);
  return 0;
}
```

Build it with `add_executable(demo main.c)` and `target_link_libraries(demo PRIVATE fbs::factions)` after adding this repository with `add_subdirectory` or FetchContent.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
```

This runs two tests. `factions` runs tests/test_factions.c, which checks the resolution order, one-way and symmetric edges, id stability across retire and a save/load round trip, the team-id bridge, query direction, every capacity limit, allocator failure, truncated output buffers, invalid arguments on every entry point, rejection of malformed blobs, and a 3000-step seeded randomized run compared against an independent reference model. It also compares serialization output byte for byte with the committed fixture tests/fixtures/factions/table.bin. `factions_example` runs `fbs_factions_example` (examples/basic.c), which creates and destroys a table and prints the API version.

CMake options: `FBS_BUILD_TESTS` and `FBS_BUILD_EXAMPLES` (both ON). Tests also need `BUILD_TESTING`.

Requirements: CMake 3.16 or newer and a C99 compiler. The library uses only the standard C library (`malloc`, `free`, `memcpy`, `memmove`, `memset`, `memcmp`); on non-MSVC toolchains the CMake target also links `m`. There is no vendored code and no dependency on other FinalBuildSystems modules.

To use it from another CMake project, pin a reviewed commit:

```cmake
include(FetchContent)
FetchContent_Declare(fbs_factions
  GIT_REPOSITORY https://github.com/finalbuildgames-com/fbs-factions.git
  GIT_TAG <full-commit-sha>)
FetchContent_MakeAvailable(fbs_factions)
target_link_libraries(your_target PRIVATE fbs::factions)
```

`add_subdirectory(fbs-factions)` works the same way. `cmake --install` installs the library, headers and a `FinalBuildFactionsTargets` export, but no package config file, so `find_package` is not supported.

This repository ships the C library only. Engine bindings and adapters are not included.

## Design notes

- **Determinism.** There is no randomness. Two tables with the same factions and edges serialize to identical bytes regardless of the order the edges were added, and deserializing then re-serializing gives the same bytes (`test_ft8_determinism`). The blob is written byte by byte in little-endian order with a `FBSF` magic and a schema version. `fbs_factions_deserialize` validates it strictly (exact length, sorted unique edges, no edges to retired factions, no duplicate live keys, zeroed reserved bytes) and returns `FBS_FACTION_E_SCHEMA` otherwise. Only live keys are written, so a save/load cycle compacts key storage; retired ids are kept so ids stay valid.
- **Memory.** `fbs_factions_create` makes exactly one allocation sized by the config, and nothing is allocated afterwards (`test_allocator` counts this). Pass a `fbs_faction_allocator` (`alloc`, `free`, `user`) to supply your own, or NULL for `malloc`/`free`. `fbs_factions_memory` reports the block size. Pointers from `fbs_faction_key` stay valid until the table is cleared or destroyed.
- **Threading.** No globals and no static mutable state, so separate tables are independent. A single table has no locking; synchronize writes yourself.
- **Errors.** Functions return `fbs_faction_status` (0 on success, negative on error) and `fbs_faction_status_name` gives a short string. On error, outputs are left untouched, except `FBS_FACTION_E_TRUNCATED`, which writes the required count or length so you can size a buffer and call again.
- **Versioning.** `fbs_faction_version()` returns `FBS_FACTION_VERSION` (`major * 10000 + minor * 100 + patch`, currently 100). The header is wrapped in `extern "C"` for C++ callers. Config and descriptor structs have no size or version fields.

## License

Final Build Games' original code is MIT licensed; see [LICENSE](LICENSE).

Parts of this module are derived from Piperift's [FactionsExtension](https://github.com/PipeRift/FactionsExtension) at commit `f8194a7bb0195a14e81a7804b1a97544974adb2b` (Apache-2.0, Copyright 2015-2018 Piperift), rewritten in C with changed semantics. Those derived portions remain under Apache-2.0; the MIT license does not replace it. See [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) and [third_party/piperift/](third_party/piperift/).
