// vim:ts=4:shiftwidth=4:et
/*
   mod_evasive for Apache 2
   Copyright (c) by Jonathan A. Zdziarski

   LICENSE

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <syslog.h>
#include <errno.h>
#include <unistd.h>  // getpid(2)

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "httpd.h"
#include "http_core.h"
#include "http_config.h"
#include "http_log.h"
#include "http_main.h"
#include "http_request.h"

/* BEGIN DoS Evasive Maneuvers Definitions */

AP_DECLARE_MODULE(evasive);

#define MAILER  "/bin/mail %s"
#define LOG( A, ... ) { openlog("mod_evasive", LOG_PID, LOG_DAEMON); syslog( A, __VA_ARGS__ ); closelog(); }

#define DEFAULT_HASH_TBL_SIZE   3097UL  // Default hash table size
#define DEFAULT_PAGE_COUNT      2       // Default maximum page hit count per interval
#define DEFAULT_SITE_COUNT      50      // Default maximum site hit count per interval
#define DEFAULT_PAGE_INTERVAL   1       // Default 1 Second page interval
#define DEFAULT_SITE_INTERVAL   1       // Default 1 Second site interval
#define DEFAULT_BLOCKING_PERIOD 10      // Default for Detected IPs; blocked for 10 seconds
#define DEFAULT_LOG_DIR         "/tmp"  // Default temp directory
#define DEFAULT_HTTP_REPLY      HTTP_FORBIDDEN // Default HTTP Reply code (403)

/* END DoS Evasive Maneuvers Definitions */

/* BEGIN NTT (Named Timestamp Tree) Headers */

enum { ntt_num_primes = 28 };

/* ntt root tree */
struct ntt {
    size_t size;
    size_t items;
    struct ntt_node **tbl;
};

/* ntt node (entry in the ntt root tree) */
struct ntt_node {
    char *key;
    time_t timestamp;
    size_t count;
    struct ntt_node *next;
};

/* ntt cursor */
struct ntt_c {
    size_t iter_index;
    struct ntt_node *iter_next;
};

static struct ntt *ntt_create(size_t size);
static int ntt_destroy(struct ntt *ntt);
static struct ntt_node *ntt_find(struct ntt *ntt, const char *key);
static struct ntt_node *ntt_insert(struct ntt *ntt, const char *key, time_t timestamp);
static int ntt_delete(struct ntt *ntt, const char *key);
static size_t ntt_hashcode(const struct ntt *ntt, const char *key);
static struct ntt_node *c_ntt_first(struct ntt *ntt, struct ntt_c *c);
static struct ntt_node *c_ntt_next(struct ntt *ntt, struct ntt_c *c);

/* END NTT (Named Timestamp Tree) Headers */


/* BEGIN DoS Evasive Maneuvers Globals */

struct pcre_node {
    pcre2_code *re;
    struct pcre_node *next;
};

typedef struct {
    int enabled;
    struct ntt *hit_list;   // Our dynamic hash table
    size_t hash_table_size;
    struct pcre_node *uri_whitelist;
    unsigned int page_count;
    int page_interval;
    unsigned int site_count;
    int site_interval;
    int blocking_period;
    char *email_notify;
    char *log_dir;
    char *system_command;
    int http_reply;
} evasive_config;

static int is_whitelisted(const char *ip, evasive_config *cfg);

static int is_uri_whitelisted(const char *uri, evasive_config *cfg);

/* END DoS Evasive Maneuvers Globals */

static void * create_dir_conf(apr_pool_t *p, __attribute__((unused)) char *context)
{
    /* Create a new hit list for this listener */
    evasive_config *cfg = apr_pcalloc(p, sizeof(evasive_config));
    if (cfg) {
        cfg->enabled = 0;
        cfg->hash_table_size = DEFAULT_HASH_TBL_SIZE;
        cfg->hit_list = ntt_create(cfg->hash_table_size);
        cfg->uri_whitelist = NULL;
        cfg->page_count = DEFAULT_PAGE_COUNT;
        cfg->page_interval = DEFAULT_PAGE_INTERVAL;
        cfg->site_count = DEFAULT_SITE_COUNT;
        cfg->site_interval = DEFAULT_SITE_INTERVAL;
        cfg->email_notify = NULL;
        cfg->log_dir = NULL;
        cfg->system_command = NULL;
        cfg->http_reply = DEFAULT_HTTP_REPLY;
    }

    return cfg;
}

static const char *whitelist(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *ip)
{
    evasive_config *cfg = (evasive_config *) dconfig;
    char entry[128];
    snprintf(entry, sizeof(entry), "WHITELIST_%s", ip);
    ntt_insert(cfg->hit_list, entry, time(NULL));

    return NULL;
}

static const char *whitelist_uri(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *uri_re)
{
    evasive_config *cfg = (evasive_config *) dconfig;
    struct pcre_node *node;

    node = (struct pcre_node *) malloc(sizeof(struct pcre_node));
    if (node == NULL) {
        return NULL;
    }

    int errornumber;
    PCRE2_SIZE erroroffset;

    PCRE2_SPTR pattern;
    pattern = (PCRE2_SPTR) uri_re;

    node->re = pcre2_compile(
            pattern,               /* the pattern */
            PCRE2_ZERO_TERMINATED, /* indicates pattern is zero-terminated */
            0,                     /* default options */
            &errornumber,          /* for error number */
            &erroroffset,          /* for error offset */
            NULL);                 /* use default compile context */

    /* Compilation failed: print the error message and exit. */

    if (node->re == NULL)
    {
        PCRE2_UCHAR buffer[256];
        pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf, "PCRE2 compilation of regex '%s' failed at offset %lu: %s\n",
                     uri_re, (unsigned long) erroroffset, buffer);
        free(node);
        return NULL;
    }

    node->next = cfg->uri_whitelist;
    cfg->uri_whitelist = node;
    return NULL;
}

static int access_checker(request_rec *r)
{
    evasive_config *cfg = (evasive_config *) ap_get_module_config(r->per_dir_config, &evasive_module);

    int ret = OK;

    /* BEGIN DoS Evasive Maneuvers Code */

    if (cfg->enabled && r->prev == NULL && r->main == NULL && cfg->hit_list != NULL) {
        char hash_key[2048];
        struct ntt_node *n;
        time_t t = time(NULL);

        /* Check whitelist */
        if (is_whitelisted(r->useragent_ip, cfg))
            return OK;

        /* First see if the IP itself is on "hold" */
        n = ntt_find(cfg->hit_list, r->useragent_ip);

        if (n != NULL && t-n->timestamp<cfg->blocking_period) {

            /* If the IP is on "hold", make it wait longer in 403 land */
            ret = cfg->http_reply;
            n->timestamp = t;

            /* Not on hold, check hit stats */
        } else {

            /* Check whitelisted uris */
            if (is_uri_whitelisted(r->uri, cfg))
                return OK;

            /* Has URI been hit too much? */
            snprintf(hash_key, sizeof(hash_key), "%s_%s", r->useragent_ip, r->uri);

            n = ntt_find(cfg->hit_list, hash_key);
            if (n != NULL) {

                /* If URI is being hit too much, add to "hold" list and 403 */
                if (t-n->timestamp<cfg->page_interval && n->count>=cfg->page_count) {
                    ret = cfg->http_reply;
                    ntt_insert(cfg->hit_list, r->useragent_ip, t);
                } else {

                    /* Reset our hit count list as necessary */
                    if (t-n->timestamp>=cfg->page_interval) {
                        n->count=0;
                    }
                }
                n->timestamp = t;
                n->count++;
            } else {
                ntt_insert(cfg->hit_list, hash_key, t);
            }

            /* Has site been hit too much? */
            snprintf(hash_key, sizeof(hash_key), "%s_SITE", r->useragent_ip);
            n = ntt_find(cfg->hit_list, hash_key);
            if (n != NULL) {

                /* If site is being hit too much, add to "hold" list and 403 */
                if (t-n->timestamp<cfg->site_interval && n->count>=cfg->site_count) {
                    ret = cfg->http_reply;
                    ntt_insert(cfg->hit_list, r->useragent_ip, t);
                } else {

                    /* Reset our hit count list as necessary */
                    if (t-n->timestamp>=cfg->site_interval) {
                        n->count=0;
                    }
                }
                n->timestamp = t;
                n->count++;
            } else {
                ntt_insert(cfg->hit_list, hash_key, t);
            }
        }

        /* Perform email notification and system functions */
        if (ret == cfg->http_reply) {
            char filename[1024];
            struct stat s;
            FILE *file;

            snprintf(filename, sizeof(filename), "%s/dos-%s", cfg->log_dir != NULL ? cfg->log_dir : DEFAULT_LOG_DIR, r->useragent_ip);
            if (stat(filename, &s)) {
                file = fopen(filename, "w");
                if (file != NULL) {
                    fprintf(file, "%ld\n", getpid());
                    fclose(file);

                    LOG(LOG_ALERT, "Blacklisting address %s: possible DoS attack.", r->useragent_ip);
                    if (cfg->email_notify != NULL) {
                        snprintf(filename, sizeof(filename), MAILER, cfg->email_notify);
                        file = popen(filename, "w");
                        if (file != NULL) {
                            fprintf(file, "To: %s\n", cfg->email_notify);
                            fprintf(file, "Subject: HTTP BLACKLIST %s\n\n", r->useragent_ip);
                            fprintf(file, "mod_evasive HTTP Blacklisted %s\n", r->useragent_ip);
                            pclose(file);
                        }
                    }

                    if (cfg->system_command != NULL) {
                        snprintf(filename, sizeof(filename), cfg->system_command, r->useragent_ip);
                        system(filename);
                    }

                } else {
                    LOG(LOG_ALERT, "Couldn't open logfile %s: %s",filename, strerror(errno));
                }

            } /* if (temp file does not exist) */

        } /* if (ret == cfg->http_reply) */

    } /* if (r->prev == NULL && r->main == NULL && cfg->hit_list != NULL) */

    /* END DoS Evasive Maneuvers Code */

    if (ret == cfg->http_reply
            && (ap_satisfies(r) != SATISFY_ANY || !ap_some_auth_required(r))) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                "client denied by server configuration: %s",
                r->filename);
    }

    return ret;
}

