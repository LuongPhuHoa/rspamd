/*-
 * Copyright 2018 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "map_helpers.h"
#include "map_private.h"
#include "khash.h"
#include "radix.h"
#include "rspamd.h"
#include "cryptobox.h"

#ifdef WITH_HYPERSCAN
#include "hs.h"
#endif
#ifndef WITH_PCRE2
#include <pcre.h>
#else
#include <pcre2.h>
#endif


static const guint64 map_hash_seed = 0xdeadbabeULL;
static const gchar *hash_fill = "1";

struct rspamd_map_helper_value {
	gsize hits;
	gconstpointer key;
	gchar value[]; /* Null terminated */
};

KHASH_INIT (rspamd_map_hash, const gchar *,
		struct rspamd_map_helper_value *, true,
		rspamd_strcase_hash, rspamd_strcase_equal);

struct rspamd_radix_map_helper {
	rspamd_mempool_t *pool;
	khash_t(rspamd_map_hash) *htb;
	radix_compressed_t *trie;
	rspamd_cryptobox_fast_hash_state_t hst;
};

struct rspamd_hash_map_helper {
	rspamd_mempool_t *pool;
	khash_t(rspamd_map_hash) *htb;
	rspamd_cryptobox_fast_hash_state_t hst;
};

struct rspamd_regexp_map_helper {
	rspamd_mempool_t *pool;
	struct rspamd_map *map;
	GPtrArray *regexps;
	GPtrArray *values;
	khash_t(rspamd_map_hash) *htb;
	rspamd_cryptobox_fast_hash_state_t hst;
	enum rspamd_regexp_map_flags map_flags;
#ifdef WITH_HYPERSCAN
	hs_database_t *hs_db;
	hs_scratch_t *hs_scratch;
	const gchar **patterns;
	gint *flags;
	gint *ids;
#endif
};

/**
 * FSM for parsing lists
 */

#define MAP_STORE_KEY do { \
	while (g_ascii_isspace (*c) && p > c) { c ++; } \
	key = g_malloc (p - c + 1); \
	rspamd_strlcpy (key, c, p - c + 1); \
	key = g_strstrip (key); \
} while (0)

#define MAP_STORE_VALUE do { \
	while (g_ascii_isspace (*c) && p > c) { c ++; } \
	value = g_malloc (p - c + 1); \
	rspamd_strlcpy (value, c, p - c + 1); \
	value = g_strstrip (value); \
} while (0)

