/*
 * repmgr.c - Command interpreter for the repmgr
 * Copyright (C) 2ndQuadrant, 2010-2015
 *
 * This module is a command-line utility to easily setup a cluster of
 * hot standby servers for an HA environment
 *
 * Commands implemented are.
 * MASTER REGISTER
 * STANDBY REGISTER, STANDBY CLONE, STANDBY FOLLOW, STANDBY PROMOTE
 * CLUSTER SHOW, CLUSTER CLEANUP
 * WITNESS CREATE
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "repmgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>			/* for stat() */
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "pqexpbuffer.h"

#include "log.h"
#include "config.h"
#include "check_dir.h"
#include "strutil.h"
#include "version.h"

#define RECOVERY_FILE "recovery.conf"

#define NO_ACTION		 0		/* Not a real action, just to initialize */
#define MASTER_REGISTER  1
#define STANDBY_REGISTER 2
#define STANDBY_CLONE	 3
#define STANDBY_PROMOTE  4
#define STANDBY_FOLLOW	 5
#define WITNESS_CREATE	 6
#define CLUSTER_SHOW	 7
#define CLUSTER_CLEANUP  8

static bool create_recovery_file(const char *data_dir);
static int	test_ssh_connection(char *host, char *remote_user);
static int  copy_remote_files(char *host, char *remote_user, char *remote_path,
				  char *local_path);
static int  run_basebackup(void);
static bool check_parameters_for_action(const int action);
static bool create_schema(PGconn *conn);
static bool copy_configuration(PGconn *masterconn, PGconn *witnessconn);
static void write_primary_conninfo(char *line);
static bool write_recovery_file_line(FILE *recovery_file, char *recovery_file_path, char *line);
static int	check_server_version(PGconn *conn, char *server_type, bool exit_on_error, char *server_version_string);
static bool check_upstream_config(PGconn *conn, int server_version_num, bool exit_on_error);
static bool create_node_record(PGconn *conn, char *action, int node, char *type, int upstream_node, char *cluster_name, char *node_name, char *conninfo, int priority);
static char *make_pg_path(char *file);

static void do_master_register(void);
static void do_standby_register(void);
static void do_standby_clone(void);
static void do_standby_promote(void);
static void do_standby_follow(void);
static void do_witness_create(void);
static void do_cluster_show(void);
static void do_cluster_cleanup(void);
static void do_check_upstream_config(void);

static void usage(void);
static void help(const char *progname);

/* Global variables */
static const char *progname;
static const char *keywords[6];
static const char *values[6];
bool		need_a_node = true;

/* XXX This should be mapped into a command line option */
bool		require_password = false;

/* Initialization of runtime options */
t_runtime_options runtime_options = T_RUNTIME_OPTIONS_INITIALIZER;
t_configuration_options options = T_CONFIGURATION_OPTIONS_INITIALIZER;

static char *server_mode = NULL;
static char *server_cmd = NULL;

static char pg_bindir[MAXLEN] = "";
static char repmgr_slot_name[MAXLEN] = "";
static char path_buf[MAXLEN] = "";

int
main(int argc, char **argv)
{
	static struct option long_options[] =
	{
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"superuser", required_argument, NULL, 'S'},
		{"dest-dir", required_argument, NULL, 'D'},
		{"local-port", required_argument, NULL, 'l'},
		{"config-file", required_argument, NULL, 'f'},
		{"remote-user", required_argument, NULL, 'R'},
		{"wal-keep-segments", required_argument, NULL, 'w'},
		{"keep-history", required_argument, NULL, 'k'},
		{"force", no_argument, NULL, 'F'},
		{"wait", no_argument, NULL, 'W'},
		{"min-recovery-apply-delay", required_argument, NULL, 'r'},
		{"verbose", no_argument, NULL, 'v'},
		{"pg_bindir", required_argument, NULL, 'b'},
		{"initdb-no-pwprompt", no_argument, NULL, 1},
		{"check-upstream-config", no_argument, NULL, 2},
		{NULL, 0, NULL, 0}
	};

	int			optindex;
	int			c, targ;
	int			action = NO_ACTION;
	bool 		check_master_config = false;
	bool 		wal_keep_segments_used  = false;
	char 	   *ptr = NULL;

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(SUCCESS);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			printf("%s %s (PostgreSQL %s)\n", progname, REPMGR_VERSION, PG_VERSION);
			exit(SUCCESS);
		}
	}


	while ((c = getopt_long(argc, argv, "d:h:p:U:S:D:l:f:R:w:k:FWIvr:b:", long_options,
							&optindex)) != -1)
	{
		switch (c)
		{
			case 'd':
				strncpy(runtime_options.dbname, optarg, MAXLEN);
				break;
			case 'h':
				strncpy(runtime_options.host, optarg, MAXLEN);
				break;
			case 'p':
				if (atoi(optarg) > 0)
					strncpy(runtime_options.masterport, optarg, MAXLEN);
				break;
			case 'U':
				strncpy(runtime_options.username, optarg, MAXLEN);
				break;
			case 'S':
				strncpy(runtime_options.superuser, optarg, MAXLEN);
				break;
			case 'D':
				strncpy(runtime_options.dest_dir, optarg, MAXFILENAME);
				break;
			case 'l':
				if (atoi(optarg) > 0)
					strncpy(runtime_options.localport, optarg, MAXLEN);
				break;
			case 'f':
				strncpy(runtime_options.config_file, optarg, MAXLEN);
				break;
			case 'R':
				strncpy(runtime_options.remote_user, optarg, MAXLEN);
				break;
			case 'w':
				if (atoi(optarg) > 0)
				{
					strncpy(runtime_options.wal_keep_segments, optarg, MAXLEN);
					wal_keep_segments_used = true;
				}
				break;
			case 'k':
				if (atoi(optarg) > 0)
					runtime_options.keep_history = atoi(optarg);
				else
					runtime_options.keep_history = 0;
				break;
			case 'F':
				runtime_options.force = true;
				break;
			case 'W':
				runtime_options.wait_for_master = true;
				break;
			case 'I':
				runtime_options.ignore_rsync_warn = true;
				break;
			case 'r':
				targ = strtol(optarg, &ptr, 10);

				if(targ < 0) {
					usage();
					exit(ERR_BAD_CONFIG);
				}
				if(ptr && *ptr) {
					if(strcmp(ptr, "ms") != 0 && strcmp(ptr, "s") != 0 &&
					   strcmp(ptr, "min") != 0 && strcmp(ptr, "h") != 0 &&
					   strcmp(ptr, "d") != 0)
					{
						usage();
						exit(ERR_BAD_CONFIG);
					}
				}

				strncpy(runtime_options.min_recovery_apply_delay, optarg, MAXLEN);
				break;
			case 'v':
				runtime_options.verbose = true;
				break;
			case 'b':
				strncpy(runtime_options.pg_bindir, optarg, MAXLEN);
				break;
			case 1:
				runtime_options.initdb_no_pwprompt = true;
				break;
			case 2:
				check_master_config = true;
				break;
			default:
				usage();
				exit(ERR_BAD_CONFIG);
		}
	}

	if(check_master_config == true)
	{
		do_check_upstream_config();
		exit(SUCCESS);
	}

	/*
	 * Now we need to obtain the action, this comes in one of these forms:
	 * MASTER REGISTER | STANDBY {REGISTER | CLONE [node] | PROMOTE | FOLLOW
	 * [node]} | WITNESS CREATE CLUSTER {SHOW | CLEANUP}
	 *
	 * the node part is optional, if we receive it then we shouldn't have
	 * received a -h option
	 */
	if (optind < argc)
	{
		server_mode = argv[optind++];
		if (strcasecmp(server_mode, "STANDBY") != 0 &&
			strcasecmp(server_mode, "MASTER") != 0 &&
			strcasecmp(server_mode, "WITNESS") != 0 &&
			strcasecmp(server_mode, "CLUSTER") != 0)
		{
			usage();
			exit(ERR_BAD_CONFIG);
		}
	}

	if (optind < argc)
	{
		server_cmd = argv[optind++];
		/* check posibilities for all server modes */
		if (strcasecmp(server_mode, "MASTER") == 0)
		{
			if (strcasecmp(server_cmd, "REGISTER") == 0)
				action = MASTER_REGISTER;
		}
		else if (strcasecmp(server_mode, "STANDBY") == 0)
		{
			if (strcasecmp(server_cmd, "REGISTER") == 0)
				action = STANDBY_REGISTER;
			else if (strcasecmp(server_cmd, "CLONE") == 0)
				action = STANDBY_CLONE;
			else if (strcasecmp(server_cmd, "PROMOTE") == 0)
				action = STANDBY_PROMOTE;
			else if (strcasecmp(server_cmd, "FOLLOW") == 0)
				action = STANDBY_FOLLOW;
		}
		else if (strcasecmp(server_mode, "CLUSTER") == 0)
		{
			if (strcasecmp(server_cmd, "SHOW") == 0)
				action = CLUSTER_SHOW;
			else if (strcasecmp(server_cmd, "CLEANUP") == 0)
				action = CLUSTER_CLEANUP;
		}
		else if (strcasecmp(server_mode, "WITNESS") == 0)
			if (strcasecmp(server_cmd, "CREATE") == 0)
				action = WITNESS_CREATE;
	}

	if (action == NO_ACTION)
	{
		usage();
		exit(ERR_BAD_CONFIG);
	}

	/* For some actions we still can receive a last argument */
	if (action == STANDBY_CLONE)
	{
		if (optind < argc)
		{
			if (runtime_options.host[0])
			{
				log_err(_("Conflicting parameters:  you can't use -h while providing a node separately.\n"));
				usage();
				exit(ERR_BAD_CONFIG);
			}
			strncpy(runtime_options.host, argv[optind++], MAXLEN);
		}
	}

	if (optind < argc)
	{
		log_err(_("%s: too many command-line arguments (first extra is \"%s\")\n"),
				progname, argv[optind]);
		usage();
		exit(ERR_BAD_CONFIG);
	}

	if (!check_parameters_for_action(action))
		exit(ERR_BAD_CONFIG);

	if (!runtime_options.dbname[0])
	{
		if (getenv("PGDATABASE"))
			strncpy(runtime_options.dbname, getenv("PGDATABASE"), MAXLEN);
		else if (getenv("PGUSER"))
			strncpy(runtime_options.dbname, getenv("PGUSER"), MAXLEN);
		else
			strncpy(runtime_options.dbname, DEFAULT_DBNAME, MAXLEN);
	}

	/* We check that port number is not null */
	if (!runtime_options.dbname[0])
	{
		strncpy(runtime_options.masterport, DEFAULT_MASTER_PORT, MAXLEN);
	}

	/*
	 * If a configuration file was provided, check it exists, otherwise
	 * emit an error
	 */
	if (runtime_options.config_file[0])
	{
		struct stat config;
		if(stat(runtime_options.config_file, &config) != 0)
		{
			log_err(_("Provided configuration file '%s' not found: %s\n"),
					runtime_options.config_file,
					strerror(errno)
				);
			exit(ERR_BAD_CONFIG);
		}
	}

	/*
	 * If no configuration file was provided, set to a default file
	 * which `parse_config()` will attempt to read if it exists
	 */
	else
	{
		strncpy(runtime_options.config_file, DEFAULT_CONFIG_FILE, MAXLEN);
	}

	if (runtime_options.verbose)
		printf(_("Opening configuration file: %s\n"),
			   runtime_options.config_file);

	/*
	 * The configuration file is not required for some actions (e.g. 'standby clone'),
	 * however if available we'll parse it anyway for options like 'log_level',
	 * 'use_replication_slots' etc.
	 */
	parse_config(runtime_options.config_file, &options);

	/*
	 * Initialise pg_bindir - command line parameter will override
	 * any setting in the configuration file
	 */
	if(!strlen(runtime_options.pg_bindir))
	{
		strncpy(runtime_options.pg_bindir, options.pg_bindir, MAXLEN);
	}

	/* Add trailing slash */
	if(strlen(runtime_options.pg_bindir))
	{
		int len = strlen(runtime_options.pg_bindir);
		if(runtime_options.pg_bindir[len - 1] != '/')
		{
			maxlen_snprintf(pg_bindir, "%s/", runtime_options.pg_bindir);
		}
		else
		{
			strncpy(pg_bindir, runtime_options.pg_bindir, MAXLEN);
		}
	}

	keywords[2] = "user";
	values[2] = (runtime_options.username[0]) ? runtime_options.username : NULL;
	keywords[3] = "dbname";
	values[3] = runtime_options.dbname;
	keywords[4] = "application_name";
	values[4] = (char *) progname;
	keywords[5] = NULL;
	values[5] = NULL;

	/*
	 * Initialize the logger.  If verbose command line parameter was input,
	 * make sure that the log level is at least INFO.  This is mainly useful
	 * for STANDBY CLONE.  That doesn't require a configuration file where a
	 * logging level might be specified at, but it often requires detailed
	 * logging to troubleshoot problems.
	 */
	logger_init(&options, progname, options.loglevel, options.logfacility);
	if (runtime_options.verbose)
		logger_min_verbose(LOG_INFO);

	/*
	 * Node configuration information is not needed for all actions, with
	 * STANDBY CLONE being the main exception.
	 */
	if (need_a_node)
	{
		if (options.node == -1)
		{
			log_err(_("Node information is missing. "
					  "Check the configuration file.\n"));
			exit(ERR_BAD_CONFIG);
		}
	}


	/*
	 * If `use_replication_slots` set in the configuration file
	 * and command line parameter `--wal-keep-segments` was used,
	 * emit a warning as to the latter's redundancy. Note that
	 * the version check for 9.4 or later will occur afterwards.
	 */

	if(options.use_replication_slots && wal_keep_segments_used)
	{
		log_warning(_("-w/--wal-keep-segments has no effect when replication slots in use\n"));
	}

	/* Initialise the repmgr schema name */
	maxlen_snprintf(repmgr_schema, "%s%s", DEFAULT_REPMGR_SCHEMA_PREFIX,
			 options.cluster_name);

	/* Initialise slot name, if required (9.4 and later) */
	if(options.use_replication_slots)
	{
		maxlen_snprintf(repmgr_slot_name, "repmgr_slot_%i", options.node);
	}

	switch (action)
	{
		case MASTER_REGISTER:
			do_master_register();
			break;
		case STANDBY_REGISTER:
			do_standby_register();
			break;
		case STANDBY_CLONE:
			do_standby_clone();
			break;
		case STANDBY_PROMOTE:
			do_standby_promote();
			break;
		case STANDBY_FOLLOW:
			do_standby_follow();
			break;
		case WITNESS_CREATE:
			do_witness_create();
			break;
		case CLUSTER_SHOW:
			do_cluster_show();
			break;
		case CLUSTER_CLEANUP:
			do_cluster_cleanup();
			break;
		default:
			usage();
			exit(ERR_BAD_CONFIG);
	}
	logger_shutdown();

	return 0;
}

static void
do_cluster_show(void)
{
	PGconn	   *conn;
	PGresult   *res;
	char		sqlquery[QUERY_STR_LEN];
	char		node_role[MAXLEN];
	int			i;

	/* We need to connect to check configuration */
	log_info(_("%s connecting to database\n"), progname);
	conn = establish_db_connection(options.conninfo, true);

	sqlquery_snprintf(sqlquery,
					  "SELECT conninfo, type "
					  "  FROM %s.repl_nodes ",
					  get_repmgr_schema_quoted(conn));
	res = PQexec(conn, sqlquery);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err(_("Can't get nodes information, have you registered them?\n%s\n"),
				PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}
	PQfinish(conn);

	printf("Role      | Connection String \n");
	for (i = 0; i < PQntuples(res); i++)
	{
		// ZZZ witness
		conn = establish_db_connection(PQgetvalue(res, i, 0), false);
		if (PQstatus(conn) != CONNECTION_OK)
			strcpy(node_role, "  FAILED");
		else if (strcmp(PQgetvalue(res, i, 1), "witness") == 0)
			strcpy(node_role, "  witness");
		else if (is_standby(conn))
			strcpy(node_role, "  standby");
		else
			strcpy(node_role, "* master");

		printf("%-10s", node_role);
		printf("| %s\n", PQgetvalue(res, i, 0));

		PQfinish(conn);
	}

	PQclear(res);
}

static void
do_cluster_cleanup(void)
{
	PGconn	   *conn = NULL;
	PGconn	   *master_conn = NULL;
	PGresult   *res;
	char		sqlquery[QUERY_STR_LEN];

	/* We need to connect to check configuration */
	log_info(_("%s connecting to database\n"), progname);
	conn = establish_db_connection(options.conninfo, true);

	/* check if there is a master in this cluster */
	log_info(_("%s connecting to master database\n"), progname);
	master_conn = get_master_connection(conn, options.cluster_name,
										NULL, NULL);
	if (!master_conn)
	{
		log_err(_("cluster cleanup: cannot connect to master\n"));
		PQfinish(conn);
		exit(ERR_DB_CON);
	}
	PQfinish(conn);

	if (runtime_options.keep_history > 0)
	{
		sqlquery_snprintf(sqlquery,
						  "DELETE FROM %s.repl_monitor "
						  " WHERE age(now(), last_monitor_time) >= '%d days'::interval ",
						  get_repmgr_schema_quoted(master_conn),
						  runtime_options.keep_history);
	}
	else
	{
		sqlquery_snprintf(sqlquery,
						  "TRUNCATE TABLE %s.repl_monitor",
						  get_repmgr_schema_quoted(master_conn));
	}
	res = PQexec(master_conn, sqlquery);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_err(_("cluster cleanup: Couldn't clean history\n%s\n"),
				PQerrorMessage(master_conn));
		PQclear(res);
		PQfinish(master_conn);
		exit(ERR_BAD_CONFIG);
	}
	PQclear(res);

	/*
	 * Let's VACUUM the table to avoid autovacuum to be launched in an
	 * unexpected hour
	 */
	sqlquery_snprintf(sqlquery, "VACUUM %s.repl_monitor", get_repmgr_schema_quoted(master_conn));
	res = PQexec(master_conn, sqlquery);

	/* XXX There is any need to check this VACUUM happens without problems? */

	PQclear(res);
	PQfinish(master_conn);
}


static void
do_master_register(void)
{
	PGconn	   *conn;
	PGresult   *res;
	char		sqlquery[QUERY_STR_LEN];

	bool		schema_exists = false;
	int			ret;

	bool		node_record_created;

	conn = establish_db_connection(options.conninfo, true);

	/* Verify that master is a supported server version */
	log_info(_("%s connecting to master database\n"), progname);
	check_server_version(conn, "master", true, NULL);

	/* Check we are a master */
	log_info(_("%s connected to master, checking its state\n"), progname);
	ret = is_standby(conn);

	if (ret)
	{
		log_err(_(ret == 1 ? "Trying to register a standby node as a master\n" :
				  "Connection to node lost!\n"));

		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Check if there is a schema for this cluster */
	schema_exists = check_cluster_schema(conn);

	/* If schema exists and force option not selected, raise an error */
	if(schema_exists && !runtime_options.force)
	{
		log_notice(_("Schema '%s' already exists.\n"), get_repmgr_schema());
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	if(!schema_exists)
	{
		log_info(_("master register: creating database objects inside the %s schema\n"),
				 get_repmgr_schema());

		/* ok, create the schema */
		if (!create_schema(conn))
			return;
	}
	else
	{
		PGconn	   *master_conn;

		if (runtime_options.force)
		{
			sqlquery_snprintf(sqlquery,
							  "DELETE FROM %s.repl_nodes "
							  " WHERE id = %d ",
							  get_repmgr_schema_quoted(conn),
							  options.node);
			log_debug(_("master register: %s\n"), sqlquery);

			res = PQexec(conn, sqlquery);
			if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				log_warning(_("Cannot delete node details, %s\n"),
							PQerrorMessage(conn));
				PQfinish(conn);
				exit(ERR_BAD_CONFIG);
			}
			PQclear(res);
		}

		/* Ensure there isn't any other master already registered */
		master_conn = get_master_connection(conn,
											options.cluster_name, NULL, NULL);
		if (master_conn != NULL)
		{
			PQfinish(master_conn);
			log_warning(_("There is a master already in cluster %s\n"),
						options.cluster_name);
			exit(ERR_BAD_CONFIG);
		}
	}

	/* Now register the master */
	node_record_created = create_node_record(conn,
											 "master register",
											 options.node,
											 "primary",
											 NO_UPSTREAM_NODE,
											 options.cluster_name,
											 options.node_name,
											 options.conninfo,
											 options.priority);

	PQfinish(conn);

	if(node_record_created == false)
		exit(ERR_DB_QUERY);

	log_notice(_("Master node correctly registered for cluster %s with id %d (conninfo: %s)\n"),
			   options.cluster_name, options.node, options.conninfo);
	return;
}


static void
do_standby_register(void)
{
	PGconn	   *conn;
	PGconn	   *master_conn;
	int			ret;

	PGresult   *res;
	char		sqlquery[QUERY_STR_LEN];

	char		master_version[MAXVERSIONSTR];
	int			master_version_num = 0;

	char		standby_version[MAXVERSIONSTR];
	int			standby_version_num = 0;

	bool		node_record_created;

	/* XXX: A lot of copied code from do_master_register! Refactor */

	log_info(_("%s connecting to standby database\n"), progname);
	conn = establish_db_connection(options.conninfo, true);

	/* Verify that standby is a supported server version */
	standby_version_num = check_server_version(conn, "standby", true, standby_version);

	/* Check we are a standby */
	ret = is_standby(conn);
	if (ret == 0 || ret == -1)
	{
		log_err(_(ret == 0 ? "repmgr: This node should be a standby (%s)\n" :
				"repmgr: connection to node (%s) lost\n"), options.conninfo);

		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Check if there is a schema for this cluster */
	if (check_cluster_schema(conn) == false)
	{
		/* schema doesn't exist */
		log_err(_("Schema '%s' doesn't exist.\n"), get_repmgr_schema());
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/* check if there is a master in this cluster */
	log_info(_("%s connecting to master database\n"), progname);
	master_conn = get_master_connection(conn, options.cluster_name,
										NULL, NULL);
	if (!master_conn)
	{
		log_err(_("A master must be defined before configuring a slave\n"));
		exit(ERR_BAD_CONFIG);
	}

	/* Verify that master is a supported server version */
	log_info(_("%s connected to master, checking its state\n"), progname);
	master_version_num = check_server_version(conn, "master", false, master_version);
	if(master_version_num < 0)
	{
		PQfinish(conn);
		PQfinish(master_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* master and standby version should match */
	if ((master_version_num / 100) != (standby_version_num / 100))
	{
		PQfinish(conn);
		PQfinish(master_conn);
		log_err(_("%s needs versions of both master (%s) and standby (%s) to match.\n"),
				progname, master_version, standby_version);
		exit(ERR_BAD_CONFIG);
	}

	/* Now register the standby */
	log_info(_("%s registering the standby\n"), progname);
	if (runtime_options.force)
	{
		sqlquery_snprintf(sqlquery,
						  "DELETE FROM %s.repl_nodes "
						  " WHERE id = %d",
						  get_repmgr_schema_quoted(master_conn),
						  options.node);

		log_debug(_("standby register: %s\n"), sqlquery);

		res = PQexec(master_conn, sqlquery);
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			log_err(_("Cannot delete node details, %s\n"),
					PQerrorMessage(master_conn));
			PQfinish(master_conn);
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}
		PQclear(res);
	}

	node_record_created = create_node_record(master_conn,
											 "standby register",
											 options.node,
											 "standby",
											 options.upstream_node,
											 options.cluster_name,
											 options.node_name,
											 options.conninfo,
											 options.priority);

	PQfinish(master_conn);
	PQfinish(conn);

	if(node_record_created == false)
		exit(ERR_BAD_CONFIG);

	log_info(_("%s registering the standby complete\n"), progname);
	log_notice(_("Standby node correctly registered for cluster %s with id %d (conninfo: %s)\n"),
			   options.cluster_name, options.node, options.conninfo);
	return;
}


static void
do_standby_clone(void)
{
	PGconn	   *primary_conn;
	PGresult   *res;
	char		sqlquery[QUERY_STR_LEN];

	int			server_version_num;

	char		cluster_size[MAXLEN];

	int			r = 0,
				retval = SUCCESS;

	int			i;
	bool		test_mode = false;
	bool		config_file_copy_required = false;

	char		master_data_directory[MAXFILENAME];
	char		local_data_directory[MAXFILENAME];

	char		master_config_file[MAXFILENAME] = "";
	char		local_config_file[MAXFILENAME] = "";

	char		master_hba_file[MAXFILENAME] = "";
	char		local_hba_file[MAXFILENAME] = "";

	char		master_ident_file[MAXFILENAME] = "";
	char		local_ident_file[MAXFILENAME] = "";

	TablespaceListCell *cell;
	/*
	 * if dest_dir has been provided, we copy everything in the same path if
	 * dest_dir is set and the master have tablespace, repmgr will stop
	 * because it is more complex to remap the path for the tablespaces and it
	 * does not look useful at the moment
	 *
	 * XXX test_mode a bit of a misnomer
	 */
	if (runtime_options.dest_dir[0])
	{
		test_mode = true;
		log_notice(_("%s Destination directory %s provided, try to clone everything in it.\n"),
				   progname, runtime_options.dest_dir);
	}

	/* Connection parameters for master only */
	keywords[0] = "host";
	values[0] = runtime_options.host;
	keywords[1] = "port";
	values[1] = runtime_options.masterport;

	/* We need to connect to check configuration and start a backup */
	log_info(_("%s connecting to master database\n"), progname);
	primary_conn = establish_db_connection_by_params(keywords, values, true);

	/* Verify that master is a supported server version */
	log_info(_("%s connected to master, checking its state\n"), progname);
	server_version_num = check_server_version(primary_conn, "master", true, NULL);

	check_upstream_config(primary_conn, server_version_num, true);

	/*
	 * Check that tablespaces named in any `tablespace_mapping` configuration
	 * file parameters exist.
	 *
	 * pg_basebackup doesn't verify mappings, so any errors will not be caught.
	 * We'll do that here as a value-added service.
	 *
	 * XXX -T/--tablespace-mapping not available for PostgreSQL 9.3 -
	 * emit warning or fail
	 */

	if(options.tablespace_dirs.head != NULL)
	{
		if(get_server_version(primary_conn, NULL) < 90400)
		{
			log_err(_("Configuration option `tablespace_mapping` requires PostgreSQL 9.4 or later\n"));
			PQfinish(primary_conn);
			exit(ERR_BAD_CONFIG);
		}

		for (cell = options.tablespace_dirs.head; cell; cell = cell->next)
		{

			sqlquery_snprintf(sqlquery,
							  "SELECT spcname "
							  "  FROM pg_tablespace "
							  "WHERE pg_tablespace_location(oid) = '%s'",
							  cell->old_dir);
			res = PQexec(primary_conn, sqlquery);
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				log_err(_("Unable to execute tablespace query: %s\n"), PQerrorMessage(primary_conn));
				PQclear(res);
				PQfinish(primary_conn);
				exit(ERR_BAD_CONFIG);
			}

			if (PQntuples(res) == 0)
			{
				log_err(_("No tablespace matching path '%s' found\n"), cell->old_dir);
				PQclear(res);
				PQfinish(primary_conn);
				exit(ERR_BAD_CONFIG);
			}
		}
	}

	if(get_cluster_size(primary_conn, cluster_size) == false)
		exit(ERR_DB_QUERY);

	log_info(_("Successfully primary_connected to master. Current installation size is %s\n"),
			 cluster_size);

	/*
	 * Obtain data directory and configuration file locations
	 * We'll check to see whether the configuration files are in the data
	 * directory - if not we'll have to copy them via SSH
	 *
	 * XXX: if configuration files are symlinks to targets outside the data
	 * directory, they won't be copied by pg_basebackup, but we can't tell
	 * this from the below query; we'll probably need to add a check for their
	 * presence and if missing force copy by SSH
	 */
	sqlquery_snprintf(sqlquery,
					  "  WITH dd AS ( "
					  "    SELECT setting "
					  "      FROM pg_settings "
					  "     WHERE name = 'data_directory' "
					  "  ) "
					  "    SELECT ps.name, ps.setting, "
					  "           ps.setting ~ ('^' || dd.setting) AS in_data_dir "
					  "      FROM dd, pg_settings ps "
					  "     WHERE ps.name IN ('data_directory', 'config_file', 'hba_file', 'ident_file') "
					  "  ORDER BY 1 "
		);
	log_debug(_("standby clone: %s\n"), sqlquery);
	res = PQexec(primary_conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err(_("Can't get info about data directory and configuration files: %s\n"),
				PQerrorMessage(primary_conn));
		PQclear(res);
		PQfinish(primary_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* We need all 4 parameters, and they can be retrieved only by superusers */
	if (PQntuples(res) != 4)
	{
		log_err("%s: STANDBY CLONE should be run by a SUPERUSER\n", progname);
		PQclear(res);
		PQfinish(primary_conn);
		exit(ERR_BAD_CONFIG);
	}

	log_debug(_("standby clone: %i tuples\n"), PQntuples(res));
	for (i = 0; i < PQntuples(res); i++)
	{
		if (strcmp(PQgetvalue(res, i, 0), "data_directory") == 0)
		{
			strncpy(master_data_directory, PQgetvalue(res, i, 1), MAXFILENAME);
		}
		else if (strcmp(PQgetvalue(res, i, 0), "config_file") == 0)
		{
			if(strcmp(PQgetvalue(res, i, 2), "f") == 0)
			{
				config_file_copy_required = true;
				strncpy(master_config_file, PQgetvalue(res, i, 1), MAXFILENAME);
			}
		}
		else if (strcmp(PQgetvalue(res, i, 0), "hba_file") == 0)
		{
			if(strcmp(PQgetvalue(res, i, 2), "f") == 0)
			{
				config_file_copy_required = true;
				strncpy(master_hba_file, PQgetvalue(res, i, 1), MAXFILENAME);
			}
		}
		else if (strcmp(PQgetvalue(res, i, 0), "ident_file") == 0)
		{
			if(strcmp(PQgetvalue(res, i, 2), "f") == 0)
			{
				config_file_copy_required = true;
				strncpy(master_ident_file, PQgetvalue(res, i, 1), MAXFILENAME);
			}
		}
		else
			log_warning(_("unknown parameter: %s\n"), PQgetvalue(res, i, 0));
	}
	PQclear(res);

	if (test_mode)
	{
		strncpy(local_data_directory, runtime_options.dest_dir, MAXFILENAME);
		strncpy(local_config_file, runtime_options.dest_dir, MAXFILENAME);
		strncpy(local_hba_file, runtime_options.dest_dir, MAXFILENAME);
		strncpy(local_ident_file, runtime_options.dest_dir, MAXFILENAME);
	}
	else
	{
		strncpy(local_data_directory, master_data_directory, MAXFILENAME);
		strncpy(local_config_file, master_config_file, MAXFILENAME);
		strncpy(local_hba_file, master_hba_file, MAXFILENAME);
		strncpy(local_ident_file, master_ident_file, MAXFILENAME);
	}

	log_notice(_("Starting backup...\n"));


	/* Check the directory could be used as a PGDATA dir */

	/* ZZZ maybe check tablespace, xlog dirs too */
	if (!create_pg_dir(local_data_directory, runtime_options.force))
	{
		log_err(_("%s: couldn't use directory %s ...\nUse --force option to force\n"),
				progname, local_data_directory);
		r = ERR_BAD_CONFIG;
		retval = ERR_BAD_CONFIG;
		goto stop_backup;
	}


	r = run_basebackup();
	if (r != 0)
	{
		log_warning(_("standby clone: base backup failed\n"));
		retval = ERR_BAD_BASEBACKUP;
		goto stop_backup;
	}

	/*
	 * If configuration files were not in the data directory, we need to copy
	 * them via SSH
	 *
	 * TODO: add option to place these files in the same location on the
	 * standby server?
	 */

	if(config_file_copy_required == true)
	{
		log_notice(_("Copying configuration files from master\n"));
		r = test_ssh_connection(runtime_options.host, runtime_options.remote_user);
		if (r != 0)
		{
			log_err(_("%s: Aborting, remote host %s is not reachable.\n"),
					progname, runtime_options.host);
			retval = ERR_BAD_SSH;
			goto stop_backup;
		}

		if(strlen(master_config_file))
		{
			log_info(_("standby clone: master config file '%s'\n"), master_config_file);
			r = copy_remote_files(runtime_options.host, runtime_options.remote_user,
								  master_config_file, local_config_file);
			if (r != 0)
			{
				log_warning(_("standby clone: failed copying master config file '%s'\n"),
							master_config_file);
				retval = ERR_BAD_SSH;
				goto stop_backup;
			}
		}

		if(strlen(master_hba_file))
		{
			log_info(_("standby clone: master hba file '%s'\n"), master_hba_file);
			r = copy_remote_files(runtime_options.host, runtime_options.remote_user,
								  master_hba_file, local_hba_file);
			if (r != 0)
			{
				log_warning(_("standby clone: failed copying master hba file '%s'\n"),
							master_hba_file);
				retval = ERR_BAD_SSH;
				goto stop_backup;
			}
		}

		if(strlen(master_ident_file))
		{
			log_info(_("standby clone: master ident file '%s'\n"), master_ident_file);
			r = copy_remote_files(runtime_options.host, runtime_options.remote_user,
								  master_ident_file, local_ident_file);
			if (r != 0)
			{
				log_warning(_("standby clone: failed copying master ident file '%s'\n"),
							master_ident_file);
				retval = ERR_BAD_SSH;
				goto stop_backup;
			}
		}
	}

stop_backup:

	/* If the backup failed then exit */
	if (r != 0)
	{
		log_err(_("Unable to take a base backup of the master server\n"));
		log_warning(_("The destination directory (%s) will need to be cleaned up manually\n"),
				local_data_directory);
		PQfinish(primary_conn);
		exit(retval);
	}

	/* Finally, write the recovery.conf file */
	create_recovery_file(local_data_directory);

	/*
	 * If replication slots requested, create appropriate slot on the primary;
	 * create_recovery_file() will already have written `primary_slot_name` into
	 * `recovery.conf`
	 */
	if(options.use_replication_slots)
	{
		if(create_replication_slot(primary_conn, repmgr_slot_name) == false)
		{
			PQfinish(primary_conn);
			exit(ERR_DB_QUERY);
		}
	}

	log_notice(_("%s base backup of standby complete\n"), progname);

	/*
	 * XXX It might be nice to provide the following options:
	 * - have repmgr start the daemon automatically
	 * - provide a custom pg_ctl command
	 */

	log_notice("HINT: You can now start your postgresql server\n");
	if (test_mode)
	{
		log_notice(_("for example : pg_ctl -D %s start\n"),
				   local_data_directory);
	}
	else
	{
		log_notice("for example : /etc/init.d/postgresql start\n");
	}

	PQfinish(primary_conn);
	exit(retval);
}


static void
do_standby_promote(void)
{
	PGconn	   *conn;

	char		script[MAXLEN];

	PGconn	   *old_master_conn;

	int			r,
				retval;
	char		data_dir[MAXLEN];

	int			i,
				promote_check_timeout  = 60,
				promote_check_interval = 2;
	bool		promote_sucess = false;
	bool        success;

	/* We need to connect to check configuration */
	log_info(_("%s connecting to standby database\n"), progname);
	conn = establish_db_connection(options.conninfo, true);

	/* Verify that standby is a supported server version */
	log_info(_("%s connected to standby, checking its state\n"), progname);

	check_server_version(conn, "standby", true, NULL);

	/* Check we are in a standby node */
	retval = is_standby(conn);
	if (retval == 0 || retval == -1)
	{
		log_err(_(retval == 0 ? "%s: The command should be executed on a standby node\n" :
				  "%s: connection to node lost!\n"), progname);

		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/* we also need to check if there isn't any master already */
	old_master_conn = get_master_connection(conn,
											options.cluster_name, NULL, NULL);
	if (old_master_conn != NULL)
	{
		log_err(_("This cluster already has an active master server\n"));
		PQfinish(old_master_conn);
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	log_notice(_("%s: Promoting standby\n"), progname);

	/* Get the data directory */
	success = get_pg_setting(conn, "data_directory", data_dir);
	PQfinish(conn);

	if (success == false)
	{
		log_err(_("Unable to determine data directory\n"));
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * Promote standby to master.
	 *
	 * `pg_ctl promote` returns immediately and has no -w option, so we
	 * can't be sure when or if the promotion completes.
	 * For now we'll poll the server until the default timeout (60 seconds)
	 */
	maxlen_snprintf(script, "%s -D %s promote",
					make_pg_path("pg_ctl"), data_dir);
	log_notice(_("%s: promoting server using '%s'\n"), progname,
			   script);

	r = system(script);
	if (r != 0)
	{
		log_err(_("Unable to promote server from standby to master\n"));
		exit(ERR_NO_RESTART);
	}

	/* reconnect to check we got promoted */

	log_info(_("%s reconnecting to promoted server\n"), progname);
	conn = establish_db_connection(options.conninfo, true);

	for(i = 0; i < promote_check_timeout; i += promote_check_interval)
	{
		retval = is_standby(conn);
		if(!retval)
		{
			promote_sucess = true;
			break;
		}
		sleep(promote_check_interval);
	}

	if (promote_sucess == false)
	{
		/* XXX exit with error? */
		log_err(_(retval == 1 ?
			  "%s: STANDBY PROMOTE failed, this is still a standby node.\n" :
				  "%s: connection to node lost!\n"), progname);
	}
	else
	{
		log_notice(_("%s: STANDBY PROMOTE successful.  You should REINDEX any hash indexes you have.\n"),
				progname);
	}
	PQfinish(conn);
	return;
}


static void
do_standby_follow(void)
{
	PGconn	   *conn;

	char		script[MAXLEN];
	char		master_conninfo[MAXLEN];
	PGconn	   *master_conn;

	int			r,
				retval;
	char		data_dir[MAXLEN];

	char		master_version[MAXVERSIONSTR];
	int			master_version_num = 0;

	char		standby_version[MAXVERSIONSTR];
	int			standby_version_num = 0;

	bool        success;


	/* We need to connect to check configuration */
	log_info(_("%s connecting to standby database\n"), progname);
	conn = establish_db_connection(options.conninfo, true);
	log_info(_("%s connected to standby, checking its state\n"), progname);

	/* Verify that standby is a supported server version */
	standby_version_num = check_server_version(conn, "standby", true, standby_version);

	/* Check we are in a standby node */
	retval = is_standby(conn);
	if (retval == 0 || retval == -1)
	{
		log_err(_(retval == 0 ? "%s: The command should be executed in a standby node\n" :
				  "%s: connection to node lost!\n"), progname);

		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * we also need to check if there is any master in the cluster or wait for
	 * one to appear if we have set the wait option
	 */
	log_info(_("%s discovering new master...\n"), progname);

	do
	{
		if (!is_pgup(conn, options.master_response_timeout))
		{
			conn = establish_db_connection(options.conninfo, true);
		}

		master_conn = get_master_connection(conn,
				options.cluster_name, NULL, (char *) &master_conninfo);
	}
	while (master_conn == NULL && runtime_options.wait_for_master);

	if (master_conn == NULL)
	{
		log_err(_("There isn't a master to follow in this cluster\n"));
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Check we are going to point to a master */
	retval = is_standby(master_conn);
	if (retval)
	{
		log_err(_(retval == 1 ? "%s: The node to follow should be a master\n" :
				  "%s: connection to node lost!\n"), progname);

		PQfinish(conn);
		PQfinish(master_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Verify that master is a supported server version */
	log_info(_("%s connected to master, checking its state\n"), progname);
	master_version_num = check_server_version(conn, "master", false, master_version);
	if(master_version_num < 0)
	{
		PQfinish(conn);
		PQfinish(master_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* master and standby version should match */
	if ((master_version_num / 100) != (standby_version_num / 100))
	{
		PQfinish(conn);
		PQfinish(master_conn);
		log_err(_("%s needs versions of both master (%s) and standby (%s) to match.\n"),
				progname, master_version, standby_version);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * set the host and masterport variables with the master ones before
	 * closing the connection because we will need them to recreate the
	 * recovery.conf file
	 */
	strncpy(runtime_options.host, PQhost(master_conn), MAXLEN);
	strncpy(runtime_options.masterport, PQport(master_conn), MAXLEN);
	strncpy(runtime_options.username, PQuser(master_conn), MAXLEN);
	PQfinish(master_conn);

	log_info(_("%s Changing standby's master\n"), progname);

	/* Get the data directory full path */
	success = get_pg_setting(conn, "data_directory", data_dir);
	PQfinish(conn);

	if (success == false)
	{
		log_err(_("Unable to determine data directory\n"));
		exit(ERR_BAD_CONFIG);
	}

	/* write the recovery.conf file */
	if (!create_recovery_file(data_dir))
		exit(ERR_BAD_CONFIG);

	/* Finally, restart the service */
	maxlen_snprintf(script, "%s %s -w -D %s -m fast restart",
					make_pg_path("pg_ctl"), options.pgctl_options, data_dir);

	log_notice(_("%s: restarting server using '%s'\n"), progname,
			   script);

	r = system(script);
	if (r != 0)
	{
		log_err(_("Can't restart server\n"));
		exit(ERR_NO_RESTART);
	}

	return;
}


static void
do_witness_create(void)
{
	PGconn	   *masterconn;
	PGconn	   *witnessconn;
	PGresult   *res;
	char		sqlquery[QUERY_STR_LEN];

	char		script[MAXLEN];
	char		buf[MAXLEN];
	FILE	   *pg_conf = NULL;

	int			r = 0,
				retval;

	char		master_hba_file[MAXLEN];
	bool        success;
	bool		node_record_created;

	/* Connection parameters for master only */
	keywords[0] = "host";
	values[0] = runtime_options.host;
	keywords[1] = "port";
	values[1] = runtime_options.masterport;

	/* We need to connect to check configuration and copy it */
	masterconn = establish_db_connection_by_params(keywords, values, true);
	if (!masterconn)
	{
		log_err(_("%s: could not connect to master\n"), progname);
		exit(ERR_DB_CON);
	}

	/* Verify that master is a supported server version */
	check_server_version(masterconn, "master", true, NULL);

	/* Check we are connecting to a primary node */
	retval = is_standby(masterconn);
	if (retval)
	{
		log_err(_(retval == 1 ?
				  "The command should not run on a standby node\n" :
				  "Connection to node lost!\n"));

		PQfinish(masterconn);
		exit(ERR_BAD_CONFIG);
	}

	log_info(_("Successfully connected to master.\n"));

	r = test_ssh_connection(runtime_options.host, runtime_options.remote_user);
	if (r != 0)
	{
		log_err(_("%s: Aborting, remote host %s is not reachable.\n"),
				progname, runtime_options.host);
		PQfinish(masterconn);
		exit(ERR_BAD_SSH);
	}

	/* Check this directory could be used as a PGDATA dir */
	if (!create_pg_dir(runtime_options.dest_dir, runtime_options.force))
	{
		log_err(_("witness create: couldn't create data directory (\"%s\") for witness"),
				runtime_options.dest_dir);
		exit(ERR_BAD_CONFIG);
	}


	/*
	 * To create a witness server we need to: 1) initialize the cluster 2)
	 * register the witness in repl_nodes 3) copy configuration from master
	 */

	/* Create the cluster for witness */
	if (!runtime_options.superuser[0])
		strncpy(runtime_options.superuser, "postgres", MAXLEN);

	sprintf(script, "%s %s -D %s init -o \"%s-U %s\"",
			make_pg_path("pg_ctl"),
			options.pgctl_options, runtime_options.dest_dir,
			runtime_options.initdb_no_pwprompt ? "" : "-W ",
			runtime_options.superuser);
	log_info("Initialize cluster for witness: %s.\n", script);

	r = system(script);
	if (r != 0)
	{
		log_err("Can't initialize cluster for witness server\n");
		PQfinish(masterconn);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * default port for the witness is 5499, but user can provide a different
	 * one
	 */
	xsnprintf(buf, sizeof(buf), "%s/postgresql.conf", runtime_options.dest_dir);
	pg_conf = fopen(buf, "a");
	if (pg_conf == NULL)
	{
		log_err(_("%s: could not open \"%s\" for adding extra config: %s\n"),
				progname, buf, strerror(errno));
		PQfinish(masterconn);
		exit(ERR_BAD_CONFIG);
	}

	xsnprintf(buf, sizeof(buf), "\n#Configuration added by %s\n", progname);
	fputs(buf, pg_conf);

	if (!runtime_options.localport[0])
		strncpy(runtime_options.localport, "5499", MAXLEN);
	xsnprintf(buf, sizeof(buf), "port = %s\n", runtime_options.localport);
	fputs(buf, pg_conf);

	xsnprintf(buf, sizeof(buf), "shared_preload_libraries = 'repmgr_funcs'\n");
	fputs(buf, pg_conf);

	xsnprintf(buf, sizeof(buf), "listen_addresses = '*'\n");
	fputs(buf, pg_conf);

	fclose(pg_conf);


	/* start new instance */
	sprintf(script, "%s %s -w -D %s start",
			make_pg_path("pg_ctl"),
			options.pgctl_options, runtime_options.dest_dir);
	log_info(_("Start cluster for witness: %s"), script);
	r = system(script);
	if (r != 0)
	{
		log_err(_("Can't start cluster for witness server\n"));
		PQfinish(masterconn);
		exit(ERR_BAD_CONFIG);
	}

	/* check if we need to create a user */
	if (runtime_options.username[0] && runtime_options.localport[0] && strcmp(runtime_options.username,"postgres")!=0 )
        {
		/* create required user needs to be superuser to create untrusted language function in c */
		sprintf(script, "%s -p %s --superuser --login -U %s %s",
				make_pg_path("createuser"),
				runtime_options.localport, runtime_options.superuser, runtime_options.username);
		log_info("Create user for witness db: %s.\n", script);

		r = system(script);
		if (r != 0)
		{
			log_err("Can't create user for witness server\n");
			PQfinish(masterconn);
			exit(ERR_BAD_CONFIG);
		}
	}

	/* check if we need to create a database */
	if(runtime_options.dbname[0] && strcmp(runtime_options.dbname,"postgres")!=0 && runtime_options.localport[0])
	{
		/* create required db */
		sprintf(script, "%s -p %s -U %s --owner=%s %s",
				make_pg_path("createdb"),
				runtime_options.localport, runtime_options.superuser, runtime_options.username, runtime_options.dbname);
		log_info("Create database for witness db: %s.\n", script);

		r = system(script);
		if (r != 0)
		{
			log_err("Can't create database for witness server\n");
			PQfinish(masterconn);
			exit(ERR_BAD_CONFIG);
		}
	}

	/* Get the pg_hba.conf full path */
	success = get_pg_setting(masterconn, "hba_file", master_hba_file);

	if (success == false)
	{
		log_err(_("Can't get info about pg_hba.conf\n"));
		exit(ERR_DB_QUERY);
	}

	r = copy_remote_files(runtime_options.host, runtime_options.remote_user,
						  master_hba_file, runtime_options.dest_dir);
	if (r != 0)
	{
		log_err(_("Can't rsync the pg_hba.conf file from master\n"));
		PQfinish(masterconn);
		exit(ERR_BAD_CONFIG);
	}

	/* reload to adapt for changed pg_hba.conf */
	sprintf(script, "%s %s -w -D %s reload",
			make_pg_path("pg_ctl"),
			options.pgctl_options, runtime_options.dest_dir);
	log_info(_("Reload cluster config for witness: %s"), script);
	r = system(script);
	if (r != 0)
	{
		log_err(_("Can't reload cluster for witness server\n"));
		PQfinish(masterconn);
		exit(ERR_BAD_CONFIG);
	}

	/* register ourselves in the master */

	node_record_created = create_node_record(masterconn,
											 "witness create",
											 options.node,
											 "witness",
											 NO_UPSTREAM_NODE,
											 options.cluster_name,
											 options.node_name,
											 options.conninfo,
											 options.priority);

	if(node_record_created == false)
	{
		PQfinish(masterconn);
		exit(ERR_DB_QUERY);
	}

	/* establish a connection to the witness, and create the schema */
	witnessconn = establish_db_connection(options.conninfo, true);

	log_info(_("Starting copy of configuration from master...\n"));

	if (!create_schema(witnessconn))
	{
		PQfinish(masterconn);
		PQfinish(witnessconn);
		exit(ERR_BAD_CONFIG);
	}

	/* copy configuration from master, only repl_nodes is needed */
	if (!copy_configuration(masterconn, witnessconn))
	{
		PQfinish(masterconn);
		PQfinish(witnessconn);
		exit(ERR_BAD_CONFIG);
	}

	/* drop superuser powers if needed */
	if (runtime_options.username[0] && runtime_options.localport[0] && strcmp(runtime_options.username,"postgres")!=0 )
	{
		sqlquery_snprintf(sqlquery, "ALTER ROLE %s NOSUPERUSER", runtime_options.username);
		log_info("Drop superuser powers on user for witness db: %s.\n", sqlquery);

		log_debug(_("witness create: %s\n"), sqlquery);
		res = PQexec(witnessconn, sqlquery);
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			log_err(_("Cannot alter user privileges, %s\n"),
					PQerrorMessage(witnessconn));
			PQfinish(masterconn);
			PQfinish(witnessconn);
			exit(ERR_DB_QUERY);
		}
	}
	PQfinish(masterconn);
	PQfinish(witnessconn);

	log_notice(_("Configuration has been successfully copied to the witness\n"));
}



static void
usage(void)
{
	fprintf(stderr, _("\n\n%s: Replicator manager \n"), progname);
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
}



static void
help(const char *progname)
{
	printf(_("\n%s: Replicator manager \n"), progname);
	printf(_("Usage:\n"));
	printf(_(" %s [OPTIONS] master  {register}\n"), progname);
	printf(_(" %s [OPTIONS] standby {register|clone|promote|follow}\n"),
		   progname);
	printf(_(" %s [OPTIONS] cluster {show|cleanup}\n"), progname);
	printf(_("\nGeneral options:\n"));
	printf(_("  --help                              show this help, then exit\n"));
	printf(_("  --version                           output version information, then exit\n"));
	printf(_("  --verbose                           output verbose activity information\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=DBNAME                 database to connect to\n"));
	printf(_("  -h, --host=HOSTNAME                 database server host or socket directory\n"));
	printf(_("  -p, --port=PORT                     database server port\n"));
	printf(_("  -U, --username=USERNAME             database user name to connect as\n"));
	printf(_("\nConfiguration options:\n"));
	printf(_("  -b. --pg_bindir=PATH                path to PostgreSQL binaries (optional)\n"));
	printf(_("  -D, --data-dir=DIR                  local directory where the files will be\n" \
			 "                                      copied to\n"));
	printf(_("  -l, --local-port=PORT               standby or witness server local port\n"));
	printf(_("  -f, --config-file=PATH              path to the configuration file\n"));
	printf(_("  -R, --remote-user=USERNAME          database server username for rsync\n"));
	printf(_("  -S, --superuser=USERNAME            superuser username for witness database\n" \
			 "                                      (default: postgres)\n"));
	printf(_("  -w, --wal-keep-segments=VALUE       minimum value for the GUC\n" \
			 "                                      wal_keep_segments (default: %s)\n"), DEFAULT_WAL_KEEP_SEGMENTS);
	printf(_("  -k, --keep-history=VALUE            keeps indicated number of days of\n" \
			 "                                      history\n"));
	printf(_("  -F, --force                         force potentially dangerous operations\n" \
			 "                                      to happen\n"));
	printf(_("  -W, --wait                          wait for a master to appear\n"));
	printf(_("  -r, --min-recovery-apply-delay=VALUE  enable recovery time delay, value has to be a valid time atom (e.g. 5min)\n"));
	printf(_("  --initdb-no-pwprompt                don't require superuser password when running initdb\n"));
	printf(_("  --check-upstream-config             verify upstream server configuration\n"));
	printf(_("\n%s performs some tasks like clone a node, promote it or making follow\n"), progname);
	printf(_("another node and then exits.\n\n"));
	printf(_("COMMANDS:\n"));
	printf(_(" master register         - registers the master in a cluster\n"));
	printf(_(" standby register        - registers a standby in a cluster\n"));
	printf(_(" standby clone [node]    - allows creation of a new standby\n"));
	printf(_(" standby promote         - allows manual promotion of a specific standby into\n" \
	"                           a new master in the event of a failover\n"));
	printf(_(" standby follow          - allows the standby to re-point itself to a new\n" \
			 "                           master\n"));
	printf(_(" cluster show            - print node information\n"));
	printf(_(" cluster cleanup         - cleans monitor's history\n"));
}


/*
 * Creates a recovery file for a standby.
 */
static bool
create_recovery_file(const char *data_dir)
{
	FILE	   *recovery_file;
	char		recovery_file_path[MAXLEN];
	char		line[MAXLEN];

	maxlen_snprintf(recovery_file_path, "%s/%s", data_dir, RECOVERY_FILE);

	recovery_file = fopen(recovery_file_path, "w");
	if (recovery_file == NULL)
	{
		log_err(_("Unable to create recovery.conf file at '%s'\n"), recovery_file_path);
		return false;
	}

	/* standby_mode = 'on' */
	maxlen_snprintf(line, "standby_mode = 'on'\n");

	if(write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
		return false;

	/* primary_conninfo = '...' */
	write_primary_conninfo(line);
	if(write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
		return false;

	/* recovery_target_timeline = 'latest' */
	maxlen_snprintf(line, "recovery_target_timeline = 'latest'\n");
	if(write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
		return false;

	/* min_recovery_apply_delay = ... (optional) */
	if(*runtime_options.min_recovery_apply_delay)
	{
		maxlen_snprintf(line, "min_recovery_apply_delay = %s\n",
						runtime_options.min_recovery_apply_delay);
		if(write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
			return false;
	}

	/* primary_slot_name = '...' (optional, for 9.4 and later) */
	if(options.use_replication_slots)
	{
		maxlen_snprintf(line, "primary_slot_name = %s\n",
						repmgr_slot_name);
		if(write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
			return false;
	}

	fclose(recovery_file);

	return true;
}


static bool
write_recovery_file_line(FILE *recovery_file, char *recovery_file_path, char *line)
{
	if (fputs(line, recovery_file) == EOF)
	{
		log_err(_("Unable to write to recovery file at '%s'\n"), recovery_file_path);
		fclose(recovery_file);
		return false;
	}

	return true;
}


static int
test_ssh_connection(char *host, char *remote_user)
{
	char		script[MAXLEN];
	int			r = 1, i;

	/* On some OS, true is located in a different place than in Linux
	 * we have to try them all until all alternatives are gone or we
	 * found `true' because the target OS may differ from the source
	 * OS
	 */
	const char *truebin_paths[] = {
		"/bin/true",
		"/usr/bin/true",
		NULL
	};

	/* Check if we have ssh connectivity to host before trying to rsync */
	for(i = 0; truebin_paths[i] && r != 0; ++i)
	{
		if (!remote_user[0])
			maxlen_snprintf(script, "ssh -o Batchmode=yes %s %s %s",
							options.ssh_options, host, truebin_paths[i]);
		else
			maxlen_snprintf(script, "ssh -o Batchmode=yes %s %s -l %s %s",
							options.ssh_options, host, remote_user,
							truebin_paths[i]);

		log_debug(_("command is: %s\n"), script);
		r = system(script);
	}

	if (r != 0)
		log_info(_("Can not connect to the remote host (%s)\n"), host);
	return r;
}

static int
copy_remote_files(char *host, char *remote_user, char *remote_path,
				  char *local_path)
{
	char		script[MAXLEN];
	char		rsync_flags[MAXLEN];
	char		host_string[MAXLEN];
	int			r;

	if (*options.rsync_options == '\0')
		maxlen_snprintf(
						rsync_flags, "%s",
					 "--archive --checksum --compress --progress --rsh=ssh");
	else
		maxlen_snprintf(rsync_flags, "%s", options.rsync_options);

	if (runtime_options.force)
		strcat(rsync_flags, " --delete");

	if (!remote_user[0])
	{
		maxlen_snprintf(host_string, "%s", host);
	}
	else
	{
		maxlen_snprintf(host_string, "%s@%s", remote_user, host);
	}


	maxlen_snprintf(script, "rsync %s %s:%s %s",
					rsync_flags, host_string, remote_path, local_path);

	log_info(_("rsync command line:  '%s'\n"), script);

	r = system(script);

	if (r != 0)
		log_err(_("Can't rsync from remote file (%s:%s)\n"),
				host_string, remote_path);

	return r;
}


static int
run_basebackup()
{
	char				script[MAXLEN];
	int					r = 0;
	PQExpBufferData 	params;
	TablespaceListCell *cell;

	/* Creare pg_basebackup command line options */

	initPQExpBuffer(&params);

	if(strlen(runtime_options.host))
		appendPQExpBuffer(&params, " -h %s", runtime_options.host);

	if(strlen(runtime_options.masterport))
		appendPQExpBuffer(&params, " -p %s", runtime_options.masterport);

	if(strlen(runtime_options.username))
		appendPQExpBuffer(&params, " -U %s", runtime_options.username);

	if(strlen(runtime_options.dest_dir))
		appendPQExpBuffer(&params, " -D %s", runtime_options.dest_dir);

	if(options.tablespace_dirs.head != NULL)
	{
		for (cell = options.tablespace_dirs.head; cell; cell = cell->next)
		{
			appendPQExpBuffer(&params, " -T %s=%s", cell->old_dir, cell->new_dir);
		}
	}

	maxlen_snprintf(script,
					"%s -l \"repmgr base backup\" %s %s",
					make_pg_path("pg_basebackup"),
					params.data,
					options.pg_basebackup_options
		);

	termPQExpBuffer(&params);

	log_info(_("Executing: '%s'\n"), script);
	r = system(script);

	/*
	 * As of 9.4, pg_basebackup et al only ever return 0 or 1
     */
	log_debug(_("r = %i, %i\n"), r, WEXITSTATUS(r));

	return r;
}


/*
 * Tries to avoid useless or conflicting parameters
 */
static bool
check_parameters_for_action(const int action)
{
	bool		ok = true;

	switch (action)
	{
		case MASTER_REGISTER:

			/*
			 * To register a master we only need the repmgr.conf all other
			 * parameters are at least useless and could be confusing so
			 * reject them
			 */
			if (runtime_options.host[0] || runtime_options.masterport[0] ||
				runtime_options.username[0] || runtime_options.dbname[0])
			{
				log_err(_("You can't use connection parameters to the master when issuing a MASTER REGISTER command.\n"));
				usage();
				ok = false;
			}
			if (runtime_options.dest_dir[0])
			{
				log_err(_("You don't need a destination directory for MASTER REGISTER command\n"));
				usage();
				ok = false;
			}
			break;
		case STANDBY_REGISTER:

			/*
			 * To register a standby we only need the repmgr.conf we don't
			 * need connection parameters to the master because we can detect
			 * the master in repl_nodes
			 */
			if (runtime_options.host[0] || runtime_options.masterport[0] ||
				runtime_options.username[0] || runtime_options.dbname[0])
			{
				log_err(_("You can't use connection parameters to the master when issuing a STANDBY REGISTER command.\n"));
				usage();
				ok = false;
			}
			if (runtime_options.dest_dir[0])
			{
				log_err(_("You don't need a destination directory for STANDBY REGISTER command\n"));
				usage();
				ok = false;
			}
			break;
		case STANDBY_PROMOTE:

			/*
			 * To promote a standby we only need the repmgr.conf we don't want
			 * connection parameters to the master because we will try to
			 * detect the master in repl_nodes if we can't find it then the
			 * promote action will be cancelled
			 */
			if (runtime_options.host[0] || runtime_options.masterport[0] ||
				runtime_options.username[0] || runtime_options.dbname[0])
			{
				log_err(_("You can't use connection parameters to the master when issuing a STANDBY PROMOTE command.\n"));
				usage();
				ok = false;
			}
			if (runtime_options.dest_dir[0])
			{
				log_err(_("You don't need a destination directory for STANDBY PROMOTE command\n"));
				usage();
				ok = false;
			}
			break;
		case STANDBY_FOLLOW:

			/*
			 * To make a standby follow a master we only need the repmgr.conf
			 * we don't want connection parameters to the new master because
			 * we will try to detect the master in repl_nodes if we can't find
			 * it then the follow action will be cancelled
			 */
			if (runtime_options.host[0] || runtime_options.masterport[0] ||
				runtime_options.username[0] || runtime_options.dbname[0])
			{
				log_err(_("You can't use connection parameters to the master when issuing a STANDBY FOLLOW command.\n"));
				usage();
				ok = false;
			}
			if (runtime_options.dest_dir[0])
			{
				log_err(_("You don't need a destination directory for STANDBY FOLLOW command\n"));
				usage();
				ok = false;
			}
			break;
		case STANDBY_CLONE:

			/*
			 * Previous repmgr versions issued a notice here if a configuration
			 * file was provided saying it wasn't needed, which was confusing as
			 * it was needed for the `pg_bindir` parameter.
			 *
			 * In any case it's sensible to read the configuration file if available
			 * for `pg_bindir`, `loglevel` and `use_replication_slots`.
			 */
			if (runtime_options.host == NULL)
			{
				log_notice(_("You need to use connection parameters to "
						"the master when issuing a STANDBY CLONE command."));
				ok = false;
			}
			need_a_node = false;
			break;
		case WITNESS_CREATE:
			/* allow all parameters to be supplied */
			break;
		case CLUSTER_SHOW:
			/* allow all parameters to be supplied */
			break;
		case CLUSTER_CLEANUP:
			/* allow all parameters to be supplied */
			break;
	}

	return ok;
}

static bool
create_schema(PGconn *conn)
{
	char		sqlquery[QUERY_STR_LEN];
	PGresult   *res;

	sqlquery_snprintf(sqlquery, "CREATE SCHEMA %s", get_repmgr_schema_quoted(conn));
	log_debug(_("master register: %s\n"), sqlquery);
	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_err(_("Cannot create the schema %s: %s\n"),
				get_repmgr_schema(), PQerrorMessage(conn));
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}
	PQclear(res);

	/*
	 * to avoid confusion of the time_lag field and provide a consistent UI we
	 * use these functions for providing the latest update timestamp
	 */
	sqlquery_snprintf(sqlquery,
					  "CREATE FUNCTION %s.repmgr_update_last_updated() "
					  "  RETURNS TIMESTAMP WITH TIME ZONE "
					  "  AS '$libdir/repmgr_funcs', 'repmgr_update_last_updated' "
					  "  LANGUAGE C STRICT ",
					  get_repmgr_schema_quoted(conn));

	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "Cannot create the function repmgr_update_last_updated: %s\n",
				PQerrorMessage(conn));
		return false;
	}
	PQclear(res);


	sqlquery_snprintf(sqlquery,
					  "CREATE FUNCTION %s.repmgr_get_last_updated() "
					  "  RETURNS TIMESTAMP WITH TIME ZONE "
					  "  AS '$libdir/repmgr_funcs', 'repmgr_get_last_updated' "
					  "  LANGUAGE C STRICT ",
					  get_repmgr_schema_quoted(conn));

	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "Cannot create the function repmgr_get_last_updated: %s\n",
				PQerrorMessage(conn));
		return false;
	}
	PQclear(res);


	/* ... the tables */
	sqlquery_snprintf(sqlquery,
					  "CREATE TABLE %s.repl_nodes (     "
					  "  id               INTEGER PRIMARY KEY, "
					  "  type             TEXT    NOT NULL CHECK (type IN('primary','standby','witness')), "
					  "  upstream_node_id INTEGER NULL REFERENCES %s.repl_nodes (id), "
					  "  cluster          TEXT    NOT NULL, "
					  "  name             TEXT    NOT NULL, "
					  "  conninfo         TEXT    NOT NULL, "
					  "  slot_name        TEXT    NULL, "
					  "  priority         INTEGER NOT NULL, "
					  "  active           BOOLEAN NOT NULL DEFAULT TRUE )",
					  get_repmgr_schema_quoted(conn),
					  get_repmgr_schema_quoted(conn));

	log_debug(_("master register: %s\n"), sqlquery);
	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_err(_("Cannot create the table %s.repl_nodes: %s\n"),
				get_repmgr_schema_quoted(conn), PQerrorMessage(conn));
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}
	PQclear(res);

	sqlquery_snprintf(sqlquery,
					  "CREATE TABLE %s.repl_monitor ( "
					  "  primary_node                   INTEGER NOT NULL, "
					  "  standby_node                   INTEGER NOT NULL, "
					  "  last_monitor_time              TIMESTAMP WITH TIME ZONE NOT NULL, "
					  "  last_apply_time                TIMESTAMP WITH TIME ZONE, "
					  "  last_wal_primary_location      TEXT NOT NULL,   "
					  "  last_wal_standby_location      TEXT,  "
					  "  replication_lag                BIGINT NOT NULL, "
					  "  apply_lag                      BIGINT NOT NULL) ",
					  get_repmgr_schema_quoted(conn));
	log_debug(_("master register: %s\n"), sqlquery);
	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_err(_("Cannot create the table %s.repl_monitor: %s\n"),
				get_repmgr_schema_quoted(conn), PQerrorMessage(conn));
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}
	PQclear(res);

	/* a view */
	sqlquery_snprintf(sqlquery,
					  "CREATE VIEW %s.repl_status AS "
					  "  SELECT m.primary_node, m.standby_node, n.name AS standby_name, "
					  "         n.type AS node_type, n.active, last_monitor_time, "
					  "         CASE WHEN n.type='standby' THEN m.last_wal_primary_location ELSE NULL END AS last_wal_primary_location, "
					  "         m.last_wal_standby_location, "
					  "         CASE WHEN n.type='standby' THEN pg_size_pretty(m.replication_lag) ELSE NULL END AS replication_lag, "
					  "         CASE WHEN n.type='standby' THEN age(now(), m.last_apply_time) ELSE NULL END AS replication_time_lag, "
					  "         CASE WHEN n.type='standby' THEN pg_size_pretty(m.apply_lag) ELSE NULL END AS apply_lag, "
					  "         age(now(), CASE WHEN pg_is_in_recovery() THEN %s.repmgr_get_last_updated() ELSE m.last_monitor_time END) AS communication_time_lag "
					  "    FROM %s.repl_monitor m "
					  "    JOIN %s.repl_nodes n ON m.standby_node = n.id "
					  "   WHERE (m.standby_node, m.last_monitor_time) IN ( "
					  "                 SELECT m1.standby_node, MAX(m1.last_monitor_time) "
					  "                  FROM %s.repl_monitor m1 GROUP BY 1 "
					  "            )",
					  get_repmgr_schema_quoted(conn),
					  get_repmgr_schema_quoted(conn),
					  get_repmgr_schema_quoted(conn),
					  get_repmgr_schema_quoted(conn),
					  get_repmgr_schema_quoted(conn));
	log_debug(_("master register: %s\n"), sqlquery);

	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_err(_("Cannot create the view %s.repl_status: %s\n"),
				get_repmgr_schema_quoted(conn), PQerrorMessage(conn));
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}
	PQclear(res);

	/* an index to improve performance of the view */
	sqlquery_snprintf(sqlquery,
					  "CREATE INDEX idx_repl_status_sort "
					  "    ON %s.repl_monitor (last_monitor_time, standby_node) ",
					  get_repmgr_schema_quoted(conn));

	log_debug(_("master register: %s\n"), sqlquery);
	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_err(_("Can't index table %s.repl_monitor: %s\n"),
				get_repmgr_schema_quoted(conn), PQerrorMessage(conn));
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}
	PQclear(res);

	/*
	 * XXX Here we MUST try to load the repmgr_function.sql not hardcode it
	 * here
	 */
	sqlquery_snprintf(sqlquery,
					  "CREATE OR REPLACE FUNCTION %s.repmgr_update_standby_location(text) "
					  "  RETURNS boolean "
					  "  AS '$libdir/repmgr_funcs', 'repmgr_update_standby_location' "
					  "  LANGUAGE C STRICT ",
					  get_repmgr_schema_quoted(conn));

	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "Cannot create the function repmgr_update_standby_location: %s\n",
				PQerrorMessage(conn));
		return false;
	}
	PQclear(res);

	sqlquery_snprintf(sqlquery,
					  "CREATE OR REPLACE FUNCTION %s.repmgr_get_last_standby_location() "
					  "  RETURNS text "
					  "  AS '$libdir/repmgr_funcs', 'repmgr_get_last_standby_location' "
					  "  LANGUAGE C STRICT ",
					  get_repmgr_schema_quoted(conn));

	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "Cannot create the function repmgr_get_last_standby_location: %s\n",
				PQerrorMessage(conn));
		return false;
	}
	PQclear(res);

    // ZZZ no longer needed
	sqlquery_snprintf(sqlquery,
					  "CREATE OR REPLACE FUNCTION %s.repmgr_get_primary_conninfo() "
					  "  RETURNS text "
					  "  AS '$libdir/repmgr_funcs', 'repmgr_get_primary_conninfo' "
					  "  LANGUAGE C STRICT ",
					  get_repmgr_schema_quoted(conn));

	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "Cannot create the function repmgr_get_primary_conninfo: %s\n",
				PQerrorMessage(conn));
		return false;
	}
	PQclear(res);
	return true;
}

/*
 * copy_configuration()
 *
 * Copy records in master's `repl_nodes` table to witness database
 */
static bool
copy_configuration(PGconn *masterconn, PGconn *witnessconn)
{
	char		sqlquery[MAXLEN];
	PGresult   *res;
	int			i;

	sqlquery_snprintf(sqlquery, "TRUNCATE TABLE %s.repl_nodes", get_repmgr_schema_quoted(witnessconn));
	log_debug("copy_configuration: %s\n", sqlquery);
	res = PQexec(witnessconn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "Cannot clean node details in the witness, %s\n",
				PQerrorMessage(witnessconn));
		return false;
	}

	sqlquery_snprintf(sqlquery,
					  "SELECT id, type, upstream_node_id, name, conninfo, priority FROM %s.repl_nodes",
					  get_repmgr_schema_quoted(masterconn));
	res = PQexec(masterconn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "Can't get configuration from master: %s\n",
				PQerrorMessage(masterconn));
		PQclear(res);
		return false;
	}
	for (i = 0; i < PQntuples(res); i++)
	{
		bool node_record_created;
		char *witness = PQgetvalue(res, i, 4);

		log_debug(_("copy_configuration(): %s\n"), witness);

		node_record_created = create_node_record(witnessconn,
												 "copy_configuration",
												 atoi(PQgetvalue(res, i, 0)),
												 PQgetvalue(res, i, 1),
												 strlen(PQgetvalue(res, i, 2))
												   ? atoi(PQgetvalue(res, i, 2))
												   : NO_UPSTREAM_NODE,
												 options.cluster_name,
												 PQgetvalue(res, i, 3),
												 PQgetvalue(res, i, 4),
												 atoi(PQgetvalue(res, i, 5)));

		if (node_record_created == false)
		{
			// ZZZ fix error message?
			fprintf(stderr, "Unable to create node record for witness: %s\n",
					PQerrorMessage(witnessconn));
			return false;
		}
	}
	PQclear(res);

	return true;
}

/* This function uses global variables to determine connection settings. Special
 * usage of the PGPASSWORD variable is handled, but strongly discouraged */
static void
write_primary_conninfo(char *line)
{
	char		host_buf[MAXLEN] = "";
	char		conn_buf[MAXLEN] = "";
	char		user_buf[MAXLEN] = "";
	char		appname_buf[MAXLEN] = "";
	char		password_buf[MAXLEN] = "";

	/* Environment variable for password (UGLY, please use .pgpass!) */
	const char *password = getenv("PGPASSWORD");

	if (password != NULL)
	{
		maxlen_snprintf(password_buf, " password=%s", password);
	}
	else if (require_password)
	{
		log_err(_("%s: PGPASSWORD not set, but having one is required\n"),
				progname);
		exit(ERR_BAD_PASSWORD);
	}

	if (runtime_options.host[0])
	{
		maxlen_snprintf(host_buf, " host=%s", runtime_options.host);
	}

	if (runtime_options.username[0])
	{
		maxlen_snprintf(user_buf, " user=%s", runtime_options.username);
	}

	if (options.node_name[0])
	{
		maxlen_snprintf(appname_buf, " application_name=%s", options.node_name);
	}

	maxlen_snprintf(conn_buf, "port=%s%s%s%s%s",
	   (runtime_options.masterport[0]) ? runtime_options.masterport : "5432",
					host_buf, user_buf, password_buf,
					appname_buf);

	maxlen_snprintf(line, "primary_conninfo = '%s'\n", conn_buf);

}


/**
 * check_server_version()
 *
 * Verify that the server is MIN_SUPPORTED_VERSION_NUM or later
 *
 * PGconn *conn:
 *   the connection to check
 *
 * char *server_type:
 *   either "master" or "standby"; used to format error message
 *
 * bool exit_on_error:
 *   exit if reported server version is too low; optional to enable some callers
 *   to perform additional cleanup
 *
 * char *server_version_string
 *   passed to get_server_version(), which will place the human-readble
 *   server version string there (e.g. "9.4.0")
 */
static int
check_server_version(PGconn *conn, char *server_type, bool exit_on_error, char *server_version_string)
{
	int			server_version_num = 0;

	server_version_num = get_server_version(conn, server_version_string);
	if(server_version_num < MIN_SUPPORTED_VERSION_NUM)
	{
		if (server_version_num > 0)
			log_err(_("%s needs %s to be PostgreSQL %s or better\n"),
					progname,
					server_type,
					MIN_SUPPORTED_VERSION
				);

		if(exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		return -1;
	}

	return server_version_num;
}


/*
 * check_upstream_config()
 *
 * Perform sanity check on upstream server configuration
 *
 * TODO: check replication connection is possble
 */

static bool
check_upstream_config(PGconn *conn, int server_version_num, bool exit_on_error)
{
	int			i;
	bool		config_ok = true;

	/* XXX check user is qualified to perform base backup  */

	/* And check if it is well configured */
	i = guc_set(conn, "wal_level", "=", "hot_standby");
	if (i == 0 || i == -1)
	{
		if (i == 0)
			log_err(_("%s needs parameter 'wal_level' to be set to 'hot_standby'\n"),
					progname);
		if(exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		config_ok = false;
	}

	if(options.use_replication_slots)
	{
		/* Does the server support physical replication slots? */
		if(server_version_num < 90400)
		{
			log_err(_("Server version must be 9.4 or later to enable replication slots\n"));

			if(exit_on_error == true)
			{
				PQfinish(conn);
				exit(ERR_BAD_CONFIG);
			}

			config_ok = false;
		}
		/* Server is 9.4 or greater - non-zero `max_replication_slots` required */
		else
		{
			i = guc_set_typed(conn, "max_replication_slots", ">",
							  "1", "integer");
			if (i == 0 || i == -1)
			{
				if (i == 0)
				{
					log_err(_("%s needs parameter 'max_replication_slots' must be set to at least 1 to enable replication slots\n"),
						progname);

					if(exit_on_error == true)
					{
						PQfinish(conn);
						exit(ERR_BAD_CONFIG);
					}

					config_ok = false;
				}
			}
		}

	}
	/*
	 * physical replication slots not available or not requested -
	 * ensure some reasonably high value set for `wal_keep_segments`
	 */
	else
	{
		i = guc_set_typed(conn, "wal_keep_segments", ">=",
						  runtime_options.wal_keep_segments, "integer");
		if (i == 0 || i == -1)
		{
			if (i == 0)
			{
				log_err(_("%s needs parameter 'wal_keep_segments' to be set to %s or greater (see the '-w' option or edit the postgresql.conf of the upstream server.)\n"),
						progname, runtime_options.wal_keep_segments);
				if(server_version_num >= 90400)
				{
					log_notice(_("HINT: in PostgreSQL 9.4 and later, replication slots can be used, which "
							   "do not require 'wal_keep_segments' to be set to a high value "
							   "(set parameter 'use_replication_slots' in the configuration file to enable)\n"
								 ));
				}
			}

			if(exit_on_error == true)
			{
				PQfinish(conn);
				exit(ERR_BAD_CONFIG);
			}

			config_ok = false;
		}
	}

	i = guc_set(conn, "archive_mode", "=", "on");
	if (i == 0 || i == -1)
	{
		if (i == 0)
			log_err(_("%s needs parameter 'archive_mode' to be set to 'on'\n"),
					progname);
		if(exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		config_ok = false;
	}

	i = guc_set(conn, "hot_standby", "=", "on");
	if (i == 0 || i == -1)
	{
		if (i == 0)
			log_err(_("%s needs parameter 'hot_standby' to be set to 'on'\n"),
					progname);

		if(exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		config_ok = false;
	}

	i = guc_set_typed(conn, "max_wal_senders", ">", "0", "integer");
	if (i == 0 || i == -1)
	{
		if (i == 0)
			log_err(_("%s needs parameter 'max_wal_senders' to be set to be at least 1\n"),
					progname);

		if(exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		config_ok = false;
	}

	return config_ok;
}


static void
do_check_upstream_config(void)
{
	PGconn	   *conn;
	bool		config_ok;
	int			server_version_num;

	parse_config(runtime_options.config_file, &options);

	/* Connection parameters for upstream server only */
	keywords[0] = "host";
	values[0] = runtime_options.host;
	keywords[1] = "port";
	values[1] = runtime_options.masterport;
	keywords[2] = "dbname";
	values[2] = runtime_options.dbname;

	/* We need to connect to check configuration and start a backup */
	log_info(_("%s connecting to upstream server\n"), progname);
	conn = establish_db_connection_by_params(keywords, values, true);

	/* Verify that upstream server is a supported server version */
	log_info(_("%s connected to upstream server, checking its state\n"), progname);
	server_version_num = check_server_version(conn, "upstream server", false, NULL);

	config_ok = check_upstream_config(conn, server_version_num, false);

	if(config_ok == true)
	{
		puts(_("No configuration problems found with the upstream server"));
	}

	PQfinish(conn);
}


static bool
create_node_record(PGconn *conn, char *action, int node, char *type, int upstream_node, char *cluster_name, char *node_name, char *conninfo, int priority)
{
	char		sqlquery[QUERY_STR_LEN];
	char		upstream_node_id[MAXLEN];
	char		slot_name[MAXLEN];
	PGresult   *res;

	if(upstream_node == NO_UPSTREAM_NODE)
	{
		/*
		 * No explicit upstream node id provided for standby - attempt to
		 * get primary node id
		 */
		if(strcmp(type, "standby") == 0)
		{
			int primary_node_id = get_primary_node_id(conn, cluster_name);
			maxlen_snprintf(upstream_node_id, "%i", primary_node_id);
		}
		else
		{
			maxlen_snprintf(upstream_node_id, "%s", "NULL");
		}
	}
	else
	{
		maxlen_snprintf(upstream_node_id, "%i", upstream_node);
	}

	if(options.use_replication_slots && strcmp(type, "standby") == 0)
	{
		maxlen_snprintf(slot_name, "'%s'", repmgr_slot_name);
	}
	else
	{
		maxlen_snprintf(slot_name, "%s", "NULL");
	}

	sqlquery_snprintf(sqlquery,
					  "INSERT INTO %s.repl_nodes "
					  "       (id, type, upstream_node_id, cluster, "
					  "        name, conninfo, slot_name, priority) "
					  "VALUES (%i, '%s', %s, '%s', '%s', '%s', %s, %i) ",
					  get_repmgr_schema_quoted(conn),
					  node,
					  type,
					  upstream_node_id,
					  cluster_name,
					  node_name,
					  conninfo,
					  slot_name,
					  priority);

	if(action != NULL)
	{
		log_debug(_("%s: %s\n"), action, sqlquery);
	}

	res = PQexec(conn, sqlquery);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_warning(_("Cannot insert node details, %s\n"),
					PQerrorMessage(conn));
		return false;
	}

	PQclear(res);

	return true;
}

static char *
make_pg_path(char *file)
{
	maxlen_snprintf(path_buf, "%s%s", pg_bindir, file);

	return path_buf;
}
