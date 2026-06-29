#ifndef TA_TYPECHECK_H
#define TA_TYPECHECK_H
#include "ta.h"

/* ============================================================
 * Hindley-Milner type checker for TinyActor
 *
 * Runs AFTER parsing and BEFORE code generation.
 * Infers types for all expressions, checks annotations, and
 * reports type errors. Designed to be permissive: it should
 * never produce false positives on valid (untyped) code.
 * ============================================================ */

/* Type kinds */
typedef enum {
    TY_VAR,    /* unification variable */
    TY_CON,    /* concrete type: int, bool, string, pid, nil, symbol */
    TY_ARROW,  /* function type: params -> ret */
    TY_ADT,    /* user-defined algebraic data type */
} TypeKind;

typedef struct Type Type;
struct Type {
    TypeKind kind;
    union {
        struct {
            int id;          /* unique id for occurs check */
            Type *instance;  /* resolved type (NULL if unbound) */
            int quantified;  /* 1 if generalized (for instantiate) */
        } var;
        struct {
            const char *name; /* "int", "bool", "string", etc. */
        } con;
        struct {
            Type **params;
            int nparams;
            Type *ret;
        } arrow;
        struct {
            const char *name;  /* ADT type name */
            Type **args;
            int nargs;
        } adt;
    };
};

/* Type checker context */
typedef struct {
    VM *vm;
    int next_var_id;
} TypeCtx;

/* Function annotation info captured from the reader */
typedef struct {
    char name[128];
    char param_types[16][64];  /* type annotation strings per param */
    int  nparams;
    char ret_type[64];          /* return type annotation */
    int  has_ret_annotation;
} FnAnnotation;

/* Main API — returns 0 on success, -1 on type error */
int typecheck_program(VM *vm, Val forms);

#endif /* TA_TYPECHECK_H */