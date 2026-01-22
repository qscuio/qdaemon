/*
 * QDaemon - Configuration Parser Implementation
 * INI-style configuration file parser
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "qdaemon/qd_config.h"

static char *trim(char *str) {
    if (!str) return NULL;
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    return str;
}

static qd_config_entry_t *entry_create(const char *key, const char *value) {
    qd_config_entry_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->key = key ? strdup(key) : NULL;
    e->value = value ? strdup(value) : NULL;
    return e;
}

static void entry_destroy(qd_config_entry_t *e) {
    if (!e) return;
    free(e->key);
    free(e->value);
    free(e);
}

static qd_config_section_t *section_create(const char *name) {
    qd_config_section_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->name = name ? strdup(name) : NULL;
    return s;
}

static void section_destroy(qd_config_section_t *s) {
    if (!s) return;
    free(s->name);
    for (qd_config_entry_t *e = s->entries; e; ) {
        qd_config_entry_t *next = e->next;
        entry_destroy(e);
        e = next;
    }
    free(s);
}

static qd_config_entry_t *find_entry(qd_config_section_t *s, const char *key) {
    for (qd_config_entry_t *e = s ? s->entries : NULL; e; e = e->next)
        if (e->key && strcmp(e->key, key) == 0) return e;
    return NULL;
}

static int add_entry(qd_config_section_t *s, const char *key, const char *value) {
    qd_config_entry_t *e = find_entry(s, key);
    if (e) { free(e->value); e->value = value ? strdup(value) : NULL; return QD_OK; }
    e = entry_create(key, value);
    if (!e) return QD_ERR_NOMEM;
    if (!s->entries) s->entries = e;
    else { qd_config_entry_t *t = s->entries; while(t->next) t = t->next; t->next = e; }
    return QD_OK;
}

qd_config_t *qd_config_create(void) {
    qd_config_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->global = section_create(NULL);
    if (!c->global) { free(c); return NULL; }
    return c;
}

qd_config_t *qd_config_load(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    qd_config_t *c = qd_config_create();
    if (!c) { fclose(fp); return NULL; }
    c->path = strdup(path);
    char line[1024];
    qd_config_section_t *cur = c->global;
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) { *end = '\0'; cur = qd_config_create_section(c, trim(p+1)); }
            continue;
        }
        char *eq = strchr(p, '=');
        if (eq) { *eq = '\0'; add_entry(cur, trim(p), trim(eq+1)); }
    }
    fclose(fp);
    return c;
}

qd_config_t *qd_config_load_string(const char *str) {
    if (!str) return NULL;
    qd_config_t *c = qd_config_create();
    if (!c) return NULL;
    char *copy = strdup(str), *line = copy, *next;
    qd_config_section_t *cur = c->global;
    while (line && *line) {
        next = strchr(line, '\n'); if (next) *next++ = '\0';
        char *p = trim(line);
        if (*p && *p != '#' && *p != ';') {
            if (*p == '[') {
                char *end = strchr(p, ']');
                if (end) { *end = '\0'; cur = qd_config_create_section(c, trim(p+1)); }
            } else {
                char *eq = strchr(p, '=');
                if (eq) { *eq = '\0'; add_entry(cur, trim(p), trim(eq+1)); }
            }
        }
        line = next;
    }
    free(copy);
    return c;
}

void qd_config_destroy(qd_config_t *c) {
    if (!c) return;
    free(c->path); free(c->error_msg);
    section_destroy(c->global);
    for (qd_config_section_t *s = c->sections; s; ) {
        qd_config_section_t *next = s->next;
        section_destroy(s);
        s = next;
    }
    free(c);
}

int qd_config_reload(qd_config_t *c) {
    if (!c || !c->path) return QD_ERR_INVAL;
    qd_config_t *n = qd_config_load(c->path);
    if (!n) return QD_ERR_IO;
    for (qd_config_section_t *s = c->sections; s; ) {
        qd_config_section_t *next = s->next; section_destroy(s); s = next;
    }
    section_destroy(c->global);
    c->sections = n->sections; c->global = n->global;
    n->sections = NULL; n->global = NULL;
    qd_config_destroy(n);
    return QD_OK;
}

int qd_config_save(qd_config_t *c, const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return QD_ERR_IO;
    for (qd_config_entry_t *e = c->global->entries; e; e = e->next)
        fprintf(fp, "%s = %s\n", e->key, e->value ? e->value : "");
    for (qd_config_section_t *s = c->sections; s; s = s->next) {
        fprintf(fp, "\n[%s]\n", s->name);
        for (qd_config_entry_t *e = s->entries; e; e = e->next)
            fprintf(fp, "%s = %s\n", e->key, e->value ? e->value : "");
    }
    fclose(fp);
    return QD_OK;
}

int qd_config_save_default(qd_config_t *c) { return c && c->path ? qd_config_save(c, c->path) : QD_ERR_INVAL; }
const char *qd_config_error(qd_config_t *c) { return c ? c->error_msg : NULL; }
int qd_config_error_line(qd_config_t *c) { return c ? c->error_line : 0; }

qd_config_section_t *qd_config_get_section(qd_config_t *c, const char *name) {
    if (!c) return NULL;
    if (!name || !*name) return c->global;
    for (qd_config_section_t *s = c->sections; s; s = s->next)
        if (s->name && strcmp(s->name, name) == 0) return s;
    return NULL;
}

qd_config_section_t *qd_config_create_section(qd_config_t *c, const char *name) {
    if (!c || !name) return NULL;
    qd_config_section_t *s = qd_config_get_section(c, name);
    if (s) return s;
    s = section_create(name);
    if (!s) return NULL;
    if (!c->sections) c->sections = s;
    else { qd_config_section_t *t = c->sections; while(t->next) t = t->next; t->next = s; }
    return s;
}

int qd_config_remove_section(qd_config_t *c, const char *name) {
    for (qd_config_section_t **pp = &c->sections; *pp; pp = &(*pp)->next) {
        if ((*pp)->name && strcmp((*pp)->name, name) == 0) {
            qd_config_section_t *rm = *pp; *pp = rm->next; section_destroy(rm); return QD_OK;
        }
    }
    return QD_ERR_NOENT;
}

int qd_config_has_section(qd_config_t *c, const char *n) { return qd_config_get_section(c,n) != NULL; }
qd_config_section_t *qd_config_first_section(qd_config_t *c) { return c ? c->sections : NULL; }
qd_config_section_t *qd_config_next_section(qd_config_section_t *s) { return s ? s->next : NULL; }

const char *qd_config_get_string(qd_config_t *c, const char *sec, const char *key, const char *def) {
    qd_config_entry_t *e = find_entry(qd_config_get_section(c, sec), key);
    return (e && e->value) ? e->value : def;
}

int64_t qd_config_get_int(qd_config_t *c, const char *sec, const char *key, int64_t def) {
    const char *s = qd_config_get_string(c, sec, key, NULL);
    return s ? strtoll(s, NULL, 0) : def;
}

uint64_t qd_config_get_uint(qd_config_t *c, const char *sec, const char *key, uint64_t def) {
    const char *s = qd_config_get_string(c, sec, key, NULL);
    return s ? strtoull(s, NULL, 0) : def;
}

double qd_config_get_float(qd_config_t *c, const char *sec, const char *key, double def) {
    const char *s = qd_config_get_string(c, sec, key, NULL);
    return s ? strtod(s, NULL) : def;
}

int qd_config_get_bool(qd_config_t *c, const char *sec, const char *key, int def) {
    const char *s = qd_config_get_string(c, sec, key, NULL);
    if (!s) return def;
    if (strcasecmp(s,"true")==0 || strcasecmp(s,"yes")==0 || strcmp(s,"1")==0) return 1;
    if (strcasecmp(s,"false")==0 || strcasecmp(s,"no")==0 || strcmp(s,"0")==0) return 0;
    return def;
}

size_t qd_config_get_size(qd_config_t *c, const char *sec, const char *key, size_t def) {
    const char *s = qd_config_get_string(c, sec, key, NULL);
    if (!s) return def;
    char *end; unsigned long long v = strtoull(s, &end, 0);
    switch(toupper((unsigned char)*end)) {
        case 'K': v *= 1024; break; case 'M': v *= 1024*1024; break;
        case 'G': v *= 1024ULL*1024*1024; break;
    }
    return (size_t)v;
}

int64_t qd_config_get_duration_ms(qd_config_t *c, const char *sec, const char *key, int64_t def) {
    const char *s = qd_config_get_string(c, sec, key, NULL);
    if (!s) return def;
    char *end; int64_t v = strtoll(s, &end, 0);
    if (strcasecmp(end,"s")==0) v *= 1000;
    else if (strcasecmp(end,"m")==0) v *= 60000;
    else if (strcasecmp(end,"h")==0) v *= 3600000;
    return v;
}

int qd_config_has_key(qd_config_t *c, const char *sec, const char *key) {
    return find_entry(qd_config_get_section(c, sec), key) != NULL;
}

int qd_config_set_string(qd_config_t *c, const char *sec, const char *key, const char *val) {
    qd_config_section_t *s = qd_config_get_section(c, sec);
    if (!s) s = qd_config_create_section(c, sec);
    return s ? add_entry(s, key, val) : QD_ERR_NOMEM;
}

int qd_config_set_int(qd_config_t *c, const char *sec, const char *key, int64_t v) {
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return qd_config_set_string(c, sec, key, buf);
}

int qd_config_set_float(qd_config_t *c, const char *sec, const char *key, double v) {
    char buf[64]; snprintf(buf, sizeof(buf), "%g", v);
    return qd_config_set_string(c, sec, key, buf);
}

int qd_config_set_bool(qd_config_t *c, const char *sec, const char *key, int v) {
    return qd_config_set_string(c, sec, key, v ? "true" : "false");
}

int qd_config_remove_key(qd_config_t *c, const char *sec, const char *key) {
    qd_config_section_t *s = qd_config_get_section(c, sec);
    if (!s) return QD_ERR_NOENT;
    for (qd_config_entry_t **pp = &s->entries; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->key, key) == 0) {
            qd_config_entry_t *rm = *pp; *pp = rm->next; entry_destroy(rm); return QD_OK;
        }
    }
    return QD_ERR_NOENT;
}

qd_config_entry_t *qd_config_first_entry(qd_config_section_t *s) { return s ? s->entries : NULL; }
qd_config_entry_t *qd_config_next_entry(qd_config_entry_t *e) { return e ? e->next : NULL; }
const char *qd_config_entry_key(qd_config_entry_t *e) { return e ? e->key : NULL; }
const char *qd_config_entry_value(qd_config_entry_t *e) { return e ? e->value : NULL; }

int qd_config_get_array(qd_config_t *c, const char *sec, const char *key, char ***vals, int *cnt) {
    const char *s = qd_config_get_string(c, sec, key, NULL);
    if (!s) { *vals = NULL; *cnt = 0; return QD_OK; }
    int n = 1; for (const char *p = s; *p; p++) if (*p == ',') n++;
    char **arr = calloc(n, sizeof(char*)); if (!arr) return QD_ERR_NOMEM;
    char *copy = strdup(s), *tok, *sv; int i = 0;
    for (tok = strtok_r(copy, ",", &sv); tok && i < n; tok = strtok_r(NULL, ",", &sv))
        arr[i++] = strdup(trim(tok));
    free(copy); *vals = arr; *cnt = i;
    return QD_OK;
}

void qd_config_free_array(char **vals, int cnt) {
    if (!vals) return;
    for (int i = 0; i < cnt; i++) free(vals[i]);
    free(vals);
}

int qd_config_set_array(qd_config_t *c, const char *sec, const char *key, const char **vals, int cnt) {
    if (cnt <= 0) return qd_config_set_string(c, sec, key, "");
    size_t len = 0; for (int i = 0; i < cnt; i++) len += strlen(vals[i]) + 2;
    char *buf = malloc(len+1), *p = buf;
    for (int i = 0; i < cnt; i++) { p += sprintf(p, "%s%s", vals[i], i < cnt-1 ? ", " : ""); }
    int ret = qd_config_set_string(c, sec, key, buf);
    free(buf); return ret;
}

qd_config_t *qd_config_clone(qd_config_t *c) {
    qd_config_t *n = qd_config_create();
    if (!n || !c) return n;
    for (qd_config_entry_t *e = c->global->entries; e; e = e->next)
        add_entry(n->global, e->key, e->value);
    for (qd_config_section_t *s = c->sections; s; s = s->next) {
        qd_config_section_t *ns = qd_config_create_section(n, s->name);
        for (qd_config_entry_t *e = s->entries; e; e = e->next)
            add_entry(ns, e->key, e->value);
    }
    return n;
}

int qd_config_merge(qd_config_t *d, qd_config_t *s) {
    if (!d || !s) return QD_ERR_INVAL;
    for (qd_config_entry_t *e = s->global->entries; e; e = e->next)
        add_entry(d->global, e->key, e->value);
    for (qd_config_section_t *sec = s->sections; sec; sec = sec->next) {
        qd_config_section_t *ds = qd_config_create_section(d, sec->name);
        for (qd_config_entry_t *e = sec->entries; e; e = e->next)
            add_entry(ds, e->key, e->value);
    }
    return QD_OK;
}

int qd_config_print(qd_config_t *c, FILE *fp) {
    for (qd_config_entry_t *e = c->global->entries; e; e = e->next)
        fprintf(fp, "%s = %s\n", e->key, e->value ? e->value : "");
    for (qd_config_section_t *s = c->sections; s; s = s->next) {
        fprintf(fp, "\n[%s]\n", s->name);
        for (qd_config_entry_t *e = s->entries; e; e = e->next)
            fprintf(fp, "%s = %s\n", e->key, e->value ? e->value : "");
    }
    return QD_OK;
}

char *qd_config_dump(qd_config_t *c) {
    size_t sz = 4096; char *buf = malloc(sz);
    FILE *fp = fmemopen(buf, sz, "w");
    if (fp) { qd_config_print(c, fp); fclose(fp); }
    return buf;
}

int qd_config_validate(qd_config_t *c, const qd_config_schema_entry_t *sch, int cnt) {
    for (int i = 0; i < cnt; i++)
        if (sch[i].required && !qd_config_has_key(c, sch[i].section, sch[i].key))
            return QD_ERR_INVAL;
    return QD_OK;
}

int qd_config_apply_defaults(qd_config_t *c, const qd_config_schema_entry_t *sch, int cnt) {
    for (int i = 0; i < cnt; i++)
        if (sch[i].default_value && !qd_config_has_key(c, sch[i].section, sch[i].key))
            qd_config_set_string(c, sch[i].section, sch[i].key, sch[i].default_value);
    return QD_OK;
}

int qd_config_expand_env(qd_config_t *c) { (void)c; return QD_OK; /* TODO */ }
int qd_config_set_string_expand(qd_config_t *c, const char *s, const char *k, const char *v) {
    return qd_config_set_string(c, s, k, v);
}
void qd_config_set_change_callback(qd_config_t *c, qd_config_change_cb_t cb, void *a) { (void)c; (void)cb; (void)a; }
int qd_config_watch(qd_config_t *c, struct qd_event_loop *l) { (void)c; (void)l; return QD_ERR_NOSYS; }
void qd_config_unwatch(qd_config_t *c) { (void)c; }
