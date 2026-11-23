#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/res.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

// Configuration {{{

/** Maximum number of windows remembered per program. */
constexpr int MAX_SLOTS = 4;

/** How long a window must stay still before its geometry is recorded. */
constexpr int64_t SNAPSHOT_DELAY_MS = 200;

/** How long to wait after a restore before checking that it stuck. */
constexpr int64_t SETTLE_DELAY_MS = 500;

/** How long to wait after a monitor change for the window manager to finish. */
constexpr int64_t LAYOUT_DELAY_MS = 1000;

/** How long changes are batched before being written to disk. */
constexpr int64_t FLUSH_DELAY_MS = 1000;

/** How many times a restore is sent before giving in to the program. */
constexpr int MAX_RESTORE_ATTEMPTS = 2;

/** Version number written in the header of the state file. */
constexpr int STATE_FILE_VERSION = 1;

// }}}

// Types {{{

typedef enum {
	SLOT_STATE_MAXIMIZED_VERT = 1 << 0,
	SLOT_STATE_MAXIMIZED_HORZ = 1 << 1,
} slot_state_t;

/** The remembered placement of one window. */
typedef struct slot_s {
	bool valid;
	/** Top left corner of the window frame, in root coordinates. */
	int32_t x, y;
	/** Size of the client area. */
	uint32_t width, height;
	/** Combination of slot_state_t. */
	uint32_t state;
	/** WM_WINDOW_ROLE of the window, never null when valid. */
	char* role;
} slot_t;

/** What identifies a program. */
typedef struct app_key_s {
	char* path;
	char* instance;
	char* class_name;
} app_key_t;

/** The remembered windows of one program under one monitor layout. */
typedef struct record_s {
	char* layout;
	app_key_t key;
	slot_t slots[MAX_SLOTS];
} record_t;

/** A window listed in _NET_CLIENT_LIST. */
typedef struct window_s {
	xcb_window_t id;
	/** False for windows that are deliberately left alone. */
	bool tracked;
	app_key_t key;
	char* role;
	int slot;
	/** When to record the geometry, 0 if nothing is pending. */
	int64_t snapshot_at;
	/** Geometry changes before this time are caused by a restore. */
	int64_t settle_until;
	/** Number of restores sent that have not been verified yet. */
	int restore_attempts;
} window_t;

typedef struct monitor_rect_s {
	int32_t x, y;
	uint32_t width, height;
} monitor_rect_t;

typedef struct xmemory_s {
	xcb_connection_t* conn;
	xcb_ewmh_connection_t ewmh;
	int screen_nbr;
	xcb_window_t root;
	xcb_atom_t wm_window_role;
	xcb_atom_t wm_client_machine;

	bool has_res;
	bool has_randr;
	bool has_randr_monitors;
	uint8_t randr_event_base;
	bool has_moveresize;

	/** Fingerprint of the current monitor layout. */
	char* layout;
	/** Whether the layout changed since the last time windows were placed. */
	bool layout_changed;
	/** When the monitor change is considered finished, 0 if none is pending. */
	int64_t layout_at;
	/** When to write the state file, 0 if it is up to date. */
	int64_t flush_at;

	window_t* windows;
	size_t num_windows;
	size_t windows_capacity;

	record_t* records;
	size_t num_records;
	size_t records_capacity;

	const char* state_path;
	bool verbose;
	bool quit;
} xmemory_t;

// }}}

// Utilities {{{

#define LOG_WARN(...) \
	do { \
		fprintf(stderr, "xmemory: " __VA_ARGS__); \
		fputc('\n', stderr); \
	} while (0)

#define LOG_DEBUG(XM, ...) \
	do { \
		if ((XM)->verbose) { \
			LOG_WARN(__VA_ARGS__); \
		} \
	} while (0)

/** realloc that aborts when out of memory. */
static void*
xrealloc(
	void* ptr,
	size_t size
) {
	void* result = realloc(ptr, size);
	if (result == nullptr && size != 0) {
		LOG_WARN("Out of memory");
		abort();
	}
	return result;
}

/** strdup that aborts when out of memory. */
static char*
xstrdup(
	const char* str
) {
	size_t size = strlen(str) + 1;
	return memcpy(xrealloc(nullptr, size), str, size);
}

/** Current monotonic time in milliseconds. */
static int64_t
now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Parse a whole string as a decimal integer within a range.
 *
 * @return Whether the string is a valid integer in [min, max].
 */
static bool
parse_int(
	const char* str,
	int64_t min,
	int64_t max,
	int64_t* out
) {
	char* end;
	errno = 0;
	long long value = strtoll(str, &end, 10);
	if (errno != 0 || end == str || *end != '\0' || value < min || value > max) {
		return false;
	}
	*out = value;
	return true;
}

/** Check whether two keys identify the same program. */
static bool
app_key_equal(
	const app_key_t* lhs,
	const app_key_t* rhs
) {
	return strcmp(lhs->path, rhs->path) == 0
		&& strcmp(lhs->instance, rhs->instance) == 0
		&& strcmp(lhs->class_name, rhs->class_name) == 0;
}

/** Free the strings owned by a key. */
static void
app_key_free(
	app_key_t* key
) {
	free(key->path);
	free(key->instance);
	free(key->class_name);
	*key = (app_key_t){ 0 };
}

// }}}

// Storage {{{

static const char* const STATE_NAMES[] = { "normal", "maxv", "maxh", "max" };

/**
 * Write a string as a single whitespace-free token.
 *
 * Whitespace, control characters and '%' are percent-encoded.
 * The empty string is written as "-".
 */
static void
escape_string(
	FILE* file,
	const char* str
) {
	if (str[0] == '\0') {
		fputc('-', file);
		return;
	}
	if (strcmp(str, "-") == 0) {
		fputs("%2D", file);
		return;
	}
	for (const unsigned char* c = (const unsigned char*)str; *c != '\0'; ++c) {
		if (*c <= ' ' || *c == '%' || *c == 0x7f) {
			fprintf(file, "%%%02X", *c);
		} else {
			fputc(*c, file);
		}
	}
}

/** Decode a token written by escape_string in place. */
static char*
unescape_string(
	char* str
) {
	if (strcmp(str, "-") == 0) {
		str[0] = '\0';
		return str;
	}
	char* out = str;
	const char* in = str;
	while (*in != '\0') {
		if (
			in[0] == '%'
			&& isxdigit((unsigned char)in[1])
			&& isxdigit((unsigned char)in[2])
		) {
			char hex[3] = { in[1], in[2], '\0' };
			*out++ = (char)strtol(hex, nullptr, 16);
			in += 3;
		} else {
			*out++ = *in++;
		}
	}
	*out = '\0';
	return str;
}

