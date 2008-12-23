// Copyright (C) 2007 Codership Oy <info@codership.com>

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

#include <galerautils.h>
#include <gcs.h>
#include <dirent.h>
#include <sys/stat.h>

#define __USE_BSD 1
#include <sys/time.h>

#include <galerautils.h>
#include <gcs.h>
#include <wsdb_api.h>
#include "galera.h"
#include "job_queue.h"

#define GALERA_USE_FLOW_CONTROL 1
#define GALERA_USLEEP 10000 // 10 ms

/* configuration parameter call back in application */
static galera_conf_param_fun app_configurator=NULL;

enum galera_repl_state {
    GALERA_UNINITIALIZED,
    GALERA_INITIALIZED,
    GALERA_ENABLED,
    GALERA_DISABLED,
};

struct galera_info {
    /* state of wsdb library */
    struct wsdb_info wsdb;
    /* state of gcs library */
    //struct gcs_status gcs;

    enum galera_repl_state repl_state;
};

/* application's handlers */
//static galera_context_retain_fun ctx_retain_cb      = NULL;
//static galera_context_store_fun  ctx_store_cb       = NULL;
static galera_bf_execute_fun     bf_execute_cb      = NULL;
static galera_bf_execute_fun     bf_execute_cb_rbr  = NULL;
static galera_bf_apply_row_fun   bf_apply_row_cb    = NULL;
static galera_ws_start_fun       ws_start_cb        = NULL;
static galera_log_cb_t           galera_log_handler = NULL;

/* application context pointer */
//static void *app_ctx = NULL;

/* gcs parameters */
static gcs_to_t           *to_queue     = NULL; // rename to cert_queue?
static gcs_to_t           *commit_queue = NULL;
static gcs_conn_t         *gcs_conn     = NULL;
static char               *gcs_channel  = "dummy_galera";
static char               *gcs_url      = NULL;

/* state trackers */
// Suggestion: galera_init() can take initial seqno and UUID at start-up
static gcs_seqno_t         my_seqno; // global state seqno
static gu_uuid_t           my_uuid;
static long                my_idx;

static struct job_queue   *applier_queue = NULL;

/* global status structure */
struct galera_info Galera;

static gu_mutex_t commit_mtx;

static my_bool mark_commit_early = FALSE;

static FILE *wslog_L;
static FILE *wslog_G;

void galera_log(galera_severity_t code, char *fmt, ...) {
    va_list ap;
    char msg[1024] = {0};
    char FMT[1024] = {0};
    char SYS_ERR[1024] = {0};
    GU_DBUG_ENTER("galera_log");
    if (errno) {
        sprintf(SYS_ERR, "\nSystem error: %d, %s", errno, strerror(errno));
        errno = 0;
    }
    va_start(ap, fmt);
    sprintf(FMT, "GALERA (%d): %s", code, fmt);
    vsprintf(msg, FMT, ap);
    va_end(ap);
    strcat(msg, SYS_ERR);
    if (galera_log_handler) {
    	    galera_log_handler(code, msg);
    } else {
        fprintf(stderr, "%s", msg);
    }
    GU_DBUG_VOID_RETURN;
}

/* @struct contains one write set and its TO sequence number
 */
struct job_context {
    trx_seqno_t seqno;
    struct wsdb_write_set *ws;
};

static int ws_conflict_check(void *ctx1, void *ctx2) {
    struct job_context *job1 = (struct job_context *)ctx1;
    struct job_context *job2 = (struct job_context *)ctx2;

    if (job1->seqno < job2->seqno) return 0;

    /* job1 is sequenced after job2, must check if they conflict */

    {
      /* serious mis-use of certification test
       * we mangle ws seqno's so that certification_test certifies
       * against just only the job2 ws.
       * If somebody cares to modify wsdb_certification_test, it might
       * break this logic => take care
       */
      trx_seqno_t last_seen_saved = job1->ws->last_seen_trx;
      int rcode;

      job1->ws->last_seen_trx = job2->seqno - 1;
      /* @todo: this will conflict with purging, need to use certification_mtx
       */
      rcode = wsdb_certification_test(job1->ws, (job2->seqno + 1)); 

      job1->ws->last_seen_trx = last_seen_saved;
      if (rcode) {
        return 1;
      }
    }
    return 0;
}
static void *galera_configurator (
    enum wsdb_conf_param_id id, enum wsdb_conf_param_type type
) {
    if (!app_configurator) {
        return(NULL);
    } else {
        return(app_configurator(
            (enum galera_conf_param_id)id, (enum galera_conf_param_type)type)
        );
    }
}

static enum galera_status mm_galera_set_conf_param_cb(
    galera_t *gh, galera_conf_param_fun configurator
) {
    GU_DBUG_ENTER("galera_set_conf_param_cb");

    app_configurator = configurator;
    wsdb_set_conf_param_cb(galera_configurator);


    /* consult application for early commit */
    mark_commit_early = wsdb_conf_get_param(
        GALERA_CONF_MARK_COMMIT_EARLY, GALERA_TYPE_INT
    ) ?
      *(my_bool *)wsdb_conf_get_param(GALERA_CONF_MARK_COMMIT_EARLY, GALERA_TYPE_INT) : 0;

    /* set debug logging on, if requested by app */
    gu_info("debug: %d",  *(my_bool *)configurator(GALERA_CONF_DEBUG, GALERA_TYPE_INT));
    if ( *(my_bool *)configurator(GALERA_CONF_DEBUG, GALERA_TYPE_INT)) {
        gu_conf_debug_on();
    }

    GU_DBUG_RETURN(GALERA_OK);
}

enum galera_status mm_galera_set_logger(galera_t *gh, galera_log_cb_t logger)
{
    GU_DBUG_ENTER("galera_set_logger");
    gu_conf_set_log_callback(logger);
    GU_DBUG_RETURN(GALERA_OK);
}

enum galera_status mm_galera_init(galera_t *gh,
				  const char*          group,
				  const char*          address,
				  const char*          data_dir,
				  galera_log_cb_t      logger)
{
    GU_DBUG_ENTER("galera_init");

    /* set up GCS parameters */
    if (address) {
        gcs_url = strdup (address);
    } else {
        gcs_url = "dummy://";
    }
    if (group) {
        gcs_channel = strdup (group);
    }

    /* set up initial state */
    my_seqno = GCS_SEQNO_NIL;
    my_uuid  = GU_UUID_NIL;
    my_idx   = 0;

    /* initialize wsdb */
    wsdb_init(data_dir, logger);

    gu_conf_set_log_callback(logger);

    /* initialize total order queue */
    to_queue = gcs_to_create(16384, GCS_SEQNO_FIRST);

    /* initialize commit queue */
    commit_queue = gcs_to_create(16384, GCS_SEQNO_FIRST);

    Galera.repl_state = GALERA_INITIALIZED;

    gu_mutex_init(&commit_mtx, NULL);

    /* create worker queue */
    applier_queue = job_queue_create(8, ws_conflict_check);

    /* debug level printing to /tmp directory */
    {
      DIR *dir = opendir("/tmp/galera");
      if (!dir) {
        mkdir("/tmp/galera", S_IRWXU | S_IRWXG);
      }
      wslog_L = fopen("/tmp/galera/ws_local.log", "w");
      wslog_G = fopen("/tmp/galera/ws_global.log", "w");
    }
    GU_DBUG_RETURN(GALERA_OK);
}

void mm_galera_dbug_push (galera_t *gh, const char* control)
{
    GU_DBUG_PUSH(control);
}

void mm_galera_dbug_pop (galera_t *gh)
{
    GU_DBUG_POP();
}

