/* tinyproxy - A fast light-weight HTTP proxy
 * Copyright (C) 2003 Steven Young <sdyoung@miranda.org>
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

/* This file contains source code for the handling and display of
 * HTML error pages with variable substitution.
 */

#include "main.h"

#include "common.h"
#include "buffer.h"
#include "conns.h"
#include "heap.h"
#include "html-error.h"
#include "network.h"
#include "utils.h"
#include "conf.h"

/*
 * Add an error number -> filename mapping to the errorpages list.
 */
#define ERRORNUM_BUFSIZE 8      /* this is more than required */
#define ERRPAGES_BUCKETCOUNT 16

int add_new_errorpage (char *filepath, unsigned int errornum)
{
        char errornbuf[ERRORNUM_BUFSIZE];

        config->errorpages = hashmap_create (ERRPAGES_BUCKETCOUNT);
        if (!config->errorpages)
                return (-1);

        snprintf (errornbuf, ERRORNUM_BUFSIZE, "%u", errornum);

        if (hashmap_insert (config->errorpages, errornbuf,
                            filepath, strlen (filepath) + 1) < 0)
                return (-1);

        return (0);
}

/*
 * Get the file appropriate for a given error.
 */
static char *get_html_file (unsigned int errornum)
{
        hashmap_iter result_iter;
        char errornbuf[ERRORNUM_BUFSIZE];
        char *key;
        char *val;

        assert (errornum >= 100 && errornum < 1000);

        if (!config->errorpages)
                return (config->errorpage_undef);

        snprintf (errornbuf, ERRORNUM_BUFSIZE, "%u", errornum);

        result_iter = hashmap_find (config->errorpages, errornbuf);

        if (hashmap_is_end (config->errorpages, result_iter))
                return (config->errorpage_undef);

        if (hashmap_return_entry (config->errorpages, result_iter,
                                  &key, (void **) &val) < 0)
                return (config->errorpage_undef);

        return (val);
}

static void varsubst_sendline(struct conn_s *connptr, regex_t *re, char *p) {
	int fd = connptr->client_fd;
	while(*p) {
		regmatch_t match;
		char varname[32+1], *varval;
		size_t l;
		int st = regexec(re, p, 1, &match, 0);
		if(st == 0) {
			if(match.rm_so > 0) safe_write(fd, p, match.rm_so);
			l = match.rm_eo - match.rm_so;
			assert(l>2 && l-2 < sizeof(varname));
			p += match.rm_so;
			memcpy(varname, p+1, l-2);
			varname[l-2] = 0;
			varval = lookup_variable(connptr->error_variables, varname);
			if(varval) write_message(fd, "%s", varval);
			else if(varval && !*varval) write_message(fd, "(unknown)");
			else safe_write(fd, p, l);
			p += l;
		} else {
			write_message(fd, "%s", p);
			break;
		}
	}
}

/*
 * Send an already-opened file to the client with variable substitution.
 */
int
send_html_file (FILE *infile, struct conn_s *connptr)
{
        regex_t re;
        char *inbuf = safemalloc (4096);
        (void) regcomp(&re, "{[a-z]\\{1,32\\}}", 0);

        while (fgets (inbuf, 4096, infile)) {
                varsubst_sendline(connptr, &re, inbuf);
        }

        regfree (&re);
        safefree (inbuf);
        return 1;
}

int send_http_headers (struct conn_s *connptr, int code, const char *message)
{
        const char headers[] =
            "HTTP/1.%u %d %s\r\n"
            "Server: %s/%s\r\n"
            "Content-Type: text/html\r\n"
            "%s"
            "Connection: close\r\n" "\r\n";

        const char p_auth_str[] =
            "Proxy-Authenticate: Basic realm=\""
            PACKAGE_NAME "\"\r\n";

        const char w_auth_str[] =
            "WWW-Authenticate: Basic realm=\""
            PACKAGE_NAME "\"\r\n";

	/* according to rfc7235, the 407 error must be accompanied by
           a Proxy-Authenticate header field. */
        const char *add = code == 407 ? p_auth_str : (code == 401 ? w_auth_str : "");

        return (write_message (connptr->client_fd, headers,
                               connptr->protocol.major != 1 ? 0 : connptr->protocol.minor,
                               code, message, PACKAGE, VERSION,
                               add));
}

