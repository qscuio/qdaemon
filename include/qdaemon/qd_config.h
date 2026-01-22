/*
 * QDaemon - Configuration API
 * INI-style configuration file parser
 */

#ifndef QD_CONFIG_H
#define QD_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_config qd_config_t;
typedef struct qd_config_section qd_config_section_t;
typedef struct qd_config_entry qd_config_entry_t;

/* Configuration value types */
typedef enum {
    QD_CONFIG_STRING = 0,
    QD_CONFIG_INT,
    QD_CONFIG_FLOAT,
    QD_CONFIG_BOOL,
    QD_CONFIG_ARRAY,
} qd_config_type_t;

/* Configuration entry */
struct qd_config_entry {
    char *key;
    char *value;
    qd_config_type_t type;
    struct qd_config_entry *next;
};

/* Configuration section */
struct qd_config_section {
    char *name;
    qd_config_entry_t *entries;
    struct qd_config_section *next;
};

/* Configuration file */
struct qd_config {
    char *path;
    qd_config_section_t *sections;
    qd_config_section_t *global;     /* Global section (no header) */
    int modified;
    char *error_msg;
    int error_line;
};

/*
 * Configuration Loading/Saving
 */

/* Create empty configuration */
qd_config_t *qd_config_create(void);

/* Load configuration from file */
qd_config_t *qd_config_load(const char *path);

/* Load configuration from string */
qd_config_t *qd_config_load_string(const char *str);

/* Reload configuration from file */
int qd_config_reload(qd_config_t *config);

/* Save configuration to file */
int qd_config_save(qd_config_t *config, const char *path);

/* Save to original file */
int qd_config_save_default(qd_config_t *config);

/* Destroy configuration */
void qd_config_destroy(qd_config_t *config);

/* Get last error message */
const char *qd_config_error(qd_config_t *config);

/* Get error line number */
int qd_config_error_line(qd_config_t *config);

/*
 * Section Management
 */

/* Get section by name */
qd_config_section_t *qd_config_get_section(qd_config_t *config, const char *name);

/* Create section */
qd_config_section_t *qd_config_create_section(qd_config_t *config, const char *name);

/* Remove section */
int qd_config_remove_section(qd_config_t *config, const char *name);

/* Check if section exists */
int qd_config_has_section(qd_config_t *config, const char *name);

/* Iterate sections */
qd_config_section_t *qd_config_first_section(qd_config_t *config);
qd_config_section_t *qd_config_next_section(qd_config_section_t *section);

/*
 * Value Getters
 */

/* Get string value */
const char *qd_config_get_string(qd_config_t *config, const char *section,
                                  const char *key, const char *default_val);

/* Get integer value */
int64_t qd_config_get_int(qd_config_t *config, const char *section,
                           const char *key, int64_t default_val);

/* Get unsigned integer value */
uint64_t qd_config_get_uint(qd_config_t *config, const char *section,
                             const char *key, uint64_t default_val);

/* Get floating point value */
double qd_config_get_float(qd_config_t *config, const char *section,
                            const char *key, double default_val);

/* Get boolean value */
int qd_config_get_bool(qd_config_t *config, const char *section,
                        const char *key, int default_val);

/* Get size value (supports K, M, G suffixes) */
size_t qd_config_get_size(qd_config_t *config, const char *section,
                           const char *key, size_t default_val);

/* Get duration value (supports ms, s, m, h suffixes) */
int64_t qd_config_get_duration_ms(qd_config_t *config, const char *section,
                                   const char *key, int64_t default_val);

/* Check if key exists */
int qd_config_has_key(qd_config_t *config, const char *section, const char *key);

/*
 * Value Setters
 */

/* Set string value */
int qd_config_set_string(qd_config_t *config, const char *section,
                          const char *key, const char *value);

/* Set integer value */
int qd_config_set_int(qd_config_t *config, const char *section,
                       const char *key, int64_t value);

/* Set floating point value */
int qd_config_set_float(qd_config_t *config, const char *section,
                         const char *key, double value);

/* Set boolean value */
int qd_config_set_bool(qd_config_t *config, const char *section,
                        const char *key, int value);

/* Remove key */
int qd_config_remove_key(qd_config_t *config, const char *section, const char *key);

/*
 * Array Values
 */

/* Get array value (comma-separated) */
int qd_config_get_array(qd_config_t *config, const char *section,
                         const char *key, char ***values, int *count);

/* Free array returned by qd_config_get_array */
void qd_config_free_array(char **values, int count);

/* Set array value */
int qd_config_set_array(qd_config_t *config, const char *section,
                         const char *key, const char **values, int count);

/*
 * Entry Iteration
 */

/* Get first entry in section */
qd_config_entry_t *qd_config_first_entry(qd_config_section_t *section);

/* Get next entry */
qd_config_entry_t *qd_config_next_entry(qd_config_entry_t *entry);

/* Get entry key */
const char *qd_config_entry_key(qd_config_entry_t *entry);

/* Get entry value */
const char *qd_config_entry_value(qd_config_entry_t *entry);

/*
 * Convenience Functions
 */

/* Global section shortcuts (section = NULL) */
#define qd_config_get(cfg, key, def) qd_config_get_string(cfg, NULL, key, def)
#define qd_config_get_i(cfg, key, def) qd_config_get_int(cfg, NULL, key, def)
#define qd_config_get_f(cfg, key, def) qd_config_get_float(cfg, NULL, key, def)
#define qd_config_get_b(cfg, key, def) qd_config_get_bool(cfg, NULL, key, def)

/* Merge configurations (source into dest) */
int qd_config_merge(qd_config_t *dest, qd_config_t *source);

/* Clone configuration */
qd_config_t *qd_config_clone(qd_config_t *config);

/* Print configuration to file */
int qd_config_print(qd_config_t *config, FILE *fp);

/* Dump configuration to string (caller must free) */
char *qd_config_dump(qd_config_t *config);

/*
 * Schema Validation
 */

/* Schema entry definition */
typedef struct qd_config_schema_entry {
    const char *section;
    const char *key;
    qd_config_type_t type;
    int required;
    const char *default_value;
    const char *description;
} qd_config_schema_entry_t;

/* Validate configuration against schema */
int qd_config_validate(qd_config_t *config, const qd_config_schema_entry_t *schema, int count);

/* Apply defaults from schema */
int qd_config_apply_defaults(qd_config_t *config, const qd_config_schema_entry_t *schema, int count);

/*
 * Environment Variable Expansion
 */

/* Expand environment variables in values */
int qd_config_expand_env(qd_config_t *config);

/* Set value with environment variable expansion */
int qd_config_set_string_expand(qd_config_t *config, const char *section,
                                 const char *key, const char *value);

/*
 * Watch for Changes
 */

/* Callback for configuration changes */
typedef void (*qd_config_change_cb_t)(qd_config_t *config, const char *section,
                                       const char *key, const char *old_value,
                                       const char *new_value, void *arg);

/* Set change callback */
void qd_config_set_change_callback(qd_config_t *config, qd_config_change_cb_t cb, void *arg);

/* Watch file for changes (requires event loop) */
struct qd_event_loop;
int qd_config_watch(qd_config_t *config, struct qd_event_loop *loop);

/* Stop watching file */
void qd_config_unwatch(qd_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* QD_CONFIG_H */
