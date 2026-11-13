// RAII wrappers over the SQLite C API. Every sqlite3* and sqlite3_stmt* is
// owned by exactly one object and released on every path, including errors.
#pragma once
#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/result.hpp"

namespace hm::sql {

struct DbCloser {
  void operator()(sqlite3* db) const noexcept { sqlite3_close_v2(db); }
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;

struct StmtFinalizer {
  void operator()(sqlite3_stmt* s) const noexcept { sqlite3_finalize(s); }
};

inline Error sqlite_error(sqlite3* db, int rc, std::string_view what) {
  std::string code = "sqlite";
  const int primary = rc & 0xff;
  if (primary == SQLITE_FULL) code = "sqlite_full";
  else if (primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB) code = "sqlite_corrupt";
  else if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) code = "sqlite_busy";
  else if (primary == SQLITE_READONLY) code = "sqlite_readonly";
  else if (primary == SQLITE_IOERR) code = "sqlite_ioerr";
  else if (primary == SQLITE_CANTOPEN) code = "sqlite_cantopen";
  std::string msg(what);
  msg += ": ";
  msg += db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
  return make_error(std::move(code), std::move(msg));
}

class Stmt {
 public:
  Stmt() = default;
  static Result<Stmt> prepare(sqlite3* db, std::string_view sql) {
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
    if (rc != SQLITE_OK) return sqlite_error(db, rc, "prepare");
    Stmt s;
    s.db_ = db;
    s.stmt_.reset(raw);
    return s;
  }

  Stmt& bind(int i, std::int64_t v) { sqlite3_bind_int64(stmt_.get(), i, v); return *this; }
  Stmt& bind(int i, int v) { sqlite3_bind_int(stmt_.get(), i, v); return *this; }
  Stmt& bind(int i, double v) { sqlite3_bind_double(stmt_.get(), i, v); return *this; }
  Stmt& bind(int i, std::string_view v) {
    sqlite3_bind_text(stmt_.get(), i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& bind_null(int i) { sqlite3_bind_null(stmt_.get(), i); return *this; }

  // Returns true when a row is available, false when done.
  Result<bool> step() {
    const int rc = sqlite3_step(stmt_.get());
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    return sqlite_error(db_, rc, "step");
  }
  Status exec() {
    auto r = step();
    if (!r) return r.error();
    return ok_status();
  }
  void reset() {
    sqlite3_reset(stmt_.get());
    sqlite3_clear_bindings(stmt_.get());
  }

  std::int64_t col_int(int i) const { return sqlite3_column_int64(stmt_.get(), i); }
  double col_double(int i) const { return sqlite3_column_double(stmt_.get(), i); }
  bool col_null(int i) const { return sqlite3_column_type(stmt_.get(), i) == SQLITE_NULL; }
  std::string col_text(int i) const {
    const auto* p = sqlite3_column_text(stmt_.get(), i);
    const int n = sqlite3_column_bytes(stmt_.get(), i);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n)) : std::string();
  }

 private:
  sqlite3* db_ = nullptr;  // non-owning; the Store outlives its statements
  std::unique_ptr<sqlite3_stmt, StmtFinalizer> stmt_;
};

inline Status exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = err ? err : sqlite3_errstr(rc);
    sqlite3_free(err);
    Error e = sqlite_error(db, rc, "exec");
    e.message = msg;
    return e;
  }
  return ok_status();
}

}  // namespace hm::sql
