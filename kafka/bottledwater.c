#include "connect.h"
#include "json.h"
#include "logger.h"
#include "registry.h"
#include "oid2avro.h"

#include <librdkafka/rdkafka.h>
#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>		/* k4m */
#include <sys/stat.h>	/* k4m */
#include <sys/file.h>

#define DEFAULT_REPLICATION_SLOT "bottledwater"
#define APP_NAME "bottledwater"

/* The name of the logical decoding output plugin with which the replication
 * slot is created. This must match the name of the Postgres extension. */
#define OUTPUT_PLUGIN "bottledwater"

#define DEFAULT_BROKER_LIST "localhost:9092"
#define DEFAULT_SCHEMA_REGISTRY "http://localhost:8081"

#define TABLE_NAME_BUFFER_LENGTH 128

#define check(err, call) { err = call; if (err) return err; }

#define ensure(context, call) { \
    if (call) { \
        fatal_error((context), "%s", (context)->client->error); \
    } \
}

#define fatal_error(context, fmt, ...) { \
    log_fatal(fmt, ##__VA_ARGS__); \
    exit_nicely((context), 1); \
}
#define vfatal_error(context, fmt, args) { \
    vlog_fatal(fmt, args); \
    exit_nicely((context), 1); \
}

#define config_error(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)


#define PRODUCER_CONTEXT_ERROR_LEN 512
#define MAX_IN_FLIGHT_TRANSACTIONS 1000
/* leave room for one extra empty element so the circular buffer can
 * distinguish between empty and full */
#define XACT_LIST_LEN (MAX_IN_FLIGHT_TRANSACTIONS + 1)


typedef enum {
    OUTPUT_FORMAT_UNDEFINED = 0,
    OUTPUT_FORMAT_AVRO,
    OUTPUT_FORMAT_JSON
} format_t;

static const char* DEFAULT_OUTPUT_FORMAT_NAME = "avro";
static const format_t DEFAULT_OUTPUT_FORMAT = OUTPUT_FORMAT_AVRO;


typedef enum {
    ERROR_POLICY_UNDEFINED = 0,
    ERROR_POLICY_LOG,
    ERROR_POLICY_EXIT
} error_policy_t;

static const char* DEFAULT_ERROR_POLICY_NAME = PROTOCOL_ERROR_POLICY_EXIT;
static const error_policy_t DEFAULT_ERROR_POLICY = ERROR_POLICY_EXIT;


typedef struct {
    uint32_t xid;         /* Postgres transaction identifier */
    int recvd_events;     /* Number of row-level events received so far for this transaction */
    int pending_events;   /* Number of row-level events waiting to be acknowledged by Kafka */
    uint64_t commit_lsn;  /* WAL position of the transaction's commit event */
} transaction_info;

typedef struct {
    client_context_t client;            /* The connection to Postgres */
    schema_registry_t registry;         /* Submits Avro schemas to schema registry */
    char *brokers;                      /* Comma-separated list of host:port for Kafka brokers */
    transaction_info xact_list[XACT_LIST_LEN]; /* Circular buffer */
    int xact_head;                      /* Index into xact_list currently being received from PG */
    int xact_tail;                      /* Oldest index in xact_list not yet acknowledged by Kafka */
    rd_kafka_conf_t *kafka_conf;
    rd_kafka_topic_conf_t *topic_conf;
    rd_kafka_t *kafka;
    table_mapper_t mapper;              /* Remembers topics and schemas for tables we've seen */
    format_t output_format;             /* How to encode messages for writing to Kafka */
    char *topic_prefix;                 /* String to be prepended to all topic names */
    error_policy_t error_policy;        /* What to do in case of a transient error */
    char error[PRODUCER_CONTEXT_ERROR_LEN];
} producer_context;

typedef producer_context *producer_context_t;

static inline int xact_list_length(producer_context_t context) {
    return (XACT_LIST_LEN + /* normalise negative length in case of wraparound */
            context->xact_head + 1 - context->xact_tail)
        % XACT_LIST_LEN;
}

static inline bool xact_list_full(producer_context_t context) {
    return xact_list_length(context) == XACT_LIST_LEN - 1;
}

static inline bool xact_list_empty(producer_context_t context) {
    return xact_list_length(context) == 0;
}


typedef struct {
    producer_context_t context;
    uint64_t wal_pos;
    Oid relid;
    transaction_info *xact;
} msg_envelope;

typedef msg_envelope *msg_envelope_t;

static char *progname;
static int received_shutdown_signal = 0;
extern int received_reload_signal;/* k4m : reload table list flag */
static char pidfile[MAXPGPATH];	/* k4m : pid file */
#ifdef TTA_VNV
static int save_row;
#define MAXFILECNT 32
typedef struct {
	FILE *fp;
	Oid relid;
} log_files_t;
log_files_t logfiles[MAXFILECNT];
#endif

void usage(int exit_status);
void parse_options(producer_context_t context, int argc, char **argv);
char *parse_config_option(char *option);
void init_schema_registry(producer_context_t context, char *url);
const char* output_format_name(format_t format);
void set_output_format(producer_context_t context, char *format);
void set_error_policy(producer_context_t context, char *policy);
const char* error_policy_name(error_policy_t format);
void set_kafka_config(producer_context_t context, char *property, char *value);
void set_topic_config(producer_context_t context, char *property, char *value);
char* topic_name_from_avro_schema(avro_schema_t schema);

static int handle_error(producer_context_t context, int err, const char *fmt, ...) __attribute__ ((format (printf, 3, 4)));

static int on_begin_txn(void *_context, uint64_t wal_pos, uint32_t xid);
static int on_commit_txn(void *_context, uint64_t wal_pos, uint32_t xid);
static int on_table_schema(void *_context, uint64_t wal_pos, Oid relid,
        const char *key_schema_json, size_t key_schema_len, avro_schema_t key_schema,
        const char *row_schema_json, size_t row_schema_len, avro_schema_t row_schema);
static int on_insert_row(void *_context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *new_bin, size_t new_len, avro_value_t *new_val);
static int on_update_row(void *_context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *old_bin, size_t old_len, avro_value_t *old_val,
        const void *new_bin, size_t new_len, avro_value_t *new_val);
static int on_delete_row(void *_context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *old_bin, size_t old_len, avro_value_t *old_val);
static int on_keepalive(void *_context, uint64_t wal_pos);
static int on_client_error(void *_context, int err, const char *message);
int send_kafka_msg(producer_context_t context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len,
        const void *val_bin, size_t val_len);
static void on_deliver_msg(rd_kafka_t *kafka, const rd_kafka_message_t *msg, void *envelope);
void maybe_checkpoint(producer_context_t context);
void backpressure(producer_context_t context);
client_context_t init_client(void);
producer_context_t init_producer(client_context_t client);
void start_producer(producer_context_t context);
void exit_nicely(producer_context_t context, int status);


void usage(int exit_status) {
    fprintf(stderr,
            "Exports a snapshot of a PostgreSQL database, followed by a stream of changes,\n"
            "and sends the data to a Kafka cluster.\n\n"
            "Usage:\n  %s [OPTION]...\n\nOptions:\n"
            "  -d, --postgres=postgres://user:pass@host:port/dbname   (required)\n"
            "                          Connection string or URI of the PostgreSQL server.\n"
            "  -s, --slot=slotname     Name of replication slot   (default: %s)\n"
            "                          The slot is automatically created on first use.\n"
            "  -b, --broker=host1[:port1],host2[:port2]...   (default: %s)\n"
            "                          Comma-separated list of Kafka broker hosts/ports.\n"
            "  -r, --schema-registry=http://hostname:port   (default: %s)\n"
            "                          URL of the service where Avro schemas are registered.\n"
            "                          Used only for --output-format=avro.\n"
            "                          Omit when --output-format=json.\n"
            "  -f, --output-format=[avro|json]   (default: %s)\n"
            "                          How to encode the messages for writing to Kafka.\n"
            "  -u, --allow-unkeyed     Allow export of tables that don't have a primary key.\n"
            "                          This is disallowed by default, because updates and\n"
            "                          deletes need a primary key to identify their row.\n"
            "  -p, --topic-prefix=prefix\n"
            "                          String to prepend to all topic names.\n"
            "                          e.g. with --topic-prefix=postgres, updates from table\n"
            "                          'users' will be written to topic 'postgres.users'.\n"
            "  -e, --on-error=[log|exit]   (default: %s)\n"
            "                          What to do in case of a transient error, such as\n"
            "                          failure to publish to Kafka.\n"
            "  -x, --skip-snapshot     Skip taking a consistent snapshot of the existing\n"
            "                          database contents and just start streaming any new\n"
            "                          updates.  (Ignored if the replication slot already\n"
            "                          exists.)\n"
            "  -C, --kafka-config property=value\n"
            "                          Set global configuration property for Kafka producer\n"
            "                          (see --config-help for list of properties).\n"
            "  -T, --topic-config property=value\n"
#ifdef TTA_VNV
            "  -S, --save-log\n" /* TTA VNV */
#endif
            "                          Set topic configuration property for Kafka producer.\n"
            "  --config-help           Print the list of configuration properties. See also:\n"
            "            https://github.com/edenhill/librdkafka/blob/master/CONFIGURATION.md\n"
            "  -h, --help\n"
            "                          Print this help text.\n",

            progname,
            DEFAULT_REPLICATION_SLOT,
            DEFAULT_BROKER_LIST,
            DEFAULT_SCHEMA_REGISTRY,
            DEFAULT_OUTPUT_FORMAT_NAME,
            DEFAULT_ERROR_POLICY_NAME);
    exit(exit_status);
}

/* Parse command-line options */
void parse_options(producer_context_t context, int argc, char **argv) {

    static struct option options[] = {
        {"postgres",        required_argument, NULL, 'd'},
        {"slot",            required_argument, NULL, 's'},
        {"broker",          required_argument, NULL, 'b'},
        {"schema-registry", required_argument, NULL, 'r'},
        {"output-format",   required_argument, NULL, 'f'},
        {"allow-unkeyed",   no_argument,       NULL, 'u'},
        {"topic-prefix",    required_argument, NULL, 'p'},
        {"on-error",        required_argument, NULL, 'e'},
        {"skip-snapshot",   no_argument,       NULL, 'x'},
        {"kafka-config",    required_argument, NULL, 'C'},
        {"topic-config",    required_argument, NULL, 'T'},
#ifdef TTA_VNV
        {"save-log",    	no_argument,	   NULL, 'S'}, /* TTA VNV */
#endif
        {"config-help",     no_argument,       NULL,  1 },
        {"help",            no_argument,       NULL, 'h'},
        {NULL,              0,                 NULL,  0 }
    };

    progname = argv[0];

    int option_index;
    while (true) {
#ifdef TTA_VNV
        int c = getopt_long(argc, argv, "d:s:b:r:f:up:e:xC:T:hS", options, &option_index);
#else
        int c = getopt_long(argc, argv, "d:s:b:r:f:up:e:xC:T:h", options, &option_index);
#endif
        if (c == -1) break;

        switch (c) {
            case 'd':
                context->client->conninfo = strdup(optarg);
                break;
            case 's':
                context->client->repl.slot_name = strdup(optarg);
                break;
            case 'b':
                context->brokers = strdup(optarg);
                break;
            case 'r':
                init_schema_registry(context, optarg);
                break;
            case 'f':
                set_output_format(context, optarg);
                break;
            case 'u':
                context->client->allow_unkeyed = true;
                break;
            case 'p':
                context->topic_prefix = strdup(optarg);
                break;
            case 'e':
                set_error_policy(context, optarg);
                break;
            case 'x':
                context->client->skip_snapshot = true;
                break;
            case 'C':
                set_kafka_config(context, optarg, parse_config_option(optarg));
                break;
            case 'T':
                set_topic_config(context, optarg, parse_config_option(optarg));
                break;
#ifdef TTA_VNV
            case 'S': //TTA
				save_row = 1;
				memset (logfiles, 0x00, sizeof(log_files_t)*MAXFILECNT);
                break;
#endif
            case 1:
                rd_kafka_conf_properties_show(stderr);
                exit(0);
                break;
            case 'h':
                usage(0);
            default:
                usage(1);
        }
    }

    if (!context->client->conninfo || optind < argc) usage(1);

    if (context->output_format == OUTPUT_FORMAT_AVRO && !context->registry) {
        init_schema_registry(context, DEFAULT_SCHEMA_REGISTRY);
    } else if (context->output_format == OUTPUT_FORMAT_JSON && context->registry) {
        config_error("Specifying --schema-registry doesn't make sense for "
                     "--output-format=json");
        usage(1);
    }
}

/* Splits an option string by equals sign. Modifies the option argument to be
 * only the part before the equals sign, and returns a pointer to the part after
 * the equals sign. */
char *parse_config_option(char *option) {
    char *equals = strchr(option, '=');
    if (!equals) {
        log_error("%s: Expected configuration in the form property=value, not \"%s\"",
                  progname, option);
        exit(1);
    }

    // Overwrite equals sign with null, to split key and value into two strings
    *equals = '\0';
    return equals + 1;
}

void init_schema_registry(producer_context_t context, char *url) {
    context->registry = schema_registry_new(url);

    if (!context->registry) {
        log_error("Failed to initialise schema registry!");
        exit(1);
    }
}

void set_output_format(producer_context_t context, char *format) {
    if (!strcmp("avro", format)) {
        context->output_format = OUTPUT_FORMAT_AVRO;
    } else if (!strcmp("json", format)) {
        context->output_format = OUTPUT_FORMAT_JSON;
    } else {
        config_error("invalid output format (expected avro or json): %s", format);
        exit(1);
    }
}

const char* output_format_name(format_t format) {
    switch (format) {
    case OUTPUT_FORMAT_AVRO: return "Avro";
    case OUTPUT_FORMAT_JSON: return "JSON";
    case OUTPUT_FORMAT_UNDEFINED: return "undefined (probably a bug)";
    default: return "unknown (probably a bug)";
    }
}

void set_error_policy(producer_context_t context, char *policy) {
    if (!strcmp(PROTOCOL_ERROR_POLICY_LOG, policy)) {
        context->error_policy = ERROR_POLICY_LOG;
    } else if (!strcmp(PROTOCOL_ERROR_POLICY_EXIT, policy)) {
        context->error_policy = ERROR_POLICY_EXIT;
    } else {
        config_error("invalid error policy (expected log or exit): %s", policy);
        exit(1);
    }

    db_client_set_error_policy(context->client, policy);
}

const char* error_policy_name(error_policy_t policy) {
    switch (policy) {
        case ERROR_POLICY_LOG: return PROTOCOL_ERROR_POLICY_LOG;
        case ERROR_POLICY_EXIT: return PROTOCOL_ERROR_POLICY_EXIT;
        case ERROR_POLICY_UNDEFINED: return "undefined (probably a bug)";
        default: return "unknown (probably a bug)";
    }
}

void set_kafka_config(producer_context_t context, char *property, char *value) {
    if (rd_kafka_conf_set(context->kafka_conf, property, value,
                context->error, PRODUCER_CONTEXT_ERROR_LEN) != RD_KAFKA_CONF_OK) {
        config_error("%s: %s", progname, context->error);
        exit(1);
    }
}

void set_topic_config(producer_context_t context, char *property, char *value) {
    if (rd_kafka_topic_conf_set(context->topic_conf, property, value,
                context->error, PRODUCER_CONTEXT_ERROR_LEN) != RD_KAFKA_CONF_OK) {
        config_error("%s: %s", progname, context->error);
        exit(1);
    }
}

char* topic_name_from_avro_schema(avro_schema_t schema) {

    const char *table_name = avro_schema_name(schema);

#ifdef AVRO_1_8
    /* Gets the avro schema namespace which contains the Postgres schema name */
    const char *namespace = avro_schema_namespace(schema);
#else
#warning "avro-c older than 1.8.0, will not include Postgres schema in Kafka topic name"
    const char namespace[] = "dummy";
#endif

    char topic_name[TABLE_NAME_BUFFER_LENGTH];
    /* Strips the beginning part of the namespace to extract the Postgres schema name
     * and init topic_name with it */
    int matched = sscanf(namespace, GENERATED_SCHEMA_NAMESPACE ".%s", topic_name);
    /* If the sscanf doesn't find a match with GENERATED_SCHEMA_NAMESPACE,
     * or if the Postgres schema name is 'public', we just init topic_name with the table_name. */
    if (!matched || !strcmp(topic_name, "public")) {
        strncpy(topic_name, table_name, TABLE_NAME_BUFFER_LENGTH);
        topic_name[TABLE_NAME_BUFFER_LENGTH - 1] = '\0';
    /* Otherwise we append to the topic_name previously initialized with the schema_name a "."
     * separator followed by the table_name.                    */
    } else {
        strncat(topic_name, ".", TABLE_NAME_BUFFER_LENGTH - strlen(topic_name) - 1);
        strncat(topic_name, table_name, TABLE_NAME_BUFFER_LENGTH - strlen(topic_name) - 1);
    }

    return strdup(topic_name);
}

static int handle_error(producer_context_t context, int err, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    switch (context->error_policy) {
    case ERROR_POLICY_LOG:
        vlog_error(fmt, args);
        err = 0;
        break;
    case ERROR_POLICY_EXIT:
        vfatal_error(context, fmt, args);
    default:
        fatal_error(context, "invalid error policy %s",
                    error_policy_name(context->error_policy));
    }

    va_end(args);

    return err;
}


static int on_begin_txn(void *_context, uint64_t wal_pos, uint32_t xid) {
    producer_context_t context = (producer_context_t) _context;
    replication_stream_t stream = &context->client->repl;

    if (xid == 0) {
        if (!(context->xact_tail == 0 && xact_list_empty(context))) {
            fatal_error(context, "Expected snapshot to be the first transaction.");
        }

        log_info("Created replication slot \"%s\", capturing consistent snapshot \"%s\".",
                 stream->slot_name, stream->snapshot_name);
    }

    // If the circular buffer is full, we have to block and wait for some transactions
    // to be delivered to Kafka and acknowledged for the broker.
    while (xact_list_full(context)) {
#ifdef DEBUG
        log_warn("Too many transactions in flight, applying backpressure");
#endif
        backpressure(context);
    }

    context->xact_head = (context->xact_head + 1) % XACT_LIST_LEN;
    transaction_info *xact = &context->xact_list[context->xact_head];
    xact->xid = xid;
    xact->recvd_events = 0;
    xact->pending_events = 0;
    xact->commit_lsn = 0;

    return 0;
}

static int on_commit_txn(void *_context, uint64_t wal_pos, uint32_t xid) {
    producer_context_t context = (producer_context_t) _context;
    transaction_info *xact = &context->xact_list[context->xact_head];

    if (xid == 0) {
        log_info("Snapshot complete, streaming changes from %X/%X.",
                 (uint32) (wal_pos >> 32), (uint32) wal_pos);
    }

    if (xid != xact->xid) {
        fatal_error(context,
                    "Mismatched begin/commit events (xid %u in flight, xid %u committed)",
                    xact->xid, xid);
    }

    xact->commit_lsn = wal_pos;
    maybe_checkpoint(context);
    return 0;
}


static int on_table_schema(void *_context, uint64_t wal_pos, Oid relid,
        const char *key_schema_json, size_t key_schema_len, avro_schema_t key_schema,
        const char *row_schema_json, size_t row_schema_len, avro_schema_t row_schema) {
    producer_context_t context = (producer_context_t) _context;

    char *topic_name = topic_name_from_avro_schema(row_schema);

    table_metadata_t table = table_mapper_update(context->mapper, relid, topic_name,
            key_schema_json, key_schema_len, row_schema_json, row_schema_len);

    free(topic_name);

    if (!table) {
        log_error("%s", context->mapper->error);
        /*
         * Can't really handle the error since we're in a callback.
         * See comment in body of table_mapper_update() in table_mapper.c for
         * discussion of the implications of an error registering the table.
         */
        return 1;
    }

    return 0;
}

//TTA
//
#ifdef TTA_VNV
#define TIMELEN 32
static int get_cur_time(char *now){
	char buffer[26];
//	int millisec;
	suseconds_t microsec;
	struct tm* tm_info;
	struct timeval tv;

	gettimeofday(&tv, NULL);

	//  millisec = lrint(tv.tv_usec/1000.0); // Round to nearest millisec
//	millisec = tv.tv_usec/1000; // Round to nearest millisec
	microsec = tv.tv_usec; // Round to nearest millisec
	if (microsec >=1000000) { // Allow for rounding up to nearest second
		microsec -=1000000;
		tv.tv_sec++;
	}

	tm_info = localtime(&tv.tv_sec);

	strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
	snprintf(now, TIMELEN-1, "%s.%06ld", buffer, microsec);
	return 0;
}

static int print_insert_row(producer_context_t context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *new_bin, size_t new_len, avro_value_t *new_val, int index) {
    int err = 0;
    char *key_json, *new_json;
	char now[TIMELEN];
    const char *table_name = avro_schema_name(avro_value_get_schema(new_val));
    check(err, avro_value_to_json(new_val, 1, &new_json));

	table_metadata_t table = table_mapper_lookup(context->mapper, relid);
	if (!table) {
		log_error("relid %" PRIu32 " has no registered schema", relid);
		return -1;
	}

	get_cur_time(now);
    if (key_val) {
        check(err, avro_value_to_json(key_val, 1, &key_json));
        //printf("insert to %s: %s --> %s\n", table_name, key_json, new_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s):%s to %s: %s --> %s\n", now, rd_kafka_topic_name(table->topic),
		"insert", table_name, key_json, new_json);
        free(key_json);
    } else {
        //printf("insert to %s: %s\n", table_name, new_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s)%s to %s: %s\n", now, rd_kafka_topic_name(table->topic),
		"insert", table_name, new_json);
    }
	fflush(logfiles[index].fp);

    free(new_json);
    return err;
}

static int print_update_row(producer_context_t context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *old_bin, size_t old_len, avro_value_t *old_val,
        const void *new_bin, size_t new_len, avro_value_t *new_val, int index) {
    int err = 0;
    char *key_json = NULL, *old_json = NULL, *new_json = NULL;
	char now[TIMELEN];
    const char *table_name = avro_schema_name(avro_value_get_schema(new_val));
    check(err, avro_value_to_json(new_val, 1, &new_json));

	table_metadata_t table = table_mapper_lookup(context->mapper, relid);
	if (!table) {
		log_error("relid %" PRIu32 " has no registered schema", relid);
		return -1;
	}

    if (old_val) check(err, avro_value_to_json(old_val, 1, &old_json));
    if (key_val) check(err, avro_value_to_json(key_val, 1, &key_json));

	get_cur_time(now);
    if (key_json && old_json) {
//        printf("update to %s: key %s: %s --> %s\n", table_name, key_json, old_json, new_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s):update to %s: key %s: %s --> %s\n", now, 
				rd_kafka_topic_name(table->topic), table_name, key_json, old_json, new_json);
    } else if (old_json) {
        //printf("update to %s: %s --> %s\n", table_name, old_json, new_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s):update to %s: %s --> %s\n", now, rd_kafka_topic_name(table->topic),
				table_name, old_json, new_json);
    } else if (key_json) {
        //printf("update to %s: key %s: %s\n", table_name, key_json, new_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s):update to %s: key %s: %s\n", now, rd_kafka_topic_name(table->topic),
				table_name, key_json, new_json);
    } else {
        //printf("update to %s: (?) --> %s\n", table_name, new_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s):update to %s: (?) --> %s\n", now, rd_kafka_topic_name(table->topic),
				table_name, new_json);
    }
	fflush(logfiles[index].fp);

    if (key_json) free(key_json);
    if (old_json) free(old_json);
    free(new_json);
    return err;
}

static int print_delete_row(producer_context_t context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *old_bin, size_t old_len, avro_value_t *old_val, int index) {
    int err = 0;
    char *key_json = NULL, *old_json = NULL;
    const char *table_name = NULL;
	char now[TIMELEN];

	table_metadata_t table = table_mapper_lookup(context->mapper, relid);
	if (!table) {
		log_error("relid %" PRIu32 " has no registered schema", relid);
		return -1;
	}

    if (key_val) check(err, avro_value_to_json(key_val, 1, &key_json));
    if (old_val) {
        check(err, avro_value_to_json(old_val, 1, &old_json));
        table_name = avro_schema_name(avro_value_get_schema(old_val));
    }

	get_cur_time(now);
    if (key_json && old_json) {
        printf("delete from %s: %s (was: %s)\n", table_name, key_json, old_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s):delete from %s: %s (was: %s)\n",
			now, rd_kafka_topic_name(table->topic), table_name, key_json, old_json);
    } else if (old_json) {
        printf("delete from %s: %s\n", table_name, old_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s):delete from %s: %s\n",
			now, rd_kafka_topic_name(table->topic), table_name, old_json);
    } else if (key_json) {
        printf("delete from relid %u: %s\n", relid, key_json);
        fprintf(logfiles[index].fp, "[%s] topic(%s):delete from relid %u: %s\n",
			now, rd_kafka_topic_name(table->topic), relid, key_json);
    } else {
        printf("delete to relid %u (?)\n", relid);
        fprintf(logfiles[index].fp, "[%s] topic(%s):delete to relid %u (?)\n",
			now, rd_kafka_topic_name(table->topic), relid);
    }

    if (key_json) free(key_json);
    if (old_json) free(old_json);
    return err;
}

static int save_row_func(producer_context_t context, Oid relid) {
	FILE* fp;
    int i = 0;
	char now[TIMELEN], logfile[256];

	if(logfiles[0].fp)
		return 0;
	else{
		table_metadata_t table = table_mapper_lookup(context->mapper, relid);
		if (!table) {
			log_error("relid %" PRIu32 " has no registered schema", relid);
			return -1;
		}

		get_cur_time(now);
		snprintf(logfile, sizeof(pidfile)-1, "/tmp/TTA_VNV_TEST.log");
		if ((fp = fopen(logfile, "w")) == NULL){
			return -1; 
		}
		logfiles[0].fp = fp;
		logfiles[0].relid = relid;
	}
	return i;
}

//static int save_row_func(producer_context_t context, Oid relid) {
//	FILE* fp;
//    int i = 0, found = 0;
//	char now[TIMELEN], logfile[256];
//
//	for(i = 0; i < MAXFILECNT; i++){
//		if(logfiles[i].relid == relid){
//			found = 1;
//			break;
//		}
//	}
//
//	if(found){
//		fp = logfiles[i].fp;
//	}
//	else{
//		if(save_row > MAXFILECNT) return -1;
//		table_metadata_t table = table_mapper_lookup(context->mapper, relid);
//		if (!table) {
//			log_error("relid %" PRIu32 " has no registered schema", relid);
//			return -1;
//		}
//
//		get_cur_time(now);
//		snprintf(logfile, sizeof(pidfile)-1, "/tmp/%s_%s.log", rd_kafka_topic_name(table->topic), now);
//		if ((fp = fopen(logfile, "w")) == NULL){
//			return -1; 
//		}
//		for(i = 0; i < MAXFILECNT; i++){
//			if(logfiles[i].fp == NULL){
//				logfiles[i].fp = fp;
//				logfiles[i].relid = relid;
//				save_row++;
//				break;
//			}
//		}
//	}
//	return i;
//}
#endif
static int on_insert_row(void *_context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *new_bin, size_t new_len, avro_value_t *new_val) {
    producer_context_t context = (producer_context_t) _context;
#ifdef TTA_VNV
	if(save_row){
		print_insert_row(_context, wal_pos, relid,
		key_bin, key_len, key_val,
		new_bin, new_len, new_val, save_row_func(context, relid));
	}
#endif
    return send_kafka_msg(context, wal_pos, relid, key_bin, key_len, new_bin, new_len);
}

static int on_update_row(void *_context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *old_bin, size_t old_len, avro_value_t *old_val,
        const void *new_bin, size_t new_len, avro_value_t *new_val) {
    producer_context_t context = (producer_context_t) _context;
#ifdef TTA_VNV
	if(save_row){ //TTA
		print_update_row(context, wal_pos, relid,
        key_bin, key_len, key_val,
        old_bin, old_len, old_val,
        new_bin, new_len, new_val, save_row_func(context, relid));
	}
#endif
    return send_kafka_msg(context, wal_pos, relid, key_bin, key_len, new_bin, new_len);
}

static int on_delete_row(void *_context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len, avro_value_t *key_val,
        const void *old_bin, size_t old_len, avro_value_t *old_val) {
    producer_context_t context = (producer_context_t) _context;

#ifdef TTA_VNV
    if (key_bin){
		if(save_row){ //TTA
		}
        return send_kafka_msg(context, wal_pos, relid, key_bin, key_len, NULL, 0);
	}
#else
	if (key_bin)
		return send_kafka_msg(context, wal_pos, relid, key_bin, key_len, NULL, 0);
#endif
    else
        return 0; // delete on unkeyed table --> can't do anything
}

static int on_keepalive(void *_context, uint64_t wal_pos) {
    producer_context_t context = (producer_context_t) _context;

    if (xact_list_empty(context)) {
        return 0;
    } else {
        return FRAME_READER_SYNC_PENDING;
    }
}

static int on_client_error(void *_context, int err, const char *message) {
    producer_context_t context = (producer_context_t) _context;
    return handle_error(context, err, "Client error: %s", message);
}


int send_kafka_msg(producer_context_t context, uint64_t wal_pos, Oid relid,
        const void *key_bin, size_t key_len,
        const void *val_bin, size_t val_len) {

    transaction_info *xact = &context->xact_list[context->xact_head];
    xact->recvd_events++;
    xact->pending_events++;

    msg_envelope_t envelope = malloc(sizeof(msg_envelope));
    memset(envelope, 0, sizeof(msg_envelope));
    envelope->context = context;
    envelope->wal_pos = wal_pos;
    envelope->relid = relid;
    envelope->xact = xact;

    void *key = NULL, *val = NULL;
    size_t key_encoded_len, val_encoded_len;
    table_metadata_t table = table_mapper_lookup(context->mapper, relid);
    if (!table) {
        log_error("relid %" PRIu32 " has no registered schema", relid);
        return 1;
    }

    int err;

    switch (context->output_format) {
    case OUTPUT_FORMAT_JSON:
        err = json_encode_msg(table,
                key_bin, key_len, (char **) &key, &key_encoded_len,
                val_bin, val_len, (char **) &val, &val_encoded_len);

        if (err) {
            log_error("%s: error %s encoding JSON for topic %s",
                      progname, strerror(err), rd_kafka_topic_name(table->topic));
            return err;
        }
        break;
    case OUTPUT_FORMAT_AVRO:
        err = schema_registry_encode_msg(table->key_schema_id, table->row_schema_id,
                key_bin, key_len, &key, &key_encoded_len,
                val_bin, val_len, &val, &val_encoded_len);

        if (err) {
            log_error("%s: error %s encoding Avro for topic %s",
                      progname, strerror(err), rd_kafka_topic_name(table->topic));
            return err;
        }
        break;
    default:
        fatal_error(context, "invalid output format %s",
                    output_format_name(context->output_format));
    }

    bool enqueued = false;
    while (!enqueued) {
        int err = rd_kafka_produce(table->topic,
                RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_FREE,
                val, val == NULL ? 0 : val_encoded_len,
                key, key == NULL ? 0 : key_encoded_len,
                envelope);
        enqueued = (err == 0);

        // If data from Postgres is coming in faster than we can send it on to Kafka, we
        // create backpressure by blocking until the producer's queue has drained a bit.
        if (rd_kafka_errno2err(errno) == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
#ifdef DEBUG
            log_warn("Kafka producer queue is full, applying backpressure");
#endif
            backpressure(context);

        } else if (err != 0) {
            log_error("%s: Failed to produce to Kafka (topic %s): %s",
                      progname,
                      rd_kafka_topic_name(table->topic),
                      rd_kafka_err2str(rd_kafka_errno2err(errno)));
            if (val != NULL) free(val);
            if (key != NULL) free(key);
            return err;
        }
    }

    if (key)
        free(key);
    return 0;
}


/* Called by Kafka producer once per message sent, to report the delivery status
 * (whether success or failure). */
static void on_deliver_msg(rd_kafka_t *kafka, const rd_kafka_message_t *msg, void *opaque) {
    // The pointer that is the last argument to rd_kafka_produce is passed back
    // to us in the _private field in the struct. Seems a bit risky to rely on
    // a field called _private, but it seems to be the only way?
    msg_envelope_t envelope = (msg_envelope_t) msg->_private;

    int err;
    if (msg->err) {
        err = handle_error(envelope->context, msg->err,
                "Message delivery to topic %s failed: %s",
                rd_kafka_topic_name(msg->rkt),
                rd_kafka_err2str(msg->err));
        // err == 0 if handled
    } else {
        // Message successfully delivered to Kafka
        err = 0;
    }

    if (!err) {
        envelope->xact->pending_events--;
        maybe_checkpoint(envelope->context);
    }
    free(envelope);
}


/* When a Postgres transaction has been durably written to Kafka (i.e. we've seen the
 * commit event from Postgres, so we know the transaction is complete, and the Kafka
 * broker has acknowledged all messages in the transaction), we checkpoint it. This
 * allows the WAL for that transaction to be cleaned up in Postgres. */
void maybe_checkpoint(producer_context_t context) {
    transaction_info *xact = &context->xact_list[context->xact_tail];

    while (xact->pending_events == 0 && (xact->commit_lsn > 0 || xact->xid == 0)) {

        // Set the replication stream's "fsync LSN" (i.e. the WAL position up to which
        // the data has been durably written). This will be sent back to Postgres in the
        // next keepalive message, and used as the restart position if this client dies.
        // This should ensure that no data is lost (although messages may be duplicated).
        replication_stream_t stream = &context->client->repl;

        if (stream->fsync_lsn > xact->commit_lsn) {
            log_warn("%s: Commits not in WAL order! "
                     "Checkpoint LSN is %X/%X, commit LSN is %X/%X.", progname,
                     (uint32) (stream->fsync_lsn >> 32), (uint32) stream->fsync_lsn,
                     (uint32) (xact->commit_lsn  >> 32), (uint32) xact->commit_lsn);
        }

        if (stream->fsync_lsn < xact->commit_lsn) {
            log_debug("Checkpointing %d events for xid %u, WAL position %X/%X.",
                      xact->recvd_events, xact->xid,
                      (uint32) (xact->commit_lsn >> 32), (uint32) xact->commit_lsn);
        }

        stream->fsync_lsn = xact->commit_lsn;

        // xid==0 is the initial snapshot transaction. Clear the flag when it's complete.
        if (xact->xid == 0 && xact->commit_lsn > 0) {
            context->client->taking_snapshot = false;
        }

        context->xact_tail = (context->xact_tail + 1) % XACT_LIST_LEN;

        if (xact_list_empty(context)) break;

        xact = &context->xact_list[context->xact_tail];
    }
}


/* If the producing of messages to Kafka can't keep up with the consuming of messages from
 * Postgres, this function applies backpressure. It blocks for a little while, until a
 * timeout or until some network activity occurs in the Kafka client. At the same time, it
 * keeps the Postgres connection alive (without consuming any more data from it). This
 * function can be called in a loop until the buffer has drained. */
void backpressure(producer_context_t context) {
    rd_kafka_poll(context->kafka, 200);

    if (received_shutdown_signal) {
        log_info("%s during backpressure. Shutting down...", strsignal(received_shutdown_signal));
        exit_nicely(context, 0);
    }

    // Keep the replication connection alive, even if we're not consuming data from it.
    int err = replication_stream_keepalive(&context->client->repl);
    if (err) {
        fatal_error(context, "While sending standby status update for keepalive: %s",
                    context->client->repl.error);
    }
}


/* Initializes the client context, which holds everything we need to know about
 * our connection to Postgres. */
client_context_t init_client() {
    frame_reader_t frame_reader = frame_reader_new();
    frame_reader->on_begin_txn    = on_begin_txn;
    frame_reader->on_commit_txn   = on_commit_txn;
    frame_reader->on_table_schema = on_table_schema;
    frame_reader->on_insert_row   = on_insert_row;
    frame_reader->on_update_row   = on_update_row;
    frame_reader->on_delete_row   = on_delete_row;
    frame_reader->on_keepalive    = on_keepalive;
    frame_reader->on_error        = on_client_error;

    client_context_t client = db_client_new();
    client->app_name = strdup(APP_NAME);
    db_client_set_error_policy(client, DEFAULT_ERROR_POLICY_NAME);
    client->allow_unkeyed = false;
    client->repl.slot_name = strdup(DEFAULT_REPLICATION_SLOT);
    client->repl.output_plugin = strdup(OUTPUT_PLUGIN);
    client->repl.frame_reader = frame_reader;
    return client;
}

/* Initializes the producer context, which holds everything we need to know about
 * our connection to Kafka. */
producer_context_t init_producer(client_context_t client) {
    producer_context_t context = malloc(sizeof(producer_context));
    memset(context, 0, sizeof(producer_context));
    client->repl.frame_reader->cb_context = context;

    context->client = client;

    context->output_format = DEFAULT_OUTPUT_FORMAT;
    context->error_policy = DEFAULT_ERROR_POLICY;

    context->brokers = DEFAULT_BROKER_LIST;
    context->kafka_conf = rd_kafka_conf_new();
    context->topic_conf = rd_kafka_topic_conf_new();

    context->xact_head = XACT_LIST_LEN - 1;
    /* xact_tail and xact_list are set to zero by memset() above; this results
     * in the circular buffer starting out empty, since the tail is one ahead
     * of the head. */

#if RD_KAFKA_VERSION >= 0x000901ff
    /* librdkafka 0.9.1 provides a "consistent_random" partitioner, which is
     * a good choice for us: "Uses consistent hashing to map identical keys
     * onto identical partitions, and messages without keys will be assigned
     * via the random partitioner." */
    rd_kafka_topic_conf_set_partitioner_cb(context->topic_conf, &rd_kafka_msg_partitioner_consistent_random);
#elif RD_KAFKA_VERSION >= 0x00090000
#warning "rdkafka 0.9.0, using consistent partitioner - unkeyed messages will all get sent to a single partition!"
    /* librdkafka 0.9.0 provides a "consistent hashing partitioner", which we
     * can use to ensure that all updates for a given key go to the same
     * partition.  However, for unkeyed messages (such as we send for tables
     * with no primary key), it sends them all to the same partition, rather
     * than randomly partitioning them as would be preferable for scalability.
     */
    rd_kafka_topic_conf_set_partitioner_cb(context->topic_conf, &rd_kafka_msg_partitioner_consistent);
#else
#warning "rdkafka older than 0.9.0, messages will be partitioned randomly!"
    /* librdkafka prior to 0.9.0 does not provide a consistent partitioner, so
     * each message will be assigned to a random partition.  This will lead to
     * incorrect log compaction behaviour: e.g. if the initial insert for row
     * 42 goes to partition 0, then a subsequent delete for row 42 goes to
     * partition 1, then log compaction will be unable to garbage-collect the
     * insert. It will also break any consumer relying on seeing all updates
     * relating to a given key (e.g. for a stream-table join). */
#endif

    set_topic_config(context, "produce.offset.report", "true");
    rd_kafka_conf_set_dr_msg_cb(context->kafka_conf, on_deliver_msg);
    return context;
}

/* Connects to Kafka. This should be done before connecting to Postgres, as it
 * simply calls exit(1) on failure. */
void start_producer(producer_context_t context) {
    context->kafka = rd_kafka_new(RD_KAFKA_PRODUCER, context->kafka_conf,
            context->error, PRODUCER_CONTEXT_ERROR_LEN);
    if (!context->kafka) {
        log_error("%s: Could not create Kafka producer: %s", progname, context->error);
        exit(1);
    }

    if (rd_kafka_brokers_add(context->kafka, context->brokers) == 0) {
        log_error("%s: No valid Kafka brokers specified", progname);
        exit(1);
    }

    context->mapper = table_mapper_new(
            context->kafka,
            context->topic_conf,
            context->registry,
            context->topic_prefix);

    log_info("Writing messages to Kafka in %s format",
             output_format_name(context->output_format));
}

/* Shuts everything down and exits the process. */
void exit_nicely(producer_context_t context, int status) {
    // If a snapshot was in progress and not yet complete, and an error occurred, try to
    // drop the replication slot, so that the snapshot is retried when the user tries again.
    if (context->client->taking_snapshot && status != 0) {
        log_info("Dropping replication slot since the snapshot did not complete successfully.");
        if (replication_slot_drop(&context->client->repl) != 0) {
            log_error("%s: %s", progname, context->client->repl.error);
        }
    }

    if (context->topic_prefix) free(context->topic_prefix);
    table_mapper_free(context->mapper);
    if (context->registry) schema_registry_free(context->registry);
    frame_reader_free(context->client->repl.frame_reader);
    db_client_free(context->client);
    if (context->kafka) rd_kafka_destroy(context->kafka);
    curl_global_cleanup();
    rd_kafka_wait_destroyed(2000);
	unlink(pidfile); /* k4m */
#ifdef TTA_VNV
	for(int i = 0; i < MAXFILECNT; i++){ //TTA
		if(logfiles[i].fp != NULL){
			fclose(logfiles[i].fp);
			memset(logfiles, 0x00, sizeof(log_files_t));
		}
	}
#endif
    exit(status);
}

static void handle_shutdown_signal(int sig) {
    received_shutdown_signal = sig;
}

/* k4m: signal handler SIGUSR2 for reloading config */
static void handle_reload_signal(int sig) {
    received_reload_signal = sig;
	printf("signal [%d]\n", received_reload_signal );
    signal(SIGUSR2, handle_reload_signal);
}

static int make_pidfile(producer_context_t context){
	FILE * fp = NULL;
	int fd;
	snprintf(pidfile, sizeof(pidfile)-1, "/tmp/bw_%s.pid", context->client->repl.slot_name);

    /* Check the process already exists. if not exist, create file*/
	if (((fd = open(pidfile, O_RDWR|O_CREAT|O_EXCL, 0644)) == -1)
			|| ((fp = fdopen(fd, "r+")) == NULL) ) {
		return 1;
	}

	if (flock(fd, LOCK_EX|LOCK_NB) == -1) {
		fclose(fp);
		return 1;
	}

    fprintf(fp, "%d", getpid());
	fflush(fp);

	if (flock(fd, LOCK_UN) == -1) {
		close(fd);
		return 1;
	}
    close(fd);

    /* Make PID file world readable */
    if (chmod(pidfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
		return 1;
    
	return 0;
}
/* k4m: signal handler SIGUSR2 for reloading config */

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_ALL);
    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGUSR2, handle_reload_signal); /* k4m */

    producer_context_t context = init_producer(init_client());
    parse_options(context, argc, argv);

	if(make_pidfile(context)){				/* k4m */
        config_error("Can't make pidfile.");/* k4m */
        exit(1);
	}

    start_producer(context);
    ensure(context, db_client_start(context->client));

    replication_stream_t stream = &context->client->repl;

    if (!context->client->slot_created) {
        log_info("Replication slot \"%s\" exists, streaming changes from %X/%X.",
                 stream->slot_name,
                 (uint32) (stream->start_lsn >> 32), (uint32) stream->start_lsn);
    } else if (context->client->skip_snapshot) {
        log_info("Created replication slot \"%s\", skipping snapshot and streaming changes from %X/%X.",
                 stream->slot_name,
                 (uint32) (stream->start_lsn >> 32), (uint32) stream->start_lsn);
    } else {
        assert(context->client->taking_snapshot);
    }

	received_reload_signal = 1; /* k4m: in order to get mapping table info when the process start */

    while (context->client->status >= 0 && !received_shutdown_signal) {
        ensure(context, db_client_poll(context->client));

        if (context->client->status == 0) {
            ensure(context, db_client_wait(context->client));
        }

        rd_kafka_poll(context->kafka, 0);
    }

    if (received_shutdown_signal) {
        log_info("%s, shutting down...", strsignal(received_shutdown_signal));
    }

    exit_nicely(context, 0);
    return 0;
}
