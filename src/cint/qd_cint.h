/*
 * QD-CINT - Clean-room C Interpreter
 * Main Public API
 */

#ifndef QD_CINT_H
#define QD_CINT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * QD-CINT Context (Opaque)
 */
typedef struct qd_cint qd_cint_t;

/*
 * Create CINT interpreter
 */
qd_cint_t *qd_cint_create(void);

/*
 * Destroy CINT interpreter
 */
void qd_cint_destroy(qd_cint_t *cint);

/*
 * Evaluate code string
 * Returns 0 on success, -1 on error
 */
int qd_cint_eval(qd_cint_t *cint, const char *code);

/*
 * Execute file
 */
int qd_cint_exec_file(qd_cint_t *cint, const char *filename);

/*
 * Run interactive REPL
 */
int qd_cint_repl(qd_cint_t *cint, const char *prompt);

/*
 * Register native C function
 * Signature format: "ret_type func_name(arg1_type, arg2_type, ...)"
 * Example: "int my_func(int, int)"
 */
int qd_cint_register_native(qd_cint_t *cint, const char *name, void *func_ptr,
                            int num_args);

/*
 * Get last error message
 */
const char *qd_cint_error(qd_cint_t *cint);

/*
 * Set verbosity
 */
void qd_cint_set_verbose(qd_cint_t *cint, int verbose);

#ifdef __cplusplus
}
#endif

#endif /* QD_CINT_H */