static int is_whitelisted(const char *ip, evasive_config *cfg) {
    char hashkey[128];
    char octet[4][4];
    char *dip;
    char *oct;
    int i = 0;

    memset(octet, 0, 16);
    dip = strdup(ip);
    if (dip == NULL)
        return 0;

    oct = strtok(dip, ".");
    while(oct != NULL && i<4) {
        if (strlen(oct)<=3)
            strcpy(octet[i], oct);
        i++;
        oct = strtok(NULL, ".");
    }
    free(dip);

    /* Exact Match */
    snprintf(hashkey, sizeof(hashkey), "WHITELIST_%s", ip);
    if (ntt_find(cfg->hit_list, hashkey)!=NULL)
        return 1;

    /* IPv4 Wildcards */
    snprintf(hashkey, sizeof(hashkey), "WHITELIST_%s.*.*.*", octet[0]);
    if (ntt_find(cfg->hit_list, hashkey)!=NULL)
        return 1;

    snprintf(hashkey, sizeof(hashkey), "WHITELIST_%s.%s.*.*", octet[0], octet[1]);
    if (ntt_find(cfg->hit_list, hashkey)!=NULL)
        return 1;

    snprintf(hashkey, sizeof(hashkey), "WHITELIST_%s.%s.%s.*", octet[0], octet[1], octet[2]);
    if (ntt_find(cfg->hit_list, hashkey)!=NULL)
        return 1;

    /* No match */
    return 0;
}

