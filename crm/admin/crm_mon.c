/* $Id: crm_mon.c,v 1.3 2005/07/06 09:38:27 andrew Exp $ */

/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <portability.h>

#include <sys/param.h>

#include <crm/crm.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>

#include <heartbeat.h>
#include <hb_api.h>
#include <clplumbing/uids.h>
#include <clplumbing/cl_pidfile.h>
#include <clplumbing/Gmain_timeout.h>

#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/ctrl.h>
#include <crm/common/ipc.h>

#include <crm/cib.h>
#include <crm/pengine/pengine.h>

#ifdef HAVE_GETOPT_H
#  include <getopt.h>
#endif

#include <crm/dmalloc_wrapper.h>

/* GMainLoop *mainloop = NULL; */
const char *crm_system_name = "crm_mon";

#define OPTARGS	"V?i:nrh:cdp:"

#if CURSES_ENABLED
void usage(const char *cmd, int exit_status);
void blank_screen(void);
int print_status(crm_data_t *cib);
#define printw_at(line, fmt...) move(line, 0); printw(fmt); line++
void wait_for_refresh(int offset, const char *prefix, int seconds);
int print_html_status(crm_data_t *cib, const char *filename);
void make_daemon(gboolean daemonize, const char *pidfile);

char *as_html_file = NULL;
char *pid_file = NULL;
gboolean as_console = FALSE;
gboolean group_by_node = FALSE;
gboolean inactive_resources = FALSE;
int interval = 15;
gboolean daemonize = FALSE;

int
main(int argc, char **argv)
{
	int failed_connections = 0;
	int options = cib_scope_local|cib_sync_call;
	crm_data_t *xml_frag = NULL;
	cib_t *cib_conn = NULL;
	int argerr = 0;
	int flag;

#ifdef HAVE_GETOPT_H
	int option_index = 0;
	static struct option long_options[] = {
		/* Top-level Options */
		{"verbose", 0, 0, 'V'},
		{"help", 0, 0, '?'},
		{"interval", 1, 0, 'i'},
		{"group-by-node", 0, 0, 'n'},
		{"inactive", 0, 0, 'r'},
		{"as-html", 1, 0, 'h'},		
		{"as-console", 0, 0, 'c'},		
		{"daemonize", 0, 0, 'd'},		
		{"pid-file", 0, 0, 'p'},		

		{0, 0, 0, 0}
	};
#endif
	pid_file = crm_strdup("/tmp/ClusterMon.pid");
	crm_system_name = basename(argv[0]);
	crm_log_init(crm_system_name);
	crm_log_level = LOG_ERR -1;
	
	while (1) {
#ifdef HAVE_GETOPT_H
		flag = getopt_long(argc, argv, OPTARGS,
				   long_options, &option_index);
#else
		flag = getopt(argc, argv, OPTARGS);
#endif
		if (flag == -1)
			break;

		switch(flag) {
			case 'V':
				cl_log_enable_stderr(TRUE);
				alter_debug(DEBUG_INC);
				break;
			case 'i':
				interval = atoi(optarg);
				break;
			case 'n':
				group_by_node = TRUE;
				break;
			case 'r':
				inactive_resources = TRUE;
				break;
			case 'd':
				daemonize = TRUE;
				break;
			case 'p':
				pid_file = crm_strdup(optarg);
				break;
			case 'h':
				as_html_file = crm_strdup(optarg);
				break;
			default:
				printf("Argument code 0%o (%c) is not (?yet?) supported\n", flag, flag);
				++argerr;
				break;
		}
	}

	if (optind < argc) {
		printf("non-option ARGV-elements: ");
		while (optind < argc)
			printf("%s ", argv[optind++]);
		printf("\n");
	}
	if (argerr) {
		usage(crm_system_name, LSB_EXIT_GENERIC);
	}

	if(as_html_file == NULL) {
		as_console = TRUE;
	}

	if(daemonize) {
		as_console = FALSE;
	}

	if(daemonize && as_html_file == NULL) {
		usage(crm_system_name, LSB_EXIT_GENERIC);
	}
	
	make_daemon(daemonize, pid_file);
	
	if(as_console) {
		initscr();
		cbreak();
		noecho();
	}
	
	while(1) {
		int offset = 0;
		const char *prefix = NULL;
		int rc = cib_ok;
		if(cib_conn == NULL || cib_conn->state != cib_connected_query){
			crm_free(cib_conn);
			crm_debug_4("Creating CIB connection");
			cib_conn = cib_new();
			if(cib_conn != NULL) {
				crm_debug_4("Connecting to the CIB");
				if(cib_ok == cib_conn->cmds->signon(
					   cib_conn, crm_system_name, cib_query)) {
					failed_connections = 0;
				}
			}
		}
		if(as_console) { blank_screen(); }
		if(cib_conn == NULL || cib_conn->state != cib_connected_query){
			failed_connections++;
			if(as_console) { refresh(); }
			wait_for_refresh(0, "Not connected: ", 2*interval);
			continue;
		}
		
		rc = cib_conn->cmds->query(cib_conn, NULL, &xml_frag, options);
/* 		add_cib_op_callback(rc, FALSE, crm_strdup(max_generation_from), */
/* 				    finalize_sync_callback); */
		if(rc == cib_ok) {
			crm_data_t *cib = NULL;
			cib = find_xml_node(xml_frag,XML_TAG_CIB,TRUE);
			if(as_html_file) {
				print_html_status(cib, as_html_file);
			}
			if(as_console) {
				offset = print_status(cib);
			}
			free_xml(xml_frag);
			
			cib_conn->cmds->signoff(cib_conn);
			
		} else {
			crm_err("Query failed: %s", cib_error2string(rc));
			prefix = "Query failed! ";
			
		}
		wait_for_refresh(0, prefix, interval);
	}
	if(as_console) {
		echo();
		nocbreak();
		endwin();
	}
	return 0;
}