gchar *
rspamd_parse_kv_list (
		gchar * chunk,
		gint len,
		struct map_cb_data *data,
		insert_func func,
		const gchar *default_value,
		gboolean final)
{
	enum {
		map_skip_spaces_before_key = 0,
		map_read_key,
		map_read_key_quoted,
		map_read_key_slashed,
		map_skip_spaces_after_key,
		map_backslash_quoted,
		map_backslash_slashed,
		map_read_key_after_slash,
		map_read_value,
		map_read_comment_start,
		map_skip_comment,
		map_read_eol,
	};

	gchar *c, *p, *key = NULL, *value = NULL, *end;
	struct rspamd_map *map = data->map;
	guint line_number = 0;

	p = chunk;
	c = p;
	end = p + len;

	while (p < end) {
		switch (data->state) {
		case map_skip_spaces_before_key:
			if (g_ascii_isspace (*p)) {
				p ++;
			}
			else {
				if (*p == '"') {
					p++;
					c = p;
					data->state = map_read_key_quoted;
				}
				else if (*p == '/') {
					/* Note that c is on '/' here as '/' is a part of key */
					c = p;
					p++;
					data->state = map_read_key_slashed;
				}
				else {
					c = p;
					data->state = map_read_key;
				}
			}
			break;
		case map_read_key:
			/* read key */
			/* Check here comments, eol and end of buffer */
			if (*p == '#' && (p == c || *(p - 1) != '\\')) {
				if (p - c > 0) {
					/* Store a single key */
					MAP_STORE_KEY;
					func (data->cur_data, key, default_value);
					msg_debug_map ("insert key only pair: %s -> %s; line: %d",
							key, default_value, line_number);
					g_free (key);
				}

				key = NULL;
				data->state = map_read_comment_start;
			}
			else if (*p == '\r' || *p == '\n') {
				if (p - c > 0) {
					/* Store a single key */
					MAP_STORE_KEY;
					func (data->cur_data, key, default_value);
					msg_debug_map ("insert key only pair: %s -> %s; line: %d",
							key, default_value, line_number);
					g_free (key);
				}

				data->state = map_read_eol;
				key = NULL;
			}
			else if (g_ascii_isspace (*p)) {
				if (p - c > 0) {
					MAP_STORE_KEY;
					data->state = map_skip_spaces_after_key;
				}
				else {
					msg_err_map ("empty or invalid key found on line %d", line_number);
					data->state = map_skip_comment;
				}
			}
			else {
				p++;
			}
			break;
		case map_read_key_quoted:
			if (*p == '\\') {
				data->state = map_backslash_quoted;
				p ++;
			}
			else if (*p == '"') {
				/* Allow empty keys in this case */
				if (p - c >= 0) {
					MAP_STORE_KEY;
					data->state = map_skip_spaces_after_key;
				}
				else {
					g_assert_not_reached ();
				}
				p ++;
			}
			else {
				p ++;
			}
			break;
		case map_read_key_slashed:
			if (*p == '\\') {
				data->state = map_backslash_slashed;
				p ++;
			}
			else if (*p == '/') {
				/* Allow empty keys in this case */
				if (p - c >= 0) {
					data->state = map_read_key_after_slash;
				}
				else {
					g_assert_not_reached ();
				}
			}
			else {
				p ++;
			}
			break;
		case map_read_key_after_slash:
			/*
			 * This state is equal to reading of key but '/' is not
			 * treated specially
			 */
			if (*p == '#') {
				if (p - c > 0) {
					/* Store a single key */
					MAP_STORE_KEY;
					func (data->cur_data, key, default_value);
					msg_debug_map ("insert key only pair: %s -> %s; line: %d",
							key, default_value, line_number);
					g_free (key);
					key = NULL;
				}

				data->state = map_read_comment_start;
			}
			else if (*p == '\r' || *p == '\n') {
				if (p - c > 0) {
					/* Store a single key */
					MAP_STORE_KEY;
					func (data->cur_data, key, default_value);

					msg_debug_map ("insert key only pair: %s -> %s; line: %d",
							key, default_value, line_number);
					g_free (key);
					key = NULL;
				}

				data->state = map_read_eol;
				key = NULL;
			}
			else if (g_ascii_isspace (*p)) {
				if (p - c > 0) {
					MAP_STORE_KEY;
					data->state = map_skip_spaces_after_key;
				}
				else {
					msg_err_map ("empty or invalid key found on line %d", line_number);
					data->state = map_skip_comment;
				}
			}
			else {
				p ++;
			}
			break;
		case map_backslash_quoted:
			p ++;
			data->state = map_read_key_quoted;
			break;
		case map_backslash_slashed:
			p ++;
			data->state = map_read_key_slashed;
			break;
		case map_skip_spaces_after_key:
			if (*p == ' ' || *p == '\t') {
				p ++;
			}
			else {
				c = p;
				data->state = map_read_value;
			}
			break;
		case map_read_value:
			if (key == NULL) {
				/* Ignore line */
				msg_err_map ("empty or invalid key found on line %d", line_number);
				data->state = map_skip_comment;
			}
			else {
				if (*p == '#') {
					if (p - c > 0) {
						/* Store a single key */
						MAP_STORE_VALUE;
						func (data->cur_data, key, value);
						msg_debug_map ("insert key value pair: %s -> %s; line: %d",
								key, value, line_number);
						g_free (key);
						g_free (value);
						key = NULL;
						value = NULL;
					} else {
						func (data->cur_data, key, default_value);
						msg_debug_map ("insert key only pair: %s -> %s; line: %d",
								key, default_value, line_number);
						g_free (key);
						key = NULL;
					}

					data->state = map_read_comment_start;
				} else if (*p == '\r' || *p == '\n') {
					if (p - c > 0) {
						/* Store a single key */
						MAP_STORE_VALUE;
						func (data->cur_data, key, value);
						msg_debug_map ("insert key value pair: %s -> %s",
								key, value);
						g_free (key);
						g_free (value);
						key = NULL;
						value = NULL;
					} else {
						func (data->cur_data, key, default_value);
						msg_debug_map ("insert key only pair: %s -> %s",
								key, default_value);
						g_free (key);
						key = NULL;
					}

					data->state = map_read_eol;
					key = NULL;
				}
				else {
					p++;
				}
			}
			break;
		case map_read_comment_start:
			if (*p == '#') {
				data->state = map_skip_comment;
				p ++;
				key = NULL;
				value = NULL;
			}
			else {
				g_assert_not_reached ();
			}
			break;
		case map_skip_comment:
			if (*p == '\r' || *p == '\n') {
				data->state = map_read_eol;
			}
			else {
				p ++;
			}
			break;
		case map_read_eol:
			/* Skip \r\n and whitespaces */
			if (*p == '\r' || *p == '\n') {
				if (*p == '\n') {
					/* We don't care about \r only line separators, they are too rare */
					line_number ++;
				}
				p++;
			}
			else {
				data->state = map_skip_spaces_before_key;
			}
			break;
		default:
			g_assert_not_reached ();
			break;
		}
	}

	if (final) {
		/* Examine the state */
		switch (data->state) {
		case map_read_key:
			if (p - c > 0) {
				/* Store a single key */
				MAP_STORE_KEY;
				func (data->cur_data, key, default_value);
				msg_debug_map ("insert key only pair: %s -> %s",
						key, default_value);
				g_free (key);
				key = NULL;
			}
			break;
		case map_read_value:
			if (key == NULL) {
				/* Ignore line */
				msg_err_map ("empty or invalid key found on line %d", line_number);
				data->state = map_skip_comment;
			}
			else {
				if (p - c > 0) {
					/* Store a single key */
					MAP_STORE_VALUE;
					func (data->cur_data, key, value);
					msg_debug_map ("insert key value pair: %s -> %s",
							key, value);
					g_free (key);
					g_free (value);
					key = NULL;
					value = NULL;
				} else {
					func (data->cur_data, key, default_value);
					msg_debug_map ("insert key only pair: %s -> %s",
							key, default_value);
					g_free (key);
					key = NULL;
				}
			}
			break;
		}

		data->state = map_skip_spaces_before_key;
	}

	return c;
}

