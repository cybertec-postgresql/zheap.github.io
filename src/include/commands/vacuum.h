/*-------------------------------------------------------------------------
 *
 * vacuum.h
 *	  header file for postgres vacuum cleaner and statistics analyzer
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/vacuum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUM_H
#define VACUUM_H

#include "access/amapi.h"
#include "access/htup.h"
#include "access/parallel.h"
#include "catalog/pg_class.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "executor/instrument.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "storage/buf.h"
#include "storage/lock.h"
#include "utils/relcache.h"

/*
 * Flags for amparallelvacuumoptions to control the participation of bulkdelete
 * and vacuumcleanup in parallel vacuum.
 */

/*
 * Both bulkdelete and vacuumcleanup are disabled by default.  This will be
 * used by IndexAM's that don't want to or cannot participate in parallel
 * vacuum.  For example, if an index AM doesn't have a way to communicate the
 * index statistics allocated by the first ambulkdelete call to the subsequent
 * ones until amvacuumcleanup, the index AM cannot participate in parallel
 * vacuum.
 */
#define VACUUM_OPTION_NO_PARALLEL			0

/*
 * bulkdelete can be performed in parallel.  This option can be used by
 * index AMs that need to scan indexes to delete tuples.
 */
#define VACUUM_OPTION_PARALLEL_BULKDEL		(1 << 0)

/*
 * vacuumcleanup can be performed in parallel if bulkdelete is not performed
 * yet.  This will be used by IndexAM's that can scan the index if the
 * bulkdelete is not performed.
 */
#define VACUUM_OPTION_PARALLEL_COND_CLEANUP	(1 << 1)

/*
 * vacuumcleanup can be performed in parallel even if bulkdelete has already
 * processed the index.  This will be used by IndexAM's that scan the index
 * during the cleanup phase of index irrespective of whether the index is
 * already scanned or not during bulkdelete phase.
 */
#define VACUUM_OPTION_PARALLEL_CLEANUP		(1 << 2)

/* value for checking vacuum flags */
#define VACUUM_OPTION_MAX_VALID_VALUE		((1 << 3) - 1)

/*
 * When a table has no indexes, vacuum the FSM after every 8GB, approximately
 * (it won't be exact because we only vacuum FSM after processing a heap/zheap
 * page that has some removable tuples).  When there are indexes, this is
 * ignored, and we vacuum FSM after each index/heap cleaning pass.
 */
#define VACUUM_FSM_EVERY_PAGES \
	((BlockNumber) (((uint64) 8 * 1024 * 1024 * 1024) / BLCKSZ))

/*----------
 * ANALYZE builds one of these structs for each attribute (column) that is
 * to be analyzed.  The struct and subsidiary data are in anl_context,
 * so they live until the end of the ANALYZE operation.
 *
 * The type-specific typanalyze function is passed a pointer to this struct
 * and must return true to continue analysis, false to skip analysis of this
 * column.  In the true case it must set the compute_stats and minrows fields,
 * and can optionally set extra_data to pass additional info to compute_stats.
 * minrows is its request for the minimum number of sample rows to be gathered
 * (but note this request might not be honored, eg if there are fewer rows
 * than that in the table).
 *
 * The compute_stats routine will be called after sample rows have been
 * gathered.  Aside from this struct, it is passed:
 *		fetchfunc: a function for accessing the column values from the
 *				   sample rows
 *		samplerows: the number of sample tuples
 *		totalrows: estimated total number of rows in relation
 * The fetchfunc may be called with rownum running from 0 to samplerows-1.
 * It returns a Datum and an isNull flag.
 *
 * compute_stats should set stats_valid true if it is able to compute
 * any useful statistics.  If it does, the remainder of the struct holds
 * the information to be stored in a pg_statistic row for the column.  Be
 * careful to allocate any pointed-to data in anl_context, which will NOT
 * be CurrentMemoryContext when compute_stats is called.
 *
 * Note: all comparisons done for statistical purposes should use the
 * underlying column's collation (attcollation), except in situations
 * where a noncollatable container type contains a collatable type;
 * in that case use the type's default collation.  Be sure to record
 * the appropriate collation in stacoll.
 *----------
 */
typedef struct VacAttrStats *VacAttrStatsP;

typedef Datum (*AnalyzeAttrFetchFunc) (VacAttrStatsP stats, int rownum,
									   bool *isNull);

typedef void (*AnalyzeAttrComputeStatsFunc) (VacAttrStatsP stats,
											 AnalyzeAttrFetchFunc fetchfunc,
											 int samplerows,
											 double totalrows);

typedef struct VacAttrStats
{
	/*
	 * These fields are set up by the main ANALYZE code before invoking the
	 * type-specific typanalyze function.
	 *
	 * Note: do not assume that the data being analyzed has the same datatype
	 * shown in attr, ie do not trust attr->atttypid, attlen, etc.  This is
	 * because some index opclasses store a different type than the underlying
	 * column/expression.  Instead use attrtypid, attrtypmod, and attrtype for
	 * information about the datatype being fed to the typanalyze function.
	 * Likewise, use attrcollid not attr->attcollation.
	 */
	Form_pg_attribute attr;		/* copy of pg_attribute row for column */
	Oid			attrtypid;		/* type of data being analyzed */
	int32		attrtypmod;		/* typmod of data being analyzed */
	Form_pg_type attrtype;		/* copy of pg_type row for attrtypid */
	Oid			attrcollid;		/* collation of data being analyzed */
	MemoryContext anl_context;	/* where to save long-lived data */

	/*
	 * These fields must be filled in by the typanalyze routine, unless it
	 * returns false.
	 */
	AnalyzeAttrComputeStatsFunc compute_stats;	/* function pointer */
	int			minrows;		/* Minimum # of rows wanted for stats */
	void	   *extra_data;		/* for extra type-specific data */

	/*
	 * These fields are to be filled in by the compute_stats routine. (They
	 * are initialized to zero when the struct is created.)
	 */
	bool		stats_valid;
	float4		stanullfrac;	/* fraction of entries that are NULL */
	int32		stawidth;		/* average width of column values */
	float4		stadistinct;	/* # distinct values */
	int16		stakind[STATISTIC_NUM_SLOTS];
	Oid			staop[STATISTIC_NUM_SLOTS];
	Oid			stacoll[STATISTIC_NUM_SLOTS];
	int			numnumbers[STATISTIC_NUM_SLOTS];
	float4	   *stanumbers[STATISTIC_NUM_SLOTS];
	int			numvalues[STATISTIC_NUM_SLOTS];
	Datum	   *stavalues[STATISTIC_NUM_SLOTS];

	/*
	 * These fields describe the stavalues[n] element types. They will be
	 * initialized to match attrtypid, but a custom typanalyze function might
	 * want to store an array of something other than the analyzed column's
	 * elements. It should then overwrite these fields.
	 */
	Oid			statypid[STATISTIC_NUM_SLOTS];
	int16		statyplen[STATISTIC_NUM_SLOTS];
	bool		statypbyval[STATISTIC_NUM_SLOTS];
	char		statypalign[STATISTIC_NUM_SLOTS];

	/*
	 * These fields are private to the main ANALYZE code and should not be
	 * looked at by type-specific functions.
	 */
	int			tupattnum;		/* attribute number within tuples */
	HeapTuple  *rows;			/* access info for std fetch function */
	TupleDesc	tupDesc;
	Datum	   *exprvals;		/* access info for index fetch function */
	bool	   *exprnulls;
	int			rowstride;
} VacAttrStats;

typedef enum VacuumOption
{
	VACOPT_VACUUM = 1 << 0,		/* do VACUUM */
	VACOPT_ANALYZE = 1 << 1,	/* do ANALYZE */
	VACOPT_VERBOSE = 1 << 2,	/* print progress info */
	VACOPT_FREEZE = 1 << 3,		/* FREEZE option */
	VACOPT_FULL = 1 << 4,		/* FULL (non-concurrent) vacuum */
	VACOPT_SKIP_LOCKED = 1 << 5,	/* skip if cannot get lock */
	VACOPT_PROCESS_TOAST = 1 << 6,	/* process the TOAST table, if any */
	VACOPT_SKIPTOAST = 1 << 7,	/* don't process the TOAST table, if any */
	VACOPT_DISABLE_PAGE_SKIPPING = 1 << 8	/* don't skip any pages */
} VacuumOption;

/*
 * Values used by index_cleanup and truncate params.
 *
 * VACOPTVALUE_UNSPECIFIED is used as an initial placeholder when VACUUM
 * command has no explicit value.  When that happens the final usable value
 * comes from the corresponding reloption (though the reloption default is
 * usually used).
 */
typedef enum VacOptValue
{
	VACOPTVALUE_UNSPECIFIED = 0,
	VACOPTVALUE_AUTO,
	VACOPTVALUE_DISABLED,
	VACOPTVALUE_ENABLED,
} VacOptValue;

/* Phases of vacuum during which we report error context. */
typedef enum
{
	VACUUM_ERRCB_PHASE_UNKNOWN,
	VACUUM_ERRCB_PHASE_SCAN_HEAP,
	VACUUM_ERRCB_PHASE_VACUUM_INDEX,
	VACUUM_ERRCB_PHASE_VACUUM_HEAP,
	VACUUM_ERRCB_PHASE_INDEX_CLEANUP,
	VACUUM_ERRCB_PHASE_TRUNCATE
} VacErrPhase;

/* Struct for saving and restoring vacuum error information. */
typedef struct LVSavedErrInfo
{
	BlockNumber blkno;
	OffsetNumber offnum;
	VacErrPhase phase;
} LVSavedErrInfo;

/*
 * Parameters customizing behavior of VACUUM and ANALYZE.
 *
 * Note that at least one of VACOPT_VACUUM and VACOPT_ANALYZE must be set
 * in options.
 */
typedef struct VacuumParams
{
	int			options;		/* bitmask of VacuumOption */
	int			freeze_min_age; /* min freeze age, -1 to use default */
	int			freeze_table_age;	/* age at which to scan whole table */
	int			multixact_freeze_min_age;	/* min multixact freeze age, -1 to
											 * use default */
	int			multixact_freeze_table_age; /* multixact age at which to scan
											 * whole table */
	bool		is_wraparound;	/* force a for-wraparound vacuum */
	int			log_min_duration;	/* minimum execution threshold in ms at
									 * which  verbose logs are activated, -1
									 * to use default */
	VacOptValue index_cleanup;	/* Do index vacuum and cleanup */
	VacOptValue truncate;		/* Truncate empty pages at the end */

	/*
	 * The number of parallel vacuum workers.  0 by default which means choose
	 * based on the number of indexes.  -1 indicates parallel vacuum is
	 * disabled.
	 */
	int			nworkers;
} VacuumParams;

/*
 * Shared information among parallel workers.  So this is allocated in the DSM
 * segment.
 */
typedef struct LVShared
{
	/*
	 * Target table relid and log level.  These fields are not modified during
	 * the lazy vacuum.
	 */
	Oid			relid;
	int			elevel;

	/*
	 * An indication for vacuum workers to perform either index vacuum or
	 * index cleanup.  first_time is true only if for_cleanup is true and
	 * bulk-deletion is not performed yet.
	 */
	bool		for_cleanup;
	bool		first_time;

	/*
	 * Fields for both index vacuum and cleanup.
	 *
	 * reltuples is the total number of input heap tuples.  We set either old
	 * live tuples in the index vacuum case or the new live tuples in the
	 * index cleanup case.
	 *
	 * estimated_count is true if reltuples is an estimated value.  (Note that
	 * reltuples could be -1 in this case, indicating we have no idea.)
	 */
	double		reltuples;
	bool		estimated_count;

	/*
	 * In single process lazy vacuum we could consume more memory during index
	 * vacuuming or cleanup apart from the memory for heap scanning.  In
	 * parallel vacuum, since individual vacuum workers can consume memory
	 * equal to maintenance_work_mem, the new maintenance_work_mem for each
	 * worker is set such that the parallel operation doesn't consume more
	 * memory than single process lazy vacuum.
	 */
	int			maintenance_work_mem_worker;

	/*
	 * Shared vacuum cost balance.  During parallel vacuum,
	 * VacuumSharedCostBalance points to this value and it accumulates the
	 * balance of each parallel vacuum worker.
	 */
	pg_atomic_uint32 cost_balance;

	/*
	 * Number of active parallel workers.  This is used for computing the
	 * minimum threshold of the vacuum cost balance before a worker sleeps for
	 * cost-based delay.
	 */
	pg_atomic_uint32 active_nworkers;

	/*
	 * Variables to control parallel vacuum.  We have a bitmap to indicate
	 * which index has stats in shared memory.  The set bit in the map
	 * indicates that the particular index supports a parallel vacuum.
	 */
	pg_atomic_uint32 idx;		/* counter for vacuuming and clean up */
	uint32		offset;			/* sizeof header incl. bitmap */
	bits8		bitmap[FLEXIBLE_ARRAY_MEMBER];	/* bit map of NULLs */

	/* Shared index statistics data follows at end of struct */
} LVShared;

#define SizeOfLVShared (offsetof(LVShared, bitmap) + sizeof(bits8))

#define GetSharedIndStats(s) \
	((LVSharedIndStats *)((char *)(s) + ((LVShared *)(s))->offset))
#define IndStatsIsNull(s, i) \
	(!(((LVShared *)(s))->bitmap[(i) >> 3] & (1 << ((i) & 0x07))))

/*
 * Struct for an index bulk-deletion statistic used for parallel vacuum.  This
 * is allocated in the DSM segment.
 */
typedef struct LVSharedIndStats
{
	bool		updated;		/* are the stats updated? */
	IndexBulkDeleteResult istat;
} LVSharedIndStats;

/*
 * Macro to check if we are in a parallel vacuum.  If true, we are in the
 * parallel mode and the DSM segment is initialized.
 */
#define ParallelVacuumIsActive(vacrel) ((vacrel)->lps != NULL)

/* Struct for maintaining a parallel vacuum state. */
typedef struct LVParallelState
{
	ParallelContext *pcxt;

	/* Shared information among parallel vacuum workers */
	LVShared   *lvshared;

	/* Points to buffer usage area in DSM */
	BufferUsage *buffer_usage;

	/* Points to WAL usage area in DSM */
	WalUsage   *wal_usage;

	/*
	 * The number of indexes that support parallel index bulk-deletion and
	 * parallel index cleanup respectively.
	 */
	int			nindexes_parallel_bulkdel;
	int			nindexes_parallel_cleanup;
	int			nindexes_parallel_condcleanup;
} LVParallelState;

/*
 * DSM keys for parallel vacuum.  Unlike other parallel execution code, since
 * we don't need to worry about DSM keys conflicting with plan_node_id we can
 * use small integers.
 */
#define PARALLEL_VACUUM_KEY_SHARED			1
#define PARALLEL_VACUUM_KEY_DEAD_TUPLES		2
#define PARALLEL_VACUUM_KEY_QUERY_TEXT		3
#define PARALLEL_VACUUM_KEY_BUFFER_USAGE	4
#define PARALLEL_VACUUM_KEY_WAL_USAGE		5

/*
 * LVDeadTuples stores the dead tuple TIDs collected during the heap scan.
 * This is allocated in the DSM segment in parallel mode and in local memory
 * in non-parallel mode.
 */
typedef struct LVDeadTuples
{
	int			max_tuples;		/* # slots allocated in array */
	int			num_tuples;		/* current # of entries */
	/* List of TIDs of tuples we intend to delete */
	/* NB: this list is ordered by TID address */
	ItemPointerData itemptrs[FLEXIBLE_ARRAY_MEMBER];	/* array of
														 * ItemPointerData */
} LVDeadTuples;

/* The dead tuple space consists of LVDeadTuples and dead tuple TIDs */
#define SizeOfDeadTuples(cnt) \
	add_size(offsetof(LVDeadTuples, itemptrs), \
			 mul_size(sizeof(ItemPointerData), cnt))
#define MAXDEADTUPLES(max_size) \
		(((max_size) - offsetof(LVDeadTuples, itemptrs)) / sizeof(ItemPointerData))

typedef struct LVRelState
{
	/* Target heap relation and its indexes */
	Relation	rel;
	Relation   *indrels;
	int			nindexes;
	/* Do index vacuuming/cleanup? */

	/* Wraparound failsafe has been triggered? */
	bool		failsafe_active;
	/* Consider index vacuuming bypass optimization? */
	bool		consider_bypass_optimization;

	/* Doing index vacuuming, index cleanup, rel truncation? */
	bool		do_index_vacuuming;
	bool		do_index_cleanup;
	bool		do_rel_truncate;

	/* Buffer access strategy and parallel state */
	BufferAccessStrategy bstrategy;
	LVParallelState *lps;

	/* Statistics from pg_class when we start out */
	BlockNumber old_rel_pages;	/* previous value of pg_class.relpages */
	double		old_live_tuples;	/* previous value of pg_class.reltuples */
	/* rel's initial relfrozenxid and relminmxid */
	TransactionId relfrozenxid;
	MultiXactId relminmxid;

	/* VACUUM operation's cutoff for pruning */
	TransactionId OldestXmin;
	/* VACUUM operation's cutoff for freezing XIDs and MultiXactIds */
	TransactionId FreezeLimit;
	MultiXactId MultiXactCutoff;

	/* Error reporting state */
	char	   *relnamespace;
	char	   *relname;
	char	   *indname;
	BlockNumber blkno;			/* used only for heap operations */
	OffsetNumber offnum;		/* used only for heap operations */
	VacErrPhase phase;

	/*
	 * State managed by lazy_scan_heap() follows
	 */
	LVDeadTuples *dead_tuples;	/* items to vacuum from indexes */
	BlockNumber rel_pages;		/* total number of pages */
	BlockNumber scanned_pages;	/* number of pages we examined */
	BlockNumber pinskipped_pages;	/* # of pages skipped due to a pin */
	BlockNumber frozenskipped_pages;	/* # of frozen pages we skipped */
	BlockNumber tupcount_pages; /* pages whose tuples we counted */
	BlockNumber pages_removed;	/* pages remove by truncation */
	BlockNumber lpdead_item_pages;	/* # pages with LP_DEAD items */
	BlockNumber nonempty_pages; /* actually, last nonempty page + 1 */

	/* Statistics output by us, for table */
	double		new_rel_tuples; /* new estimated total # of tuples */
	double		new_live_tuples;	/* new estimated total # of live tuples */
	/* Statistics output by index AMs */
	IndexBulkDeleteResult **indstats;

	/* Instrumentation counters */
	int			num_index_scans;
	int64		tuples_deleted; /* # deleted from table */
	int64		lpdead_items;	/* # deleted from indexes */
	int64		new_dead_tuples;	/* new estimated total # of dead items in
									 * table */
	int64		num_tuples;		/* total number of nonremovable tuples */
	int64		live_tuples;	/* live tuples (reltuples estimate) */
} LVRelState;

/* GUC parameters */
extern PGDLLIMPORT int default_statistics_target;	/* PGDLLIMPORT for PostGIS */
extern int	vacuum_freeze_min_age;
extern int	vacuum_freeze_table_age;
extern int	vacuum_multixact_freeze_min_age;
extern int	vacuum_multixact_freeze_table_age;
extern int	vacuum_failsafe_age;
extern int	vacuum_multixact_failsafe_age;

/* Variables for cost-based parallel vacuum */
extern pg_atomic_uint32 *VacuumSharedCostBalance;
extern pg_atomic_uint32 *VacuumActiveNWorkers;
extern int	VacuumCostBalanceLocal;


/* in commands/vacuum.c */
extern void ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt, bool isTopLevel);
extern void vacuum(List *relations, VacuumParams *params,
				   BufferAccessStrategy bstrategy, bool isTopLevel);
extern void vac_open_indexes(Relation relation, LOCKMODE lockmode,
							 int *nindexes, Relation **Irel);
extern void vac_close_indexes(int nindexes, Relation *Irel, LOCKMODE lockmode);
extern double vac_estimate_reltuples(Relation relation,
									 BlockNumber total_pages,
									 BlockNumber scanned_pages,
									 double scanned_tuples);
extern void vac_update_relstats(Relation relation,
								BlockNumber num_pages,
								double num_tuples,
								BlockNumber num_all_visible_pages,
								bool hasindex,
								TransactionId frozenxid,
								MultiXactId minmulti,
								bool in_outer_xact);
extern void vacuum_set_xid_limits(Relation rel,
								  int freeze_min_age, int freeze_table_age,
								  int multixact_freeze_min_age,
								  int multixact_freeze_table_age,
								  TransactionId *oldestXmin,
								  TransactionId *freezeLimit,
								  TransactionId *xidFullScanLimit,
								  MultiXactId *multiXactCutoff,
								  MultiXactId *mxactFullScanLimit);
extern bool vacuum_xid_failsafe_check(TransactionId relfrozenxid,
									  MultiXactId relminmxid);
extern void vac_update_datfrozenxid(void);
extern void vacuum_delay_point(void);
extern bool vacuum_is_relation_owner(Oid relid, Form_pg_class reltuple,
									 bits32 options);
extern Relation vacuum_open_relation(Oid relid, RangeVar *relation,
									 bits32 options, bool verbose, LOCKMODE lmode);

/* in commands/analyze.c */
extern void analyze_rel(Oid relid, RangeVar *relation,
						VacuumParams *params, List *va_cols, bool in_outer_xact,
						BufferAccessStrategy bstrategy);
extern bool std_typanalyze(VacAttrStats *stats);

/* in utils/misc/sampling.c --- duplicate of declarations in utils/sampling.h */
extern double anl_random_fract(void);
extern double anl_init_selection_state(int n);
extern double anl_get_next_S(double t, int n, double *stateptr);

#endif							/* VACUUM_H */