/*
 * Display an error to the client.
 */
int send_http_error_message (struct conn_s *connptr)
{
        char *error_file;
        FILE *infile;
        int ret;
        const char *fallback_error =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
            "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
            "<html>\n"
            "<head><title>%d %s</title></head>\n"
            "<body>\n"
            "<h1>%s</h1>\n"
            "<p>%s</p>\n"
            "<hr />\n"
            "<p><em>Generated by %s version %s.</em></p>\n" "</body>\n"
            "</html>\n";

        send_http_headers (connptr, connptr->error_number,
                           connptr->error_string);

        error_file = get_html_file (connptr->error_number);
        if (!(infile = fopen (error_file, "r"))) {
                char *detail = lookup_variable (connptr->error_variables, "detail");
                return (write_message (connptr->client_fd, fallback_error,
                                       connptr->error_number,
                                       connptr->error_string,
                                       connptr->error_string,
                                       detail, PACKAGE, VERSION));
        }

        ret = send_html_file (infile, connptr);
        fclose (infile);
        return (ret);
}

/*
 * Add a key -> value mapping for HTML file substitution.
 */

#define ERRVAR_BUCKETCOUNT 16

int
add_error_variable (struct conn_s *connptr, const char *key, const char *val)
{
        if (!connptr->error_variables)
                if (!
                    (connptr->error_variables =
                     hashmap_create (ERRVAR_BUCKETCOUNT)))
                        return (-1);

        return hashmap_insert (connptr->error_variables, key, val,
                               strlen (val) + 1);
}

#define ADD_VAR_RET(x, y)				   \
	do {                                               \
                if (y == NULL)                             \
                        break;                             \
		if (add_error_variable(connptr, x, y) < 0) \
			return -1;			   \
	} while (0)

/*
 * Set some standard variables used by all HTML pages
 */
int add_standard_vars (struct conn_s *connptr)
{
        char errnobuf[16];
        char timebuf[30];
        time_t global_time;

        snprintf (errnobuf, sizeof errnobuf, "%d", connptr->error_number);
        ADD_VAR_RET ("errno", errnobuf);

        ADD_VAR_RET ("cause", connptr->error_string);
        ADD_VAR_RET ("request", connptr->request_line);
        ADD_VAR_RET ("clientip", connptr->client_ip_addr);

        /* The following value parts are all non-NULL and will
         * trigger warnings in ADD_VAR_RET(), so we use
         * add_error_variable() directly.
         */

        global_time = time (NULL);
        strftime (timebuf, sizeof (timebuf), "%a, %d %b %Y %H:%M:%S GMT",
                  gmtime (&global_time));
        add_error_variable (connptr, "date", timebuf);

        add_error_variable (connptr, "website",
                            "https://tinyproxy.github.io/");
        add_error_variable (connptr, "version", VERSION);
        add_error_variable (connptr, "package", PACKAGE);

        return (0);
}

/*
 * Add the error information to the conn structure.
 */
int
indicate_http_error (struct conn_s *connptr, int number,
                     const char *message, ...)
{
        va_list ap;
        char *key, *val;

        va_start (ap, message);

        while ((key = va_arg (ap, char *))) {
                val = va_arg (ap, char *);

                if (add_error_variable (connptr, key, val) == -1) {
                        va_end (ap);
                        return (-1);
                }
        }

        connptr->error_number = number;
        connptr->error_string = safestrdup (message);

        va_end (ap);

        return (add_standard_vars (connptr));
}