/**
 * Radix tree helper function
 */
void
rspamd_map_helper_insert_radix (gpointer st, gconstpointer key, gconstpointer value)
{
	struct rspamd_radix_map_helper *r = (struct rspamd_radix_map_helper *)st;
	struct rspamd_map_helper_value *val;
	gsize vlen;
	khiter_t k;
	gconstpointer nk;
	gint res;

	vlen = strlen (value);
	val = rspamd_mempool_alloc0 (r->pool, sizeof (*val) +
			vlen + 1);
	memcpy (val->value, value, vlen);

	k = kh_get (rspamd_map_hash, r->htb, key);

	if (k == kh_end (r->htb)) {
		nk = rspamd_mempool_strdup (r->pool, key);
		k = kh_put (rspamd_map_hash, r->htb, nk, &res);
	}

	nk = kh_key (r->htb, k);
	val->key = nk;
	kh_value (r->htb, k) = val;
	rspamd_radix_add_iplist (key, ",", r->trie, val, FALSE);
	rspamd_cryptobox_fast_hash_update (&r->hst, nk, strlen (nk));
}

void
rspamd_map_helper_insert_radix_resolve (gpointer st, gconstpointer key, gconstpointer value)
{
	struct rspamd_radix_map_helper *r = (struct rspamd_radix_map_helper *)st;
	struct rspamd_map_helper_value *val;
	gsize vlen;
	khiter_t k;
	gconstpointer nk;
	gint res;

	vlen = strlen (value);
	val = rspamd_mempool_alloc0 (r->pool, sizeof (*val) +
			vlen + 1);
	memcpy (val->value, value, vlen);

	k = kh_get (rspamd_map_hash, r->htb, key);

	if (k == kh_end (r->htb)) {
		nk = rspamd_mempool_strdup (r->pool, key);
		k = kh_put (rspamd_map_hash, r->htb, nk, &res);
	}

	nk = kh_key (r->htb, k);
	val->key = nk;
	kh_value (r->htb, k) = val;
	rspamd_radix_add_iplist (key, ",", r->trie, val, TRUE);
	rspamd_cryptobox_fast_hash_update (&r->hst, nk, strlen (nk));
}

void
rspamd_map_helper_insert_hash (gpointer st, gconstpointer key, gconstpointer value)
{
	struct rspamd_hash_map_helper *ht = st;
	struct rspamd_map_helper_value *val;
	khiter_t k;
	gconstpointer nk;
	gsize vlen;
	gint r;

	k = kh_get (rspamd_map_hash, ht->htb, key);
	vlen = strlen (value);

	if (k == kh_end (ht->htb)) {
		nk = rspamd_mempool_strdup (ht->pool, key);
		k = kh_put (rspamd_map_hash, ht->htb, nk, &r);
	}
	else {
		val = kh_value (ht->htb, k);

		if (strcmp (value, val->value) == 0) {
			/* Same element, skip */
			return;
		}
	}

	/* Null termination due to alloc0 */
	val = rspamd_mempool_alloc0 (ht->pool, sizeof (*val) + vlen + 1);
	memcpy (val->value, value, vlen);

	nk = kh_key (ht->htb, k);
	val->key = nk;
	kh_value (ht->htb, k) = val;
	rspamd_cryptobox_fast_hash_update (&ht->hst, nk, strlen (nk));
}

