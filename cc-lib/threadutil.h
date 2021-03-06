
#ifndef __THREADUTIL_H
#define __THREADUTIL_H

#include <vector>
#include <thread>
#include <mutex>

#if 0 // not needed with TDM  - tom7 11 Oct 2015

#ifdef __MINGW32__
#include "mingw.thread.h"
#include "mingw.mutex.h"
// ugh, conflict...
#undef ARRAYSIZE
#endif
#endif

struct MutexLock {
  explicit MutexLock(std::mutex *m) : m(m) { m->lock(); }
  ~MutexLock() { m->unlock(); }
  std::mutex *m;
};

// Read with the mutex that protects it. T must be copyable,
// obviously!
template<class T>
T ReadWithLock(std::mutex *m, const T *t) {
  MutexLock ml(m);
  return *t;
}

// Write with the mutex that protects it. T must be copyable.
template<class T>
void WriteWithLock(std::mutex *m, T *t, const T &val) {
  MutexLock ml(m);
  *t = val;
}

// Do progress meter.
// It should be thread safe and have a way for a thread to register a sub-meter.

// TODO: Make these functions take a "callable" template param F, rather
// than requiring std::function.

// Run the function f on each element of vec in parallel, with its
// index. The caller must of course synchronize any accesses to shared
// data structures. Return value of function is ignored.
//
// TODO: Implement this in terms of ParallelComp
template<class T, class F>
void ParallelAppi(const std::vector<T> &vec, 
		  const F &f,
		  int max_concurrency) {
  // TODO: XXX This cast may really be unsafe, since these vectors
  // could exceed 32 bit ints in practice.
  max_concurrency = std::min((int)vec.size(), max_concurrency);
  // Need at least one thread for correctness.
  max_concurrency = std::max(max_concurrency, 1);
  std::mutex index_m;
  int next_index = 0;
  
  // Thread applies f repeatedly until there are no more indices.
  // PERF: Can just start each thread knowing its start index, and avoid
  // the synchronization overhead at startup.
  auto th = [&index_m, &next_index, &vec, &f]() {
    for (;;) {
      index_m.lock();
      if (next_index == vec.size()) {
	// All done. Don't increment counter so that other threads can
	// notice this too.
	index_m.unlock();
	return;
      }
      int my_index = next_index++;
      index_m.unlock();

      // Do work, not holding mutex.
      (void)f(my_index, vec[my_index]);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(max_concurrency);
  for (int i = 0; i < max_concurrency; i++) {
    threads.emplace_back(th);
  }
  // Now just wait for them all to finish.
  for (std::thread &t : threads) t.join();
}

// Same, but the typical case that the index is not needed.
template<class T, class F>
void ParallelApp(const std::vector<T> &vec, 
		 const F &f,
		 int max_concurrency) {
  std::function<void(int, const T &)> ff =
    [&f](int i_unused, const T &arg) { return f(arg); };
  ParallelAppi(vec, ff, max_concurrency);
}

// Parallel comprehension. Runs f on 0...(num-1).
// Actually is comprehension the right name for this given that it
// doesn't return anything? XXX
template<class F>
void ParallelComp(int num,
		  const F &f,
		  int max_concurrency) {
  max_concurrency = std::min(num, max_concurrency);
  // Need at least one thread for correctness.
  max_concurrency = std::max(max_concurrency, 1);
  std::mutex index_m;
  int next_index = 0;

  // Thread applies f repeatedly until there are no more indices.
  // PERF: Can just start each thread knowing its start index, and avoid
  // the synchronization overhead at startup.
  auto th = [&index_m, &next_index, num, &f]() {
    for (;;) {
      index_m.lock();
      if (next_index == num) {
	// All done. Don't increment counter so that other threads can
	// notice this too.
	index_m.unlock();
	return;
      }
      int my_index = next_index++;
      index_m.unlock();

      // Do work, not holding mutex.
      (void)f(my_index);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(max_concurrency);
  for (int i = 0; i < max_concurrency; i++) {
    threads.emplace_back(th);
  }
  // Now just wait for them all to finish.
  for (std::thread &t : threads) t.join();
}

// Drop-in serial replacement for debugging, etc.
template<class F>
void UnParallelComp(int num, const F &f, int max_concurrency_ignored) {
  for (int i = 0; i < num; i++) (void)f(i);
}

// F needs to be callable (std::function or lambda) and thread safe.
// It returns R, which must have a default constructor, and this will
// only be efficient if it has move semantics as well.
template<class T, class F>
auto ParallelMap(const std::vector<T> &vec,
		 const F &f,
		 int max_concurrency) -> std::vector<decltype(f(vec.front()))> {
  using R = decltype(f(vec.front()));
  // Needed?
  // std::mutex out_m;
  std::vector<R> result;
  result.resize(vec.size());

  // Not sure if C++11 makes thread safety guarantees about
  // vector::operator[], but if we have a data pointer then we can be
  // confident of having one writer to each slot.
  R *data = result.data();
  std::function<void(int, const T &)> run_write =
    [data, &f](int idx, const T &arg) -> void {
    data[idx] = f(arg);
  };
  ParallelAppi(vec, run_write, max_concurrency);
  return result;
}

// Drop in replacement for testing, debugging, etc.
template<class T, class F>
auto UnParallelMap(const std::vector<T> &vec,
		   const F &f, int max_concurrency_ignored) ->
  std::vector<decltype(f(vec.front()))> {
  using R = decltype(f(vec.front()));
  std::vector<R> result;
  result.resize(vec.size());

  for (int i = 0; i < vec.size(); i++) {
    result[i] = f(vec[i]);
  }

  return result;
}

// When going out of scope, wait for the given thread.
struct ThreadJoiner {
  explicit ThreadJoiner(std::thread *t) : t(t) {}
  ~ThreadJoiner() {
    t->join();
  }
  std::thread *t;
};

// Manages running up to X asynchronous tasks in separate threads. This
// is intended for use in situations like compressing and writing a
// bunch of frames of a movie out to disk. There's substantial parallelism
// opportunity, but it can bad if we eaglery generate the frames because
// they might fill the entire memory. This automatically throttles once
// the specified level of parallelism is reached, by running further calls
// synchronously.
// 
// The threads are started for each asynchronous Run (no thread pool),
// making this kinda high-overhead but easy to manage. It at least
// cleans up after itself.
struct Asynchronously {
  Asynchronously(int max_threads) : threads_active(0),
				    max_threads(max_threads) {
  }

  // Run the function asynchronously if we haven't exceeded the maximum
  // number of threads. Otherwise, run it in this thread and don't
  // return until it's done.
  void Run(std::function<void()> f) {
    m.lock();
    if (threads_active < max_threads) {
      threads_active++;
      m.unlock();
      std::thread t{[this, f]() {
	  f();
	  MutexLock ml(&this->m);
	  threads_active--;
	}};
      t.detach();
      
    } else {
      m.unlock();
      // Run synchronously.
      f();
    }
  }

  // Wait until all threads have finished.
  ~Asynchronously() {
    for (;;) {
      MutexLock ml(&m);
      if (threads_active == 0) {
	return;
      }
    }
  }
    
  std::mutex m;
  int threads_active;
  const int max_threads;
};

#endif
