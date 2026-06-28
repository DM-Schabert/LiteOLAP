# LiteOLAP

A vectorized, column-store **analytical** database engine in C++17, written
from scratch with no external database library. It is the OLAP counterpart to
[LiteQueryDB](../LiteQueryDB) (a row-store OLTP engine): same "build it
yourself to understand it" philosophy, opposite workload. Where LiteQueryDB
optimizes point lookups, LiteOLAP optimizes scans, aggregations and joins over
many rows at once.

```
SQL ─► Lexer ─► Parser ─► AST ─► Planner ─► Vectorized operator tree ─► ResultSet
                                                  │
              ┌───────────────────┬──────────────┼───────────────┬───────────────┐
              ▼                   ▼              ▼               ▼               ▼
         ColumnScan           Filter        HashAggregate     HashJoin        OrderBy
        (+ zone maps)     (vectorized)      (GROUP BY)      (build/probe)    (sort)
              │
              ▼
        ColumnReader ─► decode chunk ─► Vector (2048 values)
              │
              ▼
        PagedInputStream ─► BufferPool ─► DiskManager ─► .loap file
              ▲
        encoding: plain / RLE / dictionary / bit-packing
```

## What makes it OLAP

| Dimension      | LiteQueryDB (OLTP)        | LiteOLAP (this)                         |
|----------------|---------------------------|-----------------------------------------|
| Storage        | row-oriented slotted pages| **column-oriented chunks**              |
| Compression    | none                      | **RLE, dictionary, bit-packing**        |
| Execution      | Volcano (one tuple/call)  | **vectorized (2048 values/call)**       |
| Pruning        | B+-tree point lookups     | **zone maps (min/max per chunk)**       |
| Joins          | nested-loop               | **hash join**                           |
| Aggregation    | —                         | **COUNT/SUM/AVG/MIN/MAX + GROUP BY**    |

The storage foundation (`DiskManager`, `BufferPool`, 4 KiB `Page`), the
`Schema`/`Value` types, and the SQL lexer/parser skeleton are adapted from
LiteQueryDB; everything from the column layer up is new.

## Build & test

Requires CMake ≥ 3.20 and a C++17 compiler. Builds cleanly under
`-Wall -Wextra -Werror -Wpedantic`.

```bash
cmake -B build
cmake --build build -j
cd build && ctest --output-on-failure   # 45 tests
./liteolap_bench                        # analytical benchmark
```

The default build has **no external dependency** beyond Catch2 (fetched
automatically for tests). To compare against DuckDB in the benchmark:

```bash
cmake -B build -DLITEOLAP_WITH_DUCKDB=ON   # requires duckdb findable by CMake
cmake --build build -j && ./build/liteolap_bench
```

## Architecture

| Layer        | Files                                                | Responsibility                                  |
|--------------|------------------------------------------------------|-------------------------------------------------|
| Storage      | `storage/{disk_manager,buffer_pool,paged_stream}`    | 4 KiB pages, LRU cache, byte streams over pages |
| Compression  | `compression/encoding`                               | plain / RLE / dictionary / bit-packing + chooser|
| Column       | `column/{column_chunk,column_writer,column_reader}`  | encoded chunks in a column's paged stream       |
| Vector       | `vector/{vector,data_chunk}`                          | typed batches of ≤2048 values + validity        |
| Catalog      | `catalog/{schema,catalog}`                            | tables → row groups → per-column stream handles |
| Execution    | `execution/*`                                         | vectorized operators (scan/filter/agg/join/…)   |
| SQL          | `sql/{lexer,parser,ast}`                              | recursive-descent parse to AST                  |
| Planner      | `planner/planner`                                    | AST → operator tree (pushdown, join, aggregate) |
| Façade       | `db`                                                  | `DB::Open / Execute / Close`                     |

### Storage model

A table is a list of **row groups**; each row group has one paged byte stream
per column. Within a stream, data is a sequence of **chunks** of
`kChunkRows = 2048` rows. Because every column flushes on the same row
boundary, chunk *i* of every column covers the same rows — that alignment is
what lets a multi-column scan stay row-consistent while reading columns
independently. Each chunk header carries its encoding and, for integer
columns, a **zone map** (`min`/`max`) used to skip chunks a predicate can't
match.

### Vectorized execution

Operators pull `DataChunk`s — a set of column `Vector`s sharing a cardinality —
rather than one tuple at a time. The data arrives already decoded into
contiguous typed arrays, so filters and aggregations run as tight loops over
cache-resident memory (the `Filter` fast path specializes `col CMP literal`
into a typed compare-and-mask). This batch-at-a-time model is the core reason
column engines outrun tuple-at-a-time engines on analytical queries.

## Supported SQL

```sql
CREATE TABLE sales (region VARCHAR(16), amount BIGINT, ts BIGINT);
INSERT INTO sales VALUES ('EU', 99, 1700000000);
TRUNCATE TABLE sales;
DROP TABLE sales;

SELECT COUNT(*) FROM sales;
SELECT region, COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount)
  FROM sales
  WHERE amount > 50
  GROUP BY region
  HAVING SUM(amount) > 1000
  ORDER BY region DESC
  LIMIT 10;

SELECT * FROM sales WHERE ts BETWEEN 1700000000 AND 1700100000;
SELECT * FROM sales WHERE region IN ('EU', 'US', 'APAC');

SELECT p.brand, SUM(l.amount) AS total
  FROM sales l, parts p
  WHERE l.pid = p.id
  GROUP BY p.brand;
```

Types: `INT`, `BIGINT`, `FLOAT`, `VARCHAR(n)`, plus `NULL` throughout.
Aggregates: `COUNT(*)`, `COUNT(col)`, `SUM`, `AVG`, `MIN`, `MAX`.

## Benchmark results

TPC-H-style schema, 1,000,000 lineitems + 50,000 parts, Apple-silicon laptop,
Release build. Per-row INSERT through the SQL layer; queries averaged over
several runs.

| Metric              | LiteOLAP        |
|---------------------|-----------------|
| Bulk load           | ~766,000 rows/s |
| Q1 scan + aggregate | ~71 ms          |
| Q2 group + sort     | ~68 ms          |
| Q3 join + aggregate | ~181 ms         |
| On-disk size        | 13.3 MiB        |
| vs. plain row store | 2.4× smaller    |

Q1 = `SUM/AVG ... WHERE quantity > 25 GROUP BY returnflag`,
Q2 = `SUM ... GROUP BY returnflag ORDER BY total DESC`,
Q3 = `join lineitem×part, GROUP BY brand`. Build with `-DLITEOLAP_WITH_DUCKDB=ON`
to print a side-by-side DuckDB column.

Honest reading: DuckDB will be substantially faster — it has SIMD kernels,
parallelism, a cost-based optimizer, and far more mature encodings. LiteOLAP's
point is to demonstrate the *mechanics* (columnar storage, vectorized
operators, compression, zone maps, hash aggregation/join) at readable scale,
not to compete on absolute speed.

## Known limitations

Deliberate scope cuts, in roughly the order they'd be worth addressing:

- **No SIMD / no parallelism.** Operators are scalar and single-threaded.
- **Per-row INSERT only; bulk-load is buffered in memory** and flushed to one
  row group per table at query/close time. No `COPY`/Appender API yet.
- **Row-level DELETE/UPDATE are unsupported** — only `TRUNCATE TABLE`. This is
  the natural column-store stance (no deletion vectors implemented).
- **B+-tree-free**: there is no secondary index; pruning relies on zone maps,
  which only help when a column is clustered/sorted.
- **Zone-map pushdown is single-column, single-table, integer-only.**
- **Joins are two-table inner equi-joins** with a fixed build side (the right
  table); no join reordering, no outer joins.
- **No cost-based optimizer / no statistics.**
- **Catalog must fit in one 4 KiB page.**
- **Dropped/truncated data is not reclaimed** on disk (no vacuum).

## Design decisions

**Column storage over row storage.** Analytical queries touch few columns of
many rows. Storing each column contiguously means a `SUM(amount)` reads only
the `amount` bytes — not whole rows — and the values arrive packed for tight
loops and good compression. The cost is that reconstructing a full row is
expensive, which is exactly the OLTP operation we don't care about here.

**Chunks in a paged byte stream.** Rather than force a column chunk to fit one
page, a column is a byte *stream* spread across a chain of 4 KiB pages
(`PagedOutputStream`/`PagedInputStream`). This kept the 4 KiB page layer
identical to the OLTP engine (full reuse) while letting a 2048-row chunk be
whatever size its encoding produces. Chunk headers are read before payloads so
zone-map pruning can `Skip` a chunk's bytes without decoding them.

**Vectorized over tuple-at-a-time execution.** Each operator moves a
`DataChunk` of up to 2048 values. Decoded columnar batches let the filter and
aggregation loops run over contiguous typed arrays — branch-predictable and
auto-vectorizable — instead of chasing one boxed tuple at a time through
virtual calls. This is the single biggest architectural difference from
LiteQueryDB's Volcano model and the main source of analytical throughput.

**Per-chunk encoding chosen by trial.** At flush time each chunk is encoded
with every applicable scheme (plain, RLE, dictionary, bit-packing) and the
smallest result wins. Dictionary indices and frame-of-reference integers are
bit-packed at the minimum width. Encoding decisions are local to a chunk, so a
column whose distribution shifts over its length adapts automatically — and
decoding is dispatch-free per chunk because the header names the scheme.

**Hash join + hash aggregation over sort-based.** Both build an in-memory hash
table keyed by a serialized key. Hashing gives O(n+m) joins and single-pass
grouping, which suits the unsorted, append-oriented data a column store
ingests. Sort-based variants would win when inputs are already ordered, which
we don't currently track.

**Append-only with row groups.** Inserts accumulate in a column-wise memory
buffer and flush to a new immutable row group — the Parquet/segment model. It
makes writes cheap, keeps each row group internally consistent for encoding
and zone maps, and sidesteps in-place mutation of compressed data (the reason
row-level delete/update are out of scope).
