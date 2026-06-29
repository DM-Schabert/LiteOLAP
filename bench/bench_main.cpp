/// Analytical benchmark for LiteOLAP. Builds a small TPC-H-style data set and
/// times three classic OLAP shapes: a filtered scan+aggregate, a GROUP BY,
/// and a join feeding an aggregate. Also reports the on-disk footprint and
/// per-column compression versus a plain row encoding.
///
/// DuckDB comparison is opt-in at build time:
///   cmake -B build -DLITEOLAP_WITH_DUCKDB=ON
/// Without it, the binary reports LiteOLAP's own numbers (the engine pulls in
/// no external database dependency).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>

#include "db.h"

#if defined(LITEOLAP_WITH_DUCKDB)
#include "duckdb.hpp"
#endif

using namespace liteolap;
using namespace std::chrono;

namespace {

constexpr int kLineitems = 1'000'000;
constexpr int kParts = 50'000;

double Seconds(steady_clock::time_point a, steady_clock::time_point b) {
    return duration<double>(b - a).count();
}

std::string Brand(int i) { return "Brand#" + std::to_string(i % 25); }
std::string Flag(int i) { return (i % 4 == 0) ? "R" : (i % 4 == 1 ? "A" : "N"); }

}  // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() / "liteolap_bench.loap";
    std::filesystem::remove(path);

    std::cout << "LiteOLAP benchmark — " << kLineitems << " lineitems, " << kParts << " parts\n";

    auto db = DB::Open(path);
    db->Execute(
        "CREATE TABLE lineitem (orderkey BIGINT, partkey INT, quantity INT, "
        "extendedprice FLOAT, discount FLOAT, returnflag VARCHAR(1))");
    db->Execute("CREATE TABLE part (partkey INT, brand VARCHAR(16))");

    std::mt19937_64 rng(123);

    constexpr int kBatchSize = 1000;

    auto t0 = steady_clock::now();
    for (int base = 0; base < kLineitems; base += kBatchSize) {
        const int end = std::min(base + kBatchSize, kLineitems);
        std::string sql = "INSERT INTO lineitem VALUES ";
        for (int i = base; i < end; ++i) {
            if (i > base) sql += ", ";
            const int partkey = static_cast<int>(rng() % kParts);
            const int qty = static_cast<int>(rng() % 50) + 1;
            const double price = static_cast<double>(rng() % 100000) / 100.0;
            const double disc = static_cast<double>(rng() % 10) / 100.0;
            sql += "(" + std::to_string(i) + ", " + std::to_string(partkey) + ", " +
                   std::to_string(qty) + ", " + std::to_string(price) + ", " +
                   std::to_string(disc) + ", '" + Flag(i) + "')";
        }
        db->Execute(sql);
    }
    for (int base = 0; base < kParts; base += kBatchSize) {
        const int end = std::min(base + kBatchSize, kParts);
        std::string sql = "INSERT INTO part VALUES ";
        for (int i = base; i < end; ++i) {
            if (i > base) sql += ", ";
            sql += "(" + std::to_string(i) + ", '" + Brand(i) + "')";
        }
        db->Execute(sql);
    }
    auto t1 = steady_clock::now();
    const double load_rps = (kLineitems + kParts) / Seconds(t0, t1);

    // Force the columnar flush so the first query doesn't pay for it.
    db->Execute("SELECT COUNT(*) FROM lineitem");
    db->Execute("SELECT COUNT(*) FROM part");

    auto time_query = [&](const std::string& sql, int reps) {
        auto a = steady_clock::now();
        std::size_t rows = 0;
        for (int r = 0; r < reps; ++r) rows = db->Execute(sql).rows.size();
        auto b = steady_clock::now();
        return std::make_pair(Seconds(a, b) / reps, rows);
    };

    const std::string q1 =
        "SELECT returnflag, COUNT(*), SUM(extendedprice), AVG(discount) "
        "FROM lineitem WHERE quantity > 25 GROUP BY returnflag";
    const std::string q2 =
        "SELECT returnflag, SUM(extendedprice) AS total FROM lineitem "
        "GROUP BY returnflag ORDER BY total DESC";
    const std::string q3 =
        "SELECT p.brand, SUM(l.extendedprice) AS total FROM lineitem l, part p "
        "WHERE l.partkey = p.partkey GROUP BY p.brand";

    auto r1 = time_query(q1, 5);
    auto r2 = time_query(q2, 5);
    auto r3 = time_query(q3, 3);

    db->Close();
    const auto file_bytes = std::filesystem::file_size(path);
    // A plain row store of the same data: 8+4+4+8+8+1 bytes/lineitem row + parts.
    const double plain_bytes =
        static_cast<double>(kLineitems) * (8 + 4 + 4 + 8 + 8 + 1) +
        static_cast<double>(kParts) * (4 + 16);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\nLoad:           " << load_rps << " rows/sec\n";
    std::cout << "Q1 scan+agg:    " << r1.first * 1000 << " ms  (" << r1.second << " groups)\n";
    std::cout << "Q2 group+sort:  " << r2.first * 1000 << " ms  (" << r2.second << " groups)\n";
    std::cout << "Q3 join+agg:    " << r3.first * 1000 << " ms  (" << r3.second << " groups)\n";
    std::cout << "On-disk size:   " << file_bytes / (1024.0 * 1024.0) << " MiB\n";
    std::cout << "Plain estimate: " << plain_bytes / (1024.0 * 1024.0) << " MiB  (ratio "
              << plain_bytes / static_cast<double>(file_bytes) << "x)\n";

#if defined(LITEOLAP_WITH_DUCKDB)
    std::cout << "\n--- DuckDB comparison ---\n";
    duckdb::DuckDB ddb(nullptr);
    duckdb::Connection con(ddb);
    con.Query(
        "CREATE TABLE lineitem (orderkey BIGINT, partkey INT, quantity INT, "
        "extendedprice DOUBLE, discount DOUBLE, returnflag VARCHAR)");
    con.Query("CREATE TABLE part (partkey INT, brand VARCHAR)");
    duckdb::Appender la(con, "lineitem");
    std::mt19937_64 rng2(123);
    for (int i = 0; i < kLineitems; ++i) {
        const int partkey = static_cast<int>(rng2() % kParts);
        const int qty = static_cast<int>(rng2() % 50) + 1;
        const double price = static_cast<double>(rng2() % 100000) / 100.0;
        const double disc = static_cast<double>(rng2() % 10) / 100.0;
        la.AppendRow(static_cast<int64_t>(i), partkey, qty, price, disc, duckdb::Value(Flag(i)));
    }
    la.Close();
    duckdb::Appender pa(con, "part");
    for (int i = 0; i < kParts; ++i) pa.AppendRow(i, duckdb::Value(Brand(i)));
    pa.Close();

    auto dtime = [&](const std::string& sql, int reps) {
        auto a = steady_clock::now();
        for (int r = 0; r < reps; ++r) con.Query(sql);
        auto b = steady_clock::now();
        return Seconds(a, b) / reps * 1000;
    };
    std::cout << "Q1: " << dtime(q1, 5) << " ms\n";
    std::cout << "Q2: " << dtime(q2, 5) << " ms\n";
    std::cout << "Q3: " << dtime(q3, 3) << " ms\n";
#endif

    std::filesystem::remove(path);
    return 0;
}
