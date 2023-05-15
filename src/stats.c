/* tinyproxy - A fast light-weight HTTP proxy
 * Copyright (C) 2000 Robert James Kaes <rjkaes@users.sourceforge.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* This module handles the statistics for tinyproxy. There are only two
 * public API functions. The reason for the functions, rather than just a
 * external structure is that tinyproxy is now multi-threaded and we can
 * not allow more than one child to access the statistics at the same
 * time. This is prevented by a mutex. If there is a need for more
 * statistics in the future, just add to the structure, enum (in the header),
 * and the switch statement in update_stats().
 */

#include "main.h"

#include "log.h"
#include "heap.h"
#include "html-error.h"
#include "stats.h"
#include "utils.h"
#include "conf.h"
#include <pthread.h>

struct stat_s {
        unsigned long int num_reqs;
        unsigned long int num_badcons;
        unsigned long int num_open;
        unsigned long int num_refused;
        unsigned long int num_denied;
};

static struct stat_s stats_buf, *stats;
static pthread_mutex_t stats_update_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stats_file_lock = PTHREAD_MUTEX_INITIALIZER;

static orderedmap stat_types;
/*
 * Initialize the statistics information to zero.
 */
void init_stats (void)
{
        stats = &stats_buf;
        
        /* TODO if config->stats are defined, then init the orderedmap */
        stat_types = orderedmap_create(3);
        /* TODO fail to create */
        orderedmap_append(stat_types, "text/html", "data/templates/stats.html");
        orderedmap_append(stat_types, "application/json", "data/templates/stats.json");
}

/*
 * Display the statics of the tinyproxy server.
 */
int
showstats (struct conn_s *connptr, orderedmap request_headers)
{
        char *message_buffer;
        char opens[16], reqs[16], badconns[16], denied[16], refused[16];
        FILE *statfile;
        char *accept_header;
        char *stats_template;
        char *type_based_stats;
        size_t iter;
        char *stat_type, *type_fname;

        snprintf (opens, sizeof (opens), "%lu", stats->num_open);
        snprintf (reqs, sizeof (reqs), "%lu", stats->num_reqs);
        snprintf (badconns, sizeof (badconns), "%lu", stats->num_badcons);
        snprintf (denied, sizeof (denied), "%lu", stats->num_denied);
        snprintf (refused, sizeof (refused), "%lu", stats->num_refused);

        accept_header = orderedmap_find(request_headers, "accept");

        type_based_stats = NULL;
        iter = 0;
        while((iter = orderedmap_next(stat_types, iter, &stat_type, &type_fname))) {
            if (strstr(accept_header, stat_type)) {
                type_based_stats = type_fname;
                break;
            }
        }

        if (type_based_stats) {
            /* new type-based stats */
            stats_template = type_based_stats;
        } else if (config->statpage) {
            /* backward compatible single-page stats */
            stats_template = config->statpage;
        } else {
            /* null template yielding default minimal page */
            stats_template = NULL;
        }

        pthread_mutex_lock(&stats_file_lock);

        if (!stats_template || (!(statfile = fopen (stats_template, "r")))) {
                message_buffer = (char *) safemalloc (MAXBUFFSIZE);
                if (!message_buffer) {
err_minus_one:
                        pthread_mutex_unlock(&stats_file_lock);
                        return -1;
                }

                snprintf
                  (message_buffer, MAXBUFFSIZE,
                   "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
                   "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
                   "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
                   "<html>\n"
                   "<head><title>%s version %s run-time statistics</title></head>\n"
                   "<body>\n"
                   "<h1>%s version %s run-time statistics</h1>\n"
                   "<p>\n"
                   "Number of open connections: %lu<br />\n"
                   "Number of requests: %lu<br />\n"
                   "Number of bad connections: %lu<br />\n"
                   "Number of denied connections: %lu<br />\n"
                   "Number of refused connections due to high load: %lu\n"
                   "</p>\n"
                   "<hr />\n"
                   "<p><em>Generated by %s version %s.</em></p>\n" "</body>\n"
                   "</html>\n",
                   PACKAGE, VERSION, PACKAGE, VERSION,
                   stats->num_open,
                   stats->num_reqs,
                   stats->num_badcons, stats->num_denied,
                   stats->num_refused, PACKAGE, VERSION);

                if (send_http_message (connptr, 200, "OK",
                                       message_buffer) < 0) {
                        safefree (message_buffer);
                        goto err_minus_one;
                }

                safefree (message_buffer);
                pthread_mutex_unlock(&stats_file_lock);
                return 0;
        }
        add_error_variable (connptr, "opens", opens);
        add_error_variable (connptr, "reqs", reqs);
        add_error_variable (connptr, "badconns", badconns);
        add_error_variable (connptr, "deniedconns", denied);
        add_error_variable (connptr, "refusedconns", refused);
        add_standard_vars (connptr);
        send_http_headers (connptr, 200, "Statistic requested", "");
        send_html_file (statfile, connptr);
        fclose (statfile);
        pthread_mutex_unlock(&stats_file_lock);

        return 0;
}

/*
 * Update the value of the statistics. The update_level is defined in
 * stats.h
 */
int update_stats (status_t update_level)
{
        int ret = 0;

        pthread_mutex_lock(&stats_update_lock);
        switch (update_level) {
        case STAT_BADCONN:
                ++stats->num_badcons;
                break;
        case STAT_OPEN:
                ++stats->num_open;
                ++stats->num_reqs;
                break;
        case STAT_CLOSE:
                --stats->num_open;
                break;
        case STAT_REFUSE:
                ++stats->num_refused;
                break;
        case STAT_DENIED:
                ++stats->num_denied;
                break;
        default:
                ret = -1;
        }
        pthread_mutex_unlock(&stats_update_lock);

        return ret;
}