void mm_galera_tear_down(galera_t *gh)
{
    if (Galera.repl_state == GALERA_UNINITIALIZED)
	return;

    if (gcs_conn) gcs_destroy (gcs_conn);
    if (to_queue)     gcs_to_destroy(&to_queue);
    if (commit_queue) gcs_to_destroy(&commit_queue);

    wsdb_close();
}

enum galera_status mm_galera_enable(galera_t *gh) {
    int rcode;

    GU_DBUG_ENTER("galera_enable");
    if (gcs_conn) {
        GU_DBUG_RETURN(GALERA_NODE_FAIL);
    }

    gcs_conn = gcs_create(gcs_url);
    if (!gcs_conn) {
        gu_error ("Failed to create GCS conection handle");
        GU_DBUG_RETURN(GALERA_NODE_FAIL);
    }

    rcode = gcs_open(gcs_conn, gcs_channel);
    if (rcode) {
	gu_error("gcs_open(%p, %s, %s) failed: %d (%s)",
                    &gcs_conn, gcs_channel, gcs_url, rcode, strerror(-rcode));
	GU_DBUG_RETURN(GALERA_NODE_FAIL);
    }

    gu_info("Successfully opened GCS connection to %s", gcs_channel);

    Galera.repl_state = GALERA_ENABLED;
    GU_DBUG_RETURN(GALERA_OK);
}

enum galera_status mm_galera_disable(galera_t *gh) {
    int rcode;

    GU_DBUG_ENTER("galera_disable");
    if (!gcs_conn) {
        GU_DBUG_RETURN(GALERA_NODE_FAIL);
    }

    rcode = gcs_close(gcs_conn);
    if (rcode) {
        gu_error ("Failed to close GCS connection handle: %d (%s)",
                  rcode, strerror(-rcode));
        GU_DBUG_RETURN(GALERA_NODE_FAIL);
    }

    gu_info("Closed GCS connection");

    Galera.repl_state = GALERA_DISABLED;
    GU_DBUG_RETURN(GALERA_OK);
}

/*
enum galera_status galera_set_context_retain_handler(
    galera_context_retain_fun handler
) {
    ctx_retain_cb = handler;
    return GALERA_OK;
}

enum galera_status galera_set_context_store_handler(
    galera_context_store_fun handler
) {
       ctx_store_cb = handler;
    return GALERA_OK;
}
*/

enum galera_status mm_galera_set_execute_handler(galera_t *gh, 
						 galera_bf_execute_fun handler) {
    bf_execute_cb = handler;
    return GALERA_OK;
}

enum galera_status mm_galera_set_execute_handler_rbr(galera_t *gh, 
						     galera_bf_execute_fun handler) {
    bf_execute_cb_rbr = handler;
    return GALERA_OK;
}

enum galera_status mm_galera_set_apply_row_handler(
    galera_t *gh,
    galera_bf_apply_row_fun handler
) {
    bf_apply_row_cb = handler;
    return GALERA_OK;
}

enum galera_status mm_galera_set_ws_start_handler(
    galera_t *gh,
    galera_ws_start_fun handler) {
    ws_start_cb = handler;
    return GALERA_OK;
}

static void print_ws(FILE* fid, struct wsdb_write_set *ws, gcs_seqno_t seqno) {
    u_int16_t i;

    if (!fid) return;

    fprintf(fid, "WS: %llu\n", (long long unsigned int)seqno);
    for (i=0; i < ws->query_count; i++) {
      char *query = gu_malloc (ws->queries[i].query_len + 1);
      memset(query, '\0', ws->queries[i].query_len + 1);
      memcpy(query, ws->queries[i].query, ws->queries[i].query_len);
      fprintf(fid, "QUERY (%llu): %s\n", (long long unsigned int)seqno, query);
      gu_free (query);
    }
    fflush(fid);
}
#ifdef REMOVED
static void print_ws(struct job_worker *worker, struct wsdb_write_set *ws) {
    u_int16_t i;

    if (worker) {
      galera_log(GALERA_LOG_INFO,"job: %d",worker->id);
    } else {
      galera_log(GALERA_LOG_INFO,"LOCAL");
    }
    /* applying connection context statements */
    for (i=0; i < ws->query_count; i++) {
      char *query = gu_malloc (ws->queries[i].query_len + 1);
      memset(query, '\0', ws->queries[i].query_len + 1);
      memcpy(query, ws->queries[i].query, ws->queries[i].query_len);
      galera_log(GALERA_LOG_INFO, "QUERY: %s", query );
      gu_free (query);
    }
}
#endif

static int apply_queries(void *app_ctx, struct wsdb_write_set *ws) {
    u_int16_t i;
    GU_DBUG_ENTER(__PRETTY_FUNCTION__);

    /* SQL statement apply method */
    for (i=0; i < ws->query_count; i++) {
        int rcode = bf_execute_cb(
            app_ctx, ws->queries[i].query, ws->queries[i].query_len,
	    ws->queries[i].timeval, ws->queries[i].query_len
        );
        switch (rcode) {
        case 0: break;
        default: {
            char *query = gu_malloc (ws->queries[i].query_len + 1);
            memset(query, '\0',(ws->queries[i].query_len + 1));
            memcpy(query, ws->queries[i].query, ws->queries[i].query_len);
	    gu_error("query apply failed: %s", query);
            gu_free (query);
            GU_DBUG_RETURN(GALERA_TRX_FAIL);
            break;
        }
        }
    }
    GU_DBUG_RETURN(GALERA_OK);
}
static int apply_rows(void *app_ctx, struct wsdb_write_set *ws) {
    u_int16_t i;
    GU_DBUG_ENTER("apply_rows");

    if (bf_apply_row_cb == NULL) {
        gu_error("row data applier has not been defined"); 
        GU_DBUG_RETURN(GALERA_FATAL);
    }

    /* row data apply method */
    for (i=0; i < ws->item_count; i++) {
        int rcode;
        if (ws->items[i].data_mode != ROW) {
            gu_error("bad row mode: %d for item: %d", 
		     ws->items[i].data_mode, i);
            continue;
        }

        rcode = bf_apply_row_cb(
            app_ctx, ws->items[i].u.row.data, ws->items[i].u.row.length
        );
        switch (rcode) {
        case 0: break;
        default: {
            gu_warn("row apply failed: %d", rcode);
            GU_DBUG_RETURN(GALERA_TRX_FAIL);
            break;
        }
        }
    }
    GU_DBUG_RETURN(GALERA_OK);
}

static int apply_write_set(void *app_ctx, struct wsdb_write_set *ws) {
    u_int16_t i;
    int rcode;

    GU_DBUG_ENTER("apply_write_set");
    assert(bf_execute_cb);

    if (ws->level == WSDB_WS_QUERY) {
        /* applying connection context statements */
        for (i=0; i < ws->conn_query_count; i++) {
            int rcode = bf_execute_cb(
                app_ctx, ws->conn_queries[i].query, ws->conn_queries[i].query_len,
                (time_t)0, 0
            );
            switch (rcode) {
            case 0: break;
            default: {
                 char *query = gu_malloc (ws->conn_queries[i].query_len + 1);
                 memset(query, '\0', ws->conn_queries[i].query_len + 1);
                 memcpy(query, ws->conn_queries[i].query, 
                        ws->conn_queries[i].query_len);
                 gu_error("connection query apply failed: %s", query);
                 gu_free (query);
                 GU_DBUG_RETURN(GALERA_TRX_FAIL);
                 break;
            }}
        }
    }
    switch (ws->level) {
    case WSDB_WS_QUERY:     
         rcode = apply_queries(app_ctx, ws);
         if (rcode != GALERA_OK) GU_DBUG_RETURN(rcode);
         break;
    case WSDB_WS_DATA_ROW:
         // TODO: implement
         rcode = apply_rows(app_ctx, ws);
         break;
    case WSDB_WS_DATA_RBR:
         rcode = bf_execute_cb_rbr(app_ctx,
                                   ws->rbr_buf,
                                   ws->rbr_buf_len, 0, 0);
         if (rcode != GALERA_OK) {
             gu_error("RBR apply failed: %d", rcode);
             GU_DBUG_RETURN(rcode);
         }
         break;
    case WSDB_WS_DATA_COLS: 
        gu_error("column data replication is not supported yet");
        GU_DBUG_RETURN(GALERA_TRX_FAIL);
    default:
         assert(0);
         break;
    }
    GU_DBUG_RETURN(GALERA_OK);
}

static int apply_query(void *app_ctx, char *query, int len) {

    int rcode;

    GU_DBUG_ENTER("apply_commit");

    assert(bf_execute_cb);

    rcode = bf_execute_cb(app_ctx, query, len, (time_t)0, 0);
    if (rcode) {
        gu_error("query commit failed: %d query '%s'", rcode, query);
        GU_DBUG_RETURN(GALERA_TRX_FAIL);
    }
    
    GU_DBUG_RETURN(GALERA_OK);
}

static ulong const    report_interval = 200;
static volatile ulong report_counter  = 0;

// fast funciton to be run inside commit_queue critical section
static inline bool report_check_counter ()
{
    return (++report_counter > report_interval && !(report_counter = 0));
}

// this should be run after commit_queue is released
static inline void report_last_committed (
    gcs_conn_t* gcs_conn
) {
    gcs_seqno_t seqno = wsdb_get_safe_to_discard_seqno();
    long ret;

    gu_info ("Reporting last committed: %llu", seqno);
    if ((ret = gcs_set_last_applied(gcs_conn, seqno))) {
        gu_warn ("Failed to report last committed %llu, %d (%s)",
                 seqno, ret, strerror (-ret));
        // failure, set counter to trigger new attempt
        report_counter += report_interval;
    }
}

static inline void truncate_trx_history (gcs_seqno_t seqno)
{
    static ulong const truncate_interval = 100;
    static gcs_seqno_t last_truncated = 0;

    if (last_truncated + truncate_interval < seqno) {
        gu_debug ("Purging history up to %llu", seqno);
        wsdb_purge_trxs_upto(seqno);
        last_truncated = seqno;
        gu_debug ("Purging done to %llu", seqno);
    }
}

// a wrapper for TO funcitons which can return -EAGAIN
static inline long galera_eagain (long (*function) (gcs_to_t*, gcs_seqno_t),
                                  gcs_to_t* to, gcs_seqno_t seqno)
{
    static const struct timespec period = { 0, 10000000 }; // 10 msec
    long rcode;

    while (-EAGAIN == (rcode = function (to, seqno))) {
        nanosleep (&period, NULL);
    }

    return rcode;
}

// the following are made as macros to allow for correct line number reporting
#define GALERA_GRAB_TO_QUEUE(seqno)                                  \
{                                                                    \
    long ret = galera_eagain (gcs_to_grab, to_queue, seqno);         \
    if (gu_unlikely(ret)) {                                          \
        gu_fatal("Failed to grab to_queue at %lld: %ld (%s)",        \
                 seqno, ret, strerror(-ret));                        \
        assert(0);                                                   \
        abort();                                                     \
    }                                                                \
}

#define GALERA_RELEASE_TO_QUEUE(seqno)                               \
{                                                                    \
    long ret = gcs_to_release (to_queue, seqno);                     \
    if (gu_unlikely(ret)) {                                          \
        gu_fatal("Failed to release to_queue at %lld: %ld (%s)",     \
                 seqno, ret, strerror(-ret));                        \
        assert(0);                                                   \
        abort();                                                     \
    }                                                                \
}

#define GALERA_SELF_CANCEL_TO_QUEUE(seqno)                           \
{                                                                    \
    long ret = galera_eagain (gcs_to_self_cancel, to_queue, seqno);  \
    if (gu_unlikely(ret)) {                                          \
        gu_fatal("Failed to self-cancel to_queue at %lld: %ld (%s)", \
                 seqno, ret, strerror(-ret));                        \
        assert(0);                                                   \
        abort();                                                     \
    }                                                                \
}

#define GALERA_GRAB_COMMIT_QUEUE(seqno)                                  \
{                                                                        \
    long ret = galera_eagain (gcs_to_grab, commit_queue, seqno);         \
    if (gu_unlikely(ret)) {                                              \
        gu_fatal("Failed to grab commit_queue at %lld: %ld (%s)",        \
                 seqno, ret, strerror(-ret));                            \
        assert(0);                                                       \
        abort();                                                         \
    }                                                                    \
}

#define GALERA_RELEASE_COMMIT_QUEUE(seqno)                               \
{                                                                        \
    long ret = gcs_to_release (commit_queue, seqno);                     \
    if (gu_unlikely(ret)) {                                              \
        gu_fatal("Failed to release commit_queue at %lld: %ld (%s)",     \
                 seqno, ret, strerror(-ret));                            \
        assert(0);                                                       \
        abort();                                                         \
    }                                                                    \
}

#define GALERA_SELF_CANCEL_COMMIT_QUEUE(seqno)                           \
{                                                                        \
    long ret = galera_eagain (gcs_to_self_cancel, commit_queue, seqno);  \
    if (gu_unlikely(ret)) {                                              \
        gu_fatal("Failed to self-cancel commit_queue at %lld: %ld (%s)", \
                 seqno, ret, strerror(-ret));                            \
        assert(0);                                                       \
        abort();                                                         \
    }                                                                    \
}

// returns true if action to be applied and false if to be skipped
// should always be called while holding to_queue
static inline bool
galera_update_global_seqno (seqno)
{
    // Seems like we cannot enforce sanity check here - some replicated
    // writesets get cancelled and never make it to this point (TO monitor).
    // Hence holes in global seqno are inevitable here.
    if (gu_likely (my_seqno < seqno)) {
        my_seqno = seqno;
        return true;
    }
    else {
        return false;
    }
}

static void process_conn_write_set( 
    struct job_worker *applier, void *app_ctx, struct wsdb_write_set *ws, 
    gcs_seqno_t seqno_g, gcs_seqno_t seqno_l
) {
    bool do_report;
    int rcode;

    /* wait for total order */
    GALERA_GRAB_TO_QUEUE (seqno_l);

    if (gu_likely(galera_update_global_seqno(seqno_g))) {
        /* Global seqno ok, certification ok (not needed?) */
        rcode = apply_write_set(app_ctx, ws);
        if (rcode) {
            gu_error ("unknown galera fail: %d trx: %llu", rcode, seqno_l);
        }
    }
    
    /* release total order */
    GALERA_RELEASE_TO_QUEUE (seqno_l);

    /* Synchronize with commit resource */
    GALERA_GRAB_COMMIT_QUEUE (seqno_l);
    do_report = report_check_counter();
    GALERA_RELEASE_COMMIT_QUEUE (seqno_l);
    wsdb_set_global_trx_committed(seqno_g);
    if (do_report) report_last_committed(gcs_conn);
    
    return;
}

static int process_query_write_set_applying(
    struct job_worker *applier, void *app_ctx, struct wsdb_write_set *ws, 
    gcs_seqno_t seqno_g, gcs_seqno_t seqno_l
) {
    struct job_context ctx;
    int  rcode;
    bool do_report  = false;
    int  is_retry   = 0;
    int  retries    = 0;

#define MAX_RETRIES 0 // loop for ever
 retry:

    /* synchronize with other appliers */
    ctx.seqno = seqno_l;
    ctx.ws    = ws;
    job_queue_start_job(applier_queue, applier, (void *)&ctx);

    while((rcode = apply_write_set(app_ctx, ws))) {
        if (retries == 0) 
          gu_warn("ws apply failed for: %llu, last_seen: %llu", 
                  seqno_g, ws->last_seen_trx
          );
        if (++retries == MAX_RETRIES) break;
    }
    if (retries == MAX_RETRIES) {
        gu_warn("ws applying is not possible");
        return GALERA_TRX_FAIL;
    }

    job_queue_end_job(applier_queue, applier);

    if (is_retry == 0) {
        /* On first try grab commit_queue */
        GALERA_GRAB_COMMIT_QUEUE (seqno_l);
    }

    rcode = apply_query(app_ctx, "commit\0", 7);
    if (rcode) {
        gu_warn("ws apply commit failed for: %llu, last_seen: %llu", 
                seqno_g, ws->last_seen_trx);
        is_retry = 1;
        goto retry;
    }

    do_report = report_check_counter ();
    GALERA_RELEASE_COMMIT_QUEUE (seqno_l);
    wsdb_set_global_trx_committed(seqno_g);
    if (do_report) report_last_committed(gcs_conn);
    return GALERA_OK;
}
/*
  similar to post gcs_repl part of `galera_commit' to apply remote WS
*/
static void process_query_write_set( 
    struct job_worker *applier, void *app_ctx, struct wsdb_write_set *ws, 
    gcs_seqno_t seqno_g, gcs_seqno_t seqno_l
) {
//remove    bool do_report = false;
    int rcode;

    /* wait for total order */
    GALERA_GRAB_TO_QUEUE (seqno_l);

    if (gu_likely(galera_update_global_seqno(seqno_g))) {
        /* Global seqno OK, do certification test */
        rcode = wsdb_append_write_set(seqno_g, ws);
    }
    else {
        /* Outdated writeset, skip */
        rcode = WSDB_CERTIFICATION_SKIP;
    }

    /* release total order */
    GALERA_RELEASE_TO_QUEUE (seqno_l);

    //print_ws(wslog_G, ws, seqno_l);
    gu_debug("remote trx seqno: %llu %llu last_seen_trx: %llu, cert: %d", 
             seqno_l, seqno_g, ws->last_seen_trx, rcode
    );
    switch (rcode) {
    case WSDB_OK:   /* certification ok */
      {
        rcode = process_query_write_set_applying( 
            applier, app_ctx, ws, seqno_g, seqno_l
        );

        /* register committed transaction */
        if (rcode == WSDB_OK) {
//remove            wsdb_set_global_trx_committed(seqno_g);
        } else {
            gu_fatal("could not apply trx: %llu", seqno_g);
            abort();
        }
        break;
      }
    case WSDB_CERTIFICATION_FAIL:
        /* certification failed, release */
        gu_warn("trx certification failed: (%llu %llu) last_seen: %llu",
                seqno_l, seqno_g, ws->last_seen_trx);
        print_ws(wslog_G, ws, seqno_g);
    case WSDB_CERTIFICATION_SKIP:
        /* Cancel commit queue */
        GALERA_SELF_CANCEL_COMMIT_QUEUE (seqno_l);
        break;
    default:
        gu_error(
            "unknown galera fail: %d trdx: %llu",rcode,seqno_l
        );
        abort();
        break;
    }

    /* 
     * NOTE: Is it safe to delete global trx? Should there be consensus of 
     * last applied writesets before deleting anything from certification 
     * data?
     *
     * wsdb_delete_global_trx(seqno_g); 
     */

    return;
}

static void process_write_set( 
    struct job_worker *applier, void *app_ctx, uint8_t *data, size_t data_len, 
    gcs_seqno_t seqno_g, gcs_seqno_t seqno_l
) {
    struct wsdb_write_set ws;
    XDR xdrs;

    xdrmem_create(&xdrs, (char *)data, data_len, XDR_DECODE);
    if (!xdr_wsdb_write_set(&xdrs, &ws)) {
        gu_error("XDR allocation failed");
        return;
    }

    ws_start_cb(app_ctx, seqno_l);

    switch (ws.type) {
    case WSDB_WS_TYPE_TRX:
        process_query_write_set(applier, app_ctx, &ws, seqno_g, seqno_l);
        break;
    case WSDB_WS_TYPE_CONN:
        process_conn_write_set(applier, app_ctx, &ws, seqno_g, seqno_l);
        break;
    }

    ws_start_cb(app_ctx, 0);

    /* free xdr objects */
    xdrs.x_op = XDR_FREE;
    xdr_wsdb_write_set(&xdrs, &ws);

    return;
}

/*!
 * @return
 *        donor index (own index in case when no state transfer needed)
 *        or negative error code (-1 if configuration in non-primary)
 */
static long
galera_handle_configuration (const gcs_act_conf_t* conf, gcs_seqno_t conf_seqno)
{
    gu_uuid_t* group_uuid = (gu_uuid_t*)conf->group_uuid;

    gu_info ("New %s configuration: %lld, "
             "seqno: %lld, group UUID: "GU_UUID_FORMAT
             ", members: %zu, my idx: %zu",
             conf->conf_id > 0 ? "PRIMARY" : "NON-PRIMARY",
             (long long)conf->conf_id, (long long)conf->seqno,
             GU_UUID_ARGS(group_uuid), conf->memb_num, conf->my_idx);

    my_idx = conf->my_idx; // this is always true.

    if (conf->conf_id >= 0) {
        // PRIMARY configuration
        long ret = 0;

        if (conf->st_required) {
            // GCS determined that we need to request state transfer.
            gu_info ("State Transfer required:"
                     // seqno length chosen to fit in 80 columns
                     "\n\tLocal  seqno: %14lld, UUID: "GU_UUID_FORMAT
                     "\n\tGlobal seqno: %14lld, UUID: "GU_UUID_FORMAT,
                     my_seqno, GU_UUID_ARGS(&my_uuid),
                     conf->seqno, GU_UUID_ARGS(group_uuid));

            GALERA_GRAB_COMMIT_QUEUE (conf_seqno);

            /* TODO: Here start waiting for state transfer to begin. */
            
            do {
                // Actually this loop can be done even in this thread:
                // until we succeed in sending state transfer request, there's
                // nothing else for us to do.
                // Note, this request is very simplified.
                gcs_seqno_t seqno_l;
                ret = gcs_request_state_transfer (gcs_conn, &my_seqno, 
                                                  sizeof(my_seqno), &seqno_l);
                if (ret < 0) {
                    gu_error ("Requesting state transfer: ", strerror(-ret));
                }
                if (seqno_l > GCS_SEQNO_NIL) {
                    GALERA_SELF_CANCEL_TO_QUEUE (seqno_l);
                    GALERA_SELF_CANCEL_COMMIT_QUEUE (seqno_l);
                }
            } while ((ret == -EAGAIN) && (usleep(1000000), true));

            if (ret < 0) {
                GALERA_RELEASE_COMMIT_QUEUE (conf_seqno);
                return ret;
            }

            gu_info ("Requesting state transfer: success, donor %ld", ret);
            assert (ret != my_idx);

            /* TODO: Here wait for state transfer to complete, get my_seqno */
                     // for now pretend that state transfer was complete

            GALERA_RELEASE_COMMIT_QUEUE (conf_seqno);

            gu_info ("State transfer complete, sending JOIN: %s",
                     strerror (-gcs_join(gcs_conn, conf->seqno)));

            my_seqno = conf->seqno; // anything below this seqno must be ignored
        }
        else {
            /* no state transfer required */
            assert (my_seqno == conf->seqno); // global seqno
            GALERA_SELF_CANCEL_COMMIT_QUEUE (conf_seqno); // local seqno
            ret = my_idx;
        }

        my_uuid  = *group_uuid;

        return ret;
    }
    else {
        // NON PRIMARY configuraiton
        GALERA_SELF_CANCEL_COMMIT_QUEUE (conf_seqno); // local seqno
        return -1;
    }
}

enum galera_status mm_galera_recv(galera_t *gh, void *app_ctx) {
    int rcode;
    struct job_worker *applier;

    /* we must have gcs connection */
    if (!gcs_conn) {
        return GALERA_NODE_FAIL;
    }

    applier = job_queue_new_worker(applier_queue);
    if (!applier) {
        gu_error("galera, could not create applier");
        gu_info("active_workers: %d, max_workers: %d",
                 applier_queue->active_workers,applier_queue->max_workers
        );
        return GALERA_NODE_FAIL;
    }
    for (;;) {
        gcs_act_type_t  action_type;
        size_t          action_size;
        void*           action;
        gcs_seqno_t     seqno_g, seqno_l;

        errno = 0;
        rcode = gcs_recv(
            gcs_conn, &action, &action_size, &action_type, &seqno_g, &seqno_l
        );
//        gu_info ("gcs_recv(): act_type: %u, act_size: %u, act_id: %lld, "
//                "local: %llu, rcode: %d", // make seqno_g signed to display -1
//                action_type, action_size, (long long)seqno_g, seqno_l, rcode);

	if (rcode < 0) return GALERA_CONN_FAIL;

        assert (GCS_SEQNO_ILL != seqno_l);

        gu_debug("worker: %d recvd", applier->id);

        switch (action_type) {
        case GCS_ACT_DATA:
            assert (GCS_SEQNO_ILL != seqno_g);
            process_write_set(
                applier, app_ctx, action, action_size, seqno_g, seqno_l
                );
            break;
        case GCS_ACT_COMMIT_CUT:
            // synchronize
            GALERA_GRAB_TO_QUEUE (seqno_l);
            truncate_trx_history (*(gcs_seqno_t*)action);
            GALERA_RELEASE_TO_QUEUE (seqno_l);

            // Let other transaction continue to commit
            GALERA_SELF_CANCEL_COMMIT_QUEUE (seqno_l);
            break;
        case GCS_ACT_CONF:
        {
            GALERA_GRAB_TO_QUEUE (seqno_l);
            galera_handle_configuration (action, seqno_l);
            GALERA_RELEASE_TO_QUEUE (seqno_l);
            break;
        }
        case GCS_ACT_STATE_REQ:
            if (0 <= seqno_l) { // should it be an assert?
                gu_info ("Got state transfer request.");

                // synchronize with app.
                GALERA_GRAB_TO_QUEUE (seqno_l);
                GALERA_GRAB_COMMIT_QUEUE (seqno_l);

                /* At this point database is still, do the state transfer */

                GALERA_RELEASE_TO_QUEUE (seqno_l);
                GALERA_RELEASE_COMMIT_QUEUE (seqno_l);

                gu_info ("State transfer complete, sending JOIN: %s",
                         strerror (-gcs_join(gcs_conn, seqno_g)));
            }
            break;
        default:
            return GALERA_FATAL;
        }
        free (action); /* TODO: cache DATA actions at the end of commit queue
                        * processing. Therefore do not free them here. */
    }
    return GALERA_OK;
}

enum galera_status mm_galera_cancel_commit(galera_t *gh, trx_id_t victim_trx) {
    enum galera_status ret_code = GALERA_OK;
    int rcode;
    wsdb_trx_info_t victim;

    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;
    /* take commit mutex to be sure, committing trx does not
     * conflict with us
     */
    
    gu_mutex_lock(&commit_mtx);

    wsdb_get_local_trx_info(victim_trx, &victim);
    
    /* continue to kill the victim */
    switch (victim.state) {
    case WSDB_TRX_ABORTED:
        gu_debug("trx marketed aborting already: %lld", victim.seqno_l);
        break;

    case WSDB_TRX_MISSING:
        gu_debug("trx missing at cancel commit: %lld", victim.seqno_l);
        break;

    case WSDB_TRX_VOID:
        ret_code = GALERA_WARNING;
        rcode = wsdb_assign_trx_state(victim_trx, WSDB_TRX_ABORTED);
        if (rcode) {
            /* this is going to hang */
            gu_error("could not mark trx, aborting: trx %lld seqno: %lld", 
                     victim_trx, victim.seqno_l
            );
            //abort();
        } else {
          gu_warn("no seqno for trx, marked trx aborted: %llu", victim_trx);
        }
        break;

    case WSDB_TRX_REPLICATING:
        gu_debug("victim trx is replicating: %lld", victim.seqno_l);
        while (victim.state == WSDB_TRX_REPLICATING ) {
          gu_mutex_unlock(&commit_mtx);
          usleep (GALERA_USLEEP);
          gu_mutex_lock(&commit_mtx);
          wsdb_get_local_trx_info(victim_trx, &victim);
        }
        gu_debug("victim trx has replicated: %lld", victim.seqno_l);

        //falling through, we have valid seqno now

    default:
        gu_debug("interrupting trx commit: trx_id %lld seqno %lld", 
                victim_trx, victim.seqno_l);
        //rcode = gcs_to_cancel(to_queue, victim_seqno);
        rcode = gcs_to_interrupt(to_queue, victim.seqno_l);
        if (rcode) {
            gu_debug("trx interupt fail in to_queue: %d", rcode);
            ret_code = GALERA_OK;
            rcode = gcs_to_interrupt(commit_queue, victim.seqno_l);
            if (rcode) {
                gu_warn("trx interrupt fail in commit_queue: %d", rcode);
                ret_code = GALERA_WARNING;
            }

        } else {
            ret_code = GALERA_OK;
        }
    }
    gu_mutex_unlock(&commit_mtx);
    
    return ret_code;
}

enum galera_status galera_assign_timestamp(uint32_t timestamp) {
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;
    return 0;
}

uint32_t galera_get_timestamp() {
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;
    return 0;
}

enum galera_status mm_galera_committed(galera_t *gh, trx_id_t trx_id) {

    bool do_report = false;
    wsdb_trx_info_t trx;

    GU_DBUG_ENTER("galera_committed");
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;
    GU_DBUG_PRINT("galera", ("trx: %llu", trx_id));

    gu_mutex_lock(&commit_mtx);
    wsdb_get_local_trx_info(trx_id, &trx);

    if (trx.state == WSDB_TRX_REPLICATED) {

        do_report = report_check_counter ();
/*
	if (gcs_to_release(commit_queue, trx.seqno_l)) {
	    gu_fatal("Could not release commit resource for %lld", trx.seqno_l);
	    abort();
	}
*/
        GALERA_RELEASE_COMMIT_QUEUE(trx.seqno_l);
        if (!mark_commit_early) {
            wsdb_set_local_trx_committed(trx_id);
        }

        wsdb_delete_local_trx_info(trx_id);
    } else if (trx.state != WSDB_TRX_MISSING) {
        gu_debug("trx state: %d at galera_committed for: %lld", 
                 trx.state, trx.seqno_l
        );
    }

    gu_mutex_unlock(&commit_mtx);

    if (do_report) report_last_committed (gcs_conn);

    GU_DBUG_RETURN(GALERA_OK);
}

enum galera_status mm_galera_rolledback(galera_t *gh, trx_id_t trx_id) {

    wsdb_trx_info_t trx;

    GU_DBUG_ENTER("galera_rolledback");
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;
    GU_DBUG_PRINT("galera", ("trx: %llu", trx_id));

    gu_mutex_lock(&commit_mtx);
    wsdb_get_local_trx_info(trx_id, &trx);
    if (trx.state == WSDB_TRX_REPLICATED) {

	if (gcs_to_release(commit_queue, trx.seqno_l)) {
	    gu_fatal("Could not release commit resource for %lld", trx.seqno_l);
	    abort();
	}
        wsdb_delete_local_trx(trx_id);
        wsdb_delete_local_trx_info(trx_id);
    } else if (trx.state != WSDB_TRX_MISSING) {
        gu_debug("trx state: %d at galera_rolledback for: %lld", 
                 trx.state, trx.seqno_l
        );
    }

    gu_mutex_unlock(&commit_mtx);

    //gu_warn("GALERA rolledback, removed trx: %lu %llu", trx_id, seqno_l);
    GU_DBUG_RETURN(GALERA_OK);
}

static int check_certification_status_for_aborted(
    trx_seqno_t seqno_l, trx_seqno_t seqno_g, struct wsdb_write_set *ws
) {
    int rcode;
    /*
     * not sure if certification needs to wait for total order or not.
     * local trx has conflicted with some remote trx and we are interested 
     * to find out if this is a true conflict or dbms lock granularity issue.
     *
     * It would be safe to wait for all preceding trxs to certificate before
     * us. However, this is not simple to guarantee. There is a limited number 
     * of slave threads and each slave will eventually end up waiting for 
     * commit_queue. Therefore, all preceding remote trxs might not have passed
     * to_queue and we would hang here waiting for to_queue => deadlock.
     * 
     * for the time being, I just certificate here with all trxs which were
     * fast enough to certificate. This is wrong, and must be fixed.
     *
     * Maybe 'light weight' to_queue would work here: We would just check that
     * seqno_l - 1 has certified and then do our certification.
     */

    rcode = wsdb_certification_test(ws, seqno_g);
    switch (rcode) {
    case WSDB_OK:
        gu_warn ("BF conflicting local trx has certified, "
                 "seqno: %llu %llu last_seen_trx: %llu", 
                 seqno_l, seqno_g, ws->last_seen_trx);
        /* certification ok */
        return GALERA_OK;

    case WSDB_CERTIFICATION_FAIL:
        /* certification failed, release */
        gu_debug("BF conflicting local trx certification fail: %llu - %llu",
                seqno_l, ws->last_seen_trx);
        print_ws(wslog_L, ws, seqno_l);
        return GALERA_TRX_FAIL;

    default:  
        gu_fatal("wsdb append failed: seqno_g %llu seqno_l %llu",
                 seqno_g, seqno_l);
        abort();
        break;
    }
}


enum galera_status
mm_galera_commit(
    galera_t *gh,
    trx_id_t trx_id, conn_id_t conn_id, const char *rbr_data, size_t rbr_data_len
    )
{

    int                    rcode;
    struct wsdb_write_set *ws;
    XDR                    xdrs;
    int                    data_max = 34000; /* only fixed xdr buf supported */
    //    uint8_t                data[data_max];
    uint8_t                *data;
    int                    len;
    gcs_seqno_t            seqno_g, seqno_l;
    enum galera_status     retcode;
    wsdb_trx_info_t        trx;

    GU_DBUG_ENTER("galera_commit");
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;

    GU_DBUG_PRINT("galera", ("trx: %llu", trx_id));

    errno = 0;

#ifdef GALERA_USE_FLOW_CONTROL
   do {
#endif
    /* hold commit time mutex */
    gu_mutex_lock(&commit_mtx);
    /* check if trx was cancelled before we got here */
    wsdb_get_local_trx_info(trx_id, &trx);
    switch (trx.state) {
    case WSDB_TRX_ABORTED:
	gu_debug("trx has been cancelled already: %llu", trx_id);
	if ((rcode = wsdb_delete_local_trx(trx_id))) {
	    gu_debug("could not delete trx: %llu", trx_id);
	}
	gu_mutex_unlock(&commit_mtx);
	GU_DBUG_RETURN(GALERA_TRX_FAIL);
        break;
    case WSDB_TRX_MISSING:
	gu_debug("trx is missing from galera: %llu", trx_id);
	gu_mutex_unlock(&commit_mtx);
	GU_DBUG_RETURN(GALERA_TRX_MISSING);
        break;
    default:
      break;
    }
#ifdef GALERA_USE_FLOW_CONTROL
    /* what is happening here:
     * - first,  (gcs_wait() > 0) is evaluated, if not true, loop exits
     * - second, (unlock(), usleep(), true) is evaluated always to true,
     *   so we always keep on looping.
     *   AFAIK usleep() is evaluated after unlock()
     */
     } while ((gcs_wait (gcs_conn) > 0) && 
              (gu_mutex_unlock(&commit_mtx), usleep (GALERA_USLEEP), true)
     );
#endif

    /* retrieve write set */
    ws = wsdb_get_write_set(trx_id, conn_id, rbr_data, rbr_data_len);
    if (!ws) {
        /* this is possibly autocommit query, need to let it continue */
        gu_mutex_unlock(&commit_mtx);
        gu_debug("empty local trx ws %llu", trx_id);
        GU_DBUG_RETURN(GALERA_OK);
    }

    /* ws can be removed from local cache already now */
    if ((rcode = wsdb_delete_local_trx(trx_id))) {
      gu_warn("could not delete trx: %llu", trx_id);
    }

    /* avoid sending empty write sets */
    if (ws->query_count == 0) {
        gu_warn("empty write set for: %llu", trx_id);
        GU_DBUG_RETURN(GALERA_OK);
    }

    /* encode with xdr */
    /* TODO: is not optimal to allocate data buffer for xdr encoding
     *       intermediate result.
     *       Should use xdrrec stream instead and encode directly on
     *       gcs channel as we go.
     */
    data_max = xdr_estimate_wsdb_size(ws) * 2 + rbr_data_len;
    data = (uint8_t *)gu_malloc(data_max);
    memset(data, 0, data_max);
    xdrmem_create(&xdrs, (char *)data, data_max, XDR_ENCODE);
    if (!xdr_wsdb_write_set(&xdrs, ws)) {
        gu_error("xdr failed for: %llu", trx_id);
        gu_mutex_unlock(&commit_mtx);
        GU_DBUG_RETURN(GALERA_TRX_FAIL);
    }
    len = xdr_getpos(&xdrs);

    wsdb_assign_trx_state(trx_id, WSDB_TRX_REPLICATING);
    /* */
    gu_mutex_unlock(&commit_mtx);

    /* replicate through gcs */
    rcode = gcs_repl(gcs_conn, data, len, GCS_ACT_DATA, &seqno_g, &seqno_l);
//    gu_info ("gcs_repl(): act_type: %u, act_size: %u, act_id: %llu, "
//             "local: %llu, ret: %d",
//             GCS_ACT_DATA, len, seqno_g, seqno_l, rcode);
    if (rcode != len) {
        gu_error("gcs failed for: %llu, len: %d, rcode: %d", trx_id,len,rcode);
        assert (GCS_SEQNO_ILL == seqno_l);
        retcode = GALERA_CONN_FAIL;
        goto cleanup;
    }

    assert (GCS_SEQNO_ILL != seqno_g);
    assert (GCS_SEQNO_ILL != seqno_l);

    gu_mutex_lock(&commit_mtx);

    /* record seqnos for local transaction */
    wsdb_assign_trx_seqno(trx_id, seqno_l, seqno_g, WSDB_TRX_REPLICATED);
    gu_mutex_unlock(&commit_mtx);

    // cant use it here - GALERA_GRAB_TO_QUEUE (seqno_l);
    if ((rcode = galera_eagain (gcs_to_grab, to_queue, seqno_l))) {
        gu_warn("gcs_to_grab aborted: %d seqno %llu", rcode, seqno_l);
        retcode = GALERA_TRX_FAIL;

        if (check_certification_status_for_aborted(
              seqno_l, seqno_g, ws) == GALERA_OK
        ) {
            retcode = GALERA_BF_ABORT;
            wsdb_assign_trx_ws(trx_id, ws);
            wsdb_assign_trx_pos(trx_id, WSDB_TRX_POS_TO_QUEUE);
            wsdb_assign_trx_state(trx_id, WSDB_TRX_ABORTED);
            GU_DBUG_RETURN(retcode);
        } else {
            GALERA_SELF_CANCEL_TO_QUEUE (seqno_l);
            GALERA_SELF_CANCEL_COMMIT_QUEUE (seqno_l);
            retcode = GALERA_TRX_FAIL;
        }
        goto cleanup;
    }

    if (gu_likely(galera_update_global_seqno (seqno_g))) {
        /* Gloabl seqno OK, do certification test */
        //print_ws(wslog_L, ws, seqno_l);
        rcode = wsdb_append_write_set(seqno_g, ws);
        switch (rcode) {
        case WSDB_OK:
            gu_debug ("local trx certified, "
                      "seqno: %llu %llu last_seen_trx: %llu", 
                      seqno_l, seqno_g, ws->last_seen_trx);
            /* certification ok */
            retcode = GALERA_OK;
            break;
        case WSDB_CERTIFICATION_FAIL:
            /* certification failed, release */
            retcode = GALERA_TRX_FAIL;
            gu_debug("local trx commit certification failed: %llu - %llu",
                    seqno_l, ws->last_seen_trx);
            print_ws(wslog_L, ws, seqno_l);
            break;
        default:  
            retcode = GALERA_CONN_FAIL;
            gu_fatal("wsdb append failed: seqno_g %llu seqno_l %llu",
                     seqno_g, seqno_l);
            abort();
            break;
        }
    }
    else {
        // theoretically it is possible with poorly written application
        // (trying to replicate something before completing state transfer)
        gu_warn ("Local action replicated with outdated seqno: "
                 "current seqno %lld, action seqno %lld", my_seqno, seqno_g);
        // this situation is as good as cancelled transaction. See above.
        retcode = GALERA_TRX_FAIL;
        // commit queue will be cancelled below.
    }

    // call release only if grab was successfull
    GALERA_RELEASE_TO_QUEUE (seqno_l);

    if (retcode == GALERA_OK) {
        assert (seqno_l >= 0);
	/* Grab commit queue for commit time */
        // can't use it here GALERA_GRAB_COMMIT_QUEUE (seqno_l);
        rcode = galera_eagain (gcs_to_grab, commit_queue, seqno_l);

        switch (rcode) {
        case 0: break;
        case -ECANCELED:
	    gu_warn("canceled in commit queue for %llu", seqno_l);
            wsdb_assign_trx_state(trx_id, WSDB_TRX_ABORTED);
            GU_DBUG_RETURN(GALERA_TRX_FAIL);
            break;
        case -EINTR:
	    gu_warn("interrupted in commit queue for %llu", seqno_l);
            retcode = GALERA_BF_ABORT;
            wsdb_assign_trx_ws(trx_id, ws);
            wsdb_assign_trx_pos(trx_id, WSDB_TRX_POS_COMMIT_QUEUE);
            wsdb_assign_trx_state(trx_id, WSDB_TRX_ABORTED);
            GU_DBUG_RETURN(GALERA_BF_ABORT);
            break;
        default:
	    gu_fatal("Failed to grab commit queue for %llu", seqno_l);
	    abort();
        }

        // we can update last seen trx counter already here
        if (mark_commit_early) {
            wsdb_set_local_trx_committed(trx_id);
        }
    } else {
	/* Cancel commit queue since we are going to rollback */
        GALERA_SELF_CANCEL_COMMIT_QUEUE (seqno_l);
    }

cleanup:

    gu_free(data); // TODO: cache writeset for 
    // was referenced by wsdb_get_write_set() above
    wsdb_deref_seqno (ws->last_seen_trx);
    wsdb_write_set_free(ws);
    GU_DBUG_RETURN(retcode);
}

enum galera_status mm_galera_append_query(
    galera_t *gh,
    const trx_id_t trx_id, const char *query, const time_t timeval, const uint32_t randseed) {

    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;

    errno = 0;
    switch (wsdb_append_query(trx_id, (char*)query, timeval, randseed)) {
    case WSDB_OK:              return GALERA_OK;
    case WSDB_ERR_TRX_UNKNOWN: return GALERA_TRX_FAIL;
    default:                   return GALERA_CONN_FAIL;
    }
    return GALERA_OK;
}

enum galera_status galera_append_row(
    trx_id_t trx_id,
    uint16_t len,
    uint8_t *data
) {
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;
    errno = 0;

    switch(wsdb_append_row(trx_id, len, data)) {
    case WSDB_OK:               return GALERA_OK;
    case WSDB_ERR_TRX_UNKNOWN:  return GALERA_TRX_FAIL;
    default:                    return GALERA_CONN_FAIL;
    }
}

enum galera_status mm_galera_append_row_key(
    galera_t *gh,
    const trx_id_t trx_id,
    const char    *dbtable,
    const size_t dbtable_len,
    const char *key,
    const size_t key_len,
    const enum galera_action action
) {
    struct wsdb_key_rec   wsdb_key;
    struct wsdb_table_key table_key;
    struct wsdb_key_part  key_part;
    char wsdb_action  = WSDB_ACTION_UPDATE;

    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;
    errno = 0;

    /* TODO: make this setupping static, needs mutex protection */
    wsdb_key.key             = &table_key;
    table_key.key_part_count = 1;
    table_key.key_parts      = &key_part;
    key_part.type            = WSDB_TYPE_VOID;

    /* assign key info */
    wsdb_key.dbtable     = (char*)dbtable;
    wsdb_key.dbtable_len = dbtable_len;
    key_part.length      = key_len;
    key_part.data        = (uint8_t*)key;

    switch (action) {
    case GALERA_UPDATE: wsdb_action=WSDB_ACTION_UPDATE; break;
    case GALERA_DELETE: wsdb_action=WSDB_ACTION_DELETE; break;
    case GALERA_INSERT: wsdb_action=WSDB_ACTION_INSERT; break;
    }

    switch(wsdb_append_row_key(trx_id, &wsdb_key, wsdb_action)) {
    case WSDB_OK:               return GALERA_OK;
    case WSDB_ERR_TRX_UNKNOWN:  return GALERA_TRX_FAIL;
    default:                    return GALERA_CONN_FAIL;
    }
}

enum galera_status mm_galera_set_variable(
    galera_t *gh,
    const conn_id_t  conn_id,
    const char *key,  const  size_t key_len, 
    const char *query, const size_t query_len
) {
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;

    errno = 0;
    switch(wsdb_store_set_variable(conn_id, (char*)key, key_len, 
				   (char*)query, query_len)) {
    case WSDB_OK:              return GALERA_OK;
    case WSDB_ERR_TRX_UNKNOWN: return GALERA_TRX_FAIL;
    default:                   return GALERA_CONN_FAIL;
    }
    return GALERA_OK;
}

enum galera_status mm_galera_set_database(
    galera_t *gh,
    const conn_id_t conn_id, const char *query, const size_t query_len
) {
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;

    errno = 0;
    switch(wsdb_store_set_database(conn_id, (char*)query, query_len)) {
    case WSDB_OK:              return GALERA_OK;
    case WSDB_ERR_TRX_UNKNOWN: return GALERA_TRX_FAIL;
    default:                   return GALERA_CONN_FAIL;
    }
    return GALERA_OK;
}

