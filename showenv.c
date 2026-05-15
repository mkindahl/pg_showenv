#include "postgres.h"
#include "funcapi.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/*
 * environment_variables() - return all process environment variables as (name, value) rows.
 *
 * Security demonstration: a superuser-only function that proves PostgreSQL
 * server processes can expose environment variables — including passwords
 * injected via PGPASSWORD, DATABASE_URL, or similar conventions.
 */

/*
 * The environment variables array is defined in POSIX.1 standard, but not
 * available using only C standard functions. Since we are going to iterate over
 * all environment variables, we need to use the platform-specific environ
 * variable.
 */
extern char **environ;

typedef struct EnvEntry
{
    char *name;
    char *value;
} EnvEntry;

PG_FUNCTION_INFO_V1(environment_variables);

Datum environment_variables(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    int call_cntr;
    int max_calls;
    EnvEntry *entries;
    AttInMetadata *attinmeta;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;
        TupleDesc tupdesc;
        int envvar_count = 0;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* count environment entries */
        while (environ[envvar_count] != NULL)
            envvar_count++;
        funcctx->max_calls = envvar_count;

        /*
         * Snapshot the environment into the multi-call memory context so
         * subsequent calls read a stable copy rather than re-dereferencing
         * environ.
         */
        entries = palloc(envvar_count * sizeof(EnvEntry));
        for (int i = 0; i < envvar_count; i++)
        {
            const char *entry = environ[i];
            const char *eq = strchr(entry, '=');

            if (eq != NULL)
            {
                entries[i].name = pnstrdup(entry, eq - entry);
                entries[i].value = pstrdup(eq + 1);
            }
            else
            {
                entries[i].name = pstrdup(entry);
                entries[i].value = pstrdup("");
            }
        }
        funcctx->user_fctx = entries;

        /* build (name text, value text) tuple descriptor */
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context "
                            "that cannot accept type record")));

        funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;
    attinmeta = funcctx->attinmeta;
    entries = funcctx->user_fctx;

    if (call_cntr < max_calls)
    {
        char *values[] = {
            entries[call_cntr].name,
            entries[call_cntr].value,
        };
        HeapTuple tuple = BuildTupleFromCStrings(attinmeta, values);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    SRF_RETURN_DONE(funcctx);
}
