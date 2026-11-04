/* wasm: pthread_exit for wasi-threads (see compat/wasm_plat.h).
 * Compiled without the force-include so it calls the real pthread_create. */
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
struct tramp { void *(*fn)(void *); void *arg; };
static _Thread_local jmp_buf *t_exit_jb;
static _Thread_local void *t_exit_val;
static void *tramp_main(void *p){
    struct tramp t = *(struct tramp *)p; free(p);
    jmp_buf jb;
    if (setjmp(jb)) return t_exit_val;
    t_exit_jb = &jb;
    return t.fn(t.arg);
}
int nfsu2_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg){
    struct tramp *t = malloc(sizeof *t);
    if (!t) return 12; /* ENOMEM */
    t->fn = fn; t->arg = arg;
    int r = pthread_create(th, attr, tramp_main, t);
    if (r) free(t);
    return r;
}
_Noreturn void nfsu2_pthread_exit(void *value){
    if (t_exit_jb){ t_exit_val = value; longjmp(*t_exit_jb, 1); }
    fprintf(stderr, "pthread_exit outside a trampolined thread\n");
    abort();
}