enum galera_status mm_galera_to_execute_start(
    galera_t *gh,
    const conn_id_t conn_id, const char *query, const size_t query_len
) {

    int                    rcode;
    struct wsdb_write_set *ws;
    XDR                    xdrs;
    int                    data_max = 34000; /* only fixed xdr buf supported */
    uint8_t                data[data_max];
    int                    len;
    gcs_seqno_t            seqno_g, seqno_l;
    bool                   do_apply;

    GU_DBUG_ENTER("galera_to_execute_start");
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;
    GU_DBUG_PRINT("galera", ("conn: %llu", conn_id));

    /* retrieve write set */
    ws = wsdb_get_conn_write_set(conn_id);
    if (!ws) {
        GU_DBUG_RETURN(GALERA_CONN_FAIL);
    }

    /* append the query to be executed */
    wsdb_set_exec_query(ws, (char*)query, query_len);

    /* encode with xdr */
    /* TODO: is not optimal to allocate data buffer for xdr encoding
     *       intermediate result.
     *       Should use xdrrec stream instead and encode directly on
     *       gcs channel as we go.
     */
    memset(data, 0, data_max);
    xdrmem_create(&xdrs, (char *)data, data_max, XDR_ENCODE);
    if (!xdr_wsdb_write_set(&xdrs, ws)) {
        gu_error("xdr failed for: %llu", conn_id);
        GU_DBUG_RETURN(GALERA_CONN_FAIL);
    }
    len = xdr_getpos(&xdrs);

#ifdef GALERA_USE_FLOW_CONTROL
    while ((rcode = gcs_wait(gcs_conn)) && rcode > 0) usleep (GALERA_USLEEP);
    if (rcode >= 0) // execute the following operation conditionally
#endif

    /* replicate through gcs */
    rcode = gcs_repl(gcs_conn, data, len, GCS_ACT_DATA, &seqno_g, &seqno_l);
//    gu_info ("gcs_repl(): act_type: %u, act_size: %u, act_id: %llu, "
//             "local: %llu, ret: %d",
//             GCS_ACT_DATA, len, seqno_g, seqno_l, rcode);
    if (rcode < 0) {
        gu_error("gcs failed for: %llu, %d", conn_id, rcode);
        assert (GCS_SEQNO_ILL == seqno_l);
        rcode = GALERA_CONN_FAIL;
        goto cleanup;
    }

    assert (GCS_SEQNO_ILL != seqno_g);
    assert (GCS_SEQNO_ILL != seqno_l);

    /* wait for total order */
    GALERA_GRAB_TO_QUEUE (seqno_l);
    
    /* update global seqno */
    if ((do_apply = galera_update_global_seqno (seqno_g))) {
        /* record local sequence number in connection info */
        wsdb_conn_set_seqno(conn_id, seqno_l);
    }

    GALERA_RELEASE_TO_QUEUE (seqno_l);

    if (do_apply) {
        /* Grab commit queue */
        GALERA_GRAB_COMMIT_QUEUE (seqno_l);
        rcode = GALERA_OK;
    }
    else {
        // theoretically it is possible with poorly written application
        // (trying to replicate something before completing state transfer)
        gu_warn ("Local action replicated with outdated seqno: "
                 "current seqno %lld, action seqno %lld", my_seqno, seqno_g);
        GALERA_SELF_CANCEL_COMMIT_QUEUE (seqno_l);
        // this situation is as good as failed gcs_repl() call.
        rcode = GALERA_CONN_FAIL;
    }

cleanup:

    wsdb_write_set_free(ws); // cache for incremental state transfer if applied
    GU_DBUG_RETURN(rcode);
}

enum galera_status mm_galera_to_execute_end(galera_t *gh, const conn_id_t conn_id) {
    bool do_report;
    struct wsdb_conn_info conn_info;
    int rcode;

    GU_DBUG_ENTER("galera_to_execute_end");
    if (Galera.repl_state != GALERA_ENABLED) return GALERA_OK;

    if ((rcode = wsdb_conn_get_info(conn_id, &conn_info) != WSDB_OK)) {
        gu_warn("missing connection for: %lld, rcode: %d", conn_id, rcode);
        GU_DBUG_RETURN(GALERA_CONN_FAIL);
    }

    do_report = report_check_counter ();

    /* release commit queue */
    GALERA_RELEASE_COMMIT_QUEUE (conn_info.seqno);
    
    /* cleanup seqno reference */
    wsdb_conn_reset_seqno(conn_id);
    
    if (do_report) report_last_committed (gcs_conn);

    GU_DBUG_RETURN(WSDB_OK);
}

enum galera_status mm_galera_replay_trx(galera_t *gh, const trx_id_t trx_id, void *app_ctx) {
    struct job_worker *applier;
    int                rcode;
    wsdb_trx_info_t    trx;

    wsdb_get_local_trx_info(trx_id, &trx);

    gu_debug("trx_replay for: %lld %lld state: %d, rbr len: %d", 
            trx.seqno_l, trx.seqno_g, trx.state, trx.ws->rbr_buf_len);

    switch (trx.state) {
    case WSDB_TRX_ABORTED:
        break;
    default:
        gu_error("replayed trx in bad state: %d", trx.state);
        return GALERA_NODE_FAIL;
    }

    applier = job_queue_new_worker(applier_queue);
    if (!applier) {
        gu_error("galera, could not create applier");
        gu_info("active_workers: %d, max_workers: %d",
                 applier_queue->active_workers, applier_queue->max_workers
        );
        return GALERA_NODE_FAIL;
    }
    gu_debug("applier %d", applier->id );

    //ws_start_cb(app_ctx, trx.seqno_l);

    if (trx.ws->type == WSDB_WS_TYPE_TRX) {

        switch (trx.position) {
        case WSDB_TRX_POS_TO_QUEUE:
            process_query_write_set(
                applier, app_ctx, trx.ws, trx.seqno_g, trx.seqno_l
            );
            break;

        case WSDB_TRX_POS_COMMIT_QUEUE:
            rcode = process_query_write_set_applying( 
              applier, app_ctx, trx.ws, trx.seqno_g, trx.seqno_l
            );

            /* register committed transaction */
            if (!rcode) {
                wsdb_set_global_trx_committed(trx.seqno_g);
            } else {
                gu_fatal("could not re-apply trx: %llu", trx.seqno_g);
                abort();
            }
            break;
        default:
            gu_fatal("bad trx pos in reapplying: %d %llu", 
                     trx.position, trx.seqno_g
            );
            abort();

        }
        job_queue_remove_worker(applier_queue, applier);
    } else {
        gu_error("replayed trx ws has bad type: %d", trx.ws->type);
        return GALERA_NODE_FAIL;
        job_queue_remove_worker(applier_queue, applier);
    }
    wsdb_assign_trx_state(trx_id, WSDB_TRX_REPLICATED);
    //wsdb_delete_local_trx_info(trx_id);

    //ws_start_cb(app_ctx, 0);
    wsdb_deref_seqno (trx.ws->last_seen_trx);
    wsdb_write_set_free(trx.ws);

    return GALERA_OK;
}



static galera_t mm_galera_str = {
    GALERA_INTERFACE_VERSION,
    &mm_galera_init,
    &mm_galera_enable,
    &mm_galera_disable,
    &mm_galera_recv,
    &mm_galera_dbug_push,
    &mm_galera_dbug_pop,
    &mm_galera_set_logger,
    &mm_galera_set_conf_param_cb,
    &mm_galera_set_execute_handler,
    &mm_galera_set_execute_handler_rbr,
    &mm_galera_set_ws_start_handler,
    &mm_galera_commit,
    &mm_galera_replay_trx,
    &mm_galera_cancel_commit,
    &mm_galera_committed,
    &mm_galera_rolledback,
    &mm_galera_append_query,
    &mm_galera_append_row_key,
    &mm_galera_set_variable,
    &mm_galera_set_database,
    &mm_galera_to_execute_start,
    &mm_galera_to_execute_end,
    &mm_galera_tear_down,
    NULL,
    NULL
};




int galera_loader(galera_t **hptr)
{

    if (!hptr)
	return EINVAL;
    *hptr = &mm_galera_str;
    return GALERA_OK;
}