/** Find the record of a program under a monitor layout, or nullptr. */
static record_t*
store_find(
	xmemory_t* xm,
	const char* layout,
	const app_key_t* key
) {
	for (size_t i = 0; i < xm->num_records; ++i) {
		record_t* record = &xm->records[i];
		if (strcmp(record->layout, layout) == 0 && app_key_equal(&record->key, key)) {
			return record;
		}
	}
	return nullptr;
}

/**
 * Find or create the record of a program under a monitor layout.
 *
 * The returned pointer is only valid until the next call.
 */
static record_t*
store_get(
	xmemory_t* xm,
	const char* layout,
	const app_key_t* key
) {
	record_t* record = store_find(xm, layout, key);
	if (record != nullptr) { return record; }

	if (xm->num_records == xm->records_capacity) {
		xm->records_capacity = xm->records_capacity > 0 ? xm->records_capacity * 2 : 16;
		xm->records = xrealloc(xm->records, xm->records_capacity * sizeof(record_t));
	}
	record = &xm->records[xm->num_records++];
	*record = (record_t){
		.layout = xstrdup(layout),
		.key = {
			.path = xstrdup(key->path),
			.instance = xstrdup(key->instance),
			.class_name = xstrdup(key->class_name),
		},
	};
	return record;
}

/** Schedule the state file to be written. */
static void
store_mark_dirty(
	xmemory_t* xm
) {
	if (xm->flush_at == 0) {
		xm->flush_at = now_ms() + FLUSH_DELAY_MS;
	}
}

/** Parse the arguments of a "slot" line into a record. */
static bool
store_parse_slot(
	record_t* record,
	char** tokens,
	int num_tokens
) {
	int64_t index, x, y, width, height;
	if (
		num_tokens != 8
		|| !parse_int(tokens[1], 0, MAX_SLOTS - 1, &index)
		|| !parse_int(tokens[2], INT16_MIN, INT16_MAX, &x)
		|| !parse_int(tokens[3], INT16_MIN, INT16_MAX, &y)
		|| !parse_int(tokens[4], 1, UINT16_MAX, &width)
		|| !parse_int(tokens[5], 1, UINT16_MAX, &height)
	) {
		return false;
	}

	uint32_t state = 0;
	while (strcmp(tokens[6], STATE_NAMES[state]) != 0) {
		if (++state == sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0])) { return false; }
	}

	slot_t* slot = &record->slots[index];
	free(slot->role);
	*slot = (slot_t){
		.valid = true,
		.x = (int32_t)x,
		.y = (int32_t)y,
		.width = (uint32_t)width,
		.height = (uint32_t)height,
		.state = state,
		.role = xstrdup(unescape_string(tokens[7])),
	};
	return true;
}

/** Load the state file, if there is one. */
static void
store_load(
	xmemory_t* xm
) {
	FILE* file = fopen(xm->state_path, "r");
	if (file == nullptr) {
		if (errno != ENOENT) {
			LOG_WARN("Could not open %s: %s", xm->state_path, strerror(errno));
		}
		return;
	}

	char* line = nullptr;
	size_t line_capacity = 0;
	int line_number = 0;
	char* layout = nullptr;
	bool has_record = false;
	size_t record_index = 0;

	while (getline(&line, &line_capacity, file) >= 0) {
		++line_number;

		char* tokens[10];
		int num_tokens = 0;
		char* save;
		for (
			char* token = strtok_r(line, " \t\r\n", &save);
			token != nullptr && num_tokens < 10;
			token = strtok_r(nullptr, " \t\r\n", &save)
		) {
			tokens[num_tokens++] = token;
		}
		if (num_tokens == 0) { continue; }

		bool ok = false;
		if (line_number == 1) {
			int64_t version;
			if (
				num_tokens != 2
				|| strcmp(tokens[0], "xmemory") != 0
				|| !parse_int(tokens[1], STATE_FILE_VERSION, STATE_FILE_VERSION, &version)
			) {
				LOG_WARN("%s: Unsupported file format, ignoring", xm->state_path);
				break;
			}
			ok = true;
		} else if (strcmp(tokens[0], "layout") == 0 && num_tokens == 2) {
			free(layout);
			layout = xstrdup(unescape_string(tokens[1]));
			has_record = false;
			ok = true;
		} else if (strcmp(tokens[0], "app") == 0 && num_tokens == 4 && layout != nullptr) {
			app_key_t key = {
				.path = unescape_string(tokens[1]),
				.instance = unescape_string(tokens[2]),
				.class_name = unescape_string(tokens[3]),
			};
			record_index = (size_t)(store_get(xm, layout, &key) - xm->records);
			has_record = true;
			ok = true;
		} else if (strcmp(tokens[0], "slot") == 0 && has_record) {
			ok = store_parse_slot(&xm->records[record_index], tokens, num_tokens);
		}

		if (!ok) {
			LOG_WARN("%s:%d: Malformed line, ignoring", xm->state_path, line_number);
		}
	}

	LOG_DEBUG(xm, "Loaded %zu records from %s", xm->num_records, xm->state_path);
	free(layout);
	free(line);
	fclose(file);
}

/** Order records by layout then program, for a stable state file. */
static int
store_compare_records(
	const void* lhs,
	const void* rhs
) {
	const record_t* lhs_record = lhs;
	const record_t* rhs_record = rhs;
	int result;
	if ((result = strcmp(lhs_record->layout, rhs_record->layout)) != 0) { return result; }
	if ((result = strcmp(lhs_record->key.path, rhs_record->key.path)) != 0) { return result; }
	if ((result = strcmp(lhs_record->key.instance, rhs_record->key.instance)) != 0) { return result; }
	return strcmp(lhs_record->key.class_name, rhs_record->key.class_name);
}

/** Create all the missing parent directories of a file. */
static void
make_parent_dirs(
	const char* path
) {
	char* copy = xstrdup(path);
	for (char* separator = strchr(copy + 1, '/'); separator != nullptr; separator = strchr(separator + 1, '/')) {
		*separator = '\0';
		mkdir(copy, 0700);
		*separator = '/';
	}
	free(copy);
}

/** Write all records to an already opened file. */
static void
store_write(
	xmemory_t* xm,
	FILE* file
) {
	fprintf(file, "xmemory %d\n", STATE_FILE_VERSION);

	const char* layout = nullptr;
	for (size_t i = 0; i < xm->num_records; ++i) {
		const record_t* record = &xm->records[i];

		bool has_slots = false;
		for (int j = 0; j < MAX_SLOTS; ++j) {
			has_slots |= record->slots[j].valid;
		}
		if (!has_slots) { continue; }

		if (layout == nullptr || strcmp(layout, record->layout) != 0) {
			layout = record->layout;
			fputs("layout ", file);
			escape_string(file, layout);
			fputc('\n', file);
		}

		fputs("app ", file);
		escape_string(file, record->key.path);
		fputc(' ', file);
		escape_string(file, record->key.instance);
		fputc(' ', file);
		escape_string(file, record->key.class_name);
		fputc('\n', file);

		for (int j = 0; j < MAX_SLOTS; ++j) {
			const slot_t* slot = &record->slots[j];
			if (!slot->valid) { continue; }

			fprintf(
				file,
				"slot %d %" PRId32 " %" PRId32 " %" PRIu32 " %" PRIu32 " %s ",
				j, slot->x, slot->y, slot->width, slot->height,
				STATE_NAMES[slot->state]
			);
			escape_string(file, slot->role);
			fputc('\n', file);
		}
	}
}

/** Atomically replace the state file with the current records. */
static void
store_save(
	xmemory_t* xm
) {
	xm->flush_at = 0;
	qsort(xm->records, xm->num_records, sizeof(record_t), store_compare_records);

	size_t tmp_path_size = strlen(xm->state_path) + sizeof(".tmp");
	char* tmp_path = xrealloc(nullptr, tmp_path_size);
	snprintf(tmp_path, tmp_path_size, "%s.tmp", xm->state_path);

	make_parent_dirs(xm->state_path);
	FILE* file = fopen(tmp_path, "w");
	if (file == nullptr) {
		LOG_WARN("Could not open %s: %s", tmp_path, strerror(errno));
		free(tmp_path);
		return;
	}

	store_write(xm, file);

	bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
	ok &= fclose(file) == 0;
	ok = ok && rename(tmp_path, xm->state_path) == 0;
	if (ok) {
		LOG_DEBUG(xm, "Saved %s", xm->state_path);
	} else {
		LOG_WARN("Could not write %s: %s", xm->state_path, strerror(errno));
		unlink(tmp_path);
	}
	free(tmp_path);
}

// }}}

// Monitor layout {{{

/** Order monitors by position then size. */
static int
layout_compare_rects(
	const void* lhs,
	const void* rhs
) {
	const monitor_rect_t* lhs_rect = lhs;
	const monitor_rect_t* rhs_rect = rhs;
	if (lhs_rect->x != rhs_rect->x) { return lhs_rect->x < rhs_rect->x ? -1 : 1; }
	if (lhs_rect->y != rhs_rect->y) { return lhs_rect->y < rhs_rect->y ? -1 : 1; }
	if (lhs_rect->width != rhs_rect->width) { return lhs_rect->width < rhs_rect->width ? -1 : 1; }
	if (lhs_rect->height != rhs_rect->height) { return lhs_rect->height < rhs_rect->height ? -1 : 1; }
	return 0;
}

/**
 * Compute the fingerprint of the current monitor layout.
 *
 * The fingerprint is the sorted list of the active monitors' geometry, such
 * as "1920x1080+0+0,2560x1440+1920+0".
 * Only geometry is used since it is the only thing window positions depend on.
 *
 * @return A heap allocated string.
 */
static char*
layout_query(
	xmemory_t* xm
) {
	monitor_rect_t* rects = nullptr;
	int num_rects = 0;

	if (xm->has_randr_monitors) {
		xcb_randr_get_monitors_reply_t* reply = xcb_randr_get_monitors_reply(
			xm->conn,
			xcb_randr_get_monitors(xm->conn, xm->root, true),
			nullptr
		);
		if (reply != nullptr) {
			rects = xrealloc(
				nullptr,
				(size_t)xcb_randr_get_monitors_monitors_length(reply) * sizeof(monitor_rect_t)
			);
			for (
				xcb_randr_monitor_info_iterator_t itr = xcb_randr_get_monitors_monitors_iterator(reply);
				itr.rem > 0;
				xcb_randr_monitor_info_next(&itr)
			) {
				rects[num_rects++] = (monitor_rect_t){
					.x = itr.data->x,
					.y = itr.data->y,
					.width = itr.data->width,
					.height = itr.data->height,
				};
			}
			free(reply);
		}
	}

	// Without usable RandR, the whole screen is the only monitor
	if (num_rects == 0) {
		xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(
			xm->conn,
			xcb_get_geometry(xm->conn, xm->root),
			nullptr
		);
		rects = xrealloc(rects, sizeof(monitor_rect_t));
		rects[num_rects++] = (monitor_rect_t){
			.width = reply != nullptr ? reply->width : 0,
			.height = reply != nullptr ? reply->height : 0,
		};
		free(reply);
	}

	qsort(rects, (size_t)num_rects, sizeof(monitor_rect_t), layout_compare_rects);

	constexpr size_t MAX_RECT_LENGTH = sizeof("65535x65535+-32768+-32768,");
	size_t capacity = (size_t)num_rects * MAX_RECT_LENGTH + 1;
	char* layout = xrealloc(nullptr, capacity);
	size_t length = 0;
	for (int i = 0; i < num_rects; ++i) {
		length += (size_t)snprintf(
			layout + length, capacity - length,
			"%s%" PRIu32 "x%" PRIu32 "+%" PRId32 "+%" PRId32,
			i > 0 ? "," : "",
			rects[i].width, rects[i].height, rects[i].x, rects[i].y
		);
	}

	free(rects);
	return layout;
}

/**
 * Re-read the monitor layout.
 *
 * @return Whether it is different from the previously known one.
 */
static bool
layout_refresh(
	xmemory_t* xm
) {
	char* layout = layout_query(xm);
	if (xm->layout != nullptr && strcmp(layout, xm->layout) == 0) {
		free(layout);
		return false;
	}

	LOG_DEBUG(xm, "Monitor layout: %s", layout);
	free(xm->layout);
	xm->layout = layout;
	return true;
}

// }}}

// Windows {{{

/**
 * Find a known window by its id, or nullptr.
 *
 * The returned pointer is only valid until a window is added or removed.
 */
static window_t*
window_find(
	xmemory_t* xm,
	xcb_window_t id
) {
	for (size_t i = 0; i < xm->num_windows; ++i) {
		if (xm->windows[i].id == id) { return &xm->windows[i]; }
	}
	return nullptr;
}

/** Stop tracking a window. Its slot keeps the last recorded geometry. */
static void
window_remove(
	xmemory_t* xm,
	window_t* window
) {
	if (window->tracked) {
		LOG_DEBUG(xm, "0x%08x: Closed", window->id);
	}
	app_key_free(&window->key);
	free(window->role);
	*window = xm->windows[--xm->num_windows];
}

/**
 * Fetch the raw content of a property.
 *
 * @param length Receives the length of the content.
 * @return A heap allocated and null-terminated buffer, or nullptr if the
 *   property does not exist.
 */
static char*
window_get_property(
	xmemory_t* xm,
	xcb_window_t id,
	xcb_atom_t property,
	size_t* length
) {
	xcb_get_property_reply_t* reply = xcb_get_property_reply(
		xm->conn,
		xcb_get_property(xm->conn, false, id, property, XCB_GET_PROPERTY_TYPE_ANY, 0, 1024),
		nullptr
	);
	if (reply == nullptr || reply->type == XCB_ATOM_NONE || reply->format != 8) {
		free(reply);
		return nullptr;
	}

	*length = (size_t)xcb_get_property_value_length(reply);
	char* result = xrealloc(nullptr, *length + 1);
	memcpy(result, xcb_get_property_value(reply), *length);
	result[*length] = '\0';
	free(reply);
	return result;
}

/**
 * Check whether a window is a regular application window.
 *
 * Dialogs, utility windows, docks, menus... are left alone.
 */
static bool
window_is_normal(
	xmemory_t* xm,
	xcb_window_t id
) {
	xcb_get_property_cookie_t type_cookie = xcb_ewmh_get_wm_window_type(&xm->ewmh, id);
	xcb_get_property_cookie_t transient_cookie = xcb_get_property(
		xm->conn, false, id, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1
	);

	bool normal = true;

	xcb_ewmh_get_atoms_reply_t types;
	if (xcb_ewmh_get_wm_window_type_reply(&xm->ewmh, type_cookie, &types, nullptr)) {
		if (types.atoms_len > 0) {
			normal = false;
			for (uint32_t i = 0; i < types.atoms_len; ++i) {
				normal |= types.atoms[i] == xm->ewmh._NET_WM_WINDOW_TYPE_NORMAL;
			}
		}
		xcb_ewmh_get_atoms_reply_wipe(&types);
	}

	xcb_get_property_reply_t* transient = xcb_get_property_reply(xm->conn, transient_cookie, nullptr);
	if (
		transient != nullptr
		&& transient->type == XCB_ATOM_WINDOW
		&& transient->format == 32
		&& xcb_get_property_value_length(transient) >= (int)sizeof(xcb_window_t)
	) {
		normal &= *(xcb_window_t*)xcb_get_property_value(transient) == XCB_WINDOW_NONE;
	}
	free(transient);

	return normal;
}

/**
 * Find the process that owns a window.
 *
 * @return The pid, or 0 if it can not be determined.
 */
static uint32_t
window_query_pid(
	xmemory_t* xm,
	xcb_window_t id
) {
	uint32_t pid = 0;

	// Ask the server first since it can not be spoofed
	if (xm->has_res) {
		xcb_res_client_id_spec_t spec = {
			.client = id,
			.mask = XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID,
		};
		xcb_res_query_client_ids_reply_t* reply = xcb_res_query_client_ids_reply(
			xm->conn,
			xcb_res_query_client_ids(xm->conn, 1, &spec),
			nullptr
		);
		if (reply != nullptr) {
			for (
				xcb_res_client_id_value_iterator_t itr = xcb_res_query_client_ids_ids_iterator(reply);
				itr.rem > 0;
				xcb_res_client_id_value_next(&itr)
			) {
				if (
					(itr.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) != 0
					&& xcb_res_client_id_value_value_length(itr.data) >= 1
				) {
					pid = xcb_res_client_id_value_value(itr.data)[0];
					break;
				}
			}
			free(reply);
		}
	}
	if (pid != 0) { return pid; }

	// Fallback to what the client claims, as long as it is on this machine
	size_t machine_length;
	char* machine = window_get_property(xm, id, xm->wm_client_machine, &machine_length);
	char hostname[HOST_NAME_MAX + 1] = { 0 };
	gethostname(hostname, sizeof(hostname) - 1);
	bool local = machine == nullptr || strcmp(machine, hostname) == 0;
	free(machine);

	if (local) {
		xcb_ewmh_get_wm_pid_reply(&xm->ewmh, xcb_ewmh_get_wm_pid(&xm->ewmh, id), &pid, nullptr);
	}
	return pid;
}

/**
 * Find the path to the executable that owns a window.
 *
 * @return A heap allocated string or nullptr.
 */
static char*
window_query_exe(
	xmemory_t* xm,
	xcb_window_t id
) {
	uint32_t pid = window_query_pid(xm, id);
	if (pid == 0) { return nullptr; }

	char link_path[64];
	snprintf(link_path, sizeof(link_path), "/proc/%" PRIu32 "/exe", pid);
	char exe_path[PATH_MAX];
	ssize_t length = readlink(link_path, exe_path, sizeof(exe_path) - 1);
	if (length <= 0) { return nullptr; }
	exe_path[length] = '\0';

	// The executable was replaced while the program is running (e.g: upgraded)
	static const char DELETED_SUFFIX[] = " (deleted)";
	constexpr ssize_t DELETED_SUFFIX_LENGTH = sizeof(DELETED_SUFFIX) - 1;
	if (
		length > DELETED_SUFFIX_LENGTH
		&& strcmp(exe_path + length - DELETED_SUFFIX_LENGTH, DELETED_SUFFIX) == 0
	) {
		exe_path[length - DELETED_SUFFIX_LENGTH] = '\0';
	}

	return xstrdup(exe_path);
}

/**
 * Read the current placement of a window.
 *
 * @param snapshot Receives the placement. Its role is not set.
 * @param normal Receives whether the geometry is the window's regular one.
 *   That is: it is not maximized, fullscreen or minimized.
 * @return false if the window no longer exists.
 */
static bool
window_snapshot(
	xmemory_t* xm,
	xcb_window_t id,
	slot_t* snapshot,
	bool* normal
) {
	xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry(xm->conn, id);
	xcb_translate_coordinates_cookie_t translate_cookie = xcb_translate_coordinates(
		xm->conn, id, xm->root, 0, 0
	);
	xcb_get_property_cookie_t extents_cookie = xcb_ewmh_get_frame_extents(&xm->ewmh, id);
	xcb_get_property_cookie_t state_cookie = xcb_ewmh_get_wm_state(&xm->ewmh, id);

	xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(
		xm->conn, geometry_cookie, nullptr
	);
	xcb_translate_coordinates_reply_t* translate = xcb_translate_coordinates_reply(
		xm->conn, translate_cookie, nullptr
	);
	xcb_ewmh_get_extents_reply_t extents = { 0 };
	if (!xcb_ewmh_get_frame_extents_reply(&xm->ewmh, extents_cookie, &extents, nullptr)) {
		extents = (xcb_ewmh_get_extents_reply_t){ 0 };
	}
	xcb_ewmh_get_atoms_reply_t states;
	bool has_states = xcb_ewmh_get_wm_state_reply(&xm->ewmh, state_cookie, &states, nullptr);

	bool exists = geometry != nullptr && translate != nullptr;
	if (exists) {
		// (0, 0) is the corner inside the window's border.
		// What gets recorded is the outer corner of the frame since that is
		// what _NET_MOVERESIZE_WINDOW expects with NorthWest gravity.
		*snapshot = (slot_t){
			.valid = true,
			.x = translate->dst_x - geometry->border_width - (int32_t)extents.left,
			.y = translate->dst_y - geometry->border_width - (int32_t)extents.top,
			.width = geometry->width,
			.height = geometry->height,
		};
		*normal = true;
		for (uint32_t i = 0; has_states && i < states.atoms_len; ++i) {
			xcb_atom_t state = states.atoms[i];
			if (state == xm->ewmh._NET_WM_STATE_MAXIMIZED_VERT) {
				snapshot->state |= SLOT_STATE_MAXIMIZED_VERT;
				*normal = false;
			} else if (state == xm->ewmh._NET_WM_STATE_MAXIMIZED_HORZ) {
				snapshot->state |= SLOT_STATE_MAXIMIZED_HORZ;
				*normal = false;
			} else if (
				state == xm->ewmh._NET_WM_STATE_FULLSCREEN
				|| state == xm->ewmh._NET_WM_STATE_HIDDEN
			) {
				*normal = false;
			}
		}
	}

	if (has_states) { xcb_ewmh_get_atoms_reply_wipe(&states); }
	free(geometry);
	free(translate);
	return exists;
}

/** Record the geometry of a window once it stops changing. */
static void
window_schedule_snapshot(
	window_t* window
) {
	int64_t snapshot_at = now_ms() + SNAPSHOT_DELAY_MS;
	window->snapshot_at = snapshot_at > window->settle_until ? snapshot_at : window->settle_until;
}

/**
 * Ask the window manager to place a window according to a slot.
 *
 * The window stays on whichever virtual desktop it is on.
 * Unlike the geometry, that does not depend on the monitor layout and a new
 * window is expected to show up where the user is, not where its predecessor
 * was closed.
 */
static void
window_restore(
	xmemory_t* xm,
	window_t* window,
	const slot_t* slot
) {
	xcb_ewmh_connection_t* ewmh = &xm->ewmh;
	LOG_DEBUG(
		xm,
		"0x%08x: Restoring to %" PRIu32 "x%" PRIu32 "+%" PRId32 "+%" PRId32 " %s",
		window->id, slot->width, slot->height, slot->x, slot->y,
		STATE_NAMES[slot->state]
	);

	// The window must be unmaximized first, otherwise, the window manager
	// would not apply the regular geometry.
	xcb_ewmh_request_change_wm_state(
		ewmh, xm->screen_nbr, window->id,
		XCB_EWMH_WM_STATE_REMOVE,
		ewmh->_NET_WM_STATE_MAXIMIZED_VERT, ewmh->_NET_WM_STATE_MAXIMIZED_HORZ,
		XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER
	);

	if (xm->has_moveresize) {
		xcb_ewmh_request_moveresize_window(
			ewmh, xm->screen_nbr, window->id,
			XCB_GRAVITY_NORTH_WEST,
			XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER,
			XCB_EWMH_MOVERESIZE_WINDOW_X
			| XCB_EWMH_MOVERESIZE_WINDOW_Y
			| XCB_EWMH_MOVERESIZE_WINDOW_WIDTH
			| XCB_EWMH_MOVERESIZE_WINDOW_HEIGHT,
			(uint32_t)slot->x, (uint32_t)slot->y, slot->width, slot->height
		);
	} else {
		xcb_configure_window(
			xm->conn, window->id,
			XCB_CONFIG_WINDOW_X
			| XCB_CONFIG_WINDOW_Y
			| XCB_CONFIG_WINDOW_WIDTH
			| XCB_CONFIG_WINDOW_HEIGHT,
			(uint32_t[]){ (uint32_t)slot->x, (uint32_t)slot->y, slot->width, slot->height }
		);
	}

	xcb_atom_t maximized[2] = { XCB_ATOM_NONE, XCB_ATOM_NONE };
	int num_maximized = 0;
	if ((slot->state & SLOT_STATE_MAXIMIZED_VERT) != 0) {
		maximized[num_maximized++] = ewmh->_NET_WM_STATE_MAXIMIZED_VERT;
	}
	if ((slot->state & SLOT_STATE_MAXIMIZED_HORZ) != 0) {
		maximized[num_maximized++] = ewmh->_NET_WM_STATE_MAXIMIZED_HORZ;
	}
	if (num_maximized > 0) {
		xcb_ewmh_request_change_wm_state(
			ewmh, xm->screen_nbr, window->id,
			XCB_EWMH_WM_STATE_ADD,
			maximized[0], maximized[1],
			XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER
		);
	}

	// Check back later instead of recording the changes caused by the restore
	++window->restore_attempts;
	window->settle_until = now_ms() + SETTLE_DELAY_MS;
	window->snapshot_at = window->settle_until;
}

/**
 * Check whether a restored window ended up where it was supposed to be.
 *
 * @param saved The slot that was restored.
 * @param snapshot The current placement of the window.
 * @param normal Whether the snapshot has the window's regular geometry.
 */
static bool
window_is_restored(
	const slot_t* saved,
	const slot_t* snapshot,
	bool normal
) {
	if (saved->state != snapshot->state) { return false; }
	// The regular geometry is not observable while the window is maximized,
	// fullscreen or minimized.
	if (!normal) { return true; }

	return saved->x == snapshot->x
		&& saved->y == snapshot->y
		&& saved->width == snapshot->width
		&& saved->height == snapshot->height;
}

/**
 * Take a snapshot of a window and store it in the window's slot.
 *
 * If the window was recently restored and it is not where it should be, the
 * restore is retried instead.
 *
 * @return false if the window no longer exists and was removed.
 */
static bool
window_record(
	xmemory_t* xm,
	window_t* window
) {
	window->snapshot_at = 0;

	slot_t snapshot;
	bool normal = false;
	if (!window_snapshot(xm, window->id, &snapshot, &normal)) {
		window_remove(xm, window);
		return false;
	}

	record_t* record = store_get(xm, xm->layout, &window->key);
	slot_t* slot = &record->slots[window->slot];

	if (window->restore_attempts > 0) {
		if (
			slot->valid
			&& window->restore_attempts < MAX_RESTORE_ATTEMPTS
			&& !window_is_restored(slot, &snapshot, normal)
		) {
			window_restore(xm, window, slot);
			return true;
		}
		window->restore_attempts = 0;
	}

	if (slot->valid) {
		// Keep the regular geometry so that it can be restored later
		if (!normal) {
			snapshot.x = slot->x;
			snapshot.y = slot->y;
			snapshot.width = slot->width;
			snapshot.height = slot->height;
		}
		if (
			snapshot.x == slot->x
			&& snapshot.y == slot->y
			&& snapshot.width == slot->width
			&& snapshot.height == slot->height
			&& snapshot.state == slot->state
			&& strcmp(slot->role, window->role) == 0
		) {
			return true;
		}
	}

	free(slot->role);
	*slot = snapshot;
	slot->role = xstrdup(window->role);
	store_mark_dirty(xm);
	LOG_DEBUG(
		xm,
		"0x%08x: Recorded %" PRIu32 "x%" PRIu32 "+%" PRId32 "+%" PRId32 " %s",
		window->id, slot->width, slot->height, slot->x, slot->y,
		STATE_NAMES[slot->state]
	);
	return true;
}

/**
 * Place a window according to its slot under the current monitor layout.
 *
 * If nothing was saved, the current placement is recorded instead.
 */
static void
window_apply_layout(
	xmemory_t* xm,
	window_t* window
) {
	window->restore_attempts = 0;
	window->settle_until = 0;

	const record_t* record = store_find(xm, xm->layout, &window->key);
	const slot_t* slot = record != nullptr ? &record->slots[window->slot] : nullptr;
	if (slot != nullptr && slot->valid) {
		window_restore(xm, window, slot);
	} else {
		window_schedule_snapshot(window);
	}
}

/**
 * Pick a slot for a new window.
 *
 * @return The slot index, or -1 if the program already has too many windows.
 */
static int
window_choose_slot(
	xmemory_t* xm,
	const window_t* window
) {
	bool occupied[MAX_SLOTS] = { 0 };
	for (size_t i = 0; i < xm->num_windows; ++i) {
		const window_t* other = &xm->windows[i];
		if (other->tracked && app_key_equal(&other->key, &window->key)) {
			occupied[other->slot] = true;
		}
	}

	const record_t* record = store_find(xm, xm->layout, &window->key);

	// Prefer a slot that was last used by the same kind of window
	for (int i = 0; record != nullptr && i < MAX_SLOTS; ++i) {
		const slot_t* slot = &record->slots[i];
		if (!occupied[i] && slot->valid && strcmp(slot->role, window->role) == 0) {
			return i;
		}
	}

	// The role is only a hint and can not be relied upon.
	// Some programs give every window a unique role (e.g: xfce4-terminal).
	for (int i = 0; i < MAX_SLOTS; ++i) {
		if (!occupied[i]) { return i; }
	}

	return -1;
}

/**
 * Start managing a window that just appeared in _NET_CLIENT_LIST.
 *
 * @param restore Whether to move the window to its saved position.
 */
static void
window_adopt(
	xmemory_t* xm,
	xcb_window_t id,
	bool restore
) {
	// _NET_CLIENT_LIST can be out of date and list a window that is already
	// destroyed.
	// Once this succeeds, a DestroyNotify is guaranteed so the window can not
	// be left behind, even when its id is reused.
	xcb_generic_error_t* error = xcb_request_check(
		xm->conn,
		xcb_change_window_attributes_checked(
			xm->conn, id,
			XCB_CW_EVENT_MASK,
			(uint32_t[]){ XCB_EVENT_MASK_STRUCTURE_NOTIFY }
		)
	);
	if (error != nullptr) {
		free(error);
		return;
	}

	// Windows that are not tracked are also remembered so that they are only
	// inspected once.
	if (xm->num_windows == xm->windows_capacity) {
		xm->windows_capacity = xm->windows_capacity > 0 ? xm->windows_capacity * 2 : 16;
		xm->windows = xrealloc(xm->windows, xm->windows_capacity * sizeof(window_t));
	}
	window_t* window = &xm->windows[xm->num_windows++];
	*window = (window_t){ .id = id, .slot = -1 };

	if (!window_is_normal(xm, id)) {
		LOG_DEBUG(xm, "0x%08x: Ignored, not a normal window", id);
		return;
	}

	char* path = window_query_exe(xm, id);
	if (path == nullptr) {
		LOG_DEBUG(xm, "0x%08x: Ignored, unknown program", id);
		return;
	}

	// WM_CLASS is two consecutive null-terminated strings
	size_t class_length = 0;
	char* class_bytes = window_get_property(xm, id, XCB_ATOM_WM_CLASS, &class_length);
	const char* instance = class_bytes != nullptr ? class_bytes : "";
	size_t instance_length = strlen(instance);
	const char* class_name = instance_length + 1 < class_length
		? class_bytes + instance_length + 1
		: "";

	size_t role_length;
	char* role = window_get_property(xm, id, xm->wm_window_role, &role_length);

	window->key = (app_key_t){
		.path = path,
		.instance = xstrdup(instance),
		.class_name = xstrdup(class_name),
	};
	window->role = role != nullptr ? role : xstrdup("");
	free(class_bytes);

	window->slot = window_choose_slot(xm, window);
	if (window->slot < 0) {
		LOG_DEBUG(xm, "0x%08x: Ignored, %s has too many windows", id, window->key.path);
		return;
	}
	window->tracked = true;
	LOG_DEBUG(
		xm, "0x%08x: Tracking as slot %d of %s (%s.%s)",
		id, window->slot, window->key.path, window->key.instance, window->key.class_name
	);

	xcb_change_window_attributes(
		xm->conn, id,
		XCB_CW_EVENT_MASK,
		(uint32_t[]){ XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE }
	);

	if (restore) {
		window_apply_layout(xm, window);
	} else {
		window_schedule_snapshot(window);
	}
}

/**
 * Bring the list of windows in line with _NET_CLIENT_LIST.
 *
 * @param restore Whether to move new windows to their saved positions.
 */
static void
client_list_sync(
	xmemory_t* xm,
	bool restore
) {
	xcb_ewmh_get_windows_reply_t clients = { 0 };
	bool has_clients = xcb_ewmh_get_client_list_reply(
		&xm->ewmh,
		xcb_ewmh_get_client_list(&xm->ewmh, xm->screen_nbr),
		&clients,
		nullptr
	);
	if (!has_clients) { clients.windows_len = 0; }

	for (size_t i = 0; i < xm->num_windows;) {
		bool listed = false;
		for (uint32_t j = 0; j < clients.windows_len && !listed; ++j) {
			listed = clients.windows[j] == xm->windows[i].id;
		}

		if (listed) {
			++i;
		} else {
			window_remove(xm, &xm->windows[i]);
		}
	}

	for (uint32_t i = 0; i < clients.windows_len; ++i) {
		if (window_find(xm, clients.windows[i]) == nullptr) {
			window_adopt(xm, clients.windows[i], restore);
		}
	}

	if (has_clients) { xcb_ewmh_get_windows_reply_wipe(&clients); }
}

// }}}

// Event loop {{{

/** Check what the window manager is capable of. */
static void
wm_refresh_supported(
	xmemory_t* xm
) {
	xm->has_moveresize = false;

	xcb_ewmh_get_atoms_reply_t supported;
	if (
		!xcb_ewmh_get_supported_reply(
			&xm->ewmh,
			xcb_ewmh_get_supported(&xm->ewmh, xm->screen_nbr),
			&supported,
			nullptr
		)
	) {
		return;
	}

	for (uint32_t i = 0; i < supported.atoms_len; ++i) {
		xm->has_moveresize |= supported.atoms[i] == xm->ewmh._NET_MOVERESIZE_WINDOW;
	}
	xcb_ewmh_get_atoms_reply_wipe(&supported);
}

/**
 * React to the monitors being reconfigured.
 *
 * Recording is suspended until things settle down so that the windows being
 * shuffled around by the window manager do not overwrite the positions saved
 * for the previous layout.
 */
static void
layout_begin_change(
	xmemory_t* xm
) {
	xm->layout_changed |= layout_refresh(xm);
	xm->layout_at = now_ms() + LAYOUT_DELAY_MS;
}

/** Place all windows according to the new monitor layout. */
static void
layout_finish_change(
	xmemory_t* xm
) {
	xm->layout_at = 0;
	xm->layout_changed |= layout_refresh(xm);
	if (!xm->layout_changed) { return; }
	xm->layout_changed = false;

	for (size_t i = 0; i < xm->num_windows; ++i) {
		if (xm->windows[i].tracked) {
			window_apply_layout(xm, &xm->windows[i]);
		}
	}
}

/** Dispatch a single X event. */
static void
handle_event(
	xmemory_t* xm,
	const xcb_generic_event_t* event
) {
	uint8_t type = event->response_type & 0x7f;

	if (
		xm->has_randr
		&& (
			type == xm->randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY
			|| type == xm->randr_event_base + XCB_RANDR_NOTIFY
		)
	) {
		layout_begin_change(xm);
		return;
	}

	switch (type) {
		case XCB_PROPERTY_NOTIFY: {
			const xcb_property_notify_event_t* property_event = (const void*)event;
			if (property_event->window == xm->root) {
				if (property_event->atom == xm->ewmh._NET_CLIENT_LIST) {
					client_list_sync(xm, true);
				} else if (property_event->atom == xm->ewmh._NET_SUPPORTED) {
					wm_refresh_supported(xm);
				}
				break;
			}

			window_t* window = window_find(xm, property_event->window);
			if (
				window != nullptr
				&& window->tracked
				&& (
					property_event->atom == xm->ewmh._NET_WM_STATE
					|| property_event->atom == xm->ewmh._NET_FRAME_EXTENTS
				)
			) {
				window_schedule_snapshot(window);
			}
		} break;
		case XCB_CONFIGURE_NOTIFY: {
			const xcb_configure_notify_event_t* configure_event = (const void*)event;
			window_t* window = window_find(xm, configure_event->window);
			if (window != nullptr && window->tracked) {
				window_schedule_snapshot(window);
			}
		} break;
		case XCB_DESTROY_NOTIFY: {
			const xcb_destroy_notify_event_t* destroy_event = (const void*)event;
			window_t* window = window_find(xm, destroy_event->window);
			if (window != nullptr) {
				window_remove(xm, window);
			}
		} break;
		case XCB_SELECTION_CLEAR: {
			LOG_WARN("Replaced by another instance");
			xm->quit = true;
		} break;
		default: {
			// Errors are expected since windows can disappear at any moment
		} break;
	}
}

/** Do everything that is due. */
static void
run_timers(
	xmemory_t* xm
) {
	int64_t now = now_ms();

	if (xm->layout_at != 0 && now >= xm->layout_at) {
		layout_finish_change(xm);
	}

	if (xm->layout_at == 0) {
		for (size_t i = 0; i < xm->num_windows;) {
			window_t* window = &xm->windows[i];
			if (
				!window->tracked
				|| window->snapshot_at == 0
				|| now < window->snapshot_at
				|| window_record(xm, window)
			) {
				++i;
			}
		}
	}

	if (xm->flush_at != 0 && now >= xm->flush_at) {
		store_save(xm);
	}
}

/**
 * Compute how long the event loop can sleep.
 *
 * @return A timeout in milliseconds, or -1 if there is nothing scheduled.
 */
static int
next_timeout(
	xmemory_t* xm
) {
	int64_t deadline = INT64_MAX;
	if (xm->flush_at != 0 && xm->flush_at < deadline) {
		deadline = xm->flush_at;
	}

	if (xm->layout_at != 0) {
		if (xm->layout_at < deadline) { deadline = xm->layout_at; }
	} else {
		for (size_t i = 0; i < xm->num_windows; ++i) {
			const window_t* window = &xm->windows[i];
			if (window->tracked && window->snapshot_at != 0 && window->snapshot_at < deadline) {
				deadline = window->snapshot_at;
			}
		}
	}

	if (deadline == INT64_MAX) { return -1; }
	int64_t timeout = deadline - now_ms();
	if (timeout < 0) { return 0; }
	return timeout < INT_MAX ? (int)timeout : INT_MAX;
}

/**
 * Process events until asked to stop.
 *
 * @param signal_fd A signalfd that becomes readable when it is time to exit.
 */
static void
run(
	xmemory_t* xm,
	int signal_fd
) {
	struct pollfd fds[] = {
		{ .fd = xcb_get_file_descriptor(xm->conn), .events = POLLIN },
		{ .fd = signal_fd, .events = POLLIN },
	};

	while (true) {
		xcb_generic_event_t* event;
		while ((event = xcb_poll_for_event(xm->conn)) != nullptr) {
			handle_event(xm, event);
			free(event);
		}
		if (xcb_connection_has_error(xm->conn)) {
			LOG_WARN("Lost connection to the X server");
			break;
		}
		if (xm->quit) { break; }

		run_timers(xm);
		xcb_flush(xm->conn);

		// Waiting for replies in the timers could have queued up some events
		if ((event = xcb_poll_for_queued_event(xm->conn)) != nullptr) {
			handle_event(xm, event);
			free(event);
			continue;
		}

		if (poll(fds, sizeof(fds) / sizeof(fds[0]), next_timeout(xm)) < 0 && errno != EINTR) {
			LOG_WARN("poll: %s", strerror(errno));
			break;
		}
		if ((fds[1].revents & POLLIN) != 0) { break; }
	}
}

// }}}

// Setup {{{

/** Intern a single atom. */
static xcb_atom_t
get_atom(
	xmemory_t* xm,
	const char* name
) {
	xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(
		xm->conn,
		xcb_intern_atom(xm->conn, false, (uint16_t)strlen(name), name),
		nullptr
	);
	xcb_atom_t atom = reply != nullptr ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return atom;
}

/**
 * Make sure that there is only one instance per screen.
 *
 * @return false if another instance is already running.
 */
static bool
acquire_selection(
	xmemory_t* xm
) {
	char name[32];
	snprintf(name, sizeof(name), "_XMEMORY_S%d", xm->screen_nbr);
	xcb_atom_t selection = get_atom(xm, name);

	xcb_grab_server(xm->conn);
	xcb_get_selection_owner_reply_t* owner = xcb_get_selection_owner_reply(
		xm->conn,
		xcb_get_selection_owner(xm->conn, selection),
		nullptr
	);
	bool acquired = owner != nullptr && owner->owner == XCB_WINDOW_NONE;
	free(owner);

	if (acquired) {
		xcb_window_t window = xcb_generate_id(xm->conn);
		xcb_create_window(
			xm->conn,
			XCB_COPY_FROM_PARENT, window, xm->root,
			-1, -1, 1, 1, 0,
			XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
			0, nullptr
		);
		xcb_set_selection_owner(xm->conn, window, selection, XCB_CURRENT_TIME);
	}
	xcb_ungrab_server(xm->conn);
	xcb_flush(xm->conn);

	return acquired;
}

/** Detect the optional extensions and subscribe to their events. */
static void
init_extensions(
	xmemory_t* xm
) {
	const xcb_query_extension_reply_t* res = xcb_get_extension_data(xm->conn, &xcb_res_id);
	if (res != nullptr && res->present) {
		xcb_res_query_version_reply_t* version = xcb_res_query_version_reply(
			xm->conn,
			xcb_res_query_version(xm->conn, 1, 2),
			nullptr
		);
		xm->has_res = version != nullptr
			&& (version->server_major > 1 || (version->server_major == 1 && version->server_minor >= 2));
		free(version);
	}
	if (!xm->has_res) {
		LOG_WARN("X-Resource 1.2 is not available, falling back to _NET_WM_PID");
	}

	const xcb_query_extension_reply_t* randr = xcb_get_extension_data(xm->conn, &xcb_randr_id);
	if (randr != nullptr && randr->present) {
		xcb_randr_query_version_reply_t* version = xcb_randr_query_version_reply(
			xm->conn,
			xcb_randr_query_version(xm->conn, 1, 5),
			nullptr
		);
		xm->has_randr = version != nullptr;
		xm->has_randr_monitors = version != nullptr
			&& (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 5));
		xm->randr_event_base = randr->first_event;
		free(version);
	}
	if (xm->has_randr) {
		xcb_randr_select_input(
			xm->conn, xm->root,
			XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE
			| XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE
			| XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE
		);
	}
	if (!xm->has_randr_monitors) {
		LOG_WARN("RandR 1.5 is not available, treating the screen as a single monitor");
	}
}

/**
 * Pick where the state file is.
 *
 * @return A heap allocated string.
 */
static char*
default_state_path(void) {
	const char* state_home = getenv("XDG_STATE_HOME");
	const char* home = getenv("HOME");
	char* path = nullptr;
	int length;
	if (state_home != nullptr && state_home[0] == '/') {
		length = asprintf(&path, "%s/xmemory/positions", state_home);
	} else {
		length = asprintf(&path, "%s/.local/state/xmemory/positions", home != nullptr ? home : "");
	}
	if (length < 0) {
		LOG_WARN("Out of memory");
		abort();
	}
	return path;
}

/** Release everything and disconnect from the X server. */
static void
cleanup(
	xmemory_t* xm
) {
	while (xm->num_windows > 0) {
		window_t* window = &xm->windows[--xm->num_windows];
		app_key_free(&window->key);
		free(window->role);
	}
	free(xm->windows);

	for (size_t i = 0; i < xm->num_records; ++i) {
		record_t* record = &xm->records[i];
		free(record->layout);
		app_key_free(&record->key);
		for (int j = 0; j < MAX_SLOTS; ++j) {
			free(record->slots[j].role);
		}
	}
	free(xm->records);
	free(xm->layout);

	xcb_ewmh_connection_wipe(&xm->ewmh);
	xcb_disconnect(xm->conn);
}

int
main(
	int argc,
	char* argv[]
) {
	xmemory_t xm = { 0 };
	char* state_path = nullptr;

	int option;
	while ((option = getopt(argc, argv, "f:vh")) != -1) {
		switch (option) {
			case 'f': {
				free(state_path);
				state_path = xstrdup(optarg);
			} break;
			case 'v': {
				xm.verbose = true;
			} break;
			default: {
				fprintf(
					option == 'h' ? stdout : stderr,
					"Usage: %s [-f state_file] [-v]\n"
					"\n"
					"Save and restore window positions.\n"
					"\n"
					"  -f state_file  Where to store the positions.\n"
					"                 Default: $XDG_STATE_HOME/xmemory/positions\n"
					"  -v             Log what is being done to stderr.\n",
					argv[0]
				);
				free(state_path);
				return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
			} break;
		}
	}
	if (state_path == nullptr) { state_path = default_state_path(); }
	xm.state_path = state_path;

	// Signals are handled in the event loop so that the state can be saved
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGHUP);
	sigprocmask(SIG_BLOCK, &signals, nullptr);
	int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
	if (signal_fd < 0) {
		LOG_WARN("signalfd: %s", strerror(errno));
		free(state_path);
		return EXIT_FAILURE;
	}

	xm.conn = xcb_connect(nullptr, &xm.screen_nbr);
	if (xcb_connection_has_error(xm.conn)) {
		LOG_WARN("Could not connect to the X server");
		xcb_disconnect(xm.conn);
		close(signal_fd);
		free(state_path);
		return EXIT_FAILURE;
	}

	int exit_code = EXIT_FAILURE;
	if (!xcb_ewmh_init_atoms_replies(&xm.ewmh, xcb_ewmh_init_atoms(xm.conn, &xm.ewmh), nullptr)) {
		LOG_WARN("Could not initialize EWMH atoms");
		xcb_disconnect(xm.conn);
		close(signal_fd);
		free(state_path);
		return EXIT_FAILURE;
	}
	xm.root = xm.ewmh.screens[xm.screen_nbr]->root;
	xm.wm_window_role = get_atom(&xm, "WM_WINDOW_ROLE");
	xm.wm_client_machine = get_atom(&xm, "WM_CLIENT_MACHINE");

	if (acquire_selection(&xm)) {
		init_extensions(&xm);
		xcb_change_window_attributes(
			xm.conn, xm.root,
			XCB_CW_EVENT_MASK,
			(uint32_t[]){ XCB_EVENT_MASK_PROPERTY_CHANGE }
		);

		store_load(&xm);
		layout_refresh(&xm);
		wm_refresh_supported(&xm);
		// Windows that are already there are left where they are
		client_list_sync(&xm, false);

		run(&xm, signal_fd);

		if (xm.flush_at != 0) { store_save(&xm); }
		exit_code = EXIT_SUCCESS;
	} else {
		LOG_WARN("Another instance is already running");
	}

	cleanup(&xm);
	close(signal_fd);
	free(state_path);
	return exit_code;
}

// }}}
