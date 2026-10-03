/*
 * src/factions.c — FinalBuildSystems faction registry and directed attitude
 * relations. Implements include/fbs/factions.h.
 *
 * Derived from PipeRift/FactionsExtension (Apache License 2.0, Copyright (c)
 * 2015-2018 Piperift) at commit f8194a7bb0195a14e81a7804b1a97544974adb2b,
 * rewritten in C with changed semantics; see third_party/piperift/NOTICE.md.
 * These files have been changed from the original work: this is a from-scratch
 * C implementation of a deliberately different model (strictly directed edges,
 * permanent dense ids with tombstones, a single allocation, an explicit
 * little-endian schema). It fixes defects of the source, among them a relation
 * set whose membership depended on insertion order, relations left dangling
 * after a faction was removed, team ids that shifted on removal, a team
 * conversion that aliased real factions past 256, and queries that silently
 * measured the attitude in the other direction.
 *
 * C99. Standard library only (no libm). No globals, no static mutable state.
 */

#include "fbs/factions.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Limits and small helpers                                                  */
/* ------------------------------------------------------------------------- */

#define FBS_FACTION_SCHEMA_VERSION 1u

#define FBS_FACTION_MAX_FACTIONS_LIMIT 1048576u  /* 1 << 20 */
#define FBS_FACTION_MAX_RELATIONS_LIMIT 4194304u /* 1 << 22 */
#define FBS_FACTION_MAX_KEY_BYTES_LIMIT 67108864u /* 1 << 26 */
#define FBS_FACTION_MAX_KEY_LEN 255u

#define FBS_FACTION_HEADER_BYTES 24u
#define FBS_FACTION_RECORD_BYTES 16u
#define FBS_RELATION_RECORD_BYTES 12u

#define FBS_FACTION_BLOCK_ALIGN 8u

static size_t fbs_align_up(size_t v) {
  return (v + (FBS_FACTION_BLOCK_ALIGN - 1u)) & ~(size_t)(FBS_FACTION_BLOCK_ALIGN - 1u);
}

static int fbs_is_attitude(int a) {
  return a == FBS_ATTITUDE_HOSTILE || a == FBS_ATTITUDE_NEUTRAL || a == FBS_ATTITUDE_FRIENDLY;
}

static void *fbs_faction_default_alloc(void *user, size_t bytes) {
  (void)user;
  return malloc(bytes);
}

static void fbs_faction_default_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

/* ------------------------------------------------------------------------- */
/* Internal representation                                                   */
/* ------------------------------------------------------------------------- */

typedef struct fbs_faction_slot {
  unsigned key_off; /* byte offset into the key arena (0 when not live)      */
  unsigned key_len; /* 1..255 while live, 0 once retired                     */
  signed char self_att;
  signed char ext_att;
  unsigned char live;
  unsigned char pad;
} fbs_faction_slot;

typedef struct fbs_faction_edge {
  unsigned from;
  unsigned to;
  signed char att;
  unsigned char pad[3];
} fbs_faction_edge;

struct fbs_factions {
  fbs_faction_allocator alloc;
  size_t block_size;
  unsigned max_factions;
  unsigned max_relations;
  unsigned max_key_bytes;
  unsigned hash_cap; /* power of two, >= 2 * max_factions */
  unsigned count;    /* ids issued so far (live + retired) */
  unsigned live;     /* live factions */
  unsigned rel_count;
  unsigned key_top;        /* bump pointer into the key arena */
  unsigned live_key_bytes; /* sum of key_len over live factions */
  signed char default_att;
  fbs_faction_slot *slots; /* slots[id - 1] */
  fbs_faction_edge *edges; /* sorted ascending by (from, to) */
  unsigned *hash;          /* open addressing, 0 = empty, else a faction id */
  char *keys;
};

/* ------------------------------------------------------------------------- */
/* Status names and version                                                  */
/* ------------------------------------------------------------------------- */

const char *fbs_faction_status_name(int status) {
  switch (status) {
    case FBS_FACTION_OK: return "ok";
    case FBS_FACTION_E_INVALID: return "invalid";
    case FBS_FACTION_E_NOT_FOUND: return "not_found";
    case FBS_FACTION_E_EXISTS: return "exists";
    case FBS_FACTION_E_FULL: return "full";
    case FBS_FACTION_E_RANGE: return "range";
    case FBS_FACTION_E_SCHEMA: return "schema";
    case FBS_FACTION_E_TRUNCATED: return "truncated";
    case FBS_FACTION_E_MEMORY: return "memory";
    default: return "unknown";
  }
}

unsigned fbs_faction_version(void) { return FBS_FACTION_VERSION; }

fbs_faction_config fbs_faction_config_default(void) {
  fbs_faction_config cfg;
  cfg.max_factions = 64u;
  cfg.max_relations = 512u;
  cfg.max_key_bytes = 4096u;
  cfg.default_attitude = FBS_ATTITUDE_NEUTRAL;
  return cfg;
}