void
rspamd_map_helper_insert_re (gpointer st, gconstpointer key, gconstpointer value)
{
	struct rspamd_regexp_map_helper *re_map = st;
	struct rspamd_map *map;
	rspamd_regexp_t *re;
	gchar *escaped;
	GError *err = NULL;
	gint pcre_flags;
	gsize escaped_len;
	struct rspamd_map_helper_value *val;
	khiter_t k;
	gconstpointer nk;
	gsize vlen;
	gint r;

	map = re_map->map;

	if (re_map->map_flags & RSPAMD_REGEXP_MAP_FLAG_GLOB) {
		escaped = rspamd_str_regexp_escape (key, strlen (key), &escaped_len,
				RSPAMD_REGEXP_ESCAPE_GLOB|RSPAMD_REGEXP_ESCAPE_UTF);
		re = rspamd_regexp_new (escaped, NULL, &err);
		g_free (escaped);
	}
	else {
		re = rspamd_regexp_new (key, NULL, &err);
	}

	if (re == NULL) {
		msg_err_map ("cannot parse regexp %s: %e", key, err);

		if (err) {
			g_error_free (err);
		}

		return;
	}

	vlen = strlen (value);
	val = rspamd_mempool_alloc0 (re_map->pool, sizeof (*val) +
			vlen + 1);
	memcpy (val->value, value, vlen);

	k = kh_get (rspamd_map_hash, re_map->htb, key);

	if (k == kh_end (re_map->htb)) {
		nk = rspamd_mempool_strdup (re_map->pool, key);
		k = kh_put (rspamd_map_hash, re_map->htb, nk, &r);
	}

	nk = kh_key (re_map->htb, k);
	val->key = nk;
	kh_value (re_map->htb, k) = val;
	rspamd_cryptobox_fast_hash_update (&re_map->hst, nk, strlen (nk));

	pcre_flags = rspamd_regexp_get_pcre_flags (re);

#ifndef WITH_PCRE2
	if (pcre_flags & PCRE_FLAG(UTF8)) {
		re_map->map_flags |= RSPAMD_REGEXP_MAP_FLAG_UTF;
	}
#else
	if (pcre_flags & PCRE_FLAG(UTF)) {
		re_map->map_flags |= RSPAMD_REGEXP_MAP_FLAG_UTF;
	}
#endif

	g_ptr_array_add (re_map->regexps, re);
	g_ptr_array_add (re_map->values, val);
}

static void
rspamd_map_helper_traverse_regexp (void *data,
		rspamd_map_traverse_cb cb,
		gpointer cbdata,
		gboolean reset_hits)
{
	gconstpointer k;
	struct rspamd_map_helper_value *val;
	struct rspamd_regexp_map_helper *re_map = data;

	kh_foreach (re_map->htb, k, val, {
		if (!cb (k, val->value, val->hits, cbdata)) {
			break;
		}

		if (reset_hits) {
			val->hits = 0;
		}
	});
}

struct rspamd_hash_map_helper *
rspamd_map_helper_new_hash (struct rspamd_map *map)
{
	struct rspamd_hash_map_helper *htb;
	rspamd_mempool_t *pool;

	if (map) {
		pool = rspamd_mempool_new (rspamd_mempool_suggest_size (),
				map->tag);
	}
	else {
		pool = rspamd_mempool_new (rspamd_mempool_suggest_size (),
				NULL);
	}

	htb = rspamd_mempool_alloc0 (pool, sizeof (*htb));
	htb->htb = kh_init (rspamd_map_hash);
	htb->pool = pool;
	rspamd_cryptobox_fast_hash_init (&htb->hst, map_hash_seed);

	return htb;
}

void
rspamd_map_helper_destroy_hash (struct rspamd_hash_map_helper *r)
{
	if (r == NULL || r->pool == NULL) {
		return;
	}

	rspamd_mempool_t *pool = r->pool;
	kh_destroy (rspamd_map_hash, r->htb);
	memset (r, 0, sizeof (*r));
	rspamd_mempool_delete (pool);
}

static void
rspamd_map_helper_traverse_hash (void *data,
		rspamd_map_traverse_cb cb,
		gpointer cbdata,
		gboolean reset_hits)
{
	gconstpointer k;
	struct rspamd_map_helper_value *val;
	struct rspamd_hash_map_helper *ht = data;

	kh_foreach (ht->htb, k, val, {
		if (!cb (k, val->value, val->hits, cbdata)) {
			break;
		}

		if (reset_hits) {
			val->hits = 0;
		}
	});
}

