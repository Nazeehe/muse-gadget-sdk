/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/*
 * "Did you mean …?" for Muse apps: unknown globals (apps_vm), manifest keys and
 * handler names. Plain C, no Lua.
 */

/* Names longer than this are never matched, as name or as candidate. */
#define APPS_SUGGEST_MAX_LEN 64

/* The most edit-distance table cells one call scores. A call that would need more
 * suggests nothing, so its cost stays bounded however many and long the candidates. */
#define APPS_SUGGEST_MAX_CELLS 65536

/*
 * The candidate nearest to name by edit distance (Levenshtein, over bytes, so case
 * counts), when that distance is at most 2 and less than strlen(name): a one-letter
 * name gets no suggestion, a two-letter one only at distance 1. Among equally near
 * candidates the first wins. Candidates equal to name and NULL entries are skipped.
 * Returns one of cands' pointers, or NULL when none qualifies or the scoring would
 * pass APPS_SUGGEST_MAX_CELLS.
 */
const char *apps_suggest(const char *name, const char *const *cands, int n);