static int is_uri_whitelisted(const char *uri, evasive_config *cfg) {

    int rc;
    pcre2_match_data *match_data;

    PCRE2_SPTR subject;
    size_t subject_length;

    subject = (PCRE2_SPTR) uri;
    subject_length = strlen((char *)subject);

    for (struct pcre_node *node = cfg->uri_whitelist; node; node = node->next) {
        match_data = pcre2_match_data_create_from_pattern(node->re, NULL);
        if (!match_data) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf, "Failed to allocate pcre2 match data");
            continue;
        }

        rc = pcre2_match(
                node->re,             /* the compiled pattern */
                subject,              /* the subject string */
                subject_length,       /* the length of the subject */
                0,                    /* start at offset 0 in the subject */
                0,                    /* default options */
                match_data,           /* block for storing the result */
                NULL);                /* use default match context */

        pcre2_match_data_free(match_data);   /* Release memory used for the match */

        if (rc >= 0) {
            // match
            return 1;
        }
    }

    // no match
    return 0;
}

static apr_status_t destroy_config(void *dconfig) {
    evasive_config *cfg = (evasive_config *) dconfig;
    if (cfg != NULL) {
        ntt_destroy(cfg->hit_list);
        free(cfg->email_notify);
        free(cfg->log_dir);
        free(cfg->system_command);
        free(cfg);
   }
   return APR_SUCCESS;
}


/* BEGIN NTT (Named Timestamp Tree) Functions */

static const size_t ntt_prime_list[ntt_num_primes] =
{
    53UL,         97UL,         193UL,       389UL,       769UL,
    1543UL,       3079UL,       6151UL,      12289UL,     24593UL,
    49157UL,      98317UL,      196613UL,    393241UL,    786433UL,
    1572869UL,    3145739UL,    6291469UL,   12582917UL,  25165843UL,
    50331653UL,   100663319UL,  201326611UL, 402653189UL, 805306457UL,
    1610612741UL, 3221225473UL, 4294967291UL
};


/* Find the numeric position in the hash table based on key and modulus */

static size_t ntt_hashcode(const struct ntt *ntt, const char *key) {
    size_t val = 0;
    for (; *key; ++key) val = 5 * val + *key;
    return(val % ntt->size);
}

/* Creates a single node in the tree */

static struct ntt_node *ntt_node_create(const char *key, time_t timestamp) {
    char *node_key;
    struct ntt_node* node;

    node = (struct ntt_node *) malloc(sizeof(struct ntt_node));
    if (node == NULL) {
        return NULL;
    }
    if ((node_key = strdup(key)) == NULL) {
        free(node);
        return NULL;
    }
    *node = (struct ntt_node) {
        .key = node_key,
        .timestamp = timestamp,
        .count = 0,
        .next = NULL,
    };
    return(node);
}

/* Tree initializer */

static struct ntt *ntt_create(size_t size) {
    size_t i = 0;
    struct ntt *ntt = (struct ntt *) malloc(sizeof(struct ntt));

    if (ntt == NULL)
        return NULL;
    while (i < (ntt_num_primes - 1) && ntt_prime_list[i] < size) { i++; }
    ntt->size  = ntt_prime_list[i];
    ntt->items = 0;
    ntt->tbl   = (struct ntt_node **) calloc(ntt->size, sizeof(struct ntt_node *));
    if (ntt->tbl == NULL) {
        free(ntt);
        return NULL;
    }
    return(ntt);
}

/* Find an object in the tree */

static struct ntt_node *ntt_find(struct ntt *ntt, const char *key) {
    size_t hash_code;
    struct ntt_node *node;

    if (ntt == NULL) return NULL;

    hash_code = ntt_hashcode(ntt, key);
    node = ntt->tbl[hash_code];

    while (node) {
        if (!strcmp(key, node->key)) {
            return(node);
        }
        node = node->next;
    }
    return((struct ntt_node *)NULL);
}

/* Insert a node into the tree */

static struct ntt_node *ntt_insert(struct ntt *ntt, const char *key, time_t timestamp) {
    size_t hash_code;
    struct ntt_node *parent;
    struct ntt_node *node;
    struct ntt_node *new_node = NULL;

    if (ntt == NULL || ntt->items == SIZE_MAX) return NULL;

    hash_code = ntt_hashcode(ntt, key);
    parent  = NULL;
    node    = ntt->tbl[hash_code];

    while (node != NULL) {
        if (strcmp(key, node->key) == 0) {
            new_node = node;
            node = NULL;
        }

        if (new_node == NULL) {
            parent = node;
            node = node->next;
        }
    }