static int fbs_config_valid(const fbs_faction_config *cfg) {
  if (cfg->max_factions < 1u || cfg->max_factions > FBS_FACTION_MAX_FACTIONS_LIMIT) return 0;
  if (cfg->max_relations > FBS_FACTION_MAX_RELATIONS_LIMIT) return 0;
  if (cfg->max_key_bytes < 1u || cfg->max_key_bytes > FBS_FACTION_MAX_KEY_BYTES_LIMIT) return 0;
  if (!fbs_is_attitude(cfg->default_attitude)) return 0;
  return 1;
}

/* ------------------------------------------------------------------------- */
/* Key hash index (insert only; ids are never reused, so no deletion needed)  */
/* ------------------------------------------------------------------------- */

static unsigned fbs_key_hash(const char *key, size_t len) {
  unsigned h = 2166136261u; /* FNV-1a, 32 bit */
  size_t i;
  for (i = 0; i < len; ++i) {
    h ^= (unsigned)(unsigned char)key[i];
    h *= 16777619u;
  }
  return h;
}

static unsigned fbs_hash_cap_for(unsigned max_factions) {
  unsigned need = max_factions * 2u; /* <= 1 << 21, cannot overflow */
  unsigned cap = 8u;
  while (cap < need) cap <<= 1;
  return cap;
}

static void fbs_hash_insert(fbs_factions *f, fbs_faction_id id) {
  const fbs_faction_slot *s = &f->slots[id - 1u];
  unsigned mask = f->hash_cap - 1u;
  unsigned i = fbs_key_hash(f->keys + s->key_off, s->key_len) & mask;
  while (f->hash[i] != 0u) i = (i + 1u) & mask;
  f->hash[i] = id;
}

/* Returns the live faction registered under `key`, or FBS_FACTION_NONE. */
static fbs_faction_id fbs_hash_find(const fbs_factions *f, const char *key, size_t len) {
  unsigned mask = f->hash_cap - 1u;
  unsigned i = fbs_key_hash(key, len) & mask;
  while (f->hash[i] != 0u) {
    fbs_faction_id id = f->hash[i];
    const fbs_faction_slot *s = &f->slots[id - 1u];
    if (s->live && (size_t)s->key_len == len && memcmp(f->keys + s->key_off, key, len) == 0) return id;
    i = (i + 1u) & mask;
  }
  return FBS_FACTION_NONE;
}

/* ------------------------------------------------------------------------- */
/* Relation storage: a sorted array, binary search, shifting insert           */
/* ------------------------------------------------------------------------- */

/* Returns 1 when the edge exists; *pos is its index, or the insertion point. */
static int fbs_edge_find(const fbs_factions *f, unsigned from, unsigned to, unsigned *pos) {
  unsigned lo = 0u, hi = f->rel_count;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo) / 2u;
    const fbs_faction_edge *e = &f->edges[mid];
    if (e->from < from || (e->from == from && e->to < to))
      lo = mid + 1u;
    else
      hi = mid;
  }
  *pos = lo;
  return lo < f->rel_count && f->edges[lo].from == from && f->edges[lo].to == to;
}

/* Caller has validated the ids and the attitude. */
static fbs_faction_status fbs_edge_set(fbs_factions *f, unsigned from, unsigned to, int attitude) {
  unsigned pos;
  if (fbs_edge_find(f, from, to, &pos)) {
    f->edges[pos].att = (signed char)attitude;
    return FBS_FACTION_OK;
  }
  if (f->rel_count >= f->max_relations) return FBS_FACTION_E_FULL;
  if (pos < f->rel_count)
    memmove(&f->edges[pos + 1u], &f->edges[pos], (size_t)(f->rel_count - pos) * sizeof(fbs_faction_edge));
  f->edges[pos].from = from;
  f->edges[pos].to = to;
  f->edges[pos].att = (signed char)attitude;
  f->edges[pos].pad[0] = 0;
  f->edges[pos].pad[1] = 0;
  f->edges[pos].pad[2] = 0;
  f->rel_count += 1u;
  return FBS_FACTION_OK;
}

static fbs_faction_status fbs_edge_clear(fbs_factions *f, unsigned from, unsigned to) {
  unsigned pos;
  if (!fbs_edge_find(f, from, to, &pos)) return FBS_FACTION_E_NOT_FOUND;
  if (pos + 1u < f->rel_count)
    memmove(&f->edges[pos], &f->edges[pos + 1u],
            (size_t)(f->rel_count - pos - 1u) * sizeof(fbs_faction_edge));
  f->rel_count -= 1u;
  return FBS_FACTION_OK;
}

/* ------------------------------------------------------------------------- */
/* Id validation                                                             */
/* ------------------------------------------------------------------------- */

/* FBS_FACTION_OK when `id` names a live faction, E_INVALID when the id was
 * never issued (0 or beyond the issued count), E_NOT_FOUND when it is retired. */
static fbs_faction_status fbs_check_id(const fbs_factions *f, fbs_faction_id id) {
  if (id == FBS_FACTION_NONE || id > f->count) return FBS_FACTION_E_INVALID;
  if (!f->slots[id - 1u].live) return FBS_FACTION_E_NOT_FOUND;
  return FBS_FACTION_OK;
}

