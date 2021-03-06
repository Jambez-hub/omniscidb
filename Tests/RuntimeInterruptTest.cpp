/*
 * Copyright 2020, OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "TestHelpers.h"

#include <gtest/gtest.h>
#include <boost/program_options.hpp>
#include <exception>
#include <future>
#include <stdexcept>

#include "Catalog/Catalog.h"
#include "Logger/Logger.h"
#include "QueryEngine/CompilationOptions.h"
#include "QueryEngine/Execute.h"
#include "QueryEngine/ResultSet.h"
#include "QueryRunner/QueryRunner.h"
#include "Shared/StringTransform.h"

using QR = QueryRunner::QueryRunner;
unsigned PENDING_QUERY_INTERRUPT_CHECK_FREQ = 10;
double RUNNING_QUERY_INTERRUPT_CHECK_FREQ = 0.9;

#ifndef BASE_PATH
#define BASE_PATH "./tmp"
#endif

bool g_cpu_only{false};
// nested loop over 1M * 1M
std::string test_query_large{"SELECT count(1) FROM t_large t1, t_large t2;"};
// nested loop over 100k * 100k
std::string test_query_medium{"SELECT count(1) FROM t_medium t1, t_medium t2;"};
// nested loop over 1k * 1k
std::string test_query_small{"SELECT count(1) FROM t_small t1, t_small t2;"};

std::string pending_query_interrupted_msg =
    "Query execution has been interrupted (pending query)";
std::string running_query_interrupted_msg = "Query execution has been interrupted";

namespace {

std::shared_ptr<ResultSet> run_query(const std::string& query_str,
                                     std::shared_ptr<Executor> executor,
                                     const ExecutorDeviceType device_type,
                                     const std::string& session_id = "") {
  if (session_id.length() != 32) {
    LOG(ERROR) << "Incorrect or missing session info.";
  }
  return QR::get()->runSQLWithAllowingInterrupt(
      query_str, executor, session_id, device_type, PENDING_QUERY_INTERRUPT_CHECK_FREQ);
}

inline void run_ddl_statement(const std::string& create_table_stmt) {
  QR::get()->runDDLStatement(create_table_stmt);
}

int create_and_populate_table() {
  try {
    run_ddl_statement("DROP TABLE IF EXISTS t_large;");
    run_ddl_statement("DROP TABLE IF EXISTS t_medium;");
    run_ddl_statement("DROP TABLE IF EXISTS t_small;");
    run_ddl_statement("CREATE TABLE t_large (x int not null);");
    run_ddl_statement("CREATE TABLE t_medium (x int not null);");
    run_ddl_statement("CREATE TABLE t_small (x int not null);");

    // write a temporary datafile used in the test
    // because "INSERT INTO ..." stmt for this takes too much time
    // and add pre-generated dataset increases meaningless LOC of this test code
    const auto file_path_small =
        boost::filesystem::path("../../Tests/Import/datafiles/interrupt_table_small.txt");
    if (boost::filesystem::exists(file_path_small)) {
      boost::filesystem::remove(file_path_small);
    }
    std::ofstream small_out(file_path_small.string());
    for (int i = 0; i < 1000; i++) {
      if (small_out.is_open()) {
        small_out << "1\n";
      }
    }
    small_out.close();

    const auto file_path_medium = boost::filesystem::path(
        "../../Tests/Import/datafiles/interrupt_table_medium.txt");
    if (boost::filesystem::exists(file_path_medium)) {
      boost::filesystem::remove(file_path_medium);
    }
    std::ofstream medium_out(file_path_medium.string());
    for (int i = 0; i < 100000; i++) {
      if (medium_out.is_open()) {
        medium_out << "1\n";
      }
    }
    medium_out.close();

    const auto file_path_large =
        boost::filesystem::path("../../Tests/Import/datafiles/interrupt_table_large.txt");
    if (boost::filesystem::exists(file_path_large)) {
      boost::filesystem::remove(file_path_large);
    }
    std::ofstream large_out(file_path_large.string());
    for (int i = 0; i < 1000000; i++) {
      if (large_out.is_open()) {
        large_out << "1\n";
      }
    }
    large_out.close();

    std::string import_small_table_str{
        "COPY t_small FROM "
        "'../../Tests/Import/datafiles/interrupt_table_small.txt' WITH "
        "(header='false')"};
    std::string import_medium_table_str{
        "COPY t_medium FROM "
        "'../../Tests/Import/datafiles/interrupt_table_medium.txt' "
        "WITH (header='false')"};
    std::string import_large_table_str{
        "COPY t_large FROM "
        "'../../Tests/Import/datafiles/interrupt_table_large.txt' WITH "
        "(header='false')"};
    run_ddl_statement(import_small_table_str);
    run_ddl_statement(import_medium_table_str);
    run_ddl_statement(import_large_table_str);

  } catch (...) {
    LOG(ERROR) << "Failed to (re-)create table";
    return -1;
  }
  return 0;
}

int drop_table() {
  try {
    run_ddl_statement("DROP TABLE IF EXISTS t_large;");
    run_ddl_statement("DROP TABLE IF EXISTS t_medium;");
    run_ddl_statement("DROP TABLE IF EXISTS t_small;");
    boost::filesystem::remove("../../Tests/Import/datafiles/interrupt_table_small.txt");
    boost::filesystem::remove("../../Tests/Import/datafiles/interrupt_table_medium.txt");
    boost::filesystem::remove("../../Tests/Import/datafiles/interrupt_table_large.txt");
  } catch (...) {
    LOG(ERROR) << "Failed to drop table";
    return -1;
  }
  return 0;
}

template <class T>
T v(const TargetValue& r) {
  auto scalar_r = boost::get<ScalarTargetValue>(&r);
  CHECK(scalar_r);
  auto p = boost::get<T>(scalar_r);
  CHECK(p);
  return *p;
}

}  // namespace

TEST(Interrupt, Kill_RunningQuery) {
  auto dt = ExecutorDeviceType::CPU;
  auto executor = QR::get()->getExecutor();
  bool startQueryExec = false;
  executor->enableRuntimeQueryInterrupt(RUNNING_QUERY_INTERRUPT_CHECK_FREQ,
                                        PENDING_QUERY_INTERRUPT_CHECK_FREQ);
  std::shared_ptr<ResultSet> res1 = nullptr;
  std::exception_ptr exception_ptr = nullptr;
  try {
    std::string query_session = generate_random_string(32);
    // we first run the query as async function call
    auto query_thread1 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        res = run_query(test_query_large, executor, dt, query_session);
      } catch (...) {
        exception_ptr = std::current_exception();
      }
      return res;
    });

    // wait until our server starts to process the first query
    std::string curRunningSession = "";
    while (!startQueryExec) {
      mapd_shared_lock<mapd_shared_mutex> session_read_lock(executor->getSessionLock());
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      curRunningSession = executor->getCurrentQuerySession(session_read_lock);
      if (curRunningSession == query_session) {
        startQueryExec = true;
      }
    }
    // then, after query execution is started, we try to interrupt the running query
    // by providing the interrupt signal with the running session info
    executor->interrupt(query_session, query_session);
    res1 = query_thread1.get();
    if (exception_ptr != nullptr) {
      std::rethrow_exception(exception_ptr);
    } else {
      // when we reach here, it means the query is finished without interrupted
      // due to some reasons, i.e., very fast query execution
      // so, instead, we check whether query result is correct
      CHECK_EQ(1, (int64_t)res1.get()->rowCount());
      auto crt_row = res1.get()->getNextRow(false, false);
      auto ret_val = v<int64_t>(crt_row[0]);
      CHECK_EQ((int64_t)1000000 * 1000000, ret_val);
    }
  } catch (const std::runtime_error& e) {
    std::string received_err_msg = e.what();
    CHECK(running_query_interrupted_msg.compare(received_err_msg) == 0)
        << received_err_msg;
  }
}

TEST(Interrupt, Kill_PendingQuery) {
  auto dt = ExecutorDeviceType::CPU;
  std::future<std::shared_ptr<ResultSet>> query_thread1;
  std::future<std::shared_ptr<ResultSet>> query_thread2;
  QR::get()->resizeDispatchQueue(2);
  auto executor = QR::get()->getExecutor();
  executor->enableRuntimeQueryInterrupt(RUNNING_QUERY_INTERRUPT_CHECK_FREQ,
                                        PENDING_QUERY_INTERRUPT_CHECK_FREQ);
  bool startQueryExec = false;
  std::exception_ptr exception_ptr1 = nullptr;
  std::exception_ptr exception_ptr2 = nullptr;
  std::shared_ptr<ResultSet> res1 = nullptr;
  std::shared_ptr<ResultSet> res2 = nullptr;
  try {
    std::string session1 = generate_random_string(32);
    std::string session2 = generate_random_string(32);
    // we first run the query as async function call
    query_thread1 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        res = run_query(test_query_medium, executor, dt, session1);
      } catch (...) {
        exception_ptr1 = std::current_exception();
      }
      return res;
    });
    // make sure our server recognizes a session for running query correctly
    std::string curRunningSession = "";
    while (!startQueryExec) {
      mapd_shared_lock<mapd_shared_mutex> session_read_lock(executor->getSessionLock());
      curRunningSession = executor->getCurrentQuerySession(session_read_lock);
      if (curRunningSession == session1) {
        startQueryExec = true;
      }
    }
    query_thread2 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        // run pending query as async call
        res = run_query(test_query_medium, executor, dt, session2);
      } catch (...) {
        exception_ptr2 = std::current_exception();
      }
      return res;
    });
    bool s2_enrolled = false;
    while (!s2_enrolled) {
      mapd_shared_lock<mapd_shared_mutex> session_read_lock(executor->getSessionLock());
      s2_enrolled = executor->checkIsQuerySessionEnrolled(session2, session_read_lock);
      if (s2_enrolled) {
        break;
      }
    }
    // then, we try to interrupt the pending query
    // by providing the interrupt signal with the pending query's session info
    if (startQueryExec) {
      executor->interrupt(session2, session2);
    }
    res2 = query_thread2.get();
    res1 = query_thread1.get();
    if (exception_ptr2 != nullptr) {
      // pending query throws an runtime exception due to query interrupt
      std::rethrow_exception(exception_ptr2);
    }
    if (exception_ptr1 != nullptr) {
      // running query should never return the runtime exception
      CHECK(false);
    }
  } catch (const std::runtime_error& e) {
    // catch exception due to runtime query interrupt
    // and compare thrown message to confirm that
    // this exception comes from our interrupt request
    std::string received_err_msg = e.what();
    std::cout << received_err_msg << "\n";
    CHECK(pending_query_interrupted_msg.compare(received_err_msg) == 0)
        << received_err_msg;
  }
  // check running query's result
  CHECK_EQ(1, (int64_t)res1.get()->rowCount());
  auto crt_row = res1.get()->getNextRow(false, false);
  auto ret_val = v<int64_t>(crt_row[0]);
  CHECK_EQ((int64_t)100000 * 100000, ret_val);
}

TEST(Interrupt, Make_PendingQuery_Run) {
  auto dt = ExecutorDeviceType::CPU;
  std::future<std::shared_ptr<ResultSet>> query_thread1;
  std::future<std::shared_ptr<ResultSet>> query_thread2;
  QR::get()->resizeDispatchQueue(2);
  auto executor = QR::get()->getExecutor();
  executor->enableRuntimeQueryInterrupt(RUNNING_QUERY_INTERRUPT_CHECK_FREQ,
                                        PENDING_QUERY_INTERRUPT_CHECK_FREQ);
  bool startQueryExec = false;
  std::exception_ptr exception_ptr1 = nullptr;
  std::exception_ptr exception_ptr2 = nullptr;
  std::shared_ptr<ResultSet> res1 = nullptr;
  std::shared_ptr<ResultSet> res2 = nullptr;
  try {
    std::string session1 = generate_random_string(32);
    std::string session2 = generate_random_string(32);
    // we first run the query as async function call
    query_thread1 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        res = run_query(test_query_large, executor, dt, session1);
      } catch (...) {
        exception_ptr1 = std::current_exception();
      }
      return res;
    });
    // make sure our server recognizes a session for running query correctly
    std::string curRunningSession = "";
    while (!startQueryExec) {
      mapd_shared_lock<mapd_shared_mutex> session_read_lock(executor->getSessionLock());
      curRunningSession = executor->getCurrentQuerySession(session_read_lock);
      if (curRunningSession == session1) {
        startQueryExec = true;
      }
    }
    query_thread2 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        // run pending query as async call
        res = run_query(test_query_small, executor, dt, session2);
      } catch (...) {
        exception_ptr2 = std::current_exception();
      }
      return res;
    });
    // then, we try to interrupt the running query
    // by providing the interrupt signal with the running query's session info
    // so we can expect that running query session releases all H/W resources and locks,
    // and so pending query takes them for its query execution (becomes running query)
    if (startQueryExec) {
      executor->interrupt(session1, session1);
    }
    res2 = query_thread2.get();
    res1 = query_thread1.get();
    if (exception_ptr1 != nullptr) {
      std::rethrow_exception(exception_ptr1);
    }
    if (exception_ptr2 != nullptr) {
      // pending query should never return the runtime exception
      // because it is executed after running query is interrupted
      CHECK(false);
    }
  } catch (const std::runtime_error& e) {
    // catch exception due to runtime query interrupt
    // and compare thrown message to confirm that
    // this exception comes from our interrupt request
    std::string received_err_msg = e.what();
    CHECK(running_query_interrupted_msg.compare(received_err_msg) == 0)
        << received_err_msg;
  }
  // check running query's result
  CHECK_EQ(1, (int64_t)res2.get()->rowCount());
  auto crt_row = res2.get()->getNextRow(false, false);
  auto ret_val = v<int64_t>(crt_row[0]);
  CHECK_EQ((int64_t)1000 * 1000, ret_val);
}

TEST(Interrupt, Interrupt_Session_Running_Multiple_Queries) {
  // Session1 fires four queries under four parallel executors
  // Session2 submits a single query
  // Let say Session1's query Q1 runs, then remaining queries (Q2~Q4) become pending query
  // and they are waiting to get the executor lock that is held by 1
  // At this time, Q5 of Session2 is waiting at the dispatch queue since
  // there is no available executor at the dispatch queue
  // Here, if we interrupt Session1 and let Q5 runs, we have to get the expected
  // query result of Q5 and we also can see that Q1~Q4 are interrupted
  auto dt = ExecutorDeviceType::CPU;
  std::future<std::shared_ptr<ResultSet>> query_thread1;
  std::future<std::shared_ptr<ResultSet>> query_thread2;
  std::future<std::shared_ptr<ResultSet>> query_thread3;
  std::future<std::shared_ptr<ResultSet>> query_thread4;
  std::future<void> interrupt_signal_sender;
  std::future<void> interrupt_checker;
  QR::get()->resizeDispatchQueue(4);
  auto executor = QR::get()->getExecutor();
  executor->enableRuntimeQueryInterrupt(RUNNING_QUERY_INTERRUPT_CHECK_FREQ,
                                        PENDING_QUERY_INTERRUPT_CHECK_FREQ);
  bool startQueryExec = false;
  std::atomic<bool> catchInterruption(false);
  std::exception_ptr exception_ptr1 = nullptr;
  std::exception_ptr exception_ptr2 = nullptr;
  std::exception_ptr exception_ptr3 = nullptr;
  std::exception_ptr exception_ptr4 = nullptr;
  std::shared_ptr<ResultSet> res1 = nullptr;
  std::shared_ptr<ResultSet> res2 = nullptr;
  std::shared_ptr<ResultSet> res3 = nullptr;
  std::shared_ptr<ResultSet> res4 = nullptr;
  std::future_status q1_status;
  std::future_status q2_status;
  std::future_status q3_status;
  std::future_status q4_status;

  try {
    std::string session1 = generate_random_string(32);
    std::string session2 = generate_random_string(32);
    // we first run the query as async function call
    query_thread1 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        res = run_query(test_query_large, executor, dt, session1);
      } catch (...) {
        exception_ptr1 = std::current_exception();
      }
      return res;
    });

    // make sure our server recognizes a session for running query correctly
    std::string curRunningSession = "";
    while (!startQueryExec) {
      mapd_shared_lock<mapd_shared_mutex> session_read_lock(executor->getSessionLock());
      curRunningSession = executor->getCurrentQuerySession(session_read_lock);
      if (curRunningSession.compare(session1) == 0) {
        startQueryExec = true;
      }
    }
    CHECK(startQueryExec);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    query_thread2 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        // run pending query as async call
        res = run_query(test_query_small, executor, dt, session1);
      } catch (...) {
        exception_ptr2 = std::current_exception();
      }
      return res;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    query_thread3 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        // run pending query as async call
        res = run_query(test_query_small, executor, dt, session1);
      } catch (...) {
        exception_ptr3 = std::current_exception();
      }
      return res;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    query_thread4 = std::async(std::launch::async, [&] {
      std::shared_ptr<ResultSet> res = nullptr;
      try {
        // run pending query as async call
        res = run_query(test_query_small, executor, dt, session1);
      } catch (...) {
        exception_ptr4 = std::current_exception();
      }
      return res;
    });

    // then, we try to interrupt the running query
    // by providing the interrupt signal with the running query's session info
    // so we can expect that running query session releases all H/W resources and locks,
    // and so pending query takes them for its query execution (becomes running query)
    size_t queue_size = 0;
    bool ready_to_interrupt = false;
    while (!ready_to_interrupt && startQueryExec) {
      // check all Q1~Q4 of Session1 are enrolled in the session map
      {
        mapd_shared_lock<mapd_shared_mutex> session_read_lock(executor->getSessionLock());
        queue_size = executor->getQuerySessionInfo(session1, session_read_lock).size();
      }
      if (queue_size == 4) {
        ready_to_interrupt = true;
        break;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    CHECK(ready_to_interrupt);
    executor->interrupt(session1, session1);
    bool detect_time_out = false;

    auto check_interrup_msg = [&catchInterruption](const std::string& msg,
                                                   bool is_pending_query) {
      if (is_pending_query) {
        CHECK(pending_query_interrupted_msg.compare(msg) == 0) << msg;
      } else {
        CHECK(running_query_interrupted_msg.compare(msg) == 0) << msg;
      }
      catchInterruption.store(true);
    };

    auto get_query_status_with_timeout =
        [&detect_time_out](std::future<std::shared_ptr<ResultSet>>& thread,
                           std::future_status& status,
                           std::shared_ptr<ResultSet> res,
                           size_t timeout_sec) {
          do {
            status = thread.wait_for(std::chrono::seconds(timeout_sec));
            if (status == std::future_status::timeout) {
              detect_time_out = true;
              res = thread.get();
              break;
            } else if (status == std::future_status::ready) {
              res = thread.get();
            }
          } while (status != std::future_status::ready);
        };

    get_query_status_with_timeout(query_thread1, q1_status, res1, 60);
    get_query_status_with_timeout(query_thread2, q2_status, res2, 60);
    get_query_status_with_timeout(query_thread3, q3_status, res3, 60);
    get_query_status_with_timeout(query_thread4, q4_status, res4, 60);

    if (exception_ptr1 != nullptr) {
      try {
        std::rethrow_exception(exception_ptr1);
      } catch (const std::runtime_error& e) {
        check_interrup_msg(e.what(), false);
      }
    }
    if (exception_ptr2 != nullptr) {
      try {
        std::rethrow_exception(exception_ptr2);
      } catch (const std::runtime_error& e) {
        check_interrup_msg(e.what(), true);
      }
    }
    if (exception_ptr3 != nullptr) {
      try {
        std::rethrow_exception(exception_ptr3);
      } catch (const std::runtime_error& e) {
        check_interrup_msg(e.what(), true);
      }
    }
    if (exception_ptr4 != nullptr) {
      try {
        std::rethrow_exception(exception_ptr4);
      } catch (const std::runtime_error& e) {
        check_interrup_msg(e.what(), true);
      }
    }

    if (catchInterruption.load()) {
      {
        mapd_shared_lock<mapd_shared_mutex> session_read_lock(executor->getSessionLock());
        queue_size = executor->getQuerySessionInfo(session1, session_read_lock).size();
      }
      CHECK_EQ(queue_size, (size_t)0);
      throw std::runtime_error("SUCCESS");
    }

    if (detect_time_out) {
      throw std::runtime_error("TIME_OUT");
    }

  } catch (const std::runtime_error& e) {
    // catch exception due to runtime query interrupt
    // and compare thrown message to confirm that
    // this exception comes from our interrupt request
    std::string received_err_msg = e.what();
    if (received_err_msg.compare("TIME_OUT") == 0) {
      // catch time_out scenario, so returns immediately to avoid
      // unexpected hangs of our jenkins
      return;
    } else if (received_err_msg.compare("SUCCESS") == 0) {
      // make sure we interrupt the query correctly
      CHECK(catchInterruption.load());
      // if a query is interrupted, its resultset ptr should be nullptr
      CHECK(!res1);  // for Q1 of Session1
      CHECK(!res2);  // for Q2 of Session1
      CHECK(!res3);  // for Q3 of Session1
      CHECK(!res4);  // for Q4 of Session1
    }
  }
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  TestHelpers::init_logger_stderr_only(argc, argv);

  QR::init(BASE_PATH);

  int err{0};
  try {
    err = create_and_populate_table();
    err = RUN_ALL_TESTS();
    err = drop_table();
  } catch (const std::exception& e) {
    LOG(ERROR) << e.what();
  }
  QR::reset();
  return err;
}