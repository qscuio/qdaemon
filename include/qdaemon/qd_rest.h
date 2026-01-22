/*
 * QDaemon - Simple REST API Framework
 * For daemon-side service endpoints
 * 
 * Header-only - simple JSON request/response over IPC
 */

#ifndef QD_REST_H
#define QD_REST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP-like methods */
#define QD_REST_GET     0
#define QD_REST_POST    1
#define QD_REST_PUT     2
#define QD_REST_DELETE  3

/* Status codes */
#define QD_REST_OK              200
#define QD_REST_CREATED         201
#define QD_REST_BAD_REQUEST     400
#define QD_REST_NOT_FOUND       404
#define QD_REST_ERROR           500

/* Simple JSON builder */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} qd_json_buf_t;

static inline void qd_json_init(qd_json_buf_t *j)
{
    j->buf = malloc(256);
    j->len = 0;
    j->cap = 256;
    j->buf[0] = '\0';
}

static inline void qd_json_free(qd_json_buf_t *j)
{
    free(j->buf);
    j->buf = NULL;
}

static inline void qd_json_grow(qd_json_buf_t *j, size_t need)
{
    if (j->len + need >= j->cap) {
        j->cap = (j->cap + need) * 2;
        j->buf = realloc(j->buf, j->cap);
    }
}

static inline void qd_json_append(qd_json_buf_t *j, const char *s)
{
    size_t n = strlen(s);
    qd_json_grow(j, n);
    memcpy(j->buf + j->len, s, n + 1);
    j->len += n;
}

static inline void qd_json_start_obj(qd_json_buf_t *j)
{
    qd_json_append(j, "{");
}

static inline void qd_json_end_obj(qd_json_buf_t *j)
{
    /* Remove trailing comma */
    if (j->len > 0 && j->buf[j->len - 1] == ',')
        j->len--;
    qd_json_append(j, "}");
}

static inline void qd_json_add_str(qd_json_buf_t *j, const char *key, const char *val)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "\"%s\":\"%s\",", key, val ? val : "");
    qd_json_append(j, tmp);
}

static inline void qd_json_add_int(qd_json_buf_t *j, const char *key, int64_t val)
{
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "\"%s\":%ld,", key, (long)val);
    qd_json_append(j, tmp);
}

static inline void qd_json_add_bool(qd_json_buf_t *j, const char *key, int val)
{
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "\"%s\":%s,", key, val ? "true" : "false");
    qd_json_append(j, tmp);
}

/* REST Request */
typedef struct {
    int method;
    char path[128];
    char *body;
    size_t body_len;
} qd_rest_req_t;

/* REST Response */
typedef struct {
    int status;
    qd_json_buf_t json;
} qd_rest_resp_t;

static inline void qd_rest_resp_init(qd_rest_resp_t *r)
{
    r->status = QD_REST_OK;
    qd_json_init(&r->json);
}

static inline void qd_rest_resp_free(qd_rest_resp_t *r)
{
    qd_json_free(&r->json);
}

static inline void qd_rest_ok(qd_rest_resp_t *r, const char *msg)
{
    r->status = QD_REST_OK;
    qd_json_start_obj(&r->json);
    qd_json_add_bool(&r->json, "success", 1);
    if (msg)
        qd_json_add_str(&r->json, "message", msg);
    qd_json_end_obj(&r->json);
}

static inline void qd_rest_error(qd_rest_resp_t *r, int code, const char *msg)
{
    r->status = code;
    qd_json_start_obj(&r->json);
    qd_json_add_bool(&r->json, "success", 0);
    qd_json_add_int(&r->json, "code", code);
    qd_json_add_str(&r->json, "error", msg);
    qd_json_end_obj(&r->json);
}

/* REST handler type */
typedef int (*qd_rest_handler_t)(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg);

/* Route definition */
typedef struct {
    int method;
    const char *path;
    qd_rest_handler_t handler;
    void *arg;
} qd_rest_route_t;

/* Simple router */
#define QD_REST_MAX_ROUTES 64

typedef struct {
    qd_rest_route_t routes[QD_REST_MAX_ROUTES];
    int num_routes;
} qd_rest_router_t;

static inline void qd_rest_router_init(qd_rest_router_t *r)
{
    r->num_routes = 0;
}

static inline void qd_rest_add_route(qd_rest_router_t *r, int method, 
                                      const char *path, qd_rest_handler_t h, void *arg)
{
    if (r->num_routes < QD_REST_MAX_ROUTES) {
        r->routes[r->num_routes].method = method;
        r->routes[r->num_routes].path = path;
        r->routes[r->num_routes].handler = h;
        r->routes[r->num_routes].arg = arg;
        r->num_routes++;
    }
}

static inline int qd_rest_handle(qd_rest_router_t *r, qd_rest_req_t *req, qd_rest_resp_t *resp)
{
    for (int i = 0; i < r->num_routes; i++) {
        if (r->routes[i].method == req->method &&
            strcmp(r->routes[i].path, req->path) == 0) {
            return r->routes[i].handler(req, resp, r->routes[i].arg);
        }
    }
    qd_rest_error(resp, QD_REST_NOT_FOUND, "Not found");
    return -1;
}

/* Convenience macros */
#define QD_REST_GET_ROUTE(path, handler) \
    qd_rest_add_route(&router, QD_REST_GET, path, handler, NULL)

#define QD_REST_POST_ROUTE(path, handler) \
    qd_rest_add_route(&router, QD_REST_POST, path, handler, NULL)

#define QD_REST_PUT_ROUTE(path, handler) \
    qd_rest_add_route(&router, QD_REST_PUT, path, handler, NULL)

#define QD_REST_DELETE_ROUTE(path, handler) \
    qd_rest_add_route(&router, QD_REST_DELETE, path, handler, NULL)

#ifdef __cplusplus
}
#endif

#endif /* QD_REST_H */