void
wait_for_refresh(int offset, const char *prefix, int seconds) 
{
	int lpc = seconds;
	if(as_console == FALSE) {
		crm_notice("%sRefresh in %ds...", prefix?prefix:"", lpc);
	}
	while(lpc > 0) {
		if(as_console) {
			move(offset, 0);
/* 		printw("%sRefresh in \033[01;32m%ds\033[00m...", prefix?prefix:"", lpc); */
			printw("%sRefresh in %ds...", prefix?prefix:"", lpc);
			clrtoeol();
			refresh();
		}
		lpc--;
		sleep(1);
	}
	if(as_console) {
		move(offset, 0);
		printw("Updating...");
		clrtoeol();
		refresh();
	} else {
		crm_notice("Updating...");
	}
	
}


int
print_status(crm_data_t *cib) 
{
	static int updates = 0;
	int lpc = 0;
	pe_working_set_t data_set;
	node_t *dc = NULL;
	updates++;
	set_working_set_defaults(&data_set);
	data_set.input = cib;
	stage0(&data_set);

	dc = data_set.dc_node;

	lpc++;
	
	printw_at(lpc, "============");
	if(dc == NULL) {
		printw_at(lpc, "Current DC: NONE");
	} else {
		printw_at(lpc, "Current DC: %s (%s)",
			  dc->details->uname, dc->details->id);
	}
	printw_at(lpc, "%d Nodes configured.",
		  g_list_length(data_set.nodes));
	printw_at(lpc, "%d Resources configured.",
		  g_list_length(data_set.resources));
	printw_at(lpc, "============");

	lpc++;
	
	slist_iter(node, node_t, data_set.nodes, lpc2,
		   printw_at(lpc, "Node: %s (%s): %s",
			     node->details->uname, node->details->id,
			     node->details->online?"online":"OFFLINE");
		   if(group_by_node) {
			   slist_iter(rsc, resource_t,
				      node->details->running_rsc, lpc2,
				      common_printw(rsc, "\t", &lpc);
/* 				      rsc->fns->printw(rsc, "\t", &lpc); */
				      lpc++;
				   );
		   }
		);

	lpc++;
	
	if(group_by_node && inactive_resources) {
		printw_at(lpc, "Full list of resources:");
	}
	if(group_by_node == FALSE || inactive_resources) {
		slist_iter(rsc, resource_t, data_set.resources, lpc2,
			   rsc->fns->printw(rsc, NULL, &lpc);
			   lpc++;
			);
	}
	move(lpc, 0);

	refresh();
	cleanup_calculations(&data_set);
	return lpc;
}

int
print_html_status(crm_data_t *cib, const char *filename) 
{
	static int updates = 0;
	pe_working_set_t data_set;
	node_t *dc = NULL;

	FILE *stream = fopen(filename, "w");
	if(stream == NULL) {
		return -1;
	}	
	updates++;
	set_working_set_defaults(&data_set);
	data_set.input = cib;
	stage0(&data_set);

	dc = data_set.dc_node;

	fprintf(stream, "<html>");
	fprintf(stream, "<head>");
	fprintf(stream, "<title>Cluster status</title>");
/* content="%d;url=http://webdesign.about.com" */
	fprintf(stream,
		"<meta http-equiv=\"refresh\" content=\"%d\">", interval);
	fprintf(stream, "</head>");
	fprintf(stream, "<h2>Cluster summary</h2>");
	{
		char *now_str = NULL;
		time_t now = time(NULL);
		now_str = ctime(&now);
		now_str[24] = EOS; /* replace the newline */
		fprintf(stream, "Last updated: <b>%s</b><br/>\n", now_str);
	}
	
	if(dc == NULL) {
		fprintf(stream, "Current DC: <font color=\"red\"><b>NONE</b></font><br/>");
	} else {
		fprintf(stream, "Current DC: %s (%s)<br/>",
			dc->details->uname, dc->details->id);
	}
	fprintf(stream, "%d Nodes configured.<br/>",
		  g_list_length(data_set.nodes));
	fprintf(stream, "%d Resources configured.<br/>",
		  g_list_length(data_set.resources));
	fprintf(stream, "<br/>");

	fprintf(stream, "<h2>Node list</h2>\n");
	fprintf(stream, "<ul>\n");
	slist_iter(node, node_t, data_set.nodes, lpc2,
		   fprintf(stream, "<li>");
		   fprintf(stream, "Node: %s (%s): %s",
			     node->details->uname, node->details->id,
			     node->details->online?"<font color=\"green\">online</font>":"<font color=\"orange\"><b>OFFLINE</b></font>");
		   if(group_by_node) {
			   fprintf(stream, "<ul>\n");
			   slist_iter(rsc, resource_t,
				      node->details->running_rsc, lpc2,
				      fprintf(stream, "<li>");
				      common_html(rsc, "\t", stream);
				      fprintf(stream, "</li>\n");
				   );
			   fprintf(stream, "</ul>\n");
		   }
		   fprintf(stream, "</li>\n");
		);
	fprintf(stream, "</ul>\n");
	
	if(group_by_node && inactive_resources) {
		fprintf(stream, "<h2>Full list of resources</h2>\n");
	}
	if(group_by_node == FALSE || inactive_resources) {
		slist_iter(rsc, resource_t, data_set.resources, lpc2,
			   rsc->fns->html(rsc, NULL, stream);
			);
	}

	cleanup_calculations(&data_set);
	fprintf(stream, "</html>");
	fflush(stream);
	fclose(stream);

	return 0;
}


void
blank_screen(void) 
{
	int lpc = 0;
	for(lpc = 0; lpc < LINES; lpc++) {
		move(lpc, 0);
		clrtoeol();
	}
	move(0, 0);
}


void
usage(const char *cmd, int exit_status)
{
	FILE *stream;

	stream = exit_status ? stderr : stdout;

	fprintf(stream, "usage: %s [-%s]\n", cmd, OPTARGS);
	fflush(stream);

	exit(exit_status);
}

void
make_daemon(gboolean daemonize, const char *pidfile)
{
	long pid;
	const char *devnull = "/dev/null";

	if (daemonize == FALSE){
		return;
	}
	
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "%s: could not start daemon\n",
			crm_system_name);
		perror("fork");
		exit(LSB_EXIT_GENERIC);
	} else if (pid > 0) {
		exit(LSB_EXIT_OK);
	}
	
	if (cl_lock_pidfile(pidfile) < 0 ){
		pid = cl_read_pidfile(pidfile);
		fprintf(stderr, "%s: already running [pid %ld].\n",
			crm_system_name, pid);
		exit(LSB_EXIT_OK);
	}
	
	umask(022);
	close(FD_STDIN);
	(void)open(devnull, O_RDONLY);		/* Stdin:  fd 0 */
	close(FD_STDOUT);
	(void)open(devnull, O_WRONLY);		/* Stdout: fd 1 */
	close(FD_STDERR);
	(void)open(devnull, O_WRONLY);		/* Stderr: fd 2 */
}

#else
int
main(int argc, char **argv)
{
	fprintf(stderr, "The use of %s requires ncurses to be available"
		" during the build process\n", crm_system_name);
	exit(1);
}
#endif