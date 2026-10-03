/*
 * fbs/factions.h — FinalBuildSystems faction registry and directed attitude
 * relations. C99, engine independent, no libm.
 *
 * Model (decision: docs/decisions/factions.md): factions are registered under
 * a UTF-8 key and receive a dense, permanent id (1-based; 0 is
 * FBS_FACTION_NONE). Relations are always directed edges from -> to with an
 * attitude; "symmetric" helpers write two edges. Attitude resolution order:
 *   1. explicit edge (from -> to)
 *   2. the SOURCE faction's descriptor: from == to ? self_attitude : external_attitude
 *   3. the table default (FBS_ATTITUDE_NEUTRAL unless changed)
 * The target's descriptor is never consulted (preserves the source plugin's
 * asymmetric default, pinned by its own tests). Retiring a faction tombstones
 * its id (ids are never reused or renumbered) and drops its incident edges.
 *
 * Memory: one allocation in fbs_factions_create sized by the config; nothing
 * is allocated afterwards. No globals; distinct tables are independent.
 * Errors: negative statuses leave outputs untouched except FBS_FACTION_E_TRUNCATED,
 * which reports the required size through the count/length output.
 */
#ifndef FBS_FACTIONS_H
#define FBS_FACTIONS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FBS_FACTION_VERSION 100 /* major * 10000 + minor * 100 + patch */

typedef unsigned fbs_faction_id;
#define FBS_FACTION_NONE 0u

typedef enum fbs_attitude {
  FBS_ATTITUDE_HOSTILE = -1,
  FBS_ATTITUDE_NEUTRAL = 0,
  FBS_ATTITUDE_FRIENDLY = 1
} fbs_attitude;

typedef enum fbs_faction_status {
  FBS_FACTION_OK = 0,
  FBS_FACTION_E_INVALID = -1,   /* NULL pointer, bad enum, bad id (0 or beyond count), empty key, from/to unknown */
  FBS_FACTION_E_NOT_FOUND = -2, /* unknown or retired faction / absent edge */
  FBS_FACTION_E_EXISTS = -3,    /* key already registered by a live faction; *out_id untouched */
  FBS_FACTION_E_FULL = -4,      /* max_factions, max_relations or max_key_bytes exhausted */
  FBS_FACTION_E_RANGE = -5,     /* faction id does not fit an 8-bit team id, or team id is FBS_TEAM_NONE */
  FBS_FACTION_E_SCHEMA = -6,    /* blob magic/version/length/consistency mismatch */
  FBS_FACTION_E_TRUNCATED = -7, /* output buffer too small; the required count/length is written */
  FBS_FACTION_E_MEMORY = -8     /* allocator returned NULL */
} fbs_faction_status;

const char *fbs_faction_status_name(int status);
unsigned fbs_faction_version(void);

typedef struct fbs_faction_allocator {
  void *(*alloc)(void *user, size_t bytes);
  void (*free)(void *user, void *ptr);
  void *user;
} fbs_faction_allocator;

typedef struct fbs_faction_config {
  unsigned max_factions;  /* >= 1, <= 1<<20 */
  unsigned max_relations; /* >= 0, <= 1<<22 */
  unsigned max_key_bytes; /* total UTF-8 key storage, >= 1, <= 1<<26 */
  int default_attitude;   /* fbs_attitude used when neither an edge nor a descriptor decides */
} fbs_faction_config;

/* 64 factions, 512 relations, 4096 key bytes, FBS_ATTITUDE_NEUTRAL. */
fbs_faction_config fbs_faction_config_default(void);

typedef struct fbs_factions fbs_factions;

fbs_faction_status fbs_factions_create(const fbs_faction_config *cfg,
                                       const fbs_faction_allocator *alloc,
                                       fbs_factions **out);
void fbs_factions_destroy(fbs_factions *f);
/* Removes every faction and relation; ids restart at 1; default attitude kept. */
void fbs_factions_clear(fbs_factions *f);
size_t fbs_factions_memory(const fbs_factions *f);
fbs_faction_status fbs_factions_set_default(fbs_factions *f, int attitude);
int fbs_factions_default(const fbs_factions *f);

typedef struct fbs_faction_desc {
  int self_attitude;     /* attitude toward own faction without an explicit self edge */
  int external_attitude; /* attitude toward every other faction without an explicit edge */
} fbs_faction_desc;

/* Keys are byte strings (UTF-8 by convention), compared bytewise, 1..255 bytes,
 * copied into the table. A retired faction releases its key for re-registration
 * under a NEW id; its arena bytes are reclaimed only by fbs_factions_clear, so
 * unbounded add/retire churn eventually returns FBS_FACTION_E_FULL. */
fbs_faction_status fbs_faction_add(fbs_factions *f, const char *key, size_t key_len,
                                   const fbs_faction_desc *desc, fbs_faction_id *out_id);
fbs_faction_status fbs_faction_set(fbs_factions *f, fbs_faction_id id, const fbs_faction_desc *desc);
fbs_faction_status fbs_faction_get(const fbs_factions *f, fbs_faction_id id, fbs_faction_desc *out);
fbs_faction_status fbs_faction_find(const fbs_factions *f, const char *key, size_t key_len,
                                    fbs_faction_id *out_id);
/* Pointer into the table's key storage, valid until the table is cleared or destroyed. */
fbs_faction_status fbs_faction_key(const fbs_factions *f, fbs_faction_id id,
                                   const char **out_key, size_t *out_len);
/* Ids issued so far (live + retired); valid ids are 1..count. */
unsigned fbs_faction_count(const fbs_factions *f);
unsigned fbs_faction_live_count(const fbs_factions *f);
int fbs_faction_is_live(const fbs_factions *f, fbs_faction_id id); /* 1, 0, or negative on error */
/* Tombstones the id and removes every edge that touches it. */
fbs_faction_status fbs_faction_retire(fbs_factions *f, fbs_faction_id id);

/* Directed edges. set overwrites an existing edge. clear returns E_NOT_FOUND when absent. */
fbs_faction_status fbs_relation_set(fbs_factions *f, fbs_faction_id from, fbs_faction_id to, int attitude);
fbs_faction_status fbs_relation_set_symmetric(fbs_factions *f, fbs_faction_id a, fbs_faction_id b, int attitude);
fbs_faction_status fbs_relation_clear(fbs_factions *f, fbs_faction_id from, fbs_faction_id to);
/* Clears both directions; OK when at least one existed, E_NOT_FOUND when neither did. */
fbs_faction_status fbs_relation_clear_symmetric(fbs_factions *f, fbs_faction_id a, fbs_faction_id b);
unsigned fbs_relation_count(const fbs_factions *f);

typedef struct fbs_relation {
  fbs_faction_id from, to;
  int attitude;
} fbs_relation;

/* All edges sorted by (from, to). E_TRUNCATED with *count = total when cap is too small. */
fbs_faction_status fbs_relation_list(const fbs_factions *f, fbs_relation *out, size_t cap, size_t *count);

/* Resolved attitude plus whether an explicit edge decided it. */
fbs_faction_status fbs_relation_get(const fbs_factions *f, fbs_faction_id from, fbs_faction_id to,
                                    int *out_attitude, int *out_explicit);

/* Resolution (see file header). Unknown/retired ids or NULL table yield the
 * table default (FBS_ATTITUDE_NEUTRAL for NULL); use the checked variant to detect that. */
int fbs_attitude_of(const fbs_factions *f, fbs_faction_id from, fbs_faction_id to);
fbs_faction_status fbs_attitude_of_checked(const fbs_factions *f, fbs_faction_id from,
                                           fbs_faction_id to, int *out_attitude);

typedef enum fbs_faction_direction {
  FBS_FACTION_TOWARD = 0, /* attitude of `faction` toward the listed factions */
  FBS_FACTION_FROM = 1    /* attitude of the listed factions toward `faction` */
} fbs_faction_direction;

/* Live factions (including `faction` itself) whose resolved attitude in the given
 * direction equals `attitude`, ascending ids. E_TRUNCATED with *count = total. */
fbs_faction_status fbs_factions_query(const fbs_factions *f, fbs_faction_id faction, int direction,
                                      int attitude, fbs_faction_id *out, size_t cap, size_t *count);

/* 8-bit team bridge (Unreal FGenericTeamId): team = id - 1 for ids 1..255; 255 is reserved. */
#define FBS_TEAM_NONE 255u
fbs_faction_status fbs_faction_to_team(const fbs_factions *f, fbs_faction_id id, unsigned char *out_team);
fbs_faction_status fbs_faction_from_team(const fbs_factions *f, unsigned char team, fbs_faction_id *out_id);

/* Deterministic little-endian schema (docs/decisions/factions.md section 8):
 * identical logical tables serialize to identical bytes regardless of insertion
 * order of relations. */
size_t fbs_factions_serialized_size(const fbs_factions *f);
fbs_faction_status fbs_factions_serialize(const fbs_factions *f, void *buf, size_t cap, size_t *out_len);
/* cfg NULL: capacities are the larger of the defaults and what the blob needs. */
fbs_faction_status fbs_factions_deserialize(const void *buf, size_t len, const fbs_faction_config *cfg,
                                            const fbs_faction_allocator *alloc, fbs_factions **out);

#ifdef __cplusplus
}
#endif
#endif /* FBS_FACTIONS_H */