struct rspamd_radix_map_helper *
rspamd_map_helper_new_radix (struct rspamd_map *map)
{
	struct rspamd_radix_map_helper *r;
	rspamd_mempool_t *pool;

	if (map) {
		pool = rspamd_mempool_new (rspamd_mempool_suggest_size (),
				map->tag);
	}
	else {
		pool = rspamd_mempool_new (rspamd_mempool_suggest_size (),
				NULL);
	}

	r = rspamd_mempool_alloc0 (pool, sizeof (*r));
	r->trie = radix_create_compressed_with_pool (pool);
	r->htb = kh_init (rspamd_map_hash);
	r->pool = pool;
	rspamd_cryptobox_fast_hash_init (&r->hst, map_hash_seed);

	return r;
}

void
rspamd_map_helper_destroy_radix (struct rspamd_radix_map_helper *r)
{
	if (r == NULL || !r->pool) {
		return;
	}

	kh_destroy (rspamd_map_hash, r->htb);
	rspamd_mempool_t *pool = r->pool;
	memset (r, 0, sizeof (*r));
	rspamd_mempool_delete (pool);
}

static void
rspamd_map_helper_traverse_radix (void *data,
		rspamd_map_traverse_cb cb,
		gpointer cbdata,
		gboolean reset_hits)
{
	gconstpointer k;
	struct rspamd_map_helper_value *val;
	struct rspamd_radix_map_helper *r = data;

	kh_foreach (r->htb, k, val, {
		if (!cb (k, val->value, val->hits, cbdata)) {
			break;
		}

		if (reset_hits) {
			val->hits = 0;
		}
	});
}

struct rspamd_regexp_map_helper *
rspamd_map_helper_new_regexp (struct rspamd_map *map,
		enum rspamd_regexp_map_flags flags)
{
	struct rspamd_regexp_map_helper *re_map;
	rspamd_mempool_t *pool;

	pool = rspamd_mempool_new (rspamd_mempool_suggest_size (),
			map->tag);

	re_map = rspamd_mempool_alloc0 (pool, sizeof (*re_map));
	re_map->pool = pool;
	re_map->values = g_ptr_array_new ();
	re_map->regexps = g_ptr_array_new ();
	re_map->map = map;
	re_map->map_flags = flags;
	re_map->htb = kh_init (rspamd_map_hash);
	rspamd_cryptobox_fast_hash_init (&re_map->hst, map_hash_seed);

	return re_map;
}


void
rspamd_map_helper_destroy_regexp (struct rspamd_regexp_map_helper *re_map)
{
	rspamd_regexp_t *re;
	guint i;

	if (!re_map || !re_map->regexps) {
		return;
	}

	for (i = 0; i < re_map->regexps->len; i ++) {
		re = g_ptr_array_index (re_map->regexps, i);
		rspamd_regexp_unref (re);
	}

	g_ptr_array_free (re_map->regexps, TRUE);
	g_ptr_array_free (re_map->values, TRUE);
	kh_destroy (rspamd_map_hash, re_map->htb);

#ifdef WITH_HYPERSCAN
	if (re_map->hs_scratch) {
		hs_free_scratch (re_map->hs_scratch);
	}
	if (re_map->hs_db) {
		hs_free_database (re_map->hs_db);
	}
	if (re_map->patterns) {
		g_free (re_map->patterns);
	}
	if (re_map->flags) {
		g_free (re_map->flags);
	}
	if (re_map->ids) {
		g_free (re_map->ids);
	}
#endif

	rspamd_mempool_t *pool = re_map->pool;
	memset (re_map, 0, sizeof (*re_map));
	rspamd_mempool_delete (pool);
}

gchar *
rspamd_kv_list_read (
		gchar * chunk,
		gint len,
		struct map_cb_data *data,
		gboolean final)
{
	if (data->cur_data == NULL) {
		data->cur_data = rspamd_map_helper_new_hash (data->map);
	}

	return rspamd_parse_kv_list (
			chunk,
			len,
			data,
			rspamd_map_helper_insert_hash,
			"",
			final);
}

void
rspamd_kv_list_fin (struct map_cb_data *data, void **target)
{
	struct rspamd_map *map = data->map;
	struct rspamd_hash_map_helper *htb;

	if (data->cur_data) {
		htb = (struct rspamd_hash_map_helper *)data->cur_data;
		msg_info_map ("read hash of %d elements", kh_size (htb->htb));
		data->map->traverse_function = rspamd_map_helper_traverse_hash;
		data->map->nelts = kh_size (htb->htb);
		data->map->digest = rspamd_cryptobox_fast_hash_final (&htb->hst);
	}

	if (target) {
		*target = data->cur_data;
	}

	if (data->prev_data) {
		htb = (struct rspamd_hash_map_helper *)data->prev_data;
		rspamd_map_helper_destroy_hash (htb);
	}
}

void
rspamd_kv_list_dtor (struct map_cb_data *data)
{
	struct rspamd_hash_map_helper *htb;

	if (data->cur_data) {
		htb = (struct rspamd_hash_map_helper *)data->cur_data;
		rspamd_map_helper_destroy_hash (htb);
	}
}

gchar *
rspamd_radix_read (
		gchar * chunk,
		gint len,
		struct map_cb_data *data,
		gboolean final)
{
	struct rspamd_radix_map_helper *r;
	struct rspamd_map *map = data->map;

	if (data->cur_data == NULL) {
		r = rspamd_map_helper_new_radix (map);
		data->cur_data = r;
	}

	return rspamd_parse_kv_list (
			chunk,
			len,
			data,
			rspamd_map_helper_insert_radix,
			hash_fill,
			final);
}

void
rspamd_radix_fin (struct map_cb_data *data, void **target)
{
	struct rspamd_map *map = data->map;
	struct rspamd_radix_map_helper *r;

	if (data->cur_data) {
		r = (struct rspamd_radix_map_helper *)data->cur_data;
		msg_info_map ("read radix trie of %z elements: %s",
				radix_get_size (r->trie), radix_get_info (r->trie));
		data->map->traverse_function = rspamd_map_helper_traverse_radix;
		data->map->nelts = kh_size (r->htb);
		data->map->digest = rspamd_cryptobox_fast_hash_final (&r->hst);
	}

	if (target) {
		*target = data->cur_data;
	}

	if (data->prev_data) {
		r = (struct rspamd_radix_map_helper *)data->prev_data;
		rspamd_map_helper_destroy_radix (r);
	}
}

void
rspamd_radix_dtor (struct map_cb_data *data)
{
	struct rspamd_radix_map_helper *r;

	if (data->cur_data) {
		r = (struct rspamd_radix_map_helper *)data->cur_data;
		rspamd_map_helper_destroy_radix (r);
	}
}

static void
rspamd_re_map_finalize (struct rspamd_regexp_map_helper *re_map)
{
#ifdef WITH_HYPERSCAN
	guint i;
	hs_platform_info_t plt;
	hs_compile_error_t *err;
	struct rspamd_map *map;
	rspamd_regexp_t *re;
	gint pcre_flags;

	map = re_map->map;

	if (!(map->cfg->libs_ctx->crypto_ctx->cpu_config & CPUID_SSSE3)) {
		msg_info_map ("disable hyperscan for map %s, ssse3 instructons are not supported by CPU",
				map->name);
		return;
	}

	if (hs_populate_platform (&plt) != HS_SUCCESS) {
		msg_err_map ("cannot populate hyperscan platform");
		return;
	}

	re_map->patterns = g_new (const gchar *, re_map->regexps->len);
	re_map->flags = g_new (gint, re_map->regexps->len);
	re_map->ids = g_new (gint, re_map->regexps->len);

	for (i = 0; i < re_map->regexps->len; i ++) {
		re = g_ptr_array_index (re_map->regexps, i);
		re_map->patterns[i] = rspamd_regexp_get_pattern (re);
		re_map->flags[i] = HS_FLAG_SINGLEMATCH;
		pcre_flags = rspamd_regexp_get_pcre_flags (re);

#ifndef WITH_PCRE2
		if (pcre_flags & PCRE_FLAG(UTF8)) {
			re_map->flags[i] |= HS_FLAG_UTF8;
		}
#else
		if (pcre_flags & PCRE_FLAG(UTF)) {
			re_map->flags[i] |= HS_FLAG_UTF8;
		}
#endif
		if (pcre_flags & PCRE_FLAG(CASELESS)) {
			re_map->flags[i] |= HS_FLAG_CASELESS;
		}
		if (pcre_flags & PCRE_FLAG(MULTILINE)) {
			re_map->flags[i] |= HS_FLAG_MULTILINE;
		}
		if (pcre_flags & PCRE_FLAG(DOTALL)) {
			re_map->flags[i] |= HS_FLAG_DOTALL;
		}
		if (rspamd_regexp_get_maxhits (re) == 1) {
			re_map->flags[i] |= HS_FLAG_SINGLEMATCH;
		}

		re_map->ids[i] = i;
	}

	if (re_map->regexps->len > 0 && re_map->patterns) {
		if (hs_compile_multi (re_map->patterns,
				re_map->flags,
				re_map->ids,
				re_map->regexps->len,
				HS_MODE_BLOCK,
				&plt,
				&re_map->hs_db,
				&err) != HS_SUCCESS) {

			msg_err_map ("cannot create tree of regexp when processing '%s': %s",
					err->expression >= 0 ?
							re_map->patterns[err->expression] :
							"unknown regexp", err->message);
			re_map->hs_db = NULL;
			hs_free_compile_error (err);

			return;
		}

		if (hs_alloc_scratch (re_map->hs_db, &re_map->hs_scratch) != HS_SUCCESS) {
			msg_err_map ("cannot allocate scratch space for hyperscan");
			hs_free_database (re_map->hs_db);
			re_map->hs_db = NULL;
		}
	}
	else {
		msg_err_map ("regexp map is empty");
	}
#endif
}

gchar *
rspamd_regexp_list_read_single (
		gchar *chunk,
		gint len,
		struct map_cb_data *data,
		gboolean final)
{
	struct rspamd_regexp_map_helper *re_map;

	if (data->cur_data == NULL) {
		re_map = rspamd_map_helper_new_regexp (data->map, 0);
		data->cur_data = re_map;
	}

	return rspamd_parse_kv_list (
			chunk,
			len,
			data,
			rspamd_map_helper_insert_re,
			hash_fill,
			final);
}

gchar *
rspamd_glob_list_read_single (
		gchar *chunk,
		gint len,
		struct map_cb_data *data,
		gboolean final)
{
	struct rspamd_regexp_map_helper *re_map;

	if (data->cur_data == NULL) {
		re_map = rspamd_map_helper_new_regexp (data->map, RSPAMD_REGEXP_MAP_FLAG_GLOB);
		data->cur_data = re_map;
	}

	return rspamd_parse_kv_list (
			chunk,
			len,
			data,
			rspamd_map_helper_insert_re,
			hash_fill,
			final);
}

gchar *
rspamd_regexp_list_read_multiple (
		gchar *chunk,
		gint len,
		struct map_cb_data *data,
		gboolean final)
{
	struct rspamd_regexp_map_helper *re_map;

	if (data->cur_data == NULL) {
		re_map = rspamd_map_helper_new_regexp (data->map,
				RSPAMD_REGEXP_MAP_FLAG_MULTIPLE);
		data->cur_data = re_map;
	}

	return rspamd_parse_kv_list (
			chunk,
			len,
			data,
			rspamd_map_helper_insert_re,
			hash_fill,
			final);
}

gchar *
rspamd_glob_list_read_multiple (
		gchar *chunk,
		gint len,
		struct map_cb_data *data,
		gboolean final)
{
	struct rspamd_regexp_map_helper *re_map;

	if (data->cur_data == NULL) {
		re_map = rspamd_map_helper_new_regexp (data->map,
				RSPAMD_REGEXP_MAP_FLAG_GLOB|RSPAMD_REGEXP_MAP_FLAG_MULTIPLE);
		data->cur_data = re_map;
	}

	return rspamd_parse_kv_list (
			chunk,
			len,
			data,
			rspamd_map_helper_insert_re,
			hash_fill,
			final);
}


void
rspamd_regexp_list_fin (struct map_cb_data *data, void **target)
{
	struct rspamd_regexp_map_helper *re_map;
	struct rspamd_map *map = data->map;

	if (data->cur_data) {
		re_map = data->cur_data;
		rspamd_re_map_finalize (re_map);
		msg_info_map ("read regexp list of %ud elements",
				re_map->regexps->len);
		data->map->traverse_function = rspamd_map_helper_traverse_regexp;
		data->map->nelts = kh_size (re_map->htb);
		data->map->digest = rspamd_cryptobox_fast_hash_final (&re_map->hst);
	}

	if (target) {
		*target = data->cur_data;
	}

	if (data->prev_data) {
		rspamd_map_helper_destroy_regexp (data->prev_data);
	}
}
void
rspamd_regexp_list_dtor (struct map_cb_data *data)
{
	if (data->cur_data) {
		rspamd_map_helper_destroy_regexp (data->cur_data);
	}
}

#ifdef WITH_HYPERSCAN
static int
rspamd_match_hs_single_handler (unsigned int id, unsigned long long from,
		unsigned long long to,
		unsigned int flags, void *context)
{
	guint *i = context;
	/* Always return non-zero as we need a single match here */

	*i = id;

	return 1;
}
#endif

gconstpointer
rspamd_match_regexp_map_single (struct rspamd_regexp_map_helper *map,
		const gchar *in, gsize len)
{
	guint i;
	rspamd_regexp_t *re;
	gint res = 0;
	gpointer ret = NULL;
	struct rspamd_map_helper_value *val;
	gboolean validated = FALSE;

	g_assert (in != NULL);

	if (map == NULL || len == 0 || map->regexps == NULL) {
		return NULL;
	}

	if (map->map_flags & RSPAMD_REGEXP_MAP_FLAG_UTF) {
		if (g_utf8_validate (in, len, NULL)) {
			validated = TRUE;
		}
	}
	else {
		validated = TRUE;
	}

#ifdef WITH_HYPERSCAN
	if (map->hs_db && map->hs_scratch) {

		if (validated) {

			res = hs_scan (map->hs_db, in, len, 0, map->hs_scratch,
					rspamd_match_hs_single_handler, (void *)&i);

			if (res == HS_SCAN_TERMINATED) {
				res = 1;
				val = g_ptr_array_index (map->values, i);

				ret = val->value;
				val->hits ++;
			}

			return ret;
		}
	}
#endif

	if (!res) {
		/* PCRE version */
		for (i = 0; i < map->regexps->len; i ++) {
			re = g_ptr_array_index (map->regexps, i);

			if (rspamd_regexp_search (re, in, len, NULL, NULL, !validated, NULL)) {
				val = g_ptr_array_index (map->values, i);

				ret = val->value;
				val->hits ++;
				break;
			}
		}
	}

	return ret;
}

#ifdef WITH_HYPERSCAN
struct rspamd_multiple_cbdata {
	GPtrArray *ar;
	struct rspamd_regexp_map_helper *map;
};

static int
rspamd_match_hs_multiple_handler (unsigned int id, unsigned long long from,
		unsigned long long to,
		unsigned int flags, void *context)
{
	struct rspamd_multiple_cbdata *cbd = context;
	struct rspamd_map_helper_value *val;


	if (id < cbd->map->values->len) {
		val = g_ptr_array_index (cbd->map->values, id);
		val->hits ++;
		g_ptr_array_add (cbd->ar, val->value);
	}

	/* Always return zero as we need all matches here */
	return 0;
}
#endif

GPtrArray*
rspamd_match_regexp_map_all (struct rspamd_regexp_map_helper *map,
		const gchar *in, gsize len)
{
	guint i;
	rspamd_regexp_t *re;
	GPtrArray *ret;
	gint res = 0;
	gboolean validated = FALSE;
	struct rspamd_map_helper_value *val;

	if (map == NULL || map->regexps == NULL || len == 0) {
		return NULL;
	}

	g_assert (in != NULL);

	if (map->map_flags & RSPAMD_REGEXP_MAP_FLAG_UTF) {
		if (g_utf8_validate (in, len, NULL)) {
			validated = TRUE;
		}
	}
	else {
		validated = TRUE;
	}

	ret = g_ptr_array_new ();

#ifdef WITH_HYPERSCAN
	if (map->hs_db && map->hs_scratch) {

		if (validated) {
			struct rspamd_multiple_cbdata cbd;

			cbd.ar = ret;
			cbd.map = map;

			if (hs_scan (map->hs_db, in, len, 0, map->hs_scratch,
					rspamd_match_hs_multiple_handler, &cbd) == HS_SUCCESS) {
				res = 1;
			}
		}
	}
#endif

	if (!res) {
		/* PCRE version */
		for (i = 0; i < map->regexps->len; i ++) {
			re = g_ptr_array_index (map->regexps, i);

			if (rspamd_regexp_search (re, in, len, NULL, NULL,
					!validated, NULL)) {
				val = g_ptr_array_index (map->values, i);
				val->hits ++;
				g_ptr_array_add (ret, val->value);
			}
		}
	}

	if (ret->len > 0) {
		return ret;
	}

	g_ptr_array_free (ret, TRUE);

	return NULL;
}

gconstpointer
rspamd_match_hash_map (struct rspamd_hash_map_helper *map, const gchar *in)
{
	khiter_t k;
	struct rspamd_map_helper_value *val;

	if (map == NULL || map->htb == NULL) {
		return NULL;
	}

	k = kh_get (rspamd_map_hash, map->htb, in);

	if (k != kh_end (map->htb)) {
		val = kh_value (map->htb, k);
		val->hits ++;

		return val->value;
	}

	return NULL;
}

gconstpointer
rspamd_match_radix_map (struct rspamd_radix_map_helper *map,
		const guchar *in, gsize inlen)
{
	struct rspamd_map_helper_value *val;

	if (map == NULL || map->trie == NULL) {
		return NULL;
	}

	val = (struct rspamd_map_helper_value *)radix_find_compressed (map->trie,
			in, inlen);

	if (val != (gconstpointer)RADIX_NO_VALUE) {
		val->hits ++;

		return val->value;
	}

	return NULL;
}

gconstpointer
rspamd_match_radix_map_addr (struct rspamd_radix_map_helper *map,
		const rspamd_inet_addr_t *addr)
{
	struct rspamd_map_helper_value *val;

	if (map == NULL || map->trie == NULL) {
		return NULL;
	}

	val = (struct rspamd_map_helper_value *)radix_find_compressed_addr (map->trie, addr);

	if (val != (gconstpointer)RADIX_NO_VALUE) {
		val->hits ++;

		return val->value;
	}

	return NULL;
}