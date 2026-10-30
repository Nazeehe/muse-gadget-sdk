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
 * The launcher's icons: the pixels of the manifest's icon (apps_pkg.h) for a window of apps around the carousel's centre, read on
 * the I/O worker into the apps' heap and handed to the LVGL task as ready image descriptors. An app whose icon isn't ready (not
 * asked for, still loading, refused, a bad file, or an app with none) gets NULL from apps_icons_get, and the carousel shows its
 * tile: never a blank.
 *
 * Flash (X11). The LVGL task never reads flash and never waits on the worker: apps_icons_want and apps_icons_poll queue a job
 * (apps_io_submit, which never waits for another task's progress; apps_io_submit_op is the app task's alone), and the job reads
 * APPS_STORE_BASE/<id>/<icon>, at most APPS_ICON_MAX_BYTES, with apps_store_read_file, into one block of apps_port_realloc: PSRAM,
 * through the apps' floor (APPS_RAM_RESERVE), so a block the floor refuses is a tile. The job checks it (apps_icon_check: an image
 * apps_image_check passes, square, at most APPS_ICON_MAX_PX), and a file that fails, or can't be read, is a tile with one W line,
 * "icons: <id>: <icon>: <why>; its tile shows". The store in RAM mode keeps no files, and before the mount has none: no load is
 * asked for, and every app is its tile.
 *
 * The window. apps_icons_want is told the window, centre first: at most APPS_ICONS_WINDOW apps, each with its icon's path ("" for
 * none). Loads are asked for in the window's order, at most APPS_ICONS_IN_FLIGHT at once (stale ones count until they end), so a
 * fling through many apps queues no load for each and leaves the worker's queue to installs and launches; apps_icons_poll asks for
 * the rest as they end. A ready icon that leaves the window is freed at once (the want's), so at most APPS_ICONS_WINDOW blocks are
 * shown and APPS_ICONS_IN_FLIGHT more are loading. An icon that failed stays a tile while it stays in the window (it is not read
 * again at every frame); one that leaves and comes back is asked for again. A load the worker refused (its queue full) is asked for
 * again APPS_ICONS_RETRY_MS later, not at every frame: each refusal is a W line on the console.
 *
 * Generations. apps_icons_drop_all (the launcher's close, a change of the list, a voice turn) frees every ready icon, forgets the
 * window, and starts a new generation. Every load carries the generation of the want that asked; one that ends in an older
 * generation frees its own block on the worker and is never shown, and the next want asks again, so after a reinstall the icon
 * that shows is the new file's.
 *
 * A descriptor from apps_icons_get is the file in place: its header is the file's first 12 bytes with only the premultiplied flag
 * kept, and its data the pixels after them (as apps_lv.c makes an image's). It stays valid while its app stays in the window, until
 * apps_icons_drop_all: after either changes, the caller asks again for each item's icon (NULL: its tile) before LVGL draws, so no
 * image is left pointing at a freed block. LVGL's image cache is off on the gadget (CONFIG_LV_CACHE_DEF_SIZE 0), so a descriptor
 * that is used again for another icon meets no stale entry. An A8 icon is a mask: the caller recolours it, as apps_lv.c does.
 *
 * Threads. Every function here is the LVGL task's (one task: they share plain state), outside the UI lock (apps_port_ui_lock), as
 * the apps' heap asks; the worker's job touches only its own slot until it marks it done. The table is static, in PSRAM.
 */

#include <stdbool.h>

#include "lvgl.h"

#define APPS_ICONS_WINDOW 5           /* the centre, two each side: the visible ones and one more each side */
#define APPS_ICONS_IN_FLIGHT 2        /* loads the worker has at once, stale ones included */
#define APPS_ICONS_SLOTS (APPS_ICONS_WINDOW + APPS_ICONS_IN_FLIGHT)
#define APPS_ICONS_RETRY_MS 1000      /* a load the worker refused is asked for again this much later */

/* The window, centre first: ids[i]'s icon is icons[i] (the path in its folder, "" for none). At most APPS_ICONS_WINDOW of them are
 * taken; NULL, an id or a path that isn't valid, a repeated id, and an empty icon are left out. It frees the ready icons no longer
 * wanted, and asks for the wanted ones that aren't ready or loading, nor failed while they stayed. Both arrays are copied. */
void apps_icons_want(const char *const *ids, const char *const *icons, int n);

/* id's ready descriptor, or NULL: the caller shows the tile. */
const lv_image_dsc_t *apps_icons_get(const char *id);

/* Each frame, while the launcher shows: adopts the loads that ended, frees those no longer wanted, asks for the next. True when an
 * icon became ready since the last poll: the caller asks again for its items' icons. */
bool apps_icons_poll(void);

/* The launcher's close, a change of the list, a voice turn: frees every ready icon and forgets the window. A load in flight frees
 * its block when it ends. */
void apps_icons_drop_all(void);