    if (new_node != NULL) {
        new_node->timestamp = timestamp;
        new_node->count = 0;
        return new_node;
    }

    /* Create a new node */
    new_node = ntt_node_create(key, timestamp);

    ntt->items++;

    /* Insert */
    if (parent) {  /* Existing parent */
        parent->next = new_node;
        return new_node;  /* Return the locked node */
    }

    /* No existing parent; add directly to hash table */
    ntt->tbl[hash_code] = new_node;
    return new_node;
}

/* Tree destructor */

static int ntt_destroy(struct ntt *ntt) {
    struct ntt_node *node, *next;
    struct ntt_c c;

    if (ntt == NULL) return -1;

    node = c_ntt_first(ntt, &c);
    while(node != NULL) {
        next = c_ntt_next(ntt, &c);
        ntt_delete(ntt, node->key);
        node = next;
    }

    free(ntt->tbl);
    free(ntt);

    return 0;
}

/* Delete a single node in the tree */

static int ntt_delete(struct ntt *ntt, const char *key) {
    size_t hash_code;
    struct ntt_node *parent = NULL;
    struct ntt_node *node;
    struct ntt_node *del_node = NULL;

    if (ntt == NULL) return -1;

    hash_code = ntt_hashcode(ntt, key);
    node        = ntt->tbl[hash_code];

    while (node != NULL) {
        if (strcmp(key, node->key) == 0) {
            del_node = node;
            node = NULL;
        }

        if (del_node == NULL) {
            parent = node;
            node = node->next;
        }
    }

    if (del_node != NULL) {

        if (parent) {
            parent->next = del_node->next;
        } else {
            ntt->tbl[hash_code] = del_node->next;
        }

        free(del_node->key);
        free(del_node);
        ntt->items--;

        return 0;
    }

    return -5;
}

/* Point cursor to first item in tree */

static struct ntt_node *c_ntt_first(struct ntt *ntt, struct ntt_c *c) {

    c->iter_index = 0;
    c->iter_next = (struct ntt_node *)NULL;
    return(c_ntt_next(ntt, c));
}

/* Point cursor to next iteration in tree */

static struct ntt_node *c_ntt_next(struct ntt *ntt, struct ntt_c *c) {
    size_t index;
    struct ntt_node *node = c->iter_next;

    if (ntt == NULL) return NULL;

    if (node) {
        c->iter_next = node->next;
        return (node);
    }

    while (c->iter_index < ntt->size) {
        index = c->iter_index++;

        if (ntt->tbl[index]) {
            c->iter_next = ntt->tbl[index]->next;
            return(ntt->tbl[index]);
        }
    }

    return((struct ntt_node *)NULL);
}

/* END NTT (Named Pointer Tree) Functions */


/* BEGIN Configuration Functions */

static const char *
get_enabled(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    cfg->enabled = (strcmp("true", value) == 0) ? 1 : 0;
    return NULL;
}

static const char *
get_hash_tbl_size(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    long n = strtol(value, NULL, 0);

    if (n<=0) {
        cfg->hash_table_size = DEFAULT_HASH_TBL_SIZE;
    } else  {
        cfg->hash_table_size = n;
    }
    cfg->hit_list = ntt_create(cfg->hash_table_size);

    return NULL;
}

static const char *
get_page_count(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    long n = strtol(value, NULL, 0);
    if (n<=0) {
        cfg->page_count = DEFAULT_PAGE_COUNT;
    } else {
        cfg->page_count = n;
    }

    return NULL;
}

static const char *
get_site_count(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    long n = strtol(value, NULL, 0);
    if (n<=0) {
        cfg->site_count = DEFAULT_SITE_COUNT;
    } else {
        cfg->site_count = n;
    }

    return NULL;
}

static const char *
get_page_interval(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    long n = strtol(value, NULL, 0);
    if (n<=0) {
        cfg->page_interval = DEFAULT_PAGE_INTERVAL;
    } else {
        cfg->page_interval = n;
    }

    return NULL;
}

static const char *
get_site_interval(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    long n = strtol(value, NULL, 0);
    if (n<=0) {
        cfg->site_interval = DEFAULT_SITE_INTERVAL;
    } else {
        cfg->site_interval = n;
    }

    return NULL;
}

static const char *
get_blocking_period(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    long n = strtol(value, NULL, 0);
    if (n<=0) {
        cfg->blocking_period = DEFAULT_BLOCKING_PERIOD;
    } else {
        cfg->blocking_period = n;
    }

    return NULL;
}

static const char *
get_log_dir(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    if (value != NULL && value[0] != 0) {
        if (cfg->log_dir != NULL)
            free(cfg->log_dir);
        cfg->log_dir = strdup(value);
    }

    return NULL;
}

static const char *
get_email_notify(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    if (value != NULL && value[0] != 0) {
        if (cfg->email_notify != NULL)
            free(cfg->email_notify);
        cfg->email_notify = strdup(value);
    }

    return NULL;
}

static const char *
get_system_command(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    if (value != NULL && value[0] != 0) {
        if (cfg->system_command != NULL)
            free(cfg->system_command);
        cfg->system_command = strdup(value);
    }

    return NULL;
}

static const char *
get_http_reply(__attribute__((unused)) cmd_parms *cmd, void *dconfig, const char *value) {
    evasive_config *cfg = (evasive_config *) dconfig;
    long reply = strtol(value, NULL, 0);
    if (reply <= 0) {
        cfg->http_reply = HTTP_FORBIDDEN;
    } else {
        cfg->http_reply = reply;
    }

    return NULL;
}

/* END Configuration Functions */

static const command_rec access_cmds[] =
{
    AP_INIT_TAKE1("DOSEnabled", get_enabled, NULL, RSRC_CONF,
            "Enable mod_evasive (either globally or in the virtualhost where it is specified)"),

    AP_INIT_TAKE1("DOSHashTableSize", get_hash_tbl_size, NULL, RSRC_CONF,
            "Set size of hash table"),

    AP_INIT_TAKE1("DOSPageCount", get_page_count, NULL, RSRC_CONF,
            "Set maximum page hit count per interval"),

    AP_INIT_TAKE1("DOSSiteCount", get_site_count, NULL, RSRC_CONF,
            "Set maximum site hit count per interval"),

    AP_INIT_TAKE1("DOSPageInterval", get_page_interval, NULL, RSRC_CONF,
            "Set page interval"),

    AP_INIT_TAKE1("DOSSiteInterval", get_site_interval, NULL, RSRC_CONF,
            "Set site interval"),

    AP_INIT_TAKE1("DOSBlockingPeriod", get_blocking_period, NULL, RSRC_CONF,
            "Set blocking period for detected DoS IPs"),

    AP_INIT_TAKE1("DOSEmailNotify", get_email_notify, NULL, RSRC_CONF,
            "Set email notification"),

    AP_INIT_TAKE1("DOSLogDir", get_log_dir, NULL, RSRC_CONF,
            "Set log dir"),

    AP_INIT_TAKE1("DOSSystemCommand", get_system_command, NULL, RSRC_CONF,
            "Set system command on DoS"),

    AP_INIT_ITERATE("DOSWhitelist", whitelist, NULL, RSRC_CONF,
            "IP-addresses wildcards to whitelist"),

    AP_INIT_ITERATE("DOSWhitelistUri", whitelist_uri, NULL, RSRC_CONF,
            "Files/paths regexes to whitelist"),

    AP_INIT_ITERATE("DOSHTTPStatus", get_http_reply, NULL, RSRC_CONF,
            "HTTP reply code"),

    { NULL }
};

static void register_hooks(apr_pool_t *p) {
    ap_hook_access_checker(access_checker, NULL, NULL, APR_HOOK_MIDDLE);
    apr_pool_cleanup_register(p, NULL, apr_pool_cleanup_null, destroy_config);
};

module AP_MODULE_DECLARE_DATA evasive_module =
{
    STANDARD20_MODULE_STUFF,
    create_dir_conf,
    NULL,
    NULL,
    NULL,
    access_cmds,
    register_hooks
};
