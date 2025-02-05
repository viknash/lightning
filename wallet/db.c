#include "db.h"

#include <ccan/array_size/array_size.h>
#include <ccan/tal/str/str.h>
#include <common/node_id.h>
#include <common/version.h>
#include <inttypes.h>
#include <lightningd/lightningd.h>
#include <lightningd/log.h>
#include <lightningd/plugin_hook.h>
#include <wallet/db_common.h>

#define DB_FILE "lightningd.sqlite3"
#define NSEC_IN_SEC 1000000000

struct migration {
	const char *sql;
	void (*func)(struct lightningd *ld, struct db *db);
};

static void migrate_pr2342_feerate_per_channel(struct lightningd *ld, struct db *db);

/* Do not reorder or remove elements from this array, it is used to
 * migrate existing databases from a previous state, based on the
 * string indices */
static struct migration dbmigrations[] = {
    {SQL("CREATE TABLE version (version INTEGER)"), NULL},
    {SQL("INSERT INTO version VALUES (1)"), NULL},
    {SQL("CREATE TABLE outputs ("
	 "  prev_out_tx CHAR(64)"
	 ", prev_out_index INTEGER"
	 ", value INTEGER"
	 ", type INTEGER"
	 ", status INTEGER"
	 ", keyindex INTEGER"
	 ", PRIMARY KEY (prev_out_tx, prev_out_index));"),
     NULL},
    {SQL("CREATE TABLE vars ("
	 "  name VARCHAR(32)"
	 ", val VARCHAR(255)"
	 ", PRIMARY KEY (name)"
	 ");"),
     NULL},
    {SQL("CREATE TABLE shachains ("
	 "  id INTEGER"
	 ", min_index INTEGER"
	 ", num_valid INTEGER"
	 ", PRIMARY KEY (id)"
	 ");"),
     NULL},
    {SQL("CREATE TABLE shachain_known ("
	 "  shachain_id INTEGER REFERENCES shachains(id) ON DELETE CASCADE"
	 ", pos INTEGER"
	 ", idx INTEGER"
	 ", hash BLOB"
	 ", PRIMARY KEY (shachain_id, pos)"
	 ");"),
     NULL},
    {SQL("CREATE TABLE channels ("
	 "  id INTEGER," /* chan->id */
	 "  peer_id INTEGER REFERENCES peers(id) ON DELETE CASCADE,"
	 "  short_channel_id BLOB,"
	 "  channel_config_local INTEGER,"
	 "  channel_config_remote INTEGER,"
	 "  state INTEGER,"
	 "  funder INTEGER,"
	 "  channel_flags INTEGER,"
	 "  minimum_depth INTEGER,"
	 "  next_index_local INTEGER,"
	 "  next_index_remote INTEGER,"
	 "  next_htlc_id INTEGER, "
	 "  funding_tx_id BLOB,"
	 "  funding_tx_outnum INTEGER,"
	 "  funding_satoshi INTEGER,"
	 "  funding_locked_remote INTEGER,"
	 "  push_msatoshi INTEGER,"
	 "  msatoshi_local INTEGER," /* our_msatoshi */
	 /* START channel_info */
	 "  fundingkey_remote BLOB,"
	 "  revocation_basepoint_remote BLOB,"
	 "  payment_basepoint_remote BLOB,"
	 "  htlc_basepoint_remote BLOB,"
	 "  delayed_payment_basepoint_remote BLOB,"
	 "  per_commit_remote BLOB,"
	 "  old_per_commit_remote BLOB,"
	 "  local_feerate_per_kw INTEGER,"
	 "  remote_feerate_per_kw INTEGER,"
	 /* END channel_info */
	 "  shachain_remote_id INTEGER,"
	 "  shutdown_scriptpubkey_remote BLOB,"
	 "  shutdown_keyidx_local INTEGER,"
	 "  last_sent_commit_state INTEGER,"
	 "  last_sent_commit_id INTEGER,"
	 "  last_tx BLOB,"
	 "  last_sig BLOB,"
	 "  closing_fee_received INTEGER,"
	 "  closing_sig_received BLOB,"
	 "  PRIMARY KEY (id)"
	 ");"),
     NULL},
    {SQL("CREATE TABLE peers ("
	 "  id INTEGER"
	 ", node_id BLOB UNIQUE" /* pubkey */
	 ", address TEXT"
	 ", PRIMARY KEY (id)"
	 ");"),
     NULL},
    {SQL("CREATE TABLE channel_configs ("
	 "  id INTEGER,"
	 "  dust_limit_satoshis INTEGER,"
	 "  max_htlc_value_in_flight_msat INTEGER,"
	 "  channel_reserve_satoshis INTEGER,"
	 "  htlc_minimum_msat INTEGER,"
	 "  to_self_delay INTEGER,"
	 "  max_accepted_htlcs INTEGER,"
	 "  PRIMARY KEY (id)"
	 ");"),
     NULL},
    {SQL("CREATE TABLE channel_htlcs ("
	 "  id INTEGER,"
	 "  channel_id INTEGER REFERENCES channels(id) ON DELETE CASCADE,"
	 "  channel_htlc_id INTEGER,"
	 "  direction INTEGER,"
	 "  origin_htlc INTEGER,"
	 "  msatoshi INTEGER,"
	 "  cltv_expiry INTEGER,"
	 "  payment_hash BLOB,"
	 "  payment_key BLOB,"
	 "  routing_onion BLOB,"
	 "  failuremsg BLOB,"
	 "  malformed_onion INTEGER,"
	 "  hstate INTEGER,"
	 "  shared_secret BLOB,"
	 "  PRIMARY KEY (id),"
	 "  UNIQUE (channel_id, channel_htlc_id, direction)"
	 ");"),
     NULL},
    {SQL("CREATE TABLE invoices ("
	 "  id INTEGER,"
	 "  state INTEGER,"
	 "  msatoshi INTEGER,"
	 "  payment_hash BLOB,"
	 "  payment_key BLOB,"
	 "  label TEXT,"
	 "  PRIMARY KEY (id),"
	 "  UNIQUE (label),"
	 "  UNIQUE (payment_hash)"
	 ");"),
     NULL},
    {SQL("CREATE TABLE payments ("
	 "  id INTEGER,"
	 "  timestamp INTEGER,"
	 "  status INTEGER,"
	 "  payment_hash BLOB,"
	 "  direction INTEGER,"
	 "  destination BLOB,"
	 "  msatoshi INTEGER,"
	 "  PRIMARY KEY (id),"
	 "  UNIQUE (payment_hash)"
	 ");"),
     NULL},
    /* Add expiry field to invoices (effectively infinite). */
    {SQL("ALTER TABLE invoices ADD expiry_time INTEGER;"), NULL},
    {SQL("UPDATE invoices SET expiry_time=9223372036854775807;"), NULL},
    /* Add pay_index field to paid invoices (initially, same order as id). */
    {SQL("ALTER TABLE invoices ADD pay_index INTEGER;"), NULL},
    {SQL("CREATE UNIQUE INDEX invoices_pay_index ON invoices(pay_index);"),
     NULL},
    {SQL("UPDATE invoices SET pay_index=id WHERE state=1;"),
     NULL}, /* only paid invoice */
    /* Create next_pay_index variable (highest pay_index). */
    {SQL("INSERT OR REPLACE INTO vars(name, val)"
	 "  VALUES('next_pay_index', "
	 "    COALESCE((SELECT MAX(pay_index) FROM invoices WHERE state=1), 0) "
	 "+ 1"
	 "  );"),
     NULL},
    /* Create first_block field; initialize from channel id if any.
     * This fails for channels still awaiting lockin, but that only applies to
     * pre-release software, so it's forgivable. */
    {SQL("ALTER TABLE channels ADD first_blocknum INTEGER;"), NULL},
    {SQL("UPDATE channels SET first_blocknum=CAST(short_channel_id AS INTEGER) "
	 "WHERE short_channel_id IS NOT NULL;"),
     NULL},
    {SQL("ALTER TABLE outputs ADD COLUMN channel_id INTEGER;"), NULL},
    {SQL("ALTER TABLE outputs ADD COLUMN peer_id BLOB;"), NULL},
    {SQL("ALTER TABLE outputs ADD COLUMN commitment_point BLOB;"), NULL},
    {SQL("ALTER TABLE invoices ADD COLUMN msatoshi_received INTEGER;"), NULL},
    /* Normally impossible, so at least we'll know if databases are ancient. */
    {SQL("UPDATE invoices SET msatoshi_received=0 WHERE state=1;"), NULL},
    {SQL("ALTER TABLE channels ADD COLUMN last_was_revoke INTEGER;"), NULL},
    /* We no longer record incoming payments: invoices cover that.
     * Without ALTER_TABLE DROP COLUMN support we need to do this by
     * rename & copy, which works because there are no triggers etc. */
    {SQL("ALTER TABLE payments RENAME TO temp_payments;"), NULL},
    {SQL("CREATE TABLE payments ("
	 "  id INTEGER,"
	 "  timestamp INTEGER,"
	 "  status INTEGER,"
	 "  payment_hash BLOB,"
	 "  destination BLOB,"
	 "  msatoshi INTEGER,"
	 "  PRIMARY KEY (id),"
	 "  UNIQUE (payment_hash)"
	 ");"),
     NULL},
    {SQL("INSERT INTO payments SELECT id, timestamp, status, payment_hash, "
	 "destination, msatoshi FROM temp_payments WHERE direction=1;"),
     NULL},
    {SQL("DROP TABLE temp_payments;"), NULL},
    /* We need to keep the preimage in case they ask to pay again. */
    {SQL("ALTER TABLE payments ADD COLUMN payment_preimage BLOB;"), NULL},
    /* We need to keep the shared secrets to decode error returns. */
    {SQL("ALTER TABLE payments ADD COLUMN path_secrets BLOB;"), NULL},
    /* Create time-of-payment of invoice, default already-paid
     * invoices to current time. */
    {SQL("ALTER TABLE invoices ADD paid_timestamp INTEGER;"), NULL},
    {SQL("UPDATE invoices"
	 "   SET paid_timestamp = strftime('%s', 'now')"
	 " WHERE state = 1;"),
     NULL},
    /* We need to keep the route node pubkeys and short channel ids to
     * correctly mark routing failures. We separate short channel ids
     * because we cannot safely save them as blobs due to byteorder
     * concerns. */
    {SQL("ALTER TABLE payments ADD COLUMN route_nodes BLOB;"), NULL},
    {SQL("ALTER TABLE payments ADD COLUMN route_channels TEXT;"), NULL},
    {SQL("CREATE TABLE htlc_sigs (channelid INTEGER REFERENCES channels(id) ON "
	 "DELETE CASCADE, signature BLOB);"),
     NULL},
    {SQL("CREATE INDEX channel_idx ON htlc_sigs (channelid)"), NULL},
    /* Get rid of OPENINGD entries; we don't put them in db any more */
    {SQL("DELETE FROM channels WHERE state=1"), NULL},
    /* Keep track of db upgrades, for debugging */
    {SQL("CREATE TABLE db_upgrades (upgrade_from INTEGER, lightning_version "
	 "TEXT);"),
     NULL},
    /* We used not to clean up peers when their channels were gone. */
    {SQL("DELETE FROM peers WHERE id NOT IN (SELECT peer_id FROM channels);"),
     NULL},
    /* The ONCHAIND_CHEATED/THEIR_UNILATERAL/OUR_UNILATERAL/MUTUAL are now one
     */
    {SQL("UPDATE channels SET STATE = 8 WHERE state > 8;"), NULL},
    /* Add bolt11 to invoices table*/
    {SQL("ALTER TABLE invoices ADD bolt11 TEXT;"), NULL},
    /* What do we think the head of the blockchain looks like? Used
     * primarily to track confirmations across restarts and making
     * sure we handle reorgs correctly. */
    {SQL("CREATE TABLE blocks (height INT, hash BLOB, prev_hash BLOB, "
	 "UNIQUE(height));"),
     NULL},
    /* ON DELETE CASCADE would have been nice for confirmation_height,
     * so that we automatically delete outputs that fall off the
     * blockchain and then we rediscover them if they are included
     * again. However, we have the their_unilateral/to_us which we
     * can't simply recognize from the chain without additional
     * hints. So we just mark them as unconfirmed should the block
     * die. */
    {SQL("ALTER TABLE outputs ADD COLUMN confirmation_height INTEGER "
	 "REFERENCES blocks(height) ON DELETE SET NULL;"),
     NULL},
    {SQL("ALTER TABLE outputs ADD COLUMN spend_height INTEGER REFERENCES "
	 "blocks(height) ON DELETE SET NULL;"),
     NULL},
    /* Create a covering index that covers both fields */
    {SQL("CREATE INDEX output_height_idx ON outputs (confirmation_height, "
	 "spend_height);"),
     NULL},
    {SQL("CREATE TABLE utxoset ("
	 " txid BLOB,"
	 " outnum INT,"
	 " blockheight INT REFERENCES blocks(height) ON DELETE CASCADE,"
	 " spendheight INT REFERENCES blocks(height) ON DELETE SET NULL,"
	 " txindex INT,"
	 " scriptpubkey BLOB,"
	 " satoshis BIGINT,"
	 " PRIMARY KEY(txid, outnum));"),
     NULL},
    {SQL("CREATE INDEX short_channel_id ON utxoset (blockheight, txindex, "
	 "outnum)"),
     NULL},
    /* Necessary index for long rollbacks of the blockchain, otherwise we're
     * doing table scans for every block removed. */
    {SQL("CREATE INDEX utxoset_spend ON utxoset (spendheight)"), NULL},
    /* Assign key 0 to unassigned shutdown_keyidx_local. */
    {SQL("UPDATE channels SET shutdown_keyidx_local=0 WHERE "
	 "shutdown_keyidx_local = -1;"),
     NULL},
    /* FIXME: We should rename shutdown_keyidx_local to final_key_index */
    /* -- Payment routing failure information -- */
    /* BLOB if failure was due to unparseable onion, NULL otherwise */
    {SQL("ALTER TABLE payments ADD failonionreply BLOB;"), NULL},
    /* 0 if we could theoretically retry, 1 if PERM fail at payee */
    {SQL("ALTER TABLE payments ADD faildestperm INTEGER;"), NULL},
    /* Contents of routing_failure (only if not unparseable onion) */
    {SQL("ALTER TABLE payments ADD failindex INTEGER;"),
     NULL}, /* erring_index */
    {SQL("ALTER TABLE payments ADD failcode INTEGER;"), NULL}, /* failcode */
    {SQL("ALTER TABLE payments ADD failnode BLOB;"), NULL},    /* erring_node */
    {SQL("ALTER TABLE payments ADD failchannel BLOB;"),
     NULL}, /* erring_channel */
    {SQL("ALTER TABLE payments ADD failupdate BLOB;"),
     NULL}, /* channel_update - can be NULL*/
    /* -- Payment routing failure information ends -- */
    /* Delete route data for already succeeded or failed payments */
    {SQL("UPDATE payments"
	 "   SET path_secrets = NULL"
	 "     , route_nodes = NULL"
	 "     , route_channels = NULL"
	 " WHERE status <> 0;"),
     NULL}, /* PAYMENT_PENDING */
    /* -- Routing statistics -- */
    {SQL("ALTER TABLE channels ADD in_payments_offered INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD in_payments_fulfilled INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD in_msatoshi_offered INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD in_msatoshi_fulfilled INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD out_payments_offered INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD out_payments_fulfilled INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD out_msatoshi_offered INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD out_msatoshi_fulfilled INTEGER;"), NULL},
    {SQL("UPDATE channels"
	 "   SET  in_payments_offered = 0,  in_payments_fulfilled = 0"
	 "     ,  in_msatoshi_offered = 0,  in_msatoshi_fulfilled = 0"
	 "     , out_payments_offered = 0, out_payments_fulfilled = 0"
	 "     , out_msatoshi_offered = 0, out_msatoshi_fulfilled = 0"
	 "     ;"),
     NULL},
    /* -- Routing statistics ends --*/
    /* Record the msatoshi actually sent in a payment. */
    {SQL("ALTER TABLE payments ADD msatoshi_sent INTEGER;"), NULL},
    {SQL("UPDATE payments SET msatoshi_sent = msatoshi;"), NULL},
    /* Delete dangling utxoset entries due to Issue #1280  */
    {SQL("DELETE FROM utxoset WHERE blockheight IN ("
	 "  SELECT DISTINCT(blockheight)"
	 "  FROM utxoset LEFT OUTER JOIN blocks on (blockheight == "
	 "blocks.height) "
	 "  WHERE blocks.hash IS NULL"
	 ");"),
     NULL},
    /* Record feerate range, to optimize onchaind grinding actual fees. */
    {SQL("ALTER TABLE channels ADD min_possible_feerate INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD max_possible_feerate INTEGER;"), NULL},
    /* https://bitcoinfees.github.io/#1d says Dec 17 peak was ~1M sat/kb
     * which is 250,000 sat/Sipa */
    {SQL("UPDATE channels SET min_possible_feerate=0, "
	 "max_possible_feerate=250000;"),
     NULL},
    /* -- Min and max msatoshi_to_us -- */
    {SQL("ALTER TABLE channels ADD msatoshi_to_us_min INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD msatoshi_to_us_max INTEGER;"), NULL},
    {SQL("UPDATE channels"
	 "   SET msatoshi_to_us_min = msatoshi_local"
	 "     , msatoshi_to_us_max = msatoshi_local"
	 "     ;"),
     NULL},
    /* -- Min and max msatoshi_to_us ends -- */
    /* Transactions we are interested in. Either we sent them ourselves or we
     * are watching them. We don't cascade block height deletes so we don't
     * forget any of them by accident.*/
    {SQL("CREATE TABLE transactions ("
	 "  id BLOB"
	 ", blockheight INTEGER REFERENCES blocks(height) ON DELETE SET NULL"
	 ", txindex INTEGER"
	 ", rawtx BLOB"
	 ", PRIMARY KEY (id)"
	 ");"),
     NULL},
    /* -- Detailed payment failure -- */
    {SQL("ALTER TABLE payments ADD faildetail TEXT;"), NULL},
    {SQL("UPDATE payments"
	 "   SET faildetail = 'unspecified payment failure reason'"
	 " WHERE status = 2;"),
     NULL}, /* PAYMENT_FAILED */
    /* -- Detailed payment faiure ends -- */
    {SQL("CREATE TABLE channeltxs ("
	 /* The id serves as insertion order and short ID */
	 "  id INTEGER"
	 ", channel_id INTEGER REFERENCES channels(id) ON DELETE CASCADE"
	 ", type INTEGER"
	 ", transaction_id BLOB REFERENCES transactions(id) ON DELETE CASCADE"
	 /* The input_num is only used by the txo_watch, 0 if txwatch */
	 ", input_num INTEGER"
	 /* The height at which we sent the depth notice */
	 ", blockheight INTEGER REFERENCES blocks(height) ON DELETE CASCADE"
	 ", PRIMARY KEY(id)"
	 ");"),
     NULL},
    /* -- Set the correct rescan height for PR #1398 -- */
    /* Delete blocks that are higher than our initial scan point, this is a
     * no-op if we don't have a channel. */
    {SQL("DELETE FROM blocks WHERE height > (SELECT MIN(first_blocknum) FROM "
	 "channels);"),
     NULL},
    /* Now make sure we have the lower bound block with the first_blocknum
     * height. This may introduce a block with NULL height if we didn't have any
     * blocks, remove that in the next. */
    {SQL("INSERT OR IGNORE INTO blocks (height) VALUES ((SELECT "
	 "MIN(first_blocknum) FROM channels));"),
     NULL},
    {SQL("DELETE FROM blocks WHERE height IS NULL;"), NULL},
    /* -- End of  PR #1398 -- */
    {SQL("ALTER TABLE invoices ADD description TEXT;"), NULL},
    /* FIXME: payments table 'description' is really a 'label' */
    {SQL("ALTER TABLE payments ADD description TEXT;"), NULL},
    /* future_per_commitment_point if other side proves we're out of date -- */
    {SQL("ALTER TABLE channels ADD future_per_commitment_point BLOB;"), NULL},
    /* last_sent_commit array fix */
    {SQL("ALTER TABLE channels ADD last_sent_commit BLOB;"), NULL},
    /* Stats table to track forwarded HTLCs. The values in the HTLCs
     * and their states are replicated here and the entries are not
     * deleted when the HTLC entries or the channel entries are
     * deleted to avoid unexpected drops in statistics. */
    {SQL("CREATE TABLE forwarded_payments ("
	 "  in_htlc_id INTEGER REFERENCES channel_htlcs(id) ON DELETE SET NULL"
	 ", out_htlc_id INTEGER REFERENCES channel_htlcs(id) ON DELETE SET NULL"
	 ", in_channel_scid INTEGER"
	 ", out_channel_scid INTEGER"
	 ", in_msatoshi INTEGER"
	 ", out_msatoshi INTEGER"
	 ", state INTEGER"
	 ", UNIQUE(in_htlc_id, out_htlc_id)"
	 ");"),
     NULL},
    /* Add a direction for failed payments. */
    {SQL("ALTER TABLE payments ADD faildirection INTEGER;"),
     NULL}, /* erring_direction */
    /* Fix dangling peers with no channels. */
    {SQL("DELETE FROM peers WHERE id NOT IN (SELECT peer_id FROM channels);"),
     NULL},
    {SQL("ALTER TABLE outputs ADD scriptpubkey BLOB;"), NULL},
    /* Keep bolt11 string for payments. */
    {SQL("ALTER TABLE payments ADD bolt11 TEXT;"), NULL},
    /* PR #2342 feerate per channel */
    {SQL("ALTER TABLE channels ADD feerate_base INTEGER;"), NULL},
    {SQL("ALTER TABLE channels ADD feerate_ppm INTEGER;"), NULL},
    {NULL, migrate_pr2342_feerate_per_channel},
    {SQL("ALTER TABLE channel_htlcs ADD received_time INTEGER"), NULL},
    {SQL("ALTER TABLE forwarded_payments ADD received_time INTEGER"), NULL},
    {SQL("ALTER TABLE forwarded_payments ADD resolved_time INTEGER"), NULL},
    {SQL("ALTER TABLE channels ADD remote_upfront_shutdown_script BLOB;"),
     NULL},
    /* PR #2524: Add failcode into forward_payment */
    {SQL("ALTER TABLE forwarded_payments ADD failcode INTEGER;"), NULL},
    /* remote signatures for channel announcement */
    {SQL("ALTER TABLE channels ADD remote_ann_node_sig BLOB;"), NULL},
    {SQL("ALTER TABLE channels ADD remote_ann_bitcoin_sig BLOB;"), NULL},
    /* Additional information for transaction tracking and listing */
    {SQL("ALTER TABLE transactions ADD type INTEGER;"), NULL},
    /* Not a foreign key on purpose since we still delete channels from
     * the DB which would remove this. It is mainly used to group payments
     * in the list view anyway, e.g., show all close and htlc transactions
     * as a single bundle. */
    {SQL("ALTER TABLE transactions ADD channel_id INTEGER;"), NULL},
    /* Convert pre-Adelaide short_channel_ids */
    {SQL("UPDATE channels"
	 " SET short_channel_id = REPLACE(short_channel_id, ':', 'x')"
	 " WHERE short_channel_id IS NOT NULL;"), NULL },
    {SQL("UPDATE payments SET failchannel = REPLACE(failchannel, ':', 'x')"
	 " WHERE failchannel IS NOT NULL;"), NULL },
};

/* Leak tracking. */
#if DEVELOPER
static void db_assert_no_outstanding_statements(struct db *db)
{
	struct db_stmt *stmt;

	stmt = list_top(&db->pending_statements, struct db_stmt, list);
	if (stmt)
		db_fatal("Unfinalized statement %s", stmt->location);
}
#else
static void db_assert_no_outstanding_statements(struct db *db)
{
}
#endif

static void db_stmt_free(struct db_stmt *stmt)
{
	if (stmt->inner_stmt)
		stmt->db->config->stmt_free_fn(stmt);
	assert(stmt->inner_stmt == NULL);
}

struct db_stmt *db_prepare_v2_(const char *location, struct db *db,
				     const char *query_id)
{
	struct db_stmt *stmt = tal(db, struct db_stmt);
	size_t num_slots;
	stmt->query = NULL;

	/* Normalize query_id paths, because unit tests are compiled with this
	 * prefix. */
	if (strncmp(query_id, "./", 2) == 0)
		query_id += 2;

	if (!db->in_transaction)
		db_fatal("Attempting to prepare a db_stmt outside of a "
			 "transaction: %s", location);

	/* Look up the query by its ID */
	for (size_t i = 0; i < db->config->num_queries; i++) {
		if (streq(query_id, db->config->queries[i].query)) {
			stmt->query = &db->config->queries[i];
			break;
		}
	}
	if (stmt->query == NULL)
		fatal("Could not resolve query %s", query_id);

	num_slots = stmt->query->placeholders;
	/* Allocate the slots for placeholders/bindings, zeroed next since
	 * that sets the type to DB_BINDING_UNINITIALIZED for later checks. */
	stmt->bindings = tal_arr(stmt, struct db_binding, num_slots);
	for (size_t i=0; i<num_slots; i++)
		stmt->bindings[i].type = DB_BINDING_UNINITIALIZED;

	stmt->location = location;
	stmt->error = NULL;
	stmt->db = db;
	stmt->executed = false;
	stmt->inner_stmt = NULL;

	tal_add_destructor(stmt, db_stmt_free);

	list_add(&db->pending_statements, &stmt->list);

	return stmt;
}

#define db_prepare_v2(db,query) \
	db_prepare_v2_(__FILE__ ":" stringify(__LINE__), db, query)

bool db_step(struct db_stmt *stmt)
{
	assert(stmt->executed);
	return stmt->db->config->step_fn(stmt);
}

u64 db_column_u64(struct db_stmt *stmt, int col)
{
	return stmt->db->config->column_u64_fn(stmt, col);
}

int db_column_int(struct db_stmt *stmt, int col)
{
	return stmt->db->config->column_int_fn(stmt, col);
}

size_t db_column_bytes(struct db_stmt *stmt, int col)
{
	return stmt->db->config->column_bytes_fn(stmt, col);
}

int db_column_is_null(struct db_stmt *stmt, int col)
{
	return stmt->db->config->column_is_null_fn(stmt, col);
}

const void *db_column_blob(struct db_stmt *stmt, int col)
{
	return stmt->db->config->column_blob_fn(stmt, col);
}

const unsigned char *db_column_text(struct db_stmt *stmt, int col)
{
	return stmt->db->config->column_blob_fn(stmt, col);
}

size_t db_count_changes(struct db_stmt *stmt)
{
	return stmt->db->config->count_changes_fn(stmt);
}

u64 db_last_insert_id_v2(struct db_stmt *stmt TAKES)
{
	u64 id;
	id = stmt->db->config->last_insert_id_fn(stmt);

	if (taken(stmt))
		tal_free(stmt);

	return id;
}

static void destroy_db(struct db *db)
{
	db_assert_no_outstanding_statements(db);

	if (db->config->teardown_fn)
		db->config->teardown_fn(db);
}

/* We expect min changes (ie. BEGIN TRANSACTION): report if more.
 * Optionally add "final" at the end (ie. COMMIT). */
static void db_report_changes(struct db *db, const char *final, size_t min)
{
	assert(db->changes);
	assert(tal_count(db->changes) >= min);

	if (tal_count(db->changes) > min)
		plugin_hook_db_sync(db, db->changes, final);
	db->changes = tal_free(db->changes);
}

static void db_prepare_for_changes(struct db *db)
{
	assert(!db->changes);
	db->changes = tal_arr(db, const char *, 0);
}

bool db_in_transaction(struct db *db)
{
	return db->in_transaction;
}

void db_begin_transaction_(struct db *db, const char *location)
{
	bool ok;
	if (db->in_transaction)
		db_fatal("Already in transaction from %s", db->in_transaction);

	db_prepare_for_changes(db);
	ok = db->config->begin_tx_fn(db);
	if (!ok)
		db_fatal("Failed to start DB transaction: %s", db->error);

	db->in_transaction = location;
}

void db_commit_transaction(struct db *db)
{
	bool ok;
	assert(db->in_transaction);
	db_assert_no_outstanding_statements(db);
	ok = db->config->commit_tx_fn(db);

	if (!ok)
		db_fatal("Failed to commit DB transaction: %s", db->error);

	db_report_changes(db, NULL, 0);
	db->in_transaction = NULL;
}

static void setup_open_db(struct db *db)
{
	/* This must be outside a transaction, so catch it */
	assert(!db->in_transaction);

	db_prepare_for_changes(db);
	if (db->config->setup_fn)
		db->config->setup_fn(db);
	db_report_changes(db, NULL, 0);
}

static struct db_config *db_config_find(const char *driver_name)
{
	size_t num_configs;
	struct db_config **configs = autodata_get(db_backends, &num_configs);
	for (size_t i=0; i<num_configs; i++)
		if (streq(driver_name, configs[i]->name))
			return configs[i];
	return NULL;
}

/**
 * db_open - Open or create a sqlite3 database
 */
static struct db *db_open(const tal_t *ctx, char *filename)
{
	int err;
	struct db *db;
	sqlite3 *sql;
	const char *driver_name = "sqlite3";

	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	err = sqlite3_open_v2(filename, &sql, flags, NULL);

	if (err != SQLITE_OK) {
		db_fatal("failed to open database %s: %s", filename,
			 sqlite3_errstr(err));
	}

	db = tal(ctx, struct db);
	db->filename = tal_strdup(db, filename);
	db->sql = sql;
	db->config = NULL;
	list_head_init(&db->pending_statements);

	db->config = db_config_find(driver_name);
	if (!db->config)
		db_fatal("Unable to find DB driver for %s", driver_name);

	// FIXME(cdecker) Once we parse DB connection strings this needs to be
	// instantiated correctly.
	db->conn = sql;

	tal_add_destructor(db, destroy_db);
	db->in_transaction = NULL;
	db->changes = NULL;

	setup_open_db(db);

	return db;
}

/**
 * db_get_version - Determine the current DB schema version
 *
 * Will attempt to determine the current schema version of the
 * database @db by querying the `version` table. If the table does not
 * exist it'll return schema version -1, so that migration 0 is
 * applied, which should create the `version` table.
 */
static int db_get_version(struct db *db)
{
	int res = -1;
	struct db_stmt *stmt = db_prepare_v2(db, SQL("SELECT version FROM version LIMIT 1"));
	if (!db_query_prepared(stmt)) {
		tal_free(stmt);
		return res;
	}

	if (db_step(stmt))
		res = db_column_u64(stmt, 0);

	tal_free(stmt);
	return res;
}

/**
 * db_migrate - Apply all remaining migrations from the current version
 */
static void db_migrate(struct lightningd *ld, struct db *db, struct log *log)
{
	/* Attempt to read the version from the database */
	int current, orig, available;
	struct db_stmt *stmt;

	db_begin_transaction(db);

	orig = current = db_get_version(db);
	available = ARRAY_SIZE(dbmigrations) - 1;

	if (current == -1)
		log_info(log, "Creating database");
	else if (available < current)
		db_fatal("Refusing to migrate down from version %u to %u",
			 current, available);
	else if (current != available)
		log_info(log, "Updating database from version %u to %u",
			 current, available);

	while (current < available) {
		current++;
		if (dbmigrations[current].sql) {
			struct db_stmt *stmt =
			    db_prepare_v2(db, dbmigrations[current].sql);
			db_exec_prepared_v2(stmt);
			tal_free(stmt);
		}
		if (dbmigrations[current].func)
			dbmigrations[current].func(ld, db);
	}

	/* Finally update the version number in the version table */
	stmt = db_prepare_v2(db, SQL("UPDATE version SET version=?;"));
	db_bind_u64(stmt, 0, available);
	db_exec_prepared_v2(stmt);
	tal_free(stmt);

	/* Annotate that we did upgrade, if any. */
	if (current != orig) {
		stmt = db_prepare_v2(
		    db, SQL("INSERT INTO db_upgrades VALUES (?, ?);"));
		db_bind_u64(stmt, 0, orig);
		db_bind_text(stmt, 1, version());
		db_exec_prepared_v2(stmt);
		tal_free(stmt);
	}

	db_commit_transaction(db);
}

struct db *db_setup(const tal_t *ctx, struct lightningd *ld, struct log *log)
{
	struct db *db = db_open(ctx, DB_FILE);

	db_migrate(ld, db, log);
	return db;
}

s64 db_get_intvar(struct db *db, char *varname, s64 defval)
{
	s64 res = defval;
	struct db_stmt *stmt = db_prepare_v2(
	    db, SQL("SELECT val FROM vars WHERE name= ? LIMIT 1"));
	db_bind_text(stmt, 0, varname);
	if (!db_query_prepared(stmt))
		goto done;

	if (db_step(stmt))
		res = atol((const char*)db_column_text(stmt, 0));

done:
	tal_free(stmt);
	return res;
}

void db_set_intvar(struct db *db, char *varname, s64 val)
{
	char *v = tal_fmt(NULL, "%"PRIi64, val);
	size_t changes;
	struct db_stmt *stmt = db_prepare_v2(db, SQL("UPDATE vars SET val=? WHERE name=?;"));
	db_bind_text(stmt, 0, v);
	db_bind_text(stmt, 1, varname);
	if (!db_exec_prepared_v2(stmt))
		db_fatal("Error executing update: %s", stmt->error);
	changes = db_count_changes(stmt);
	tal_free(stmt);

	if (changes == 0) {
		stmt = db_prepare_v2(db, SQL("INSERT INTO vars (name, val) VALUES (?, ?);"));
		db_bind_text(stmt, 0, varname);
		db_bind_text(stmt, 1, v);
		if (!db_exec_prepared_v2(stmt))
			db_fatal("Error executing insert: %s", stmt->error);
		tal_free(stmt);
	}
	tal_free(v);
}

/* Will apply the current config fee settings to all channels */
static void migrate_pr2342_feerate_per_channel(struct lightningd *ld, struct db *db)
{
	struct db_stmt *stmt = db_prepare_v2(
	    db, SQL("UPDATE channels SET feerate_base = ?, feerate_ppm = ?;"));

	db_bind_int(stmt, 0, ld->config.fee_base);
	db_bind_int(stmt, 1, ld->config.fee_per_satoshi);

	db_exec_prepared_v2(stmt);
	tal_free(stmt);
}

void db_bind_null(struct db_stmt *stmt, int pos)
{
	assert(pos < tal_count(stmt->bindings));
	stmt->bindings[pos].type = DB_BINDING_NULL;
}

void db_bind_int(struct db_stmt *stmt, int pos, int val)
{
	assert(pos < tal_count(stmt->bindings));
	stmt->bindings[pos].type = DB_BINDING_INT;
	stmt->bindings[pos].v.i = val;
}

void db_bind_u64(struct db_stmt *stmt, int pos, u64 val)
{
	assert(pos < tal_count(stmt->bindings));
	stmt->bindings[pos].type = DB_BINDING_UINT64;
	stmt->bindings[pos].v.u64 = val;
}

void db_bind_blob(struct db_stmt *stmt, int pos, const u8 *val, size_t len)
{
	assert(pos < tal_count(stmt->bindings));
	stmt->bindings[pos].type = DB_BINDING_BLOB;
	stmt->bindings[pos].v.blob = val;
	stmt->bindings[pos].len = len;
}

void db_bind_text(struct db_stmt *stmt, int pos, const char *val)
{
	assert(pos < tal_count(stmt->bindings));
	stmt->bindings[pos].type = DB_BINDING_TEXT;
	stmt->bindings[pos].v.text = val;
	stmt->bindings[pos].len = strlen(val);
}

void db_bind_preimage(struct db_stmt *stmt, int pos, const struct preimage *p)
{
	db_bind_blob(stmt, pos, p->r, sizeof(struct preimage));
}

void db_bind_sha256(struct db_stmt *stmt, int pos, const struct sha256 *s)
{
	db_bind_blob(stmt, pos, s->u.u8, sizeof(struct sha256));
}

void db_bind_sha256d(struct db_stmt *stmt, int pos, const struct sha256_double *s)
{
	db_bind_sha256(stmt, pos, &s->sha);
}

void db_bind_secret(struct db_stmt *stmt, int pos, const struct secret *s)
{
	assert(sizeof(s->data) == 32);
	db_bind_blob(stmt, pos, s->data, sizeof(s->data));
}

void db_bind_secret_arr(struct db_stmt *stmt, int col, const struct secret *s)
{
	size_t num = tal_count(s), elsize = sizeof(s->data);
	u8 *ser = tal_arr(stmt, u8, num * elsize);

	for (size_t i = 0; i < num; ++i)
		memcpy(ser + i * elsize, &s[i], elsize);

	db_bind_blob(stmt, col, ser, tal_count(ser));
}

void db_bind_txid(struct db_stmt *stmt, int pos, const struct bitcoin_txid *t)
{
	db_bind_sha256d(stmt, pos, &t->shad);
}

void db_bind_node_id(struct db_stmt *stmt, int pos, const struct node_id *id)
{
	db_bind_blob(stmt, pos, id->k, sizeof(id->k));
}

void db_bind_node_id_arr(struct db_stmt *stmt, int col,
			 const struct node_id *ids)
{
	/* Copy into contiguous array: ARM will add padding to struct node_id! */
	size_t n = tal_count(ids);
	u8 *arr = tal_arr(stmt, u8, n * sizeof(ids[0].k));

	for (size_t i = 0; i < n; ++i) {
		assert(node_id_valid(&ids[i]));
		memcpy(arr + sizeof(ids[i].k) * i,
		       ids[i].k,
		       sizeof(ids[i].k));
	}
	db_bind_blob(stmt, col, arr, tal_count(arr));
}

void db_bind_pubkey(struct db_stmt *stmt, int pos, const struct pubkey *pk)
{
	u8 *der = tal_arr(stmt, u8, PUBKEY_CMPR_LEN);
	pubkey_to_der(der, pk);
	db_bind_blob(stmt, pos, der, PUBKEY_CMPR_LEN);
}

void db_bind_short_channel_id(struct db_stmt *stmt, int col,
			      const struct short_channel_id *id)
{
	char *ser = short_channel_id_to_str(stmt, id);
	db_bind_text(stmt, col, ser);
}

void db_bind_short_channel_id_arr(struct db_stmt *stmt, int col,
				  const struct short_channel_id *id)
{
	u8 *ser = tal_arr(stmt, u8, 0);
	size_t num = tal_count(id);

	for (size_t i = 0; i < num; ++i)
		towire_short_channel_id(&ser, &id[i]);

	db_bind_blob(stmt, col, ser, tal_count(ser));
}

void db_bind_signature(struct db_stmt *stmt, int col,
		       const secp256k1_ecdsa_signature *sig)
{
	u8 *buf = tal_arr(stmt, u8, 64);
	int ret = secp256k1_ecdsa_signature_serialize_compact(secp256k1_ctx,
							      buf, sig);
	assert(ret == 1);
	db_bind_blob(stmt, col, buf, 64);
}

void db_bind_timeabs(struct db_stmt *stmt, int col, struct timeabs t)
{
	u64 timestamp =  t.ts.tv_nsec + (((u64) t.ts.tv_sec) * ((u64) NSEC_IN_SEC));
	db_bind_u64(stmt, col, timestamp);
}

void db_bind_tx(struct db_stmt *stmt, int col, const struct bitcoin_tx *tx)
{
	u8 *ser = linearize_tx(stmt, tx);
	assert(ser);
	db_bind_blob(stmt, col, ser, tal_count(ser));
}

void db_bind_amount_msat(struct db_stmt *stmt, int pos,
			 const struct amount_msat *msat)
{
	db_bind_u64(stmt, pos, msat->millisatoshis); /* Raw: low level function */
}

void db_bind_amount_sat(struct db_stmt *stmt, int pos,
			 const struct amount_sat *sat)
{
	db_bind_u64(stmt, pos, sat->satoshis); /* Raw: low level function */
}

void db_bind_json_escape(struct db_stmt *stmt, int pos,
			 const struct json_escape *esc)
{
	db_bind_text(stmt, pos, esc->s);
}

void db_column_preimage(struct db_stmt *stmt, int col,
			struct preimage *preimage)
{
	const u8 *raw;
	size_t size = sizeof(struct preimage);
	assert(db_column_bytes(stmt, col) == size);
	raw = db_column_blob(stmt, col);
	memcpy(preimage, raw, size);
}

void db_column_node_id(struct db_stmt *stmt, int col, struct node_id *dest)
{
	assert(db_column_bytes(stmt, col) == sizeof(dest->k));
	memcpy(dest->k, db_column_blob(stmt, col), sizeof(dest->k));
	assert(node_id_valid(dest));
}

struct node_id *db_column_node_id_arr(const tal_t *ctx, struct db_stmt *stmt,
				      int col)
{
	struct node_id *ret;
	size_t n = db_column_bytes(stmt, col) / sizeof(ret->k);
	const u8 *arr = db_column_blob(stmt, col);
	assert(n * sizeof(ret->k) == (size_t)db_column_bytes(stmt, col));
	ret = tal_arr(ctx, struct node_id, n);

	for (size_t i = 0; i < n; i++) {
		memcpy(ret[i].k, arr + i * sizeof(ret[i].k), sizeof(ret[i].k));
		if (!node_id_valid(&ret[i]))
			return tal_free(ret);
	}

	return ret;
}

void db_column_pubkey(struct db_stmt *stmt, int pos, struct pubkey *dest)
{
	bool ok;
	assert(db_column_bytes(stmt, pos) == PUBKEY_CMPR_LEN);
	ok = pubkey_from_der(db_column_blob(stmt, pos), PUBKEY_CMPR_LEN, dest);
	assert(ok);
}

bool db_column_short_channel_id(struct db_stmt *stmt, int col,
				struct short_channel_id *dest)
{
	const char *source = db_column_blob(stmt, col);
	size_t sourcelen = db_column_bytes(stmt, col);
	return short_channel_id_from_str(source, sourcelen, dest);
}

struct short_channel_id *
db_column_short_channel_id_arr(const tal_t *ctx, struct db_stmt *stmt, int col)
{
	const u8 *ser;
	size_t len;
	struct short_channel_id *ret;

	ser = db_column_blob(stmt, col);
	len = db_column_bytes(stmt, col);
	ret = tal_arr(ctx, struct short_channel_id, 0);

	while (len != 0) {
		struct short_channel_id scid;
		fromwire_short_channel_id(&ser, &len, &scid);
		tal_arr_expand(&ret, scid);
	}

	return ret;
}

bool db_column_signature(struct db_stmt *stmt, int col,
			 secp256k1_ecdsa_signature *sig)
{
	assert(db_column_bytes(stmt, col) == 64);
	return secp256k1_ecdsa_signature_parse_compact(
		   secp256k1_ctx, sig, db_column_blob(stmt, col)) == 1;
}

struct timeabs db_column_timeabs(struct db_stmt *stmt, int col)
{
	struct timeabs t;
	u64 timestamp = db_column_u64(stmt, col);
	t.ts.tv_sec = timestamp / NSEC_IN_SEC;
	t.ts.tv_nsec = timestamp % NSEC_IN_SEC;
	return t;

}

struct bitcoin_tx *db_column_tx(const tal_t *ctx, struct db_stmt *stmt, int col)
{
	const u8 *src = db_column_blob(stmt, col);
	size_t len = db_column_bytes(stmt, col);
	return pull_bitcoin_tx(ctx, &src, &len);
}

void *db_column_arr_(const tal_t *ctx, struct db_stmt *stmt, int col,
			  size_t bytes, const char *label, const char *caller)
{
	size_t sourcelen = db_column_bytes(stmt, col);
	void *p;

	if (db_column_is_null(stmt, col))
		return NULL;

	if (sourcelen % bytes != 0)
		db_fatal("%s: column size %zu not a multiple of %s (%zu)",
			 caller, sourcelen, label, bytes);

	p = tal_arr_label(ctx, char, sourcelen, label);
	memcpy(p, db_column_blob(stmt, col), sourcelen);
	return p;
}

void db_column_amount_msat(struct db_stmt *stmt, int col,
			   struct amount_msat *msat)
{
	msat->millisatoshis = db_column_u64(stmt, col); /* Raw: low level function */
}

void db_column_amount_sat(struct db_stmt *stmt, int col, struct amount_sat *sat)
{
	sat->satoshis = db_column_u64(stmt, col); /* Raw: low level function */
}

struct json_escape *db_column_json_escape(const tal_t *ctx,
					  struct db_stmt *stmt, int col)
{
	return json_escape_string_(ctx, db_column_blob(stmt, col),
				   db_column_bytes(stmt, col));
}

void db_column_sha256(struct db_stmt *stmt, int col, struct sha256 *sha)
{
	const u8 *raw;
	size_t size = sizeof(struct sha256);
	assert(db_column_bytes(stmt, col) == size);
	raw = db_column_blob(stmt, col);
	memcpy(sha, raw, size);
}

void db_column_sha256d(struct db_stmt *stmt, int col,
		       struct sha256_double *shad)
{
	const u8 *raw;
	size_t size = sizeof(struct sha256_double);
	assert(db_column_bytes(stmt, col) == size);
	raw = db_column_blob(stmt, col);
	memcpy(shad, raw, size);
}

void db_column_secret(struct db_stmt *stmt, int col, struct secret *s)
{
	const u8 *raw;
	assert(db_column_bytes(stmt, col) == sizeof(struct secret));
	raw = db_column_blob(stmt, col);
	memcpy(s, raw, sizeof(struct secret));
}

struct secret *db_column_secret_arr(const tal_t *ctx, struct db_stmt *stmt,
				    int col)
{
	return db_column_arr(ctx, stmt, col, struct secret);
}

void db_column_txid(struct db_stmt *stmt, int pos, struct bitcoin_txid *t)
{
	db_column_sha256d(stmt, pos, &t->shad);
}

bool db_exec_prepared_v2(struct db_stmt *stmt TAKES)
{
	bool ret = stmt->db->config->exec_fn(stmt);
	stmt->executed = true;
	list_del_from(&stmt->db->pending_statements, &stmt->list);

	/* The driver itself doesn't call `fatal` since we want to override it
	 * for testing. Instead we check here that the error message is set if
	 * we report an error. */
	if (!ret) {
		assert(stmt->error);
		db_fatal("Error executing statement: %s", stmt->error);
	}

	if (taken(stmt))
	    tal_free(stmt);

	return ret;
}

bool db_query_prepared(struct db_stmt *stmt)
{
	/* Make sure we don't accidentally execute a modifying query using a
	 * read-only path. */
	bool ret;
	assert(stmt->query->readonly);
	ret = stmt->db->config->query_fn(stmt);
	stmt->executed = true;
	list_del_from(&stmt->db->pending_statements, &stmt->list);
	return ret;
}

void db_changes_add(struct db_stmt *stmt, const char * expanded)
{
	struct db *db = stmt->db;

	if (stmt->query->readonly) {
		return;
	}
	/* We get a "COMMIT;" after we've sent our changes. */
	if (!db->changes) {
		assert(streq(expanded, "COMMIT;"));
		return;
	}

	tal_arr_expand(&db->changes, tal_strdup(db->changes, expanded));
}
