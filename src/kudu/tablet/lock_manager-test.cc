// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/tablet/lock_manager.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/gutil/macros.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/util/array_view.h"
#include "kudu/util/env.h"
#include "kudu/util/slice.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/test_util.h"

using std::shared_ptr;
using std::string;
using std::thread;
using std::vector;

DEFINE_int32(num_test_threads, 10, "number of stress test client threads");
DEFINE_int32(num_iterations, 1000, "number of iterations per client thread");

namespace kudu {
namespace tablet {

class LockEntry;
class OpState;

static const OpState* kFakeTransaction =
  reinterpret_cast<OpState*>(0xdeadbeef);

class LockManagerTest : public KuduTest {
 public:
  void VerifyAlreadyLocked(const Slice& key) {
    LockEntry *entry;
    ASSERT_FALSE(lock_manager_.TryLock(key, kFakeTransaction, &entry));
  }

  LockManager lock_manager_;
};

TEST_F(LockManagerTest, TestLockUnlockSingleRow) {
  Slice key_a[] = {"a"};
  for (int i = 0; i < 3; i++) {
    ScopedRowLock l(&lock_manager_, kFakeTransaction, key_a, LockManager::LOCK_EXCLUSIVE);
  }
}

// Test if the same transaction locks the same row multiple times.
TEST_F(LockManagerTest, TestMultipleLockSameRow) {
  Slice key_a[] = {"a"};
  ScopedRowLock first_lock(&lock_manager_, kFakeTransaction, key_a, LockManager::LOCK_EXCLUSIVE);
  ASSERT_TRUE(first_lock.acquired());
  VerifyAlreadyLocked(key_a[0]);

  {
    ScopedRowLock second_lock(&lock_manager_, kFakeTransaction, key_a, LockManager::LOCK_EXCLUSIVE);
    ASSERT_TRUE(second_lock.acquired());
    VerifyAlreadyLocked(key_a[0]);
  }

  ASSERT_TRUE(first_lock.acquired());
  VerifyAlreadyLocked(key_a[0]);
}

TEST_F(LockManagerTest, TestLockUnlockMultipleRows) {
  Slice key_a[] = {"a"};
  Slice key_b[] = {"b"};
  for (int i = 0; i < 3; ++i) {
    ScopedRowLock l1(&lock_manager_, kFakeTransaction, key_a, LockManager::LOCK_EXCLUSIVE);
    ScopedRowLock l2(&lock_manager_, kFakeTransaction, key_b, LockManager::LOCK_EXCLUSIVE);
    VerifyAlreadyLocked(key_a[0]);
    VerifyAlreadyLocked(key_b[0]);
  }
}

TEST_F(LockManagerTest, TestLockBatch) {
  vector<Slice> keys = {"a", "b", "c"};
  {
    ScopedRowLock l1(&lock_manager_, kFakeTransaction, keys, LockManager::LOCK_EXCLUSIVE);
    for (const auto& k : keys) {
      VerifyAlreadyLocked(k);
    }
  }
}

TEST_F(LockManagerTest, TestRelockSameRow) {
  Slice key_a[] = {"a"};
  ScopedRowLock row_lock(&lock_manager_, kFakeTransaction, key_a, LockManager::LOCK_EXCLUSIVE);
  VerifyAlreadyLocked(key_a[0]);
}

TEST_F(LockManagerTest, TestMoveLock) {
  // Acquire a lock.
  Slice key_a[] = {"a"};
  ScopedRowLock row_lock(&lock_manager_, kFakeTransaction, key_a, LockManager::LOCK_EXCLUSIVE);
  ASSERT_TRUE(row_lock.acquired());

  // Move it to a new instance.
  ScopedRowLock moved_lock(std::move(row_lock));
  ASSERT_TRUE(moved_lock.acquired());
  ASSERT_FALSE(row_lock.acquired()); // NOLINT(bugprone-use-after-move)
}

class LmTestResource {
 public:
  explicit LmTestResource(Slice id) : id_(id), owner_(0), is_owned_(false) {}

  Slice id() const { return id_; }

  void acquire(uint64_t tid) {
    std::unique_lock<std::mutex> lock(lock_);
    CHECK(!is_owned_);
    CHECK_EQ(0, owner_);
    owner_ = tid;
    is_owned_ = true;
  }

  void release(uint64_t tid) {
    std::unique_lock<std::mutex> lock(lock_);
    CHECK(is_owned_);
    CHECK_EQ(tid, owner_);
    owner_ = 0;
    is_owned_ = false;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(LmTestResource);

  const Slice id_;
  std::mutex lock_;
  uint64_t owner_;
  bool is_owned_;
};

class LmTestThread {
 public:
  LmTestThread(LockManager* manager, vector<Slice> keys, vector<LmTestResource*> resources)
      : manager_(manager), keys_(std::move(keys)), resources_(std::move(resources)) {}

  void Start() {
    thread_ = thread([this]() { this->Run(); });
  }

  void Run() {
    tid_ = Env::Default()->gettid();
    const OpState* my_txn = reinterpret_cast<OpState*>(tid_);

    std::sort(keys_.begin(), keys_.end(), Slice::Comparator());
    for (int i = 0; i < FLAGS_num_iterations; i++) {
      ScopedRowLock l(manager_, my_txn, keys_, LockManager::LOCK_EXCLUSIVE);
      for (LmTestResource* r : resources_) {
        r->acquire(tid_);
      }
      for (LmTestResource* r : resources_) {
        r->release(tid_);
      }
    }
  }

  void Join() {
    thread_.join();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(LmTestThread);
  LockManager* manager_;
  vector<Slice> keys_;
  const vector<LmTestResource*> resources_;
  uint64_t tid_;
  thread thread_;
};

static void RunPerformanceTest(const char* test_type,
                               vector<shared_ptr<LmTestThread> >* threads) {
  Stopwatch sw(Stopwatch::ALL_THREADS);
  sw.start();
  for (const shared_ptr<LmTestThread>& t : *threads) {
    t->Start();
  }

  for (const shared_ptr<LmTestThread>& t : *threads) {
    t->Join();
  }
  sw.stop();

  float num_cycles = FLAGS_num_iterations;
  num_cycles *= FLAGS_num_test_threads;

  float cycles_per_second = num_cycles / sw.elapsed().wall_seconds();
  float user_cpu_micros_per_cycle =
    (sw.elapsed().user / 1000.0) / cycles_per_second;
  float sys_cpu_micros_per_cycle =
    (sw.elapsed().system / 1000.0) / cycles_per_second;
  LOG(INFO) << "*** testing with " << FLAGS_num_test_threads << " threads, "
    << FLAGS_num_iterations << " iterations.";
  LOG(INFO) << test_type << " Lock/Unlock cycles per second:  "
    << cycles_per_second;
  LOG(INFO) << test_type << " User CPU per lock/unlock cycle: "
    << user_cpu_micros_per_cycle << "us";
  LOG(INFO) << test_type << " Sys CPU per lock/unlock cycle:  "
    << sys_cpu_micros_per_cycle << "us";
}

// Test running a bunch of threads at once that want an overlapping set of
// resources.
TEST_F(LockManagerTest, TestContention) {
  LmTestResource resource_a("a");
  LmTestResource resource_b("b");
  LmTestResource resource_c("c");
  vector<shared_ptr<LmTestThread> > threads;
  for (int i = 0; i < FLAGS_num_test_threads; ++i) {
    vector<LmTestResource*> resources;
    if (i % 3 == 0) {
      resources.push_back(&resource_a);
      resources.push_back(&resource_b);
    } else if (i % 3 == 1) {
      resources.push_back(&resource_b);
      resources.push_back(&resource_c);
    } else {
      resources.push_back(&resource_c);
      resources.push_back(&resource_a);
    }
    vector<Slice> keys;
    for (LmTestResource* r : resources) {
      keys.push_back(r->id());
    }
    threads.push_back(std::make_shared<LmTestThread>(
        &lock_manager_, keys, resources));
  }
  RunPerformanceTest("Contended", &threads);
}

// Test running a bunch of threads at once that want different
// resources.
TEST_F(LockManagerTest, TestUncontended) {
  vector<string> slice_strings;
  for (int i = 0; i < FLAGS_num_test_threads; i++) {
    slice_strings.push_back(StringPrintf("slice%03d", i));
  }
  vector<Slice> slices;
  for (int i = 0; i < FLAGS_num_test_threads; i++) {
    slices.emplace_back(slice_strings[i]);
  }
  vector<shared_ptr<LmTestResource> > resources;
  for (int i = 0; i < FLAGS_num_test_threads; i++) {
    resources.push_back(std::make_shared<LmTestResource>(slices[i]));
  }
  vector<shared_ptr<LmTestThread> > threads;
  for (int i = 0; i < FLAGS_num_test_threads; ++i) {
    vector<Slice> k;
    k.push_back(slices[i]);
    vector<LmTestResource*> r;
    r.push_back(resources[i].get());
    threads.push_back(std::make_shared<LmTestThread>(
        &lock_manager_, k, r));
  }
  RunPerformanceTest("Uncontended", &threads);
}

} // namespace tablet
} // namespace kudu
