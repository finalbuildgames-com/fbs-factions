/*
 * tests/test_factions.c — witnesses for include/fbs/factions.h.
 *
 * Self-contained: no test framework. Exit code = number of failures (clamped
 * to 100 so it survives the 8-bit exit status; the true count is printed).
 *
 * Covers the test plan witnesses F-T1..F-T9, F-T11, F-T12 (F-T10, a
 * solver-lifetime check for an Unreal integration, is out of scope for the C
 * library), plus allocator
 * failure, every capacity exhaustion, every E_TRUNCATED path, NULL/bad-enum
 * validation on every entry point, the status-name/version functions and a
 * committed golden serialization fixture.
 *
 *   ./fbs_test_factions                     compare against tests/fixtures/factions/table.bin
 *   ./fbs_test_factions --write-fixtures    rewrite that file
 *   ./fbs_test_factions --fixture-dir DIR   look for fixtures under DIR
 */

#include "fbs/factions.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Harness                                                                   */
/* ------------------------------------------------------------------------- */

static int g_checks = 0;
static int g_fails = 0;

static void check_impl(int cond, const char *expr, const char *file, int line) {
  ++g_checks;
  if (!cond) {
    ++g_fails;
    printf("FAIL %s:%d: %s\n", file, line, expr);
  }
}

#define CHECK(expr) check_impl((expr) ? 1 : 0, #expr, __FILE__, __LINE__)

static const char *g_fixture_dir = "fixtures/factions";
static int g_write_fixtures = 0;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static fbs_faction_desc mk_desc(int self_att, int ext_att) {
  fbs_faction_desc d;
  d.self_attitude = self_att;
  d.external_attitude = ext_att;
  return d;
}

static fbs_faction_id add_faction(fbs_factions *f, const char *key, int self_att, int ext_att) {
  fbs_faction_desc d = mk_desc(self_att, ext_att);
  fbs_faction_id id = FBS_FACTION_NONE;
  CHECK(fbs_faction_add(f, key, strlen(key), &d, &id) == FBS_FACTION_OK);
  return id;
}

static fbs_factions *make_table(void) {
  fbs_factions *f = NULL;
  CHECK(fbs_factions_create(NULL, NULL, &f) == FBS_FACTION_OK);
  return f;
}

/* Serialize into a fresh malloc'd buffer. */
static unsigned char *serialize_alloc(const fbs_factions *f, size_t *out_len) {
  size_t need = fbs_factions_serialized_size(f);
  size_t written = 0;
  unsigned char *buf = (unsigned char *)malloc(need ? need : 1u);
  CHECK(buf != NULL);
  CHECK(fbs_factions_serialize(f, buf, need, &written) == FBS_FACTION_OK);
  CHECK(written == need);
  *out_len = need;
  return buf;
}

static void expect_schema_reject(const unsigned char *blob, size_t len, const char *what) {
  fbs_factions *f = (fbs_factions *)0x1;
  fbs_faction_status st = fbs_factions_deserialize(blob, len, NULL, NULL, &f);
  ++g_checks;
  if (st != FBS_FACTION_E_SCHEMA || f != (fbs_factions *)0x1) {
    ++g_fails;
    printf("FAIL schema rejection (%s): status %s, out %s\n", what, fbs_faction_status_name(st),
           f == (fbs_factions *)0x1 ? "untouched" : "WRITTEN");
    if (st == FBS_FACTION_OK) fbs_factions_destroy(f);
  }
}

/* Deterministic 32-bit xorshift. */
static uint32_t g_rng = 0x9e3779b9u;

static void rng_seed(uint32_t seed) { g_rng = seed ? seed : 1u; }

static uint32_t rng_next(void) {
  uint32_t x = g_rng;
  x = (uint32_t)(x ^ (x << 13));
  x = (uint32_t)(x ^ (x >> 17));
  x = (uint32_t)(x ^ (x << 5));
  g_rng = x;
  return x;
}

static unsigned rng_below(unsigned n) { return n ? (unsigned)(rng_next() % n) : 0u; }

/* Counting allocator. */
typedef struct {
  int allocs;
  int frees;
  size_t bytes;
  int budget; /* < 0: unlimited, else the number of allocations still allowed */
} counting_alloc;

static void *ca_alloc(void *user, size_t bytes) {
  counting_alloc *c = (counting_alloc *)user;
  if (c->budget == 0) return NULL;
  if (c->budget > 0) --c->budget;
  ++c->allocs;
  c->bytes += bytes;
  return malloc(bytes);
}

static void ca_free(void *user, void *ptr) {
  counting_alloc *c = (counting_alloc *)user;
  if (!ptr) return;
  ++c->frees;
  free(ptr);
}

/* ------------------------------------------------------------------------- */
/* F-T1 — asymmetric, source-driven default (the upstream compatibility anchor)*/
/* ------------------------------------------------------------------------- */

static void test_ft1_asymmetric_default(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id b = add_faction(f, "B", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);

  CHECK(fbs_attitude_of(f, a, b) == FBS_ATTITUDE_HOSTILE);
  CHECK(fbs_attitude_of(f, b, a) == FBS_ATTITUDE_FRIENDLY);
  CHECK(fbs_attitude_of(f, a, a) == FBS_ATTITUDE_FRIENDLY);
  CHECK(fbs_attitude_of(f, b, b) == FBS_ATTITUDE_HOSTILE);

  /* The target descriptor is never consulted, so no edge means no symmetry. */
  CHECK(fbs_attitude_of(f, a, b) != fbs_attitude_of(f, b, a));
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* F-T2 — an explicit edge beats the descriptor, in one direction only        */
/* ------------------------------------------------------------------------- */

static void test_ft2_edge_beats_descriptor(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id b = add_faction(f, "B", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);
  int att = 0, expl = -1;

  CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
  CHECK(fbs_attitude_of(f, a, b) == FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_attitude_of(f, b, a) == FBS_ATTITUDE_FRIENDLY);
  CHECK(fbs_relation_count(f) == 1u);

  CHECK(fbs_relation_get(f, a, b, &att, &expl) == FBS_FACTION_OK);
  CHECK(att == FBS_ATTITUDE_NEUTRAL);
  CHECK(expl == 1);
  CHECK(fbs_relation_get(f, b, a, &att, &expl) == FBS_FACTION_OK);
  CHECK(att == FBS_ATTITUDE_FRIENDLY);
  CHECK(expl == 0);

  /* set overwrites */
  CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 1u);
  CHECK(fbs_attitude_of(f, a, b) == FBS_ATTITUDE_FRIENDLY);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* F-T3 — the symmetric helper writes two edges                              */
/* ------------------------------------------------------------------------- */

static void test_ft3_symmetric_helper(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id b = add_faction(f, "B", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);
  int att = 0, expl = -1;

  CHECK(fbs_relation_set_symmetric(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 2u);
  CHECK(fbs_attitude_of(f, a, b) == FBS_ATTITUDE_HOSTILE);
  CHECK(fbs_attitude_of(f, b, a) == FBS_ATTITUDE_HOSTILE);

  CHECK(fbs_relation_clear(f, a, b) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 1u);
  /* A -> B now falls back to A's descriptor (external = Hostile) and is no
   * longer explicit; B -> A is still the surviving edge. */
  CHECK(fbs_relation_get(f, a, b, &att, &expl) == FBS_FACTION_OK);
  CHECK(att == FBS_ATTITUDE_HOSTILE);
  CHECK(expl == 0);
  CHECK(fbs_relation_get(f, b, a, &att, &expl) == FBS_FACTION_OK);
  CHECK(att == FBS_ATTITUDE_HOSTILE);
  CHECK(expl == 1);

  /* clearing an absent edge is E_NOT_FOUND */
  CHECK(fbs_relation_clear(f, a, b) == FBS_FACTION_E_NOT_FOUND);

  /* a == b writes exactly one edge */
  CHECK(fbs_relation_set_symmetric(f, a, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 2u);
  CHECK(fbs_attitude_of(f, a, a) == FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_relation_clear_symmetric(f, a, a) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 1u);

  /* clear_symmetric with only one direction present still succeeds */
  CHECK(fbs_relation_clear_symmetric(f, a, b) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 0u);
  CHECK(fbs_relation_clear_symmetric(f, a, b) == FBS_FACTION_E_NOT_FOUND);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* F-T4 — mixed directionality survives (insertion-order hazard of source)   */
/* ------------------------------------------------------------------------- */

static void check_mixed_pair(int reverse_insertion_order) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id b = add_faction(f, "B", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);
  fbs_faction_id c = add_faction(f, "C", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  fbs_relation edges[8];
  size_t n = 0;

  CHECK(fbs_relation_set(f, c, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK); /* bystander */

  if (reverse_insertion_order) {
    CHECK(fbs_relation_set(f, b, a, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
    CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  } else {
    CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
    CHECK(fbs_relation_set(f, b, a, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
  }
  CHECK(fbs_relation_count(f) == 3u);
  CHECK(fbs_attitude_of(f, a, b) == FBS_ATTITUDE_HOSTILE);
  CHECK(fbs_attitude_of(f, b, a) == FBS_ATTITUDE_FRIENDLY);

  /* set_symmetric replaces exactly those two edges and nothing else */
  CHECK(fbs_relation_set_symmetric(f, a, b, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 3u);
  CHECK(fbs_attitude_of(f, a, b) == FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_attitude_of(f, b, a) == FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_attitude_of(f, c, a) == FBS_ATTITUDE_NEUTRAL);

  CHECK(fbs_relation_list(f, edges, 8u, &n) == FBS_FACTION_OK);
  CHECK(n == 3u);
  /* sorted ascending by (from, to) */
  CHECK(edges[0].from == a && edges[0].to == b);
  CHECK(edges[1].from == b && edges[1].to == a);
  CHECK(edges[2].from == c && edges[2].to == a);
  fbs_factions_destroy(f);
}

static void test_ft4_mixed_directionality(void) {
  check_mixed_pair(0);
  check_mixed_pair(1);
}

/* ------------------------------------------------------------------------- */
/* F-T5 — an unknown faction is reported, not silently Neutral               */
/* ------------------------------------------------------------------------- */

static void test_ft5_unknown_is_reported(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id ghost = 99u;
  int att = 12345;

  CHECK(fbs_factions_set_default(f, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_factions_default(f) == FBS_ATTITUDE_HOSTILE);

  CHECK(fbs_attitude_of_checked(f, a, ghost, &att) == FBS_FACTION_E_NOT_FOUND);
  CHECK(att == 12345); /* output untouched */
  CHECK(fbs_attitude_of_checked(f, ghost, a, &att) == FBS_FACTION_E_NOT_FOUND);
  CHECK(fbs_attitude_of_checked(f, FBS_FACTION_NONE, a, &att) == FBS_FACTION_E_NOT_FOUND);

  /* the unchecked variant answers with the configured default */
  CHECK(fbs_attitude_of(f, a, ghost) == FBS_ATTITUDE_HOSTILE);
  CHECK(fbs_attitude_of(f, ghost, a) == FBS_ATTITUDE_HOSTILE);
  CHECK(fbs_attitude_of(f, FBS_FACTION_NONE, FBS_FACTION_NONE) == FBS_ATTITUDE_HOSTILE);
  /* a NULL table is always NEUTRAL */
  CHECK(fbs_attitude_of(NULL, a, a) == FBS_ATTITUDE_NEUTRAL);

  /* a retired faction is reported the same way */
  CHECK(fbs_faction_retire(f, a) == FBS_FACTION_OK);
  CHECK(fbs_attitude_of_checked(f, a, a, &att) == FBS_FACTION_E_NOT_FOUND);
  CHECK(fbs_attitude_of(f, a, a) == FBS_ATTITUDE_HOSTILE);

  CHECK(fbs_attitude_of_checked(f, a, a, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_attitude_of_checked(NULL, a, a, &att) == FBS_FACTION_E_INVALID);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* F-T6 — a self edge overrides self_attitude                                */
/* ------------------------------------------------------------------------- */

static void test_ft6_self_edge(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  int att = 0, expl = 0;

  CHECK(fbs_attitude_of(f, a, a) == FBS_ATTITUDE_FRIENDLY);
  CHECK(fbs_relation_set(f, a, a, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_attitude_of(f, a, a) == FBS_ATTITUDE_HOSTILE);
  CHECK(fbs_relation_get(f, a, a, &att, &expl) == FBS_FACTION_OK);
  CHECK(att == FBS_ATTITUDE_HOSTILE && expl == 1);
  CHECK(fbs_relation_clear(f, a, a) == FBS_FACTION_OK);
  CHECK(fbs_attitude_of(f, a, a) == FBS_ATTITUDE_FRIENDLY);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* F-T7 — id stability across retire and a serialization round trip          */
/* ------------------------------------------------------------------------- */

static void test_ft7_id_stability(void) {
  fbs_factions *f = make_table();
  fbs_factions *g = NULL;
  fbs_faction_id a, b, c, again = FBS_FACTION_NONE, found = FBS_FACTION_NONE;
  fbs_faction_desc d;
  const char *key = NULL;
  size_t klen = 0, blob_len = 0;
  unsigned char *blob;

  a = add_faction(f, "alpha", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  b = add_faction(f, "beta", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);
  c = add_faction(f, "gamma", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_FRIENDLY);
  CHECK(a == 1u && b == 2u && c == 3u);
  CHECK(fbs_relation_set(f, a, c, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
  CHECK(fbs_relation_set_symmetric(f, b, c, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 3u);

  CHECK(fbs_faction_retire(f, b) == FBS_FACTION_OK);
  CHECK(fbs_faction_count(f) == 3u); /* ids issued: unchanged */
  CHECK(fbs_faction_live_count(f) == 2u);
  CHECK(fbs_faction_is_live(f, b) == 0);
  CHECK(fbs_faction_is_live(f, c) == 1);
  CHECK(fbs_faction_find(f, "beta", 4u, &found) == FBS_FACTION_E_NOT_FOUND);
  CHECK(fbs_faction_find(f, "gamma", 5u, &found) == FBS_FACTION_OK);
  CHECK(found == c); /* id(C) unchanged */
  CHECK(fbs_faction_key(f, b, &key, &klen) == FBS_FACTION_E_NOT_FOUND);
  CHECK(fbs_faction_get(f, b, &d) == FBS_FACTION_E_NOT_FOUND);
  CHECK(fbs_relation_count(f) == 1u); /* both edges touching B are gone */

  /* the released key comes back under a NEW id */
  d = mk_desc(FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_faction_add(f, "beta", 4u, &d, &again) == FBS_FACTION_OK);
  CHECK(again == 4u);
  CHECK(again != b);

  blob = serialize_alloc(f, &blob_len);
  CHECK(fbs_factions_deserialize(blob, blob_len, NULL, NULL, &g) == FBS_FACTION_OK);
  CHECK(fbs_faction_count(g) == 4u);
  CHECK(fbs_faction_live_count(g) == 3u);
  CHECK(fbs_faction_is_live(g, b) == 0);
  CHECK(fbs_faction_find(g, "gamma", 5u, &found) == FBS_FACTION_OK);
  CHECK(found == c);
  CHECK(fbs_faction_get(g, a, &d) == FBS_FACTION_OK);
  CHECK(d.self_attitude == FBS_ATTITUDE_FRIENDLY && d.external_attitude == FBS_ATTITUDE_HOSTILE);
  CHECK(fbs_faction_key(g, a, &key, &klen) == FBS_FACTION_OK);
  CHECK(klen == 5u && memcmp(key, "alpha", 5u) == 0);
  CHECK(fbs_relation_count(g) == 1u);
  CHECK(fbs_attitude_of(g, a, c) == FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_attitude_of(g, c, a) == FBS_ATTITUDE_FRIENDLY);

  free(blob);
  fbs_factions_destroy(g);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* F-T8 — serialization determinism                                          */
/* ------------------------------------------------------------------------- */

/* Same logical table, relations inserted in two different orders. */
static fbs_factions *build_ft8_table(int order) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "alpha", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id b = add_faction(f, "beta", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);
  fbs_faction_id c = add_faction(f, "gamma", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);

  if (order == 0) {
    CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
    CHECK(fbs_relation_set(f, c, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
    CHECK(fbs_relation_set(f, b, c, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
    CHECK(fbs_relation_set(f, a, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
  } else {
    CHECK(fbs_relation_set(f, b, c, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
    CHECK(fbs_relation_set(f, a, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
    CHECK(fbs_relation_set(f, c, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
    CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  }
  return f;
}

/* An empty table is a header and nothing else, and it round trips. */
static void test_empty_round_trip(void) {
  fbs_factions *f = make_table();
  fbs_factions *g = NULL;
  unsigned char buf[64];
  size_t len = 0;

  CHECK(fbs_factions_set_default(f, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
  CHECK(fbs_factions_serialized_size(f) == 24u);
  CHECK(fbs_factions_serialize(f, buf, sizeof buf, &len) == FBS_FACTION_OK);
  CHECK(len == 24u);
  CHECK(fbs_factions_deserialize(buf, len, NULL, NULL, &g) == FBS_FACTION_OK);
  CHECK(fbs_faction_count(g) == 0u);
  CHECK(fbs_faction_live_count(g) == 0u);
  CHECK(fbs_relation_count(g) == 0u);
  CHECK(fbs_factions_default(g) == FBS_ATTITUDE_FRIENDLY);
  CHECK(fbs_attitude_of(g, 1u, 1u) == FBS_ATTITUDE_FRIENDLY);
  {
    size_t l2 = 0;
    unsigned char again[64];
    CHECK(fbs_factions_serialize(g, again, sizeof again, &l2) == FBS_FACTION_OK);
    CHECK(l2 == len && memcmp(again, buf, len) == 0);
  }
  /* a table that is entirely tombstones also round trips */
  {
    fbs_factions *h = make_table();
    fbs_factions *k = NULL;
    unsigned char hb[128];
    size_t hl = 0;
    (void)add_faction(h, "only", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_HOSTILE);
    CHECK(fbs_faction_retire(h, 1u) == FBS_FACTION_OK);
    CHECK(fbs_factions_serialized_size(h) == 24u + 16u);
    CHECK(fbs_factions_serialize(h, hb, sizeof hb, &hl) == FBS_FACTION_OK);
    CHECK(fbs_factions_deserialize(hb, hl, NULL, NULL, &k) == FBS_FACTION_OK);
    CHECK(fbs_faction_count(k) == 1u);
    CHECK(fbs_faction_live_count(k) == 0u);
    CHECK(fbs_faction_is_live(k, 1u) == 0);
    fbs_factions_destroy(k);
    fbs_factions_destroy(h);
  }
  fbs_factions_destroy(g);
  fbs_factions_destroy(f);
}

static void test_ft8_determinism(void) {
  fbs_factions *f0 = build_ft8_table(0);
  fbs_factions *f1 = build_ft8_table(1);
  fbs_factions *round = NULL;
  size_t l0 = 0, l1 = 0, l2 = 0;
  unsigned char *b0, *b1, *b2;

  b0 = serialize_alloc(f0, &l0);
  b1 = serialize_alloc(f1, &l1);
  CHECK(l0 == l1);
  CHECK(l0 > 0u && memcmp(b0, b1, l0) == 0);

  CHECK(fbs_factions_deserialize(b0, l0, NULL, NULL, &round) == FBS_FACTION_OK);
  b2 = serialize_alloc(round, &l2);
  CHECK(l2 == l0);
  CHECK(memcmp(b0, b2, l0) == 0); /* deser(ser(t)) is a fixed point */

  free(b0);
  free(b1);
  free(b2);
  fbs_factions_destroy(round);
  fbs_factions_destroy(f1);
  fbs_factions_destroy(f0);
}

/* ------------------------------------------------------------------------- */
/* F-T9 — team-id capacity                                                   */
/* ------------------------------------------------------------------------- */

static void test_ft9_team_ids(void) {
  fbs_faction_config cfg = fbs_faction_config_default();
  fbs_factions *f = NULL;
  fbs_faction_desc d = mk_desc(FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  unsigned i;
  unsigned char team = 0;
  fbs_faction_id id = FBS_FACTION_NONE;

  cfg.max_factions = 300u;
  cfg.max_key_bytes = 4096u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_OK);

  for (i = 1u; i <= 256u; ++i) {
    char key[16];
    fbs_faction_id got = FBS_FACTION_NONE;
    sprintf(key, "f%u", i);
    CHECK(fbs_faction_add(f, key, strlen(key), &d, &got) == FBS_FACTION_OK);
    CHECK(got == i);
  }

  for (i = 1u; i <= 255u; ++i) {
    CHECK(fbs_faction_to_team(f, i, &team) == FBS_FACTION_OK);
    CHECK((unsigned)team == i - 1u);
    CHECK((unsigned)team != FBS_TEAM_NONE);
  }
  CHECK(fbs_faction_to_team(f, 256u, &team) == FBS_FACTION_E_RANGE);

  /* the reserved team id is never a faction */
  CHECK(fbs_faction_from_team(f, (unsigned char)FBS_TEAM_NONE, &id) == FBS_FACTION_E_RANGE);
  CHECK(fbs_faction_from_team(f, 0u, &id) == FBS_FACTION_OK);
  CHECK(id == 1u);
  CHECK(fbs_faction_from_team(f, 254u, &id) == FBS_FACTION_OK);
  CHECK(id == 255u);

  /* a retired faction has no team id, and its team maps to nothing */
  CHECK(fbs_faction_retire(f, 10u) == FBS_FACTION_OK);
  CHECK(fbs_faction_to_team(f, 10u, &team) == FBS_FACTION_E_NOT_FOUND);
  CHECK(fbs_faction_from_team(f, 9u, &id) == FBS_FACTION_E_NOT_FOUND);

  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* F-T11 — query direction is explicit                                       */
/* ------------------------------------------------------------------------- */

static void test_ft11_query_direction(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id b = add_faction(f, "B", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);
  fbs_faction_id out[8];
  size_t n = 0;

  /* toward: att(A,A) = Friendly (self), att(A,B) = Hostile (external) */
  CHECK(fbs_factions_query(f, a, FBS_FACTION_TOWARD, FBS_ATTITUDE_HOSTILE, out, 8u, &n) ==
        FBS_FACTION_OK);
  CHECK(n == 1u && out[0] == b);
  CHECK(fbs_factions_query(f, a, FBS_FACTION_TOWARD, FBS_ATTITUDE_FRIENDLY, out, 8u, &n) ==
        FBS_FACTION_OK);
  CHECK(n == 1u && out[0] == a); /* the faction itself is listed */

  /* from: att(A,A) = Friendly, att(B,A) = Friendly (B's external) */
  CHECK(fbs_factions_query(f, a, FBS_FACTION_FROM, FBS_ATTITUDE_HOSTILE, out, 8u, &n) ==
        FBS_FACTION_OK);
  CHECK(n == 0u);
  CHECK(fbs_factions_query(f, a, FBS_FACTION_FROM, FBS_ATTITUDE_FRIENDLY, out, 8u, &n) ==
        FBS_FACTION_OK);
  CHECK(n == 2u && out[0] == a && out[1] == b); /* ascending ids */

  /* retired factions never appear */
  CHECK(fbs_faction_retire(f, b) == FBS_FACTION_OK);
  CHECK(fbs_factions_query(f, a, FBS_FACTION_FROM, FBS_ATTITUDE_FRIENDLY, out, 8u, &n) ==
        FBS_FACTION_OK);
  CHECK(n == 1u && out[0] == a);
  CHECK(fbs_factions_query(f, b, FBS_FACTION_TOWARD, FBS_ATTITUDE_FRIENDLY, out, 8u, &n) ==
        FBS_FACTION_E_NOT_FOUND);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* F-T12 — randomized property test against an independent reference model   */
/* ------------------------------------------------------------------------- */

#define REF_MAX_FACTIONS 64
#define REF_MAX_EDGES 512

typedef struct {
  unsigned count;
  int live[REF_MAX_FACTIONS + 1];
  int self_att[REF_MAX_FACTIONS + 1];
  int ext_att[REF_MAX_FACTIONS + 1];
  char key[REF_MAX_FACTIONS + 1][12];
  size_t key_len[REF_MAX_FACTIONS + 1];
  struct {
    unsigned from, to;
    int att;
  } edges[REF_MAX_EDGES];
  unsigned edge_count;
  int def;
} ref_table;

static int ref_edge_index(const ref_table *r, unsigned from, unsigned to) {
  unsigned i;
  for (i = 0u; i < r->edge_count; ++i)
    if (r->edges[i].from == from && r->edges[i].to == to) return (int)i;
  return -1;
}

static int ref_live(const ref_table *r, unsigned id) {
  return id >= 1u && id <= r->count && r->live[id];
}

/* The three-step resolution, written independently of src/factions.c. */
static int ref_attitude(const ref_table *r, unsigned from, unsigned to) {
  int idx;
  if (!ref_live(r, from) || !ref_live(r, to)) return r->def;
  idx = ref_edge_index(r, from, to);
  if (idx >= 0) return r->edges[idx].att;
  if (from == to) return r->self_att[from];
  return r->ext_att[from];
}

static void ref_set_edge(ref_table *r, unsigned from, unsigned to, int att) {
  int idx = ref_edge_index(r, from, to);
  if (idx >= 0) {
    r->edges[idx].att = att;
    return;
  }
  if (r->edge_count >= REF_MAX_EDGES) return;
  r->edges[r->edge_count].from = from;
  r->edges[r->edge_count].to = to;
  r->edges[r->edge_count].att = att;
  r->edge_count += 1u;
}

static void ref_clear_edge(ref_table *r, unsigned from, unsigned to) {
  int idx = ref_edge_index(r, from, to);
  if (idx < 0) return;
  r->edges[idx] = r->edges[r->edge_count - 1u];
  r->edge_count -= 1u;
}

static void ref_drop_incident(ref_table *r, unsigned id) {
  unsigned i = 0u;
  while (i < r->edge_count) {
    if (r->edges[i].from == id || r->edges[i].to == id) {
      r->edges[i] = r->edges[r->edge_count - 1u];
      r->edge_count -= 1u;
    } else {
      ++i;
    }
  }
}

static int attitude_from_index(unsigned i) {
  static const int table[3] = {FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_FRIENDLY};
  return table[i % 3u];
}

static void compare_against_reference(const fbs_factions *f, const ref_table *r, const char *stage) {
  unsigned x, y;
  int mismatch = 0;
  CHECK(fbs_faction_count(f) == r->count);
  for (x = 0u; x <= r->count + 1u; ++x) {
    for (y = 0u; y <= r->count + 1u; ++y) {
      int got = fbs_attitude_of(f, x, y);
      int want = ref_attitude(r, x, y);
      if (got != want) mismatch = 1;
    }
  }
  ++g_checks;
  if (mismatch) {
    ++g_fails;
    printf("FAIL fuzz (%s): resolved attitude disagrees with the reference model\n", stage);
  }
}

static void test_ft12_fuzz(void) {
  fbs_faction_config cfg = fbs_faction_config_default();
  fbs_factions *f = NULL;
  fbs_factions *g = NULL;
  ref_table ref;
  unsigned step;
  unsigned char *blob;
  size_t blob_len = 0;

  cfg.max_factions = REF_MAX_FACTIONS;
  cfg.max_relations = REF_MAX_EDGES;
  cfg.max_key_bytes = REF_MAX_FACTIONS * 12u;
  cfg.default_attitude = FBS_ATTITUDE_NEUTRAL;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_OK);

  memset(&ref, 0, sizeof ref);
  ref.def = FBS_ATTITUDE_NEUTRAL;
  rng_seed(0x5eed1234u);

  for (step = 0u; step < 3000u; ++step) {
    unsigned op = rng_below(6u);
    if (op == 0u && ref.count < REF_MAX_FACTIONS) { /* add */
      char key[12];
      fbs_faction_desc d;
      fbs_faction_id id = FBS_FACTION_NONE;
      unsigned n = ref.count + 1u;
      sprintf(key, "k%u", n);
      d = mk_desc(attitude_from_index(rng_next()), attitude_from_index(rng_next()));
      CHECK(fbs_faction_add(f, key, strlen(key), &d, &id) == FBS_FACTION_OK);
      CHECK(id == n);
      ref.count = n;
      ref.live[n] = 1;
      ref.self_att[n] = d.self_attitude;
      ref.ext_att[n] = d.external_attitude;
      ref.key_len[n] = strlen(key);
      memcpy(ref.key[n], key, ref.key_len[n]);
    } else if (op == 1u && ref.count > 0u) { /* retire, with the invariance check */
      unsigned victim = 1u + rng_below(ref.count);
      if (ref_live(&ref, victim)) {
        unsigned x, y;
        int before[REF_MAX_FACTIONS + 2][REF_MAX_FACTIONS + 2];
        int stable = 1;
        for (x = 0u; x <= ref.count; ++x)
          for (y = 0u; y <= ref.count; ++y) before[x][y] = fbs_attitude_of(f, x, y);
        CHECK(fbs_faction_retire(f, victim) == FBS_FACTION_OK);
        ref.live[victim] = 0;
        ref_drop_incident(&ref, victim);
        /* No pair that avoids the victim may change. */
        for (x = 1u; x <= ref.count; ++x) {
          for (y = 1u; y <= ref.count; ++y) {
            if (x == victim || y == victim) continue;
            if (fbs_attitude_of(f, x, y) != before[x][y]) stable = 0;
          }
        }
        ++g_checks;
        if (!stable) {
          ++g_fails;
          printf("FAIL fuzz: retiring %u changed an unrelated pair\n", victim);
        }
      } else {
        CHECK(fbs_faction_retire(f, victim) == FBS_FACTION_E_NOT_FOUND);
      }
    } else if (op == 2u && ref.count > 0u) { /* relation_set */
      unsigned from = 1u + rng_below(ref.count), to = 1u + rng_below(ref.count);
      int att = attitude_from_index(rng_next());
      fbs_faction_status st = fbs_relation_set(f, from, to, att);
      if (ref_live(&ref, from) && ref_live(&ref, to)) {
        if (ref_edge_index(&ref, from, to) >= 0 || ref.edge_count < REF_MAX_EDGES) {
          CHECK(st == FBS_FACTION_OK);
          ref_set_edge(&ref, from, to, att);
        } else {
          CHECK(st == FBS_FACTION_E_FULL);
        }
      } else {
        CHECK(st == FBS_FACTION_E_NOT_FOUND);
      }
    } else if (op == 3u && ref.count > 0u) { /* relation_clear */
      unsigned from = 1u + rng_below(ref.count), to = 1u + rng_below(ref.count);
      fbs_faction_status st = fbs_relation_clear(f, from, to);
      if (!ref_live(&ref, from) || !ref_live(&ref, to)) {
        CHECK(st == FBS_FACTION_E_NOT_FOUND);
      } else if (ref_edge_index(&ref, from, to) >= 0) {
        CHECK(st == FBS_FACTION_OK);
        ref_clear_edge(&ref, from, to);
      } else {
        CHECK(st == FBS_FACTION_E_NOT_FOUND);
      }
    } else if (op == 4u && ref.count > 0u) { /* relation_set_symmetric */
      unsigned a = 1u + rng_below(ref.count), b = 1u + rng_below(ref.count);
      int att = attitude_from_index(rng_next());
      fbs_faction_status st = fbs_relation_set_symmetric(f, a, b, att);
      if (!ref_live(&ref, a) || !ref_live(&ref, b)) {
        CHECK(st == FBS_FACTION_E_NOT_FOUND);
      } else {
        unsigned needed = 0u;
        if (a == b) {
          needed = (ref_edge_index(&ref, a, a) >= 0) ? 0u : 1u;
        } else {
          if (ref_edge_index(&ref, a, b) < 0) ++needed;
          if (ref_edge_index(&ref, b, a) < 0) ++needed;
        }
        if (ref.edge_count + needed <= REF_MAX_EDGES) {
          CHECK(st == FBS_FACTION_OK);
          ref_set_edge(&ref, a, b, att);
          ref_set_edge(&ref, b, a, att);
        } else {
          CHECK(st == FBS_FACTION_E_FULL);
        }
      }
    } else if (op == 5u) { /* set_default */
      int att = attitude_from_index(rng_next());
      CHECK(fbs_factions_set_default(f, att) == FBS_FACTION_OK);
      ref.def = att;
    }

    CHECK(fbs_relation_count(f) == ref.edge_count);
    if ((step % 25u) == 0u) compare_against_reference(f, &ref, "live");
  }

  compare_against_reference(f, &ref, "final");

  /* ser/deser is lossless */
  blob = serialize_alloc(f, &blob_len);
  CHECK(fbs_factions_deserialize(blob, blob_len, NULL, NULL, &g) == FBS_FACTION_OK);
  compare_against_reference(g, &ref, "round trip");
  CHECK(fbs_faction_live_count(g) == fbs_faction_live_count(f));
  CHECK(fbs_relation_count(g) == fbs_relation_count(f));
  {
    unsigned id;
    for (id = 1u; id <= fbs_faction_count(f); ++id) {
      const char *k1 = NULL, *k2 = NULL;
      size_t n1 = 0, n2 = 0;
      fbs_faction_status s1 = fbs_faction_key(f, id, &k1, &n1);
      fbs_faction_status s2 = fbs_faction_key(g, id, &k2, &n2);
      if (s1 != s2) CHECK(s1 == s2);
      if (s1 == FBS_FACTION_OK) {
        CHECK(n1 == n2 && memcmp(k1, k2, n1) == 0);
      }
    }
  }
  {
    size_t l2 = 0;
    unsigned char *b2 = serialize_alloc(g, &l2);
    CHECK(l2 == blob_len && memcmp(b2, blob, blob_len) == 0);
    free(b2);
  }
  free(blob);
  fbs_factions_destroy(g);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle, capacities, allocator                                          */
/* ------------------------------------------------------------------------- */

static void test_clear_and_defaults(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a, b, found = FBS_FACTION_NONE;

  CHECK(fbs_factions_default(f) == FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_factions_set_default(f, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);

  a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  b = add_faction(f, "B", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_faction_count(f) == 2u && fbs_relation_count(f) == 1u);

  fbs_factions_clear(f);
  CHECK(fbs_faction_count(f) == 0u);
  CHECK(fbs_faction_live_count(f) == 0u);
  CHECK(fbs_relation_count(f) == 0u);
  CHECK(fbs_factions_default(f) == FBS_ATTITUDE_HOSTILE); /* default is kept */
  CHECK(fbs_faction_find(f, "A", 1u, &found) == FBS_FACTION_E_NOT_FOUND);

  /* ids restart at 1 */
  a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  CHECK(a == 1u);
  CHECK(fbs_faction_find(f, "A", 1u, &found) == FBS_FACTION_OK && found == 1u);

  fbs_factions_clear(NULL); /* no-op, must not crash */
  fbs_factions_destroy(NULL);
  fbs_factions_destroy(f);
}

static void test_duplicate_keys(void) {
  fbs_factions *f = make_table();
  fbs_faction_desc d = mk_desc(FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  fbs_faction_id a = FBS_FACTION_NONE, sentinel = 0xABCDu;

  CHECK(fbs_faction_add(f, "dup", 3u, &d, &a) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "dup", 3u, &d, &sentinel) == FBS_FACTION_E_EXISTS);
  CHECK(sentinel == 0xABCDu); /* *out_id untouched */
  CHECK(fbs_faction_count(f) == 1u);

  /* keys are compared bytewise, embedded NULs and all */
  CHECK(fbs_faction_add(f, "du\0p", 4u, &d, &sentinel) == FBS_FACTION_OK);
  CHECK(fbs_faction_count(f) == 2u);
  CHECK(fbs_faction_add(f, "du\0p", 4u, &d, &sentinel) == FBS_FACTION_E_EXISTS);

  /* after retiring, the key is free again (new id) */
  CHECK(fbs_faction_retire(f, a) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "dup", 3u, &d, &sentinel) == FBS_FACTION_OK);
  CHECK(sentinel == 3u);
  CHECK(fbs_faction_retire(f, a) == FBS_FACTION_E_NOT_FOUND); /* already retired */
  fbs_factions_destroy(f);
}

static void test_capacities(void) {
  fbs_faction_config cfg = fbs_faction_config_default();
  fbs_factions *f = NULL;
  fbs_faction_desc d = mk_desc(FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  fbs_faction_id id = FBS_FACTION_NONE, a, b, c;

  /* max_factions */
  cfg.max_factions = 2u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "one", 3u, &d, &id) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "two", 3u, &d, &id) == FBS_FACTION_OK);
  id = 777u;
  CHECK(fbs_faction_add(f, "three", 5u, &d, &id) == FBS_FACTION_E_FULL);
  CHECK(id == 777u);
  CHECK(fbs_faction_count(f) == 2u);
  fbs_factions_destroy(f);

  /* max_key_bytes */
  cfg = fbs_faction_config_default();
  cfg.max_key_bytes = 8u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "abc", 3u, &d, &id) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "def", 3u, &d, &id) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "ghij", 4u, &d, &id) == FBS_FACTION_E_FULL);
  CHECK(fbs_faction_add(f, "gh", 2u, &d, &id) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "i", 1u, &d, &id) == FBS_FACTION_E_FULL);
  /* retiring does not reclaim arena bytes (documented; keys stay pointer stable) */
  CHECK(fbs_faction_retire(f, 1u) == FBS_FACTION_OK);
  CHECK(fbs_faction_add(f, "abc", 3u, &d, &id) == FBS_FACTION_E_FULL);
  fbs_factions_destroy(f);

  /* max_relations */
  cfg = fbs_faction_config_default();
  cfg.max_relations = 1u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_OK);
  a = add_faction(f, "a", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  b = add_faction(f, "b", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  c = add_faction(f, "c", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_set(f, b, c, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_E_FULL);
  CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK); /* overwrite fits */
  CHECK(fbs_relation_count(f) == 1u);
  /* symmetric is all-or-nothing */
  CHECK(fbs_relation_clear(f, a, b) == FBS_FACTION_OK);
  CHECK(fbs_relation_set_symmetric(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_E_FULL);
  CHECK(fbs_relation_count(f) == 0u);
  /* a == b needs only one slot */
  CHECK(fbs_relation_set_symmetric(f, a, a, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_count(f) == 1u);
  fbs_factions_destroy(f);

  /* max_relations == 0 is legal and immediately full */
  cfg = fbs_faction_config_default();
  cfg.max_relations = 0u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_OK);
  a = add_faction(f, "a", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_relation_set(f, a, a, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_E_FULL);
  fbs_factions_destroy(f);
}

static void test_allocator(void) {
  counting_alloc ctx;
  fbs_faction_allocator alloc;
  fbs_factions *f = (fbs_factions *)0x1;
  fbs_faction_config cfg = fbs_faction_config_default();
  size_t blob_len = 0;
  unsigned char *blob;

  alloc.alloc = ca_alloc;
  alloc.free = ca_free;
  alloc.user = &ctx;

  /* failure: E_MEMORY, *out untouched */
  memset(&ctx, 0, sizeof ctx);
  ctx.budget = 0;
  CHECK(fbs_factions_create(&cfg, &alloc, &f) == FBS_FACTION_E_MEMORY);
  CHECK(f == (fbs_factions *)0x1);
  CHECK(ctx.allocs == 0 && ctx.frees == 0);

  /* success: exactly one allocation, one free, and memory() reports the block */
  memset(&ctx, 0, sizeof ctx);
  ctx.budget = -1;
  f = NULL;
  CHECK(fbs_factions_create(&cfg, &alloc, &f) == FBS_FACTION_OK);
  CHECK(ctx.allocs == 1 && ctx.frees == 0);
  CHECK(fbs_factions_memory(f) == ctx.bytes);
  CHECK(fbs_factions_memory(NULL) == 0u);
  add_faction(f, "a", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  add_faction(f, "b", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_relation_set(f, 1u, 2u, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(ctx.allocs == 1); /* nothing is allocated after create */
  blob = serialize_alloc(f, &blob_len);
  fbs_factions_destroy(f);
  CHECK(ctx.allocs == 1 && ctx.frees == 1);

  /* deserialize honours the allocator and its failures too */
  memset(&ctx, 0, sizeof ctx);
  ctx.budget = 0;
  f = (fbs_factions *)0x1;
  CHECK(fbs_factions_deserialize(blob, blob_len, NULL, &alloc, &f) == FBS_FACTION_E_MEMORY);
  CHECK(f == (fbs_factions *)0x1);

  memset(&ctx, 0, sizeof ctx);
  ctx.budget = -1;
  f = NULL;
  CHECK(fbs_factions_deserialize(blob, blob_len, NULL, &alloc, &f) == FBS_FACTION_OK);
  CHECK(ctx.allocs == 1);
  fbs_factions_destroy(f);
  CHECK(ctx.frees == 1);

  /* a broken allocator struct is rejected */
  alloc.alloc = NULL;
  f = (fbs_factions *)0x1;
  CHECK(fbs_factions_create(&cfg, &alloc, &f) == FBS_FACTION_E_INVALID);
  CHECK(f == (fbs_factions *)0x1);
  alloc.alloc = ca_alloc;
  alloc.free = NULL;
  CHECK(fbs_factions_create(&cfg, &alloc, &f) == FBS_FACTION_E_INVALID);

  free(blob);
}

/* ------------------------------------------------------------------------- */
/* E_TRUNCATED on every list/serialize path                                   */
/* ------------------------------------------------------------------------- */

static void test_truncated(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id b = add_faction(f, "B", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_faction_id c = add_faction(f, "C", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  fbs_relation rels[4];
  fbs_faction_id ids[4];
  unsigned char buf[512];
  size_t n = 0, need = 0;

  CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_set(f, b, c, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_set(f, c, a, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);

  /* relation_list */
  memset(rels, 0x5a, sizeof rels);
  n = 999u;
  CHECK(fbs_relation_list(f, rels, 2u, &n) == FBS_FACTION_E_TRUNCATED);
  CHECK(n == 3u);
  CHECK(rels[0].from == 0x5a5a5a5au); /* nothing written */
  n = 999u;
  CHECK(fbs_relation_list(f, NULL, 0u, &n) == FBS_FACTION_E_TRUNCATED);
  CHECK(n == 3u);
  CHECK(fbs_relation_list(f, rels, 4u, &n) == FBS_FACTION_OK);
  CHECK(n == 3u);

  /* factions_query */
  memset(ids, 0x5a, sizeof ids);
  n = 999u;
  CHECK(fbs_factions_query(f, a, FBS_FACTION_TOWARD, FBS_ATTITUDE_HOSTILE, ids, 1u, &n) ==
        FBS_FACTION_E_TRUNCATED);
  CHECK(n == 2u); /* B and C are hostile to A's external attitude */
  CHECK(ids[0] == 0x5a5a5a5au);
  n = 999u;
  CHECK(fbs_factions_query(f, a, FBS_FACTION_TOWARD, FBS_ATTITUDE_HOSTILE, NULL, 0u, &n) ==
        FBS_FACTION_E_TRUNCATED);
  CHECK(n == 2u);
  CHECK(fbs_factions_query(f, a, FBS_FACTION_TOWARD, FBS_ATTITUDE_HOSTILE, ids, 4u, &n) ==
        FBS_FACTION_OK);
  CHECK(n == 2u && ids[0] == b && ids[1] == c);

  /* serialize */
  need = fbs_factions_serialized_size(f);
  CHECK(need > 0u && need < sizeof buf);
  memset(buf, 0x5a, sizeof buf);
  n = 999u;
  CHECK(fbs_factions_serialize(f, buf, need - 1u, &n) == FBS_FACTION_E_TRUNCATED);
  CHECK(n == need);
  CHECK(buf[0] == 0x5a); /* nothing written */
  n = 999u;
  CHECK(fbs_factions_serialize(f, NULL, 0u, &n) == FBS_FACTION_E_TRUNCATED);
  CHECK(n == need);
  CHECK(fbs_factions_serialize(f, buf, sizeof buf, &n) == FBS_FACTION_OK);
  CHECK(n == need);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* NULL pointers, bad enums, bad ids on every entry point                    */
/* ------------------------------------------------------------------------- */

static void test_invalid_arguments(void) {
  fbs_factions *f = make_table();
  fbs_faction_desc d = mk_desc(FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  fbs_faction_desc bad_self = mk_desc(7, FBS_ATTITUDE_NEUTRAL);
  fbs_faction_desc bad_ext = mk_desc(FBS_ATTITUDE_NEUTRAL, -9);
  fbs_faction_desc out_desc;
  fbs_faction_id a = add_faction(f, "A", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_NEUTRAL);
  fbs_faction_id id = FBS_FACTION_NONE;
  fbs_faction_config cfg;
  fbs_relation rels[2];
  const char *key = NULL;
  unsigned char team = 0;
  size_t n = 0;
  int att = 0, expl = 0;
  char long_key[300];

  memset(long_key, 'x', sizeof long_key);

  /* create */
  CHECK(fbs_factions_create(NULL, NULL, NULL) == FBS_FACTION_E_INVALID);
  cfg = fbs_faction_config_default();
  cfg.max_factions = 0u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_E_INVALID);
  cfg = fbs_faction_config_default();
  cfg.max_factions = (1u << 20) + 1u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_E_INVALID);
  cfg = fbs_faction_config_default();
  cfg.max_relations = (1u << 22) + 1u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_E_INVALID);
  cfg = fbs_faction_config_default();
  cfg.max_key_bytes = 0u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_E_INVALID);
  cfg = fbs_faction_config_default();
  cfg.max_key_bytes = (1u << 26) + 1u;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_E_INVALID);
  cfg = fbs_faction_config_default();
  cfg.default_attitude = 4;
  CHECK(fbs_factions_create(&cfg, NULL, &f) == FBS_FACTION_E_INVALID);

  /* set_default */
  CHECK(fbs_factions_set_default(NULL, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_set_default(f, 3) == FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_default(NULL) == FBS_ATTITUDE_NEUTRAL);

  /* add */
  CHECK(fbs_faction_add(NULL, "k", 1u, &d, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_add(f, NULL, 1u, &d, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_add(f, "k", 0u, &d, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_add(f, long_key, 256u, &d, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_add(f, "k", 1u, NULL, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_add(f, "k", 1u, &d, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_add(f, "k", 1u, &bad_self, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_add(f, "k", 1u, &bad_ext, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_add(f, long_key, 255u, &d, &id) == FBS_FACTION_OK); /* 255 is legal */

  /* set / get */
  CHECK(fbs_faction_set(NULL, a, &d) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_set(f, a, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_set(f, a, &bad_self) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_set(f, FBS_FACTION_NONE, &d) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_set(f, 999u, &d) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_get(NULL, a, &out_desc) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_get(f, a, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_get(f, 999u, &out_desc) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_set(f, a, &d) == FBS_FACTION_OK);
  CHECK(fbs_faction_get(f, a, &out_desc) == FBS_FACTION_OK);
  CHECK(out_desc.self_attitude == FBS_ATTITUDE_FRIENDLY);
  CHECK(out_desc.external_attitude == FBS_ATTITUDE_NEUTRAL);

  /* find / key */
  CHECK(fbs_faction_find(NULL, "A", 1u, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_find(f, NULL, 1u, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_find(f, "A", 1u, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_find(f, "A", 0u, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_find(f, long_key, 256u, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_find(f, "nope", 4u, &id) == FBS_FACTION_E_NOT_FOUND);
  CHECK(fbs_faction_key(NULL, a, &key, &n) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_key(f, a, NULL, &n) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_key(f, a, &key, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_key(f, 0u, &key, &n) == FBS_FACTION_E_INVALID);

  /* counts / liveness */
  CHECK(fbs_faction_count(NULL) == 0u);
  CHECK(fbs_faction_live_count(NULL) == 0u);
  CHECK(fbs_relation_count(NULL) == 0u);
  CHECK(fbs_faction_is_live(NULL, a) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_is_live(f, FBS_FACTION_NONE) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_is_live(f, 999u) == FBS_FACTION_E_INVALID);

  /* retire */
  CHECK(fbs_faction_retire(NULL, a) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_retire(f, FBS_FACTION_NONE) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_retire(f, 999u) == FBS_FACTION_E_INVALID);

  /* relations */
  CHECK(fbs_relation_set(NULL, a, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_set(f, a, a, 5) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_set(f, FBS_FACTION_NONE, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_set(f, a, 999u, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_set_symmetric(NULL, a, a, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_set_symmetric(f, a, a, 5) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_set_symmetric(f, a, 999u, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_clear(NULL, a, a) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_clear(f, a, 999u) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_clear_symmetric(NULL, a, a) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_clear_symmetric(f, 999u, a) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_list(NULL, rels, 2u, &n) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_list(f, rels, 2u, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_list(f, NULL, 2u, &n) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_get(NULL, a, a, &att, &expl) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_get(f, a, a, NULL, &expl) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_get(f, a, a, &att, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_relation_get(f, 999u, a, &att, &expl) == FBS_FACTION_E_INVALID);

  /* query */
  CHECK(fbs_factions_query(NULL, a, FBS_FACTION_TOWARD, FBS_ATTITUDE_NEUTRAL, &id, 1u, &n) ==
        FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_query(f, a, FBS_FACTION_TOWARD, FBS_ATTITUDE_NEUTRAL, &id, 1u, NULL) ==
        FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_query(f, a, FBS_FACTION_TOWARD, FBS_ATTITUDE_NEUTRAL, NULL, 1u, &n) ==
        FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_query(f, a, 7, FBS_ATTITUDE_NEUTRAL, &id, 1u, &n) == FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_query(f, a, FBS_FACTION_TOWARD, 42, &id, 1u, &n) == FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_query(f, 999u, FBS_FACTION_TOWARD, FBS_ATTITUDE_NEUTRAL, &id, 1u, &n) ==
        FBS_FACTION_E_INVALID);

  /* team bridge */
  CHECK(fbs_faction_to_team(NULL, a, &team) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_to_team(f, a, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_to_team(f, 999u, &team) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_from_team(NULL, 0u, &id) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_from_team(f, 0u, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_faction_from_team(f, 200u, &id) == FBS_FACTION_E_NOT_FOUND);

  /* serialize / deserialize */
  CHECK(fbs_factions_serialized_size(NULL) == 0u);
  CHECK(fbs_factions_serialize(NULL, rels, sizeof rels, &n) == FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_serialize(f, rels, sizeof rels, NULL) == FBS_FACTION_E_INVALID);
  CHECK(fbs_factions_serialize(f, NULL, 8u, &n) == FBS_FACTION_E_INVALID);
  {
    unsigned char probe[64];
    fbs_factions *tmp = (fbs_factions *)0x1;
    memset(probe, 0, sizeof probe);
    CHECK(fbs_factions_deserialize(NULL, 0u, NULL, NULL, &tmp) == FBS_FACTION_E_INVALID);
    CHECK(fbs_factions_deserialize(probe, sizeof probe, NULL, NULL, NULL) == FBS_FACTION_E_INVALID);
    CHECK(tmp == (fbs_factions *)0x1);
  }

  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* Schema rejection                                                          */
/* ------------------------------------------------------------------------- */

static void test_schema_rejection(void) {
  fbs_factions *f = make_table();
  fbs_factions *g = NULL;
  fbs_faction_id a, b, c;
  size_t len = 0;
  unsigned char *blob;
  unsigned char *copy;
  fbs_faction_config cfg;

  a = add_faction(f, "alpha", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  b = add_faction(f, "beta", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);
  c = add_faction(f, "gamma", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_set(f, b, c, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
  blob = serialize_alloc(f, &len);
  copy = (unsigned char *)malloc(len);
  CHECK(copy != NULL);

  /* the pristine blob loads */
  CHECK(fbs_factions_deserialize(blob, len, NULL, NULL, &g) == FBS_FACTION_OK);
  fbs_factions_destroy(g);
  g = NULL;

  memcpy(copy, blob, len);
  copy[0] = 'X';
  expect_schema_reject(copy, len, "magic");

  memcpy(copy, blob, len);
  copy[4] = 2u;
  expect_schema_reject(copy, len, "version");

  memcpy(copy, blob, len);
  copy[6] = 1u;
  expect_schema_reject(copy, len, "flags");

  memcpy(copy, blob, len);
  copy[20] = 5u;
  expect_schema_reject(copy, len, "default attitude");

  memcpy(copy, blob, len);
  copy[21] = 1u;
  expect_schema_reject(copy, len, "header pad");

  memcpy(copy, blob, len);
  expect_schema_reject(copy, len - 1u, "truncated buffer");
  expect_schema_reject(copy, 4u, "short header");

  /* The length must match exactly: one trailing byte is not tolerated. */
  {
    unsigned char *longer = (unsigned char *)malloc(len + 1u);
    CHECK(longer != NULL);
    memcpy(longer, blob, len);
    longer[len] = 0u;
    expect_schema_reject(longer, len + 1u, "trailing byte (zero)");
    longer[len] = 0xffu;
    expect_schema_reject(longer, len + 1u, "trailing byte (non-zero)");
    free(longer);
  }

  memcpy(copy, blob, len);
  copy[8] = 200u; /* faction_count overruns the buffer */
  expect_schema_reject(copy, len, "faction count overrun");

  memcpy(copy, blob, len);
  copy[12] = 200u; /* relation_count overruns the buffer */
  expect_schema_reject(copy, len, "relation count overrun");

  memcpy(copy, blob, len);
  copy[16] = 200u; /* key_bytes overruns the buffer */
  expect_schema_reject(copy, len, "key bytes overrun");

  /* faction record 0: bad self attitude */
  memcpy(copy, blob, len);
  copy[24 + 8] = 9u;
  expect_schema_reject(copy, len, "faction attitude enum");

  /* faction record 0: reserved word must be zero */
  memcpy(copy, blob, len);
  copy[24 + 12] = 1u;
  expect_schema_reject(copy, len, "faction reserved");

  /* faction record 0: retired flag out of range */
  memcpy(copy, blob, len);
  copy[24 + 10] = 2u;
  expect_schema_reject(copy, len, "retired flag");

  /* faction record 0: key_off not canonical */
  memcpy(copy, blob, len);
  copy[24] = 1u;
  expect_schema_reject(copy, len, "non-canonical key offset");

  /* faction record 1 duplicates record 0's key: make both "alpha" by pointing
   * record 1 at the same length and shifting the key blob is not canonical, so
   * instead rebuild a table with two identical keys by hand below. */

  /* relation record 0: bad attitude */
  memcpy(copy, blob, len);
  copy[24 + 3u * 16u + 8u] = 9u;
  expect_schema_reject(copy, len, "relation attitude enum");

  /* relation record 0: pad must be zero */
  memcpy(copy, blob, len);
  copy[24 + 3u * 16u + 9u] = 1u;
  expect_schema_reject(copy, len, "relation pad");

  /* relation record 0: `from` out of range */
  memcpy(copy, blob, len);
  copy[24 + 3u * 16u] = 9u;
  expect_schema_reject(copy, len, "relation from out of range");

  /* relation record 0: `from` is zero */
  memcpy(copy, blob, len);
  copy[24 + 3u * 16u] = 0u;
  expect_schema_reject(copy, len, "relation from is none");

  /* swap the two relation records so they are no longer sorted */
  memcpy(copy, blob, len);
  {
    unsigned char tmp[12];
    unsigned char *r0 = copy + 24u + 3u * 16u;
    unsigned char *r1 = r0 + 12u;
    memcpy(tmp, r0, 12u);
    memcpy(r0, r1, 12u);
    memcpy(r1, tmp, 12u);
  }
  expect_schema_reject(copy, len, "unsorted relations");

  /* duplicate edge: make the second record equal to the first */
  memcpy(copy, blob, len);
  memcpy(copy + 24u + 3u * 16u + 12u, copy + 24u + 3u * 16u, 12u);
  expect_schema_reject(copy, len, "duplicate relation");

  /* An edge incident to a retired faction is inconsistent. Hand-built so the
   * blob is otherwise perfectly canonical and only that rule can reject it. */
  {
    unsigned char hand[24 + 2 * 16 + 12 + 2];
    size_t hlen = sizeof hand;
    fbs_factions *h = NULL;
    memset(hand, 0, hlen);
    hand[0] = 'F';
    hand[1] = 'B';
    hand[2] = 'S';
    hand[3] = 'F';
    hand[4] = 1u;  /* schema version */
    hand[8] = 2u;  /* faction_count */
    hand[12] = 1u; /* relation_count */
    hand[16] = 2u; /* key_bytes */
    hand[24 + 4] = 2u;              /* record 0: key_off 0, key_len 2, live */
    hand[24 + 16 + 10] = 1u;        /* record 1: retired, no key, no descriptor */
    hand[24 + 32 + 0] = 1u;         /* relation from = 1 */
    hand[24 + 32 + 4] = 2u;         /* relation to = 2 (retired) */
    hand[24 + 44 + 0] = 'a';
    hand[24 + 44 + 1] = 'a';
    expect_schema_reject(hand, hlen, "edge touching a retired faction");
    /* positive control: without the edge the same blob loads */
    hand[12] = 0u;
    memmove(hand + 24 + 32, hand + 24 + 44, 2u);
    CHECK(fbs_factions_deserialize(hand, hlen - 12u, NULL, NULL, &h) == FBS_FACTION_OK);
    CHECK(fbs_faction_count(h) == 2u);
    CHECK(fbs_faction_live_count(h) == 1u);
    fbs_factions_destroy(h);
  }

  /* a retired record must not carry a descriptor */
  {
    fbs_factions *h = make_table();
    unsigned char *hb;
    size_t hl = 0;
    add_faction(h, "one", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
    add_faction(h, "two", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
    CHECK(fbs_faction_retire(h, 1u) == FBS_FACTION_OK);
    hb = serialize_alloc(h, &hl);
    CHECK(fbs_factions_deserialize(hb, hl, NULL, NULL, &g) == FBS_FACTION_OK);
    fbs_factions_destroy(g);
    g = NULL;
    hb[24 + 8] = 1u; /* non-zero descriptor on a retired record */
    expect_schema_reject(hb, hl, "retired record descriptor");
    hb[24 + 8] = 0u;
    hb[24 + 4] = 3u; /* non-zero key_len on a retired record */
    expect_schema_reject(hb, hl, "retired record key length");
    free(hb);
    fbs_factions_destroy(h);
  }

  /* duplicate live keys: two records both pointing at "aa" */
  {
    unsigned char dup[24 + 2 * 16 + 4];
    size_t dlen = sizeof dup;
    memset(dup, 0, dlen);
    dup[0] = 'F';
    dup[1] = 'B';
    dup[2] = 'S';
    dup[3] = 'F';
    dup[4] = 1u;
    dup[8] = 2u;  /* faction_count = 2 */
    dup[16] = 4u; /* key_bytes = 4 */
    dup[24 + 0] = 0u;
    dup[24 + 4] = 2u; /* record 0: off 0, len 2 */
    dup[24 + 16 + 0] = 2u;
    dup[24 + 16 + 4] = 2u; /* record 1: off 2, len 2 */
    dup[24 + 32 + 0] = 'a';
    dup[24 + 32 + 1] = 'a';
    dup[24 + 32 + 2] = 'a';
    dup[24 + 32 + 3] = 'a';
    expect_schema_reject(dup, dlen, "duplicate live keys");
  }

  /* capacity: a cfg that cannot hold the blob is E_FULL, not E_SCHEMA */
  cfg = fbs_faction_config_default();
  cfg.max_factions = 2u;
  g = (fbs_factions *)0x1;
  CHECK(fbs_factions_deserialize(blob, len, &cfg, NULL, &g) == FBS_FACTION_E_FULL);
  CHECK(g == (fbs_factions *)0x1);
  cfg = fbs_faction_config_default();
  cfg.max_relations = 1u;
  CHECK(fbs_factions_deserialize(blob, len, &cfg, NULL, &g) == FBS_FACTION_E_FULL);
  cfg = fbs_faction_config_default();
  cfg.max_key_bytes = 4u;
  CHECK(fbs_factions_deserialize(blob, len, &cfg, NULL, &g) == FBS_FACTION_E_FULL);
  cfg = fbs_faction_config_default();
  cfg.max_factions = 0u;
  CHECK(fbs_factions_deserialize(blob, len, &cfg, NULL, &g) == FBS_FACTION_E_INVALID);
  CHECK(g == (fbs_factions *)0x1);

  /* a sufficient cfg works, and the blob's default attitude wins */
  CHECK(fbs_factions_set_default(f, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  free(blob);
  blob = serialize_alloc(f, &len);
  cfg = fbs_faction_config_default();
  cfg.max_factions = 8u;
  cfg.default_attitude = FBS_ATTITUDE_FRIENDLY;
  g = NULL;
  CHECK(fbs_factions_deserialize(blob, len, &cfg, NULL, &g) == FBS_FACTION_OK);
  CHECK(fbs_factions_default(g) == FBS_ATTITUDE_HOSTILE);
  CHECK(fbs_faction_count(g) == 3u);
  fbs_factions_destroy(g);

  free(copy);
  free(blob);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* Status names and version                                                  */
/* ------------------------------------------------------------------------- */

static void test_status_names(void) {
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_OK), "ok") == 0);
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_E_INVALID), "invalid") == 0);
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_E_NOT_FOUND), "not_found") == 0);
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_E_EXISTS), "exists") == 0);
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_E_FULL), "full") == 0);
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_E_RANGE), "range") == 0);
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_E_SCHEMA), "schema") == 0);
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_E_TRUNCATED), "truncated") == 0);
  CHECK(strcmp(fbs_faction_status_name(FBS_FACTION_E_MEMORY), "memory") == 0);
  CHECK(strcmp(fbs_faction_status_name(-999), "unknown") == 0);
  CHECK(strcmp(fbs_faction_status_name(999), "unknown") == 0);
  CHECK(fbs_faction_version() == FBS_FACTION_VERSION);
  CHECK(fbs_faction_version() == 100u);
  {
    fbs_faction_config cfg = fbs_faction_config_default();
    CHECK(cfg.max_factions == 64u);
    CHECK(cfg.max_relations == 512u);
    CHECK(cfg.max_key_bytes == 4096u);
    CHECK(cfg.default_attitude == FBS_ATTITUDE_NEUTRAL);
  }
}

/* ------------------------------------------------------------------------- */
/* Golden serialization fixture                                              */
/* ------------------------------------------------------------------------- */

static fbs_factions *build_fixture_table(void) {
  fbs_factions *f = make_table();
  fbs_faction_id a, b, c, d;
  CHECK(fbs_factions_set_default(f, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  a = add_faction(f, "alpha", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_HOSTILE);
  b = add_faction(f, "beta", FBS_ATTITUDE_HOSTILE, FBS_ATTITUDE_FRIENDLY);
  c = add_faction(f, "gamma", FBS_ATTITUDE_NEUTRAL, FBS_ATTITUDE_NEUTRAL);
  d = add_faction(f, "delta", FBS_ATTITUDE_FRIENDLY, FBS_ATTITUDE_FRIENDLY);
  CHECK(fbs_relation_set(f, a, c, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
  CHECK(fbs_faction_retire(f, c) == FBS_FACTION_OK); /* also drops (a, c) */
  CHECK(fbs_relation_set(f, a, a, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_set(f, a, b, FBS_ATTITUDE_NEUTRAL) == FBS_FACTION_OK);
  CHECK(fbs_relation_set_symmetric(f, b, d, FBS_ATTITUDE_HOSTILE) == FBS_FACTION_OK);
  CHECK(fbs_relation_set(f, d, a, FBS_ATTITUDE_FRIENDLY) == FBS_FACTION_OK);
  CHECK(fbs_faction_count(f) == 4u);
  CHECK(fbs_faction_live_count(f) == 3u);
  CHECK(fbs_relation_count(f) == 5u);
  return f;
}

static void test_fixture(void) {
  fbs_factions *f = build_fixture_table();
  fbs_factions *g = NULL;
  char path[512];
  size_t len = 0;
  unsigned char *blob = serialize_alloc(f, &len);
  FILE *fp;

  /* 24 header + 4 * 16 records + 5 * 12 relations + 14 live key bytes */
  CHECK(len == 24u + 64u + 60u + 14u);

  sprintf(path, "%s/table.bin", g_fixture_dir);
  if (g_write_fixtures) {
    fp = fopen(path, "wb");
    ++g_checks;
    if (!fp) {
      ++g_fails;
      printf("FAIL cannot open %s for writing\n", path);
    } else {
      size_t wrote = fwrite(blob, 1u, len, fp);
      fclose(fp);
      CHECK(wrote == len);
      printf("wrote %s (%lu bytes)\n", path, (unsigned long)len);
    }
  } else {
    fp = fopen(path, "rb");
    ++g_checks;
    if (!fp) {
      ++g_fails;
      printf("FAIL cannot open fixture %s (run with --write-fixtures to create it)\n", path);
    } else {
      unsigned char golden[1024];
      size_t got = fread(golden, 1u, sizeof golden, fp);
      fclose(fp);
      CHECK(got == len);
      if (got == len) CHECK(memcmp(golden, blob, len) == 0);
      /* the committed bytes must still load and re-serialize identically */
      if (got == len) {
        size_t l2 = 0;
        unsigned char *b2;
        CHECK(fbs_factions_deserialize(golden, got, NULL, NULL, &g) == FBS_FACTION_OK);
        b2 = serialize_alloc(g, &l2);
        CHECK(l2 == len && memcmp(b2, golden, len) == 0);
        free(b2);
        fbs_factions_destroy(g);
      }
    }
  }
  free(blob);
  fbs_factions_destroy(f);
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
  int i;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--write-fixtures") == 0) {
      g_write_fixtures = 1;
    } else if (strcmp(argv[i], "--fixture-dir") == 0 && i + 1 < argc) {
      g_fixture_dir = argv[++i];
    } else {
      printf("usage: %s [--write-fixtures] [--fixture-dir DIR]\n", argv[0]);
      return 2;
    }
  }

  test_ft1_asymmetric_default();
  test_ft2_edge_beats_descriptor();
  test_ft3_symmetric_helper();
  test_ft4_mixed_directionality();
  test_ft5_unknown_is_reported();
  test_ft6_self_edge();
  test_ft7_id_stability();
  test_empty_round_trip();
  test_ft8_determinism();
  test_ft9_team_ids();
  test_ft11_query_direction();
  test_ft12_fuzz();
  test_clear_and_defaults();
  test_duplicate_keys();
  test_capacities();
  test_allocator();
  test_truncated();
  test_invalid_arguments();
  test_schema_rejection();
  test_status_names();
  test_fixture();

  printf("factions: %d checks passed, %d failed\n", g_checks - g_fails, g_fails);
  return g_fails > 100 ? 100 : g_fails;
}