static int fbs_is_live(const fbs_factions *f, fbs_faction_id id) {
  return id != FBS_FACTION_NONE && id <= f->count && f->slots[id - 1u].live;
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                  */
/* ------------------------------------------------------------------------- */

static size_t fbs_block_layout(const fbs_faction_config *cfg, unsigned hash_cap, size_t *off_slots,
                               size_t *off_edges, size_t *off_hash, size_t *off_keys) {
  size_t off = fbs_align_up(sizeof(struct fbs_factions));
  *off_slots = off;
  off = fbs_align_up(off + (size_t)cfg->max_factions * sizeof(fbs_faction_slot));
  *off_edges = off;
  off = fbs_align_up(off + (size_t)cfg->max_relations * sizeof(fbs_faction_edge));
  *off_hash = off;
  off = fbs_align_up(off + (size_t)hash_cap * sizeof(unsigned));
  *off_keys = off;
  off = fbs_align_up(off + (size_t)cfg->max_key_bytes);
  return off;
}

static fbs_faction_status fbs_table_create(const fbs_faction_config *cfg,
                                           const fbs_faction_allocator *alloc, fbs_factions **table) {
  fbs_faction_allocator a;
  size_t off_slots, off_edges, off_hash, off_keys, total;
  unsigned hash_cap;
  unsigned char *block;
  fbs_factions *f;

  if (alloc) {
    if (!alloc->alloc || !alloc->free) return FBS_FACTION_E_INVALID;
    a = *alloc;
  } else {
    a.alloc = fbs_faction_default_alloc;
    a.free = fbs_faction_default_free;
    a.user = NULL;
  }

  hash_cap = fbs_hash_cap_for(cfg->max_factions);
  total = fbs_block_layout(cfg, hash_cap, &off_slots, &off_edges, &off_hash, &off_keys);

  block = (unsigned char *)a.alloc(a.user, total);
  if (!block) return FBS_FACTION_E_MEMORY;
  memset(block, 0, total);

  f = (fbs_factions *)(void *)block;
  f->alloc = a;
  f->block_size = total;
  f->max_factions = cfg->max_factions;
  f->max_relations = cfg->max_relations;
  f->max_key_bytes = cfg->max_key_bytes;
  f->hash_cap = hash_cap;
  f->count = 0u;
  f->live = 0u;
  f->rel_count = 0u;
  f->key_top = 0u;
  f->live_key_bytes = 0u;
  f->default_att = (signed char)cfg->default_attitude;
  f->slots = (fbs_faction_slot *)(void *)(block + off_slots);
  f->edges = (fbs_faction_edge *)(void *)(block + off_edges);
  f->hash = (unsigned *)(void *)(block + off_hash);
  f->keys = (char *)(void *)(block + off_keys);

  *table = f;
  return FBS_FACTION_OK;
}

fbs_faction_status fbs_factions_create(const fbs_faction_config *cfg, const fbs_faction_allocator *alloc,
                                       fbs_factions **out) {
  fbs_faction_config c;
  fbs_factions *f = NULL;
  fbs_faction_status st;

  if (!out) return FBS_FACTION_E_INVALID;
  c = cfg ? *cfg : fbs_faction_config_default();
  if (!fbs_config_valid(&c)) return FBS_FACTION_E_INVALID;

  st = fbs_table_create(&c, alloc, &f);
  if (st != FBS_FACTION_OK) return st;
  *out = f;
  return FBS_FACTION_OK;
}

void fbs_factions_destroy(fbs_factions *f) {
  fbs_faction_allocator a;
  if (!f) return;
  a = f->alloc;
  a.free(a.user, f);
}

void fbs_factions_clear(fbs_factions *f) {
  if (!f) return;
  if (f->count > 0u) memset(f->slots, 0, (size_t)f->count * sizeof(fbs_faction_slot));
  memset(f->hash, 0, (size_t)f->hash_cap * sizeof(unsigned));
  f->count = 0u;
  f->live = 0u;
  f->rel_count = 0u;
  f->key_top = 0u;
  f->live_key_bytes = 0u;
  /* the default attitude is deliberately kept */
}

size_t fbs_factions_memory(const fbs_factions *f) { return f ? f->block_size : (size_t)0; }

fbs_faction_status fbs_factions_set_default(fbs_factions *f, int attitude) {
  if (!f || !fbs_is_attitude(attitude)) return FBS_FACTION_E_INVALID;
  f->default_att = (signed char)attitude;
  return FBS_FACTION_OK;
}

int fbs_factions_default(const fbs_factions *f) { return f ? (int)f->default_att : FBS_ATTITUDE_NEUTRAL; }

/* ------------------------------------------------------------------------- */
/* Factions                                                                  */
/* ------------------------------------------------------------------------- */

fbs_faction_status fbs_faction_add(fbs_factions *f, const char *key, size_t key_len,
                                   const fbs_faction_desc *desc, fbs_faction_id *out_id) {
  fbs_faction_slot *s;
  fbs_faction_id id;

  if (!f || !key || !desc || !out_id) return FBS_FACTION_E_INVALID;
  if (key_len == 0u || key_len > (size_t)FBS_FACTION_MAX_KEY_LEN) return FBS_FACTION_E_INVALID;
  if (!fbs_is_attitude(desc->self_attitude) || !fbs_is_attitude(desc->external_attitude))
    return FBS_FACTION_E_INVALID;

  if (fbs_hash_find(f, key, key_len) != FBS_FACTION_NONE) return FBS_FACTION_E_EXISTS;
  if (f->count >= f->max_factions) return FBS_FACTION_E_FULL;
  /* The key arena is a bump allocator that only fbs_factions_clear rewinds, so
   * retired keys still occupy their bytes; see fbs_faction_retire. */
  if (key_len > (size_t)(f->max_key_bytes - f->key_top)) return FBS_FACTION_E_FULL;

  id = f->count + 1u;
  s = &f->slots[f->count];
  s->key_off = f->key_top;
  s->key_len = (unsigned)key_len;
  s->self_att = (signed char)desc->self_attitude;
  s->ext_att = (signed char)desc->external_attitude;
  s->live = 1u;
  s->pad = 0u;
  memcpy(f->keys + f->key_top, key, key_len);
  f->key_top += (unsigned)key_len;
  f->live_key_bytes += (unsigned)key_len;
  f->count = id;
  f->live += 1u;
  fbs_hash_insert(f, id);

  *out_id = id;
  return FBS_FACTION_OK;
}

fbs_faction_status fbs_faction_set(fbs_factions *f, fbs_faction_id id, const fbs_faction_desc *desc) {
  fbs_faction_status st;
  if (!f || !desc) return FBS_FACTION_E_INVALID;
  if (!fbs_is_attitude(desc->self_attitude) || !fbs_is_attitude(desc->external_attitude))
    return FBS_FACTION_E_INVALID;
  st = fbs_check_id(f, id);
  if (st != FBS_FACTION_OK) return st;
  f->slots[id - 1u].self_att = (signed char)desc->self_attitude;
  f->slots[id - 1u].ext_att = (signed char)desc->external_attitude;
  return FBS_FACTION_OK;
}

fbs_faction_status fbs_faction_get(const fbs_factions *f, fbs_faction_id id, fbs_faction_desc *out) {
  fbs_faction_status st;
  if (!f || !out) return FBS_FACTION_E_INVALID;
  st = fbs_check_id(f, id);
  if (st != FBS_FACTION_OK) return st;
  out->self_attitude = (int)f->slots[id - 1u].self_att;
  out->external_attitude = (int)f->slots[id - 1u].ext_att;
  return FBS_FACTION_OK;
}

fbs_faction_status fbs_faction_find(const fbs_factions *f, const char *key, size_t key_len,
                                    fbs_faction_id *out_id) {
  fbs_faction_id id;
  if (!f || !key || !out_id) return FBS_FACTION_E_INVALID;
  if (key_len == 0u || key_len > (size_t)FBS_FACTION_MAX_KEY_LEN) return FBS_FACTION_E_INVALID;
  id = fbs_hash_find(f, key, key_len);
  if (id == FBS_FACTION_NONE) return FBS_FACTION_E_NOT_FOUND;
  *out_id = id;
  return FBS_FACTION_OK;
}

fbs_faction_status fbs_faction_key(const fbs_factions *f, fbs_faction_id id, const char **out_key,
                                   size_t *out_len) {
  fbs_faction_status st;
  if (!f || !out_key || !out_len) return FBS_FACTION_E_INVALID;
  st = fbs_check_id(f, id);
  if (st != FBS_FACTION_OK) return st;
  *out_key = f->keys + f->slots[id - 1u].key_off;
  *out_len = (size_t)f->slots[id - 1u].key_len;
  return FBS_FACTION_OK;
}

unsigned fbs_faction_count(const fbs_factions *f) { return f ? f->count : 0u; }

unsigned fbs_faction_live_count(const fbs_factions *f) { return f ? f->live : 0u; }

int fbs_faction_is_live(const fbs_factions *f, fbs_faction_id id) {
  if (!f) return FBS_FACTION_E_INVALID;
  if (id == FBS_FACTION_NONE || id > f->count) return FBS_FACTION_E_INVALID;
  return f->slots[id - 1u].live ? 1 : 0;
}

fbs_faction_status fbs_faction_retire(fbs_factions *f, fbs_faction_id id) {
  fbs_faction_slot *s;
  fbs_faction_status st;
  unsigned r, w;

  if (!f) return FBS_FACTION_E_INVALID;
  st = fbs_check_id(f, id);
  if (st != FBS_FACTION_OK) return st;

  s = &f->slots[id - 1u];
  f->live_key_bytes -= s->key_len;
  /* Tombstone: the id stays issued, the key is released for re-registration
   * under a new id, and the descriptor is zeroed so the record has exactly one
   * canonical serialized form.
   *
   * The key ARENA bytes are deliberately NOT reclaimed: fbs_faction_key hands
   * out pointers into the arena that stay valid until the table is cleared or
   * destroyed, so compacting would invalidate them. The arena is a bump
   * allocator and only fbs_factions_clear resets it. Consequence a caller must
   * plan for: repeated add/retire churn consumes max_key_bytes and eventually
   * makes fbs_faction_add return E_FULL even though few factions are live.
   * A long-lived table that churns keys should be cleared and rebuilt, or
   * configured with max_key_bytes sized for the churn. */
  s->key_off = 0u;
  s->key_len = 0u;
  s->self_att = 0;
  s->ext_att = 0;
  s->live = 0u;
  f->live -= 1u;

  w = 0u;
  for (r = 0u; r < f->rel_count; ++r) {
    if (f->edges[r].from == id || f->edges[r].to == id) continue;
    if (w != r) f->edges[w] = f->edges[r];
    ++w;
  }
  f->rel_count = w;
  return FBS_FACTION_OK;
}

/* ------------------------------------------------------------------------- */
/* Relations                                                                 */
/* ------------------------------------------------------------------------- */

static fbs_faction_status fbs_check_pair(const fbs_factions *f, fbs_faction_id a, fbs_faction_id b) {
  fbs_faction_status st = fbs_check_id(f, a);
  if (st != FBS_FACTION_OK) return st;
  return fbs_check_id(f, b);
}

fbs_faction_status fbs_relation_set(fbs_factions *f, fbs_faction_id from, fbs_faction_id to,
                                    int attitude) {
  fbs_faction_status st;
  if (!f || !fbs_is_attitude(attitude)) return FBS_FACTION_E_INVALID;
  st = fbs_check_pair(f, from, to);
  if (st != FBS_FACTION_OK) return st;
  return fbs_edge_set(f, from, to, attitude);
}

fbs_faction_status fbs_relation_set_symmetric(fbs_factions *f, fbs_faction_id a, fbs_faction_id b,
                                              int attitude) {
  fbs_faction_status st;
  unsigned pos, needed;

  if (!f || !fbs_is_attitude(attitude)) return FBS_FACTION_E_INVALID;
  st = fbs_check_pair(f, a, b);
  if (st != FBS_FACTION_OK) return st;

  if (a == b) return fbs_edge_set(f, a, a, attitude);

  /* Reserve both edges before writing either, so a half-applied symmetric
   * relation is impossible. */
  needed = 0u;
  if (!fbs_edge_find(f, a, b, &pos)) needed += 1u;
  if (!fbs_edge_find(f, b, a, &pos)) needed += 1u;
  if ((size_t)f->rel_count + needed > (size_t)f->max_relations) return FBS_FACTION_E_FULL;

  st = fbs_edge_set(f, a, b, attitude);
  if (st != FBS_FACTION_OK) return st; /* unreachable: capacity was reserved */
  return fbs_edge_set(f, b, a, attitude);
}

fbs_faction_status fbs_relation_clear(fbs_factions *f, fbs_faction_id from, fbs_faction_id to) {
  fbs_faction_status st;
  if (!f) return FBS_FACTION_E_INVALID;
  st = fbs_check_pair(f, from, to);
  if (st != FBS_FACTION_OK) return st;
  return fbs_edge_clear(f, from, to);
}

/* Clears both directions. Partial success counts as success: OK when at least
 * one of the two edges existed and was removed, E_NOT_FOUND only when neither
 * direction existed. (a == b is a single edge, so it is just fbs_edge_clear.)
 * A caller that needs to know exactly which directions were present should ask
 * fbs_relation_get before clearing. */
fbs_faction_status fbs_relation_clear_symmetric(fbs_factions *f, fbs_faction_id a, fbs_faction_id b) {
  fbs_faction_status st, s1, s2;
  if (!f) return FBS_FACTION_E_INVALID;
  st = fbs_check_pair(f, a, b);
  if (st != FBS_FACTION_OK) return st;
  if (a == b) return fbs_edge_clear(f, a, a);
  s1 = fbs_edge_clear(f, a, b);
  s2 = fbs_edge_clear(f, b, a);
  return (s1 == FBS_FACTION_OK || s2 == FBS_FACTION_OK) ? FBS_FACTION_OK : FBS_FACTION_E_NOT_FOUND;
}

unsigned fbs_relation_count(const fbs_factions *f) { return f ? f->rel_count : 0u; }

fbs_faction_status fbs_relation_list(const fbs_factions *f, fbs_relation *out, size_t cap,
                                     size_t *count) {
  unsigned i;
  if (!f || !count) return FBS_FACTION_E_INVALID;
  if (!out && cap > 0u) return FBS_FACTION_E_INVALID;
  if ((size_t)f->rel_count > cap) {
    *count = (size_t)f->rel_count;
    return FBS_FACTION_E_TRUNCATED;
  }
  for (i = 0u; i < f->rel_count; ++i) {
    out[i].from = f->edges[i].from;
    out[i].to = f->edges[i].to;
    out[i].attitude = (int)f->edges[i].att;
  }
  *count = (size_t)f->rel_count;
  return FBS_FACTION_OK;
}

/* ------------------------------------------------------------------------- */
/* Resolution                                                                */
/* ------------------------------------------------------------------------- */

/* The three-step resolution: explicit edge, then the SOURCE descriptor, then
 * the table default. The target descriptor is never consulted. Both ids must
 * be live; the table default is what a caller gets when one of them is not
 * (see fbs_attitude_of). */
static int fbs_resolve(const fbs_factions *f, fbs_faction_id from, fbs_faction_id to,
                       int *out_explicit) {
  const fbs_faction_slot *s;
  unsigned pos;
  if (fbs_edge_find(f, from, to, &pos)) {
    if (out_explicit) *out_explicit = 1;
    return (int)f->edges[pos].att;
  }
  if (out_explicit) *out_explicit = 0;
  s = &f->slots[from - 1u];
  return from == to ? (int)s->self_att : (int)s->ext_att;
}

fbs_faction_status fbs_relation_get(const fbs_factions *f, fbs_faction_id from, fbs_faction_id to,
                                    int *out_attitude, int *out_explicit) {
  fbs_faction_status st;
  int att, expl;
  if (!f || !out_attitude || !out_explicit) return FBS_FACTION_E_INVALID;
  st = fbs_check_pair(f, from, to);
  if (st != FBS_FACTION_OK) return st;
  att = fbs_resolve(f, from, to, &expl);
  *out_attitude = att;
  *out_explicit = expl;
  return FBS_FACTION_OK;
}

int fbs_attitude_of(const fbs_factions *f, fbs_faction_id from, fbs_faction_id to) {
  if (!f) return FBS_ATTITUDE_NEUTRAL;
  /* Exactly the case fbs_attitude_of_checked reports as E_NOT_FOUND. */
  if (!fbs_is_live(f, from) || !fbs_is_live(f, to)) return (int)f->default_att;
  return fbs_resolve(f, from, to, NULL);
}

fbs_faction_status fbs_attitude_of_checked(const fbs_factions *f, fbs_faction_id from,
                                           fbs_faction_id to, int *out_attitude) {
  if (!f || !out_attitude) return FBS_FACTION_E_INVALID;
  /* Unknown *and* retired ids are both reported as E_NOT_FOUND here; this is
   * the one entry point where an id of 0 or beyond the count is not
   * E_INVALID (include/fbs/factions.h: "use the checked variant to detect
   * that"). */
  if (!fbs_is_live(f, from) || !fbs_is_live(f, to)) return FBS_FACTION_E_NOT_FOUND;
  *out_attitude = fbs_resolve(f, from, to, NULL);
  return FBS_FACTION_OK;
}

fbs_faction_status fbs_factions_query(const fbs_factions *f, fbs_faction_id faction, int direction,
                                      int attitude, fbs_faction_id *out, size_t cap, size_t *count) {
  fbs_faction_status st;
  size_t total = 0u;
  unsigned id;

  if (!f || !count) return FBS_FACTION_E_INVALID;
  if (!out && cap > 0u) return FBS_FACTION_E_INVALID;
  if (direction != FBS_FACTION_TOWARD && direction != FBS_FACTION_FROM) return FBS_FACTION_E_INVALID;
  if (!fbs_is_attitude(attitude)) return FBS_FACTION_E_INVALID;
  st = fbs_check_id(f, faction);
  if (st != FBS_FACTION_OK) return st;

  for (id = 1u; id <= f->count; ++id) {
    int a;
    if (!f->slots[id - 1u].live) continue;
    a = (direction == FBS_FACTION_TOWARD) ? fbs_resolve(f, faction, id, NULL)
                                          : fbs_resolve(f, id, faction, NULL);
    if (a == attitude) ++total;
  }
  if (total > cap) {
    *count = total;
    return FBS_FACTION_E_TRUNCATED;
  }
  total = 0u;
  for (id = 1u; id <= f->count; ++id) {
    int a;
    if (!f->slots[id - 1u].live) continue;
    a = (direction == FBS_FACTION_TOWARD) ? fbs_resolve(f, faction, id, NULL)
                                          : fbs_resolve(f, id, faction, NULL);
    if (a == attitude) out[total++] = id;
  }
  *count = total;
  return FBS_FACTION_OK;
}

/* ------------------------------------------------------------------------- */
/* 8-bit team bridge                                                         */
/* ------------------------------------------------------------------------- */

fbs_faction_status fbs_faction_to_team(const fbs_factions *f, fbs_faction_id id,
                                       unsigned char *out_team) {
  fbs_faction_status st;
  if (!f || !out_team) return FBS_FACTION_E_INVALID;
  st = fbs_check_id(f, id);
  if (st != FBS_FACTION_OK) return st;
  if (id > 255u) return FBS_FACTION_E_RANGE;
  *out_team = (unsigned char)(id - 1u);
  return FBS_FACTION_OK;
}

fbs_faction_status fbs_faction_from_team(const fbs_factions *f, unsigned char team,
                                         fbs_faction_id *out_id) {
  fbs_faction_id id;
  if (!f || !out_id) return FBS_FACTION_E_INVALID;
  if ((unsigned)team == FBS_TEAM_NONE) return FBS_FACTION_E_RANGE;
  id = (fbs_faction_id)team + 1u;
  if (!fbs_is_live(f, id)) return FBS_FACTION_E_NOT_FOUND;
  *out_id = id;
  return FBS_FACTION_OK;
}

/* ------------------------------------------------------------------------- */
/* Serialization                                                             */
/* ------------------------------------------------------------------------- */

static void fbs_put_u16(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static void fbs_put_u32(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
  p[2] = (unsigned char)((v >> 16) & 0xffu);
  p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static unsigned fbs_get_u16(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned fbs_get_u32(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int fbs_get_i8(const unsigned char *p) {
  unsigned v = (unsigned)*p;
  return v > 127u ? (int)v - 256 : (int)v;
}

size_t fbs_factions_serialized_size(const fbs_factions *f) {
  if (!f) return 0u;
  return (size_t)FBS_FACTION_HEADER_BYTES + (size_t)f->count * FBS_FACTION_RECORD_BYTES +
         (size_t)f->rel_count * FBS_RELATION_RECORD_BYTES + (size_t)f->live_key_bytes;
}

fbs_faction_status fbs_factions_serialize(const fbs_factions *f, void *buf, size_t cap,
                                          size_t *out_len) {
  unsigned char *p;
  size_t need, off;
  unsigned i, koff;

  if (!f || !out_len) return FBS_FACTION_E_INVALID;
  if (!buf && cap > 0u) return FBS_FACTION_E_INVALID;
  need = fbs_factions_serialized_size(f);
  if (cap < need) {
    *out_len = need;
    return FBS_FACTION_E_TRUNCATED;
  }

  p = (unsigned char *)buf;
  p[0] = 'F';
  p[1] = 'B';
  p[2] = 'S';
  p[3] = 'F';
  fbs_put_u16(p + 4, FBS_FACTION_SCHEMA_VERSION);
  fbs_put_u16(p + 6, 0u);
  fbs_put_u32(p + 8, f->count);
  fbs_put_u32(p + 12, f->rel_count);
  fbs_put_u32(p + 16, f->live_key_bytes);
  p[20] = (unsigned char)((unsigned)(int)f->default_att & 0xffu);
  p[21] = 0;
  p[22] = 0;
  p[23] = 0;

  off = FBS_FACTION_HEADER_BYTES;
  koff = 0u;
  for (i = 0u; i < f->count; ++i) {
    const fbs_faction_slot *s = &f->slots[i];
    unsigned char *r = p + off;
    if (s->live) {
      fbs_put_u32(r, koff);
      fbs_put_u32(r + 4, s->key_len);
      koff += s->key_len;
    } else {
      fbs_put_u32(r, 0u);
      fbs_put_u32(r + 4, 0u);
    }
    r[8] = (unsigned char)((unsigned)(int)s->self_att & 0xffu);
    r[9] = (unsigned char)((unsigned)(int)s->ext_att & 0xffu);
    r[10] = s->live ? (unsigned char)0u : (unsigned char)1u;
    r[11] = 0u;
    fbs_put_u32(r + 12, 0u);
    off += FBS_FACTION_RECORD_BYTES;
  }

  for (i = 0u; i < f->rel_count; ++i) {
    unsigned char *r = p + off;
    fbs_put_u32(r, f->edges[i].from);
    fbs_put_u32(r + 4, f->edges[i].to);
    r[8] = (unsigned char)((unsigned)(int)f->edges[i].att & 0xffu);
    r[9] = 0u;
    r[10] = 0u;
    r[11] = 0u;
    off += FBS_RELATION_RECORD_BYTES;
  }

  for (i = 0u; i < f->count; ++i) {
    const fbs_faction_slot *s = &f->slots[i];
    if (!s->live) continue;
    memcpy(p + off, f->keys + s->key_off, (size_t)s->key_len);
    off += s->key_len;
  }

  *out_len = need;
  return FBS_FACTION_OK;
}

fbs_faction_status fbs_factions_deserialize(const void *buf, size_t len, const fbs_faction_config *cfg,
                                            const fbs_faction_allocator *alloc, fbs_factions **out) {
  const unsigned char *p = (const unsigned char *)buf;
  const unsigned char *frec, *rrec, *keys;
  unsigned fcount, rcount, kbytes, koff, i;
  unsigned prev_from = 0u, prev_to = 0u;
  int def, first_edge = 1;
  size_t need;
  fbs_faction_config c;
  fbs_factions *f = NULL;
  fbs_faction_status st;

  if (!buf || !out) return FBS_FACTION_E_INVALID;
  if (cfg && !fbs_config_valid(cfg)) return FBS_FACTION_E_INVALID;

  if (len < (size_t)FBS_FACTION_HEADER_BYTES) return FBS_FACTION_E_SCHEMA;
  if (p[0] != 'F' || p[1] != 'B' || p[2] != 'S' || p[3] != 'F') return FBS_FACTION_E_SCHEMA;
  if (fbs_get_u16(p + 4) != FBS_FACTION_SCHEMA_VERSION) return FBS_FACTION_E_SCHEMA;
  if (fbs_get_u16(p + 6) != 0u) return FBS_FACTION_E_SCHEMA;
  fcount = fbs_get_u32(p + 8);
  rcount = fbs_get_u32(p + 12);
  kbytes = fbs_get_u32(p + 16);
  def = fbs_get_i8(p + 20);
  if (!fbs_is_attitude(def)) return FBS_FACTION_E_SCHEMA;
  if (p[21] != 0u || p[22] != 0u || p[23] != 0u) return FBS_FACTION_E_SCHEMA;
  if (fcount > FBS_FACTION_MAX_FACTIONS_LIMIT || rcount > FBS_FACTION_MAX_RELATIONS_LIMIT ||
      kbytes > FBS_FACTION_MAX_KEY_BYTES_LIMIT)
    return FBS_FACTION_E_SCHEMA;

  need = (size_t)FBS_FACTION_HEADER_BYTES + (size_t)fcount * FBS_FACTION_RECORD_BYTES +
         (size_t)rcount * FBS_RELATION_RECORD_BYTES + (size_t)kbytes;
  if (len != need) return FBS_FACTION_E_SCHEMA;

  frec = p + FBS_FACTION_HEADER_BYTES;
  rrec = frec + (size_t)fcount * FBS_FACTION_RECORD_BYTES;
  keys = rrec + (size_t)rcount * FBS_RELATION_RECORD_BYTES;

  /* Faction records: canonical key layout, valid enums, zeroed reserved bytes. */
  koff = 0u;
  for (i = 0u; i < fcount; ++i) {
    const unsigned char *r = frec + (size_t)i * FBS_FACTION_RECORD_BYTES;
    unsigned ko = fbs_get_u32(r), kl = fbs_get_u32(r + 4);
    int sa = fbs_get_i8(r + 8), ea = fbs_get_i8(r + 9);
    unsigned retired = (unsigned)r[10];
    if (r[11] != 0u || fbs_get_u32(r + 12) != 0u) return FBS_FACTION_E_SCHEMA;
    if (retired > 1u) return FBS_FACTION_E_SCHEMA;
    if (retired) {
      if (ko != 0u || kl != 0u || sa != 0 || ea != 0) return FBS_FACTION_E_SCHEMA;
    } else {
      if (!fbs_is_attitude(sa) || !fbs_is_attitude(ea)) return FBS_FACTION_E_SCHEMA;
      if (kl == 0u || kl > FBS_FACTION_MAX_KEY_LEN) return FBS_FACTION_E_SCHEMA;
      if (ko != koff || kl > kbytes - koff) return FBS_FACTION_E_SCHEMA;
      koff += kl;
    }
  }
  if (koff != kbytes) return FBS_FACTION_E_SCHEMA;

  /* Relation records: live endpoints, valid enum, strictly ascending (from, to). */
  for (i = 0u; i < rcount; ++i) {
    const unsigned char *r = rrec + (size_t)i * FBS_RELATION_RECORD_BYTES;
    unsigned from = fbs_get_u32(r), to = fbs_get_u32(r + 4);
    int att = fbs_get_i8(r + 8);
    if (r[9] != 0u || r[10] != 0u || r[11] != 0u) return FBS_FACTION_E_SCHEMA;
    if (!fbs_is_attitude(att)) return FBS_FACTION_E_SCHEMA;
    if (from == 0u || from > fcount || to == 0u || to > fcount) return FBS_FACTION_E_SCHEMA;
    if (frec[(size_t)(from - 1u) * FBS_FACTION_RECORD_BYTES + 10u] != 0u) return FBS_FACTION_E_SCHEMA;
    if (frec[(size_t)(to - 1u) * FBS_FACTION_RECORD_BYTES + 10u] != 0u) return FBS_FACTION_E_SCHEMA;
    if (!first_edge && !(from > prev_from || (from == prev_from && to > prev_to)))
      return FBS_FACTION_E_SCHEMA;
    prev_from = from;
    prev_to = to;
    first_edge = 0;
  }

  if (cfg) {
    c = *cfg;
    if (c.max_factions < fcount || c.max_relations < rcount || c.max_key_bytes < kbytes)
      return FBS_FACTION_E_FULL;
  } else {
    c = fbs_faction_config_default();
    if (c.max_factions < fcount) c.max_factions = fcount;
    if (c.max_relations < rcount) c.max_relations = rcount;
    if (c.max_key_bytes < kbytes) c.max_key_bytes = kbytes;
  }
  c.default_attitude = def; /* the blob owns the default attitude */

  st = fbs_table_create(&c, alloc, &f);
  if (st != FBS_FACTION_OK) return st;

  f->count = fcount;
  for (i = 0u; i < fcount; ++i) {
    const unsigned char *r = frec + (size_t)i * FBS_FACTION_RECORD_BYTES;
    fbs_faction_slot *s = &f->slots[i];
    unsigned kl = fbs_get_u32(r + 4);
    if (r[10] != 0u) continue; /* retired: the slot is already zeroed */
    if (fbs_hash_find(f, (const char *)keys + fbs_get_u32(r), (size_t)kl) != FBS_FACTION_NONE) {
      fbs_factions_destroy(f);
      return FBS_FACTION_E_SCHEMA; /* duplicate live key */
    }
    s->key_off = f->key_top;
    s->key_len = kl;
    s->self_att = (signed char)fbs_get_i8(r + 8);
    s->ext_att = (signed char)fbs_get_i8(r + 9);
    s->live = 1u;
    memcpy(f->keys + f->key_top, keys + fbs_get_u32(r), (size_t)kl);
    f->key_top += kl;
    f->live_key_bytes += kl;
    f->live += 1u;
    fbs_hash_insert(f, i + 1u);
  }

  for (i = 0u; i < rcount; ++i) {
    const unsigned char *r = rrec + (size_t)i * FBS_RELATION_RECORD_BYTES;
    f->edges[i].from = fbs_get_u32(r);
    f->edges[i].to = fbs_get_u32(r + 4);
    f->edges[i].att = (signed char)fbs_get_i8(r + 8);
    f->edges[i].pad[0] = 0;
    f->edges[i].pad[1] = 0;
    f->edges[i].pad[2] = 0;
  }
  f->rel_count = rcount;

  *out = f;
  return FBS_FACTION_OK;
}
