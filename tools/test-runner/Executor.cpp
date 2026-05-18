/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Executor.h"

#include "hermes/BCGen/HBC/BCProviderFromSrc.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/ConsoleHost/ConsoleHost.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/TimeLimitMonitor.h"
#include "hermes/hermes.h"

#include "jsi/jsi.h"

#include "llvh/ADT/ScopeExit.h"
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/MemoryBuffer.h"
#include "llvh/Support/Program.h"
#include "llvh/Support/raw_ostream.h"

#include <thread>

namespace hermes {
namespace testrunner {

namespace {

using Clock = std::chrono::steady_clock;

/// Holds the runtime environment created for a single test execution.
struct TestRuntimeEnv {
  std::unique_ptr<facebook::jsi::Runtime> jsiRuntime;
  vm::Runtime *runtime;
  std::shared_ptr<vm::TimeLimitMonitor> timeLimitMonitor;
};

/// Capture the current exception message from the runtime and clear it.
std::string captureException(vm::Runtime &runtime) {
  auto thrownVal = runtime.makeHandle(runtime.getThrownValue());
  std::string buf;
  llvh::raw_string_ostream sos(buf);
  runtime.printException(sos, thrownVal);
  runtime.clearThrownValue();
  return sos.str();
}

/// Create a configured Hermes runtime for test262 execution.
/// When \p disableHandleSan is true, GC handle sanitization is disabled
/// (sanitize rate = 0), matching the Python runner's behavior for
/// handlesan_skip_list tests.
TestRuntimeEnv createTestRuntime(
    unsigned timeoutSeconds,
    bool disableHandleSan,
    bool enableJIT,
    bool forceJIT) {
  auto gcConfigBuilder = vm::GCConfig::Builder();
  if (disableHandleSan) {
    gcConfigBuilder.withSanitizeConfig(
        vm::GCSanitizeConfig::Builder().withSanitizeRate(0.0).build());
  }

  auto runtimeConfig = vm::RuntimeConfig::Builder()
                           .withGCConfig(gcConfigBuilder.build())
                           .withES6Proxy(true)
                           .withES6BlockScoping(true)
                           .withMicrotaskQueue(true)
                           .withEnableHermesInternal(true)
                           .withEnableHermesInternalTestMethods(true)
                           .withEnableAsyncGenerators(true)
                           .withTest262(true)
                           .withEnableEval(true)
                           .withAsyncBreakCheckInEval(true)
                           .withEnableJIT(enableJIT)
                           .withForceJIT(forceJIT)
                           .build();

  auto hermesRuntime = facebook::hermes::makeHermesRuntime(runtimeConfig);
  auto *runtime = static_cast<vm::Runtime *>(
      facebook::jsi::castInterface<facebook::hermes::IHermes>(
          hermesRuntime.get())
          ->getVMRuntimeUnsafe());

  std::shared_ptr<vm::TimeLimitMonitor> timeLimitMonitor;
  if (timeoutSeconds > 0) {
    timeLimitMonitor = vm::TimeLimitMonitor::getOrCreate();
    runtime->timeLimitMonitor = timeLimitMonitor;
    timeLimitMonitor->watchRuntime(
        *runtime, std::chrono::milliseconds(timeoutSeconds * 1000u));
  }

  return {std::move(hermesRuntime), runtime, std::move(timeLimitMonitor)};
}

/// Drain the setTimeout task queue, executing each callback and its
/// microtasks. Returns true and sets exceptionMsg if an exception was thrown.
bool drainTaskQueue(
    vm::Runtime &runtime,
    ConsoleHostContext &ctx,
    std::string &exceptionMsg) {
  vm::GCScope scope{runtime};
  vm::GCScopeMarkerRAII marker{scope};
  vm::MutableHandle<vm::Callable> task{runtime};
  while (auto optTask = ctx.dequeueTask()) {
    marker.flush();
    task = std::move(*optTask);
    auto callRes = vm::Callable::executeCall0(
        task, runtime, vm::Runtime::getUndefinedValue(), false);
    if (callRes == vm::ExecutionStatus::EXCEPTION) {
      exceptionMsg = captureException(runtime);
      return true;
    }
    microtask::performCheckpoint(runtime);
  }
  return false;
}

/// Run compiled bytecode in a fresh runtime and evaluate the result
/// against negative expectations.
TestResult executeCompiledTest(
    const std::string &testName,
    std::unique_ptr<hbc::BCProvider> bytecode,
    const std::string &sourceURL,
    const NegativeExpectation &negative,
    unsigned timeoutSeconds,
    bool disableHandleSan,
    bool lazy,
    bool enableJIT,
    bool forceJIT,
    Clock::time_point startTime) {
  bool expectRuntimeError =
      !negative.phase.empty() && negative.phase == "runtime";

  auto makeResult = [&](ResultCode code, const std::string &msg) {
    auto endTime = Clock::now();
    TestResult r;
    r.testName = testName;
    r.code = code;
    r.message = msg;
    r.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime);
    return r;
  };

  auto env =
      createTestRuntime(timeoutSeconds, disableHandleSan, enableJIT, forceJIT);

  // Install console bindings (including $262, alert, setTimeout, etc.).
  vm::GCScope scope(*env.runtime);
  ConsoleHostContext ctx{*env.runtime};
  ctx.enableTestMethods_ = true;
  installConsoleBindings(*env.runtime, ctx);

  // Run the bytecode.
  // Lazy compilation cannot use persistent mode.
  vm::RuntimeModuleFlags rmFlags;
  rmFlags.persistent = !lazy;
  auto status = env.runtime->runBytecode(
      std::move(bytecode),
      rmFlags,
      sourceURL,
      vm::Runtime::makeNullHandle<vm::Environment>());

  bool threwException = status == vm::ExecutionStatus::EXCEPTION;
  std::string exceptionMsg;
  if (threwException) {
    exceptionMsg = captureException(*env.runtime);
  }

  // Check for timeout.
  if (threwException &&
      exceptionMsg.find("Javascript execution has timed out") !=
          std::string::npos) {
    if (env.timeLimitMonitor) {
      env.timeLimitMonitor->unwatchRuntime(*env.runtime);
    }
    return makeResult(ResultCode::ExecuteTimeout, "FAIL: Test timed out");
  }

  // Drain microtask queue and task queue while still under timeout protection.
  if (!threwException) {
    microtask::performCheckpoint(*env.runtime);
    if (drainTaskQueue(*env.runtime, ctx, exceptionMsg)) {
      threwException = true;
    }
  }

  // Unwatch runtime from time limit monitor after all execution is complete.
  if (env.timeLimitMonitor) {
    env.timeLimitMonitor->unwatchRuntime(*env.runtime);
  }

  // Check for timeout during microtask/task draining.
  if (threwException &&
      exceptionMsg.find("Javascript execution has timed out") !=
          std::string::npos) {
    return makeResult(ResultCode::ExecuteTimeout, "FAIL: Test timed out");
  }

  // Evaluate result based on expectations.
  if (expectRuntimeError) {
    if (!threwException) {
      return makeResult(
          ResultCode::ExecuteFailed,
          "FAIL: Expected runtime " + negative.errorType + " but test passed");
    }
    if (!negative.errorType.empty() &&
        exceptionMsg.find(negative.errorType) == std::string::npos) {
      return makeResult(
          ResultCode::ExecuteFailed,
          "FAIL: Expected " + negative.errorType + " but got: " + exceptionMsg);
    }
    return makeResult(ResultCode::Passed, "PASS (expected runtime error)");
  }

  if (threwException) {
    return makeResult(ResultCode::ExecuteFailed, "FAIL: " + exceptionMsg);
  }

  return makeResult(ResultCode::Passed, "PASS");
}

/// Process a single test entry: read file, parse frontmatter, determine
/// variants, and execute each variant.
void processTestEntry(
    const TestEntry &entry,
    const HarnessCache &harness,
    const Skiplist *skiplist,
    const ExecConfig &config,
    std::vector<TestResult> &results,
    std::mutex &resultsMutex,
    std::atomic<size_t> &featureSkipped,
    std::atomic<size_t> &permanentFeatureSkipped) {
  // Read test file.
  auto fileBuf = llvh::MemoryBuffer::getFile(entry.path);
  if (!fileBuf) {
    TestResult r;
    r.testName = entry.fullName;
    r.code = ResultCode::Failed;
    r.message = "FAIL: Cannot read file";
    std::lock_guard<std::mutex> lock(resultsMutex);
    results.push_back(std::move(r));
    return;
  }

  llvh::StringRef content = (*fileBuf)->getBuffer();
  TestRecord record = parseFrontmatter(content);

  // Feature-based skipping.
  if (skiplist) {
    for (const auto &feat : record.features) {
      SkipReason reason = skiplist->shouldSkipFeature(feat);
      if (reason != SkipReason::NotSkipped) {
        ++featureSkipped;
        if (reason == SkipReason::PermanentUnsupportedFeature)
          ++permanentFeatureSkipped;
        return;
      }
    }
  }

  // Skip module tests — Hermes doesn't support ES module execution.
  if (record.isModule()) {
    ++featureSkipped;
    return;
  }

  // Skip tests that include testIntl.js — Hermes doesn't implement all
  // Intl constructors required by this harness (matching Python runner).
  for (const auto &inc : record.includes) {
    if (inc == "testIntl.js") {
      ++featureSkipped;
      return;
    }
  }

  // Check if handle sanitizer should be disabled for this test.
  bool disableHandleSan =
      skiplist && skiplist->shouldDisableHandleSan(entry.path);

  // Determine variants.
  bool runStrict = !record.isNoStrict() && !record.isRaw();
  bool runNonStrict = !record.isOnlyStrict() && !record.isRaw();
  bool runRaw = record.isRaw();

  if (record.isModule()) {
    runStrict = false;
    runNonStrict = true;
    runRaw = false;
  }
  // At least one variant must be run.
  assert((runNonStrict || runStrict || runRaw) && "No test variant to run");
  std::vector<std::string> includes = buildTestIncludes(entry, record);

  // Match the Python runner behavior: the compiler's strict mode flag
  // is set based on whether the test CAN run in strict mode (runStrict),
  // not on which variant is currently being run.
  bool compileStrict = runStrict;

  // Run variants in order, short-circuiting on first failure (like the Python
  // runner). Push exactly one result per test file.
  auto runVariant = [&](const char *suffix, bool isStrict) -> TestResult {
    std::string source = harness.buildSource(
        includes, record.src, isStrict, record.isAsync(), config.shermes);
    std::string variantName = entry.fullName + " (" + suffix + ")";

    if (config.shermes) {
      return executeTestVariantShermes(
          variantName,
          source,
          compileStrict,
          record.isAsync(),
          record.negative,
          config.timeoutSeconds,
          config.optimize,
          disableHandleSan,
          config.shermesBinary,
          config.shermesExtraFlags);
    }

    return executeTestVariant(
        variantName,
        source,
        entry.path,
        compileStrict,
        record.negative,
        config.timeoutSeconds,
        disableHandleSan,
        config.optimize,
        config.lazy,
        config.enableJIT,
        config.forceJIT);
  };

  TestResult lastResult;
  if (runRaw) {
    lastResult = runVariant("raw", false);
  } else {
    if (runNonStrict) {
      lastResult = runVariant("default", false);
      if (lastResult.code != ResultCode::Passed) {
        std::lock_guard<std::mutex> lock(resultsMutex);
        results.push_back(std::move(lastResult));
        return;
      }
    }
    if (runStrict) {
      lastResult = runVariant("strict", true);
    }
  }

  {
    std::lock_guard<std::mutex> lock(resultsMutex);
    results.push_back(std::move(lastResult));
  }
}

/// Print percentage-based progress: "10.. 20.. 30.." etc.
void reportProgress(
    std::atomic<size_t> &completedCount,
    size_t totalTests,
    std::atomic<unsigned> &lastPrintedPct) {
  size_t done = ++completedCount;
  if (totalTests > 0) {
    unsigned pct = (unsigned)(done * 100 / totalTests);
    unsigned milestone = (pct / 10) * 10;
    if (milestone > 0) {
      unsigned expected = milestone - 10;
      if (lastPrintedPct.compare_exchange_strong(expected, milestone)) {
        if (milestone == 100)
          llvh::outs() << " done.";
        else
          llvh::outs() << " " << milestone << "..";
        llvh::outs().flush();
      }
    }
  }
}

} // namespace

std::unique_ptr<hbc::BCProvider> compileSource(
    llvh::StringRef source,
    llvh::StringRef sourceURL,
    bool strict,
    bool optimize,
    bool lazy,
    std::string &errorMsg) {
  auto llvmBuf = llvh::MemoryBuffer::getMemBufferCopy(source, sourceURL);
  auto buf = std::make_unique<OwnedMemoryBuffer>(std::move(llvmBuf));

  hbc::CompileFlags flags;
  flags.strict = strict;
  flags.test262 = true;
  flags.emitAsyncBreakCheck = true;
  flags.enableGenerator = true;
  flags.enableAsyncGenerators = true;
  flags.enableES6BlockScoping = true;
  flags.enableTDZ = true;
  flags.lazy = lazy;

  auto [provider, error] = hbc::BCProviderFromSrc::create(
      std::move(buf),
      sourceURL,
      /*sourceMap=*/"",
      flags,
      /*topLevelFunctionName=*/"global",
      /*diagHandler=*/{},
      /*diagContext=*/nullptr,
      optimize ? hbc::fullOptimizationPipeline
               : std::function<void(Module &)>{});

  if (!provider) {
    errorMsg = error;
    return nullptr;
  }

  return std::move(provider);
}

std::vector<std::string> buildTestIncludes(
    const TestEntry &entry,
    const TestRecord &record) {
  std::vector<std::string> includes;
  if (!record.isRaw()) {
    if (entry.suiteKind == SuiteKind::Test262) {
      includes.push_back("sta.js");
      includes.push_back("assert.js");
    }
    for (const auto &inc : record.includes) {
      includes.push_back(inc);
    }
    if (record.isAsync()) {
      includes.push_back("doneprintHandle.js");
    }
  }
  return includes;
}

TestResult executeTestVariant(
    const std::string &testName,
    const std::string &source,
    const std::string &sourceURL,
    bool isStrict,
    const NegativeExpectation &negative,
    unsigned timeoutSeconds,
    bool disableHandleSan,
    bool optimize,
    bool lazy,
    bool enableJIT,
    bool forceJIT) {
  auto startTime = Clock::now();

  auto makeResult = [&](ResultCode code, const std::string &msg) {
    auto endTime = Clock::now();
    TestResult r;
    r.testName = testName;
    r.code = code;
    r.message = msg;
    r.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime);
    return r;
  };

  bool hasNegative = !negative.phase.empty();
  bool expectCompileError = hasNegative && negative.phase == "parse";
  bool expectResolutionError = hasNegative && negative.phase == "resolution";

  // Compile the source.
  // Note: enableJIT/forceJIT are runtime-only settings and don't affect
  // compilation — they are passed through to createTestRuntime instead.
  std::string compileError;
  auto bytecode =
      compileSource(source, sourceURL, isStrict, optimize, lazy, compileError);

  if (!bytecode) {
    if (expectCompileError || expectResolutionError) {
      return makeResult(ResultCode::Passed, "PASS (expected compile error)");
    }
    return makeResult(
        ResultCode::CompileFailed, "FAIL: Compilation failed: " + compileError);
  }

  if (expectCompileError || expectResolutionError) {
    return makeResult(
        ResultCode::ExecuteFailed,
        "FAIL: Expected compile error but compilation succeeded");
  }

  // Check if compilation already exceeded the timeout budget.
  if (timeoutSeconds > 0) {
    auto elapsed = Clock::now() - startTime;
    if (elapsed >= std::chrono::seconds(timeoutSeconds)) {
      return makeResult(
          ResultCode::CompileTimeout, "FAIL: Compilation timed out");
    }
  }

  return executeCompiledTest(
      testName,
      std::move(bytecode),
      sourceURL,
      negative,
      timeoutSeconds,
      disableHandleSan,
      lazy,
      enableJIT,
      forceJIT,
      startTime);
}

namespace {

/// Result of running a subprocess.
struct SubprocessResult {
  int exitCode;
  std::string stdoutStr;
  std::string stderrStr;
  /// True only for genuine timeouts (not signal crashes).
  bool timedOut;
  bool execFailed;
  /// Error message from ExecuteAndWait (signal name on crash, "timed out"
  /// on timeout).
  std::string errMsg;
};

/// Run a program as a subprocess, capturing stdout and stderr.
/// Uses LLVM's ExecuteAndWait with file redirects for output capture.
SubprocessResult runProcess(
    llvh::StringRef program,
    llvh::ArrayRef<llvh::StringRef> args,
    unsigned timeoutSeconds) {
  SubprocessResult result{};
  result.exitCode = -1;
  result.timedOut = false;
  result.execFailed = false;

  // Create temp files for stdout and stderr capture.
  llvh::SmallString<128> stdoutPath, stderrPath;
  if (llvh::sys::fs::createTemporaryFile("test-stdout", "txt", stdoutPath)) {
    result.execFailed = true;
    result.stderrStr = "Failed to create temp files for output capture";
    return result;
  }
  if (llvh::sys::fs::createTemporaryFile("test-stderr", "txt", stderrPath)) {
    llvh::sys::fs::remove(stdoutPath);
    result.execFailed = true;
    result.stderrStr = "Failed to create temp files for output capture";
    return result;
  }
  auto cleanupRedirects = llvh::make_scope_exit([&] {
    llvh::sys::fs::remove(stdoutPath);
    llvh::sys::fs::remove(stderrPath);
  });

  // Set up redirects: stdin=/dev/null, stdout/stderr to temp files.
  llvh::Optional<llvh::StringRef> redirects[] = {
      llvh::StringRef(""), // stdin -> /dev/null
      llvh::StringRef(stdoutPath), // stdout -> file
      llvh::StringRef(stderrPath), // stderr -> file
  };

  std::string errMsg;
  bool execFailed = false;
  int rc = llvh::sys::ExecuteAndWait(
      program,
      args,
      /*Env=*/llvh::None,
      redirects,
      timeoutSeconds,
      /*MemoryLimit=*/0,
      &errMsg,
      &execFailed);

  result.execFailed = execFailed;
  result.exitCode = rc;
  result.errMsg = errMsg;
  // ExecuteAndWait returns -2 for BOTH timeout and signal termination
  // (SIGSEGV, SIGABRT, etc.). Distinguish using ErrMsg: LLVM sets it to
  // "Child timed out" on timeout, and to the signal name (e.g.,
  // "Segmentation fault") on crash.
  result.timedOut = (rc == -2 && errMsg.find("timed out") != std::string::npos);

  // Read captured output.
  if (auto buf = llvh::MemoryBuffer::getFile(stdoutPath))
    result.stdoutStr = (*buf)->getBuffer().str();
  if (auto buf = llvh::MemoryBuffer::getFile(stderrPath))
    result.stderrStr = (*buf)->getBuffer().str();

  return result;
}

} // namespace

TestResult executeTestVariantShermes(
    const std::string &testName,
    const std::string &source,
    bool isStrict,
    bool isAsync,
    const NegativeExpectation &negative,
    unsigned timeoutSeconds,
    bool optimize,
    bool disableHandleSan,
    const std::string &shermesBinary,
    const std::vector<std::string> &shermesExtraFlags) {
  using Clock = std::chrono::steady_clock;
  auto startTime = Clock::now();

  auto makeResult = [&](ResultCode code, const std::string &msg) {
    TestResult r;
    r.testName = testName;
    r.code = code;
    r.message = msg;
    r.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - startTime);
    return r;
  };

  bool hasNegative = !negative.phase.empty();
  bool expectCompileError = hasNegative && negative.phase == "parse";
  bool expectResolutionError = hasNegative && negative.phase == "resolution";
  bool expectRuntimeError = hasNegative && negative.phase == "runtime";

  // Write preprocessed source to temp file.
  llvh::SmallString<128> sourcePath;
  int sourceFD;
  if (llvh::sys::fs::createTemporaryFile("test", "js", sourceFD, sourcePath)) {
    return makeResult(ResultCode::Failed, "FAIL: Cannot create temp file");
  }
  {
    llvh::raw_fd_ostream sourceFile(sourceFD, /*shouldClose=*/true);
    sourceFile << source;
  }

  // Output binary path.
  llvh::SmallString<128> binaryPath(sourcePath);
  binaryPath += ".out";

  // Clean up temp files on all return paths.
  auto cleanup = llvh::make_scope_exit([&] {
    llvh::sys::fs::remove(sourcePath);
    llvh::sys::fs::remove(binaryPath);
  });

  // Step 1: Compile with shermes.
  // Matches Python runner: shermes <source> -o <binary> <COMPILE_ARGS>
  //   [-strict] [-O|-O0]
  std::vector<std::string> compileArgStorage;
  compileArgStorage.push_back(shermesBinary);
  compileArgStorage.push_back(std::string(sourcePath.str()));
  compileArgStorage.push_back("-o");
  compileArgStorage.push_back(std::string(binaryPath.str()));
  // COMPILE_ARGS from Python runner.
  compileArgStorage.push_back("-test262");
  compileArgStorage.push_back("-fno-static-builtins");
  compileArgStorage.push_back("-Xes6-block-scoping");
  compileArgStorage.push_back("-Xenable-tdz");
  compileArgStorage.push_back("-Xasync-generators");
  if (isStrict)
    compileArgStorage.push_back("-strict");
  compileArgStorage.push_back(optimize ? "-O" : "-O0");
  for (const auto &flag : shermesExtraFlags)
    compileArgStorage.push_back(flag);

  std::vector<llvh::StringRef> compileArgs;
  for (const auto &arg : compileArgStorage)
    compileArgs.push_back(arg);

  auto compileResult = runProcess(shermesBinary, compileArgs, timeoutSeconds);

  if (compileResult.timedOut) {
    return makeResult(
        ResultCode::CompileTimeout, "FAIL: Compilation timed out");
  }

  if (compileResult.exitCode != 0 || compileResult.execFailed) {
    if (expectCompileError || expectResolutionError) {
      return makeResult(ResultCode::Passed, "PASS (expected compile error)");
    }
    // Include signal info (errMsg) when the compiler was killed by a signal.
    std::string detail = compileResult.stderrStr;
    if (!compileResult.errMsg.empty())
      detail = compileResult.errMsg + "\n" + detail;
    return makeResult(
        ResultCode::CompileFailed, "FAIL: Compilation failed: " + detail);
  }

  if (expectCompileError || expectResolutionError) {
    return makeResult(
        ResultCode::ExecuteFailed,
        "FAIL: Expected compile error but compilation succeeded");
  }

  // Step 2: Run the compiled binary.
  // Matches Python runner: <binary> [-Xes6-proxy]
  //   [-Xhermes-internal-test-methods] [-Xmicrotask-queue]
  //   [-gc-sanitize-handles=0]
  std::vector<std::string> runArgStorage;
  runArgStorage.push_back(std::string(binaryPath.str()));
  runArgStorage.push_back("-Xes6-proxy");
  runArgStorage.push_back("-Xhermes-internal-test-methods");
  runArgStorage.push_back("-Xmicrotask-queue");
  if (disableHandleSan)
    runArgStorage.push_back("-gc-sanitize-handles=0");

  std::vector<llvh::StringRef> runArgs;
  for (const auto &arg : runArgStorage)
    runArgs.push_back(arg);

  auto runResult = runProcess(binaryPath.str(), runArgs, timeoutSeconds);

  if (runResult.timedOut) {
    return makeResult(ResultCode::ExecuteTimeout, "FAIL: Test timed out");
  }

  // Match Python runner's result evaluation logic for non-lazy mode.
  if (runResult.exitCode != 0) {
    if (runResult.exitCode < 0) {
      // Signal termination or exec failure.
      std::string detail = "FAIL: Execution terminated";
      if (!runResult.errMsg.empty())
        detail += ": " + runResult.errMsg;
      return makeResult(ResultCode::ExecuteFailed, detail);
    }
    if (!expectRuntimeError) {
      return makeResult(
          ResultCode::ExecuteFailed,
          "FAIL: Execution threw unexpected error: " + runResult.stderrStr);
    }
    // Expected runtime error, but check async test patterns.
    if (isAsync &&
        (runResult.stdoutStr.find("Test262:AsyncTestFailure") !=
             std::string::npos ||
         runResult.stdoutStr.find("Test262:AsyncTestComplete") ==
             std::string::npos)) {
      return makeResult(
          ResultCode::ExecuteFailed,
          "FAIL: Async test failure: " + runResult.stdoutStr);
    }
    return makeResult(ResultCode::Passed, "PASS (expected runtime error)");
  }

  // Exit code 0.
  if (expectRuntimeError) {
    return makeResult(
        ResultCode::ExecuteFailed,
        "FAIL: Expected runtime error but test passed");
  }

  return makeResult(ResultCode::Passed, "PASS");
}

void WorkQueue::push(std::function<void()> task) {
  std::lock_guard<std::mutex> lock(mutex_);
  tasks_.push_back(std::move(task));
  cv_.notify_one();
}

bool WorkQueue::pop(std::function<void()> &task) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return !tasks_.empty() || done_; });
  if (tasks_.empty())
    return false;
  task = std::move(tasks_.front());
  tasks_.pop_front();
  return true;
}

void WorkQueue::finish() {
  std::lock_guard<std::mutex> lock(mutex_);
  done_ = true;
  cv_.notify_all();
}

void runAllTests(
    const std::vector<TestEntry> &tests,
    const HarnessCache &harness,
    const Skiplist *skiplist,
    const ExecConfig &config,
    std::vector<TestResult> &results,
    std::atomic<size_t> &featureSkipped,
    std::atomic<size_t> &permanentFeatureSkipped) {
  std::mutex resultsMutex;
  std::atomic<size_t> completedCount{0};
  size_t totalTests = tests.size();
  std::atomic<unsigned> lastPrintedPct{0};

  llvh::outs() << "Testing: 0 ..";
  llvh::outs().flush();

  WorkQueue queue;
  std::vector<std::thread> workers;

  unsigned numWorkers = std::min(config.numThreads, (unsigned)tests.size());
  if (numWorkers == 0)
    numWorkers = 1;

  for (unsigned i = 0; i < numWorkers; ++i) {
    workers.emplace_back([&] {
      std::function<void()> task;
      while (queue.pop(task)) {
        task();
      }
    });
  }

  for (const auto &entry : tests) {
    queue.push([&entry,
                &harness,
                skiplist,
                &config,
                &results,
                &resultsMutex,
                &completedCount,
                totalTests,
                &featureSkipped,
                &permanentFeatureSkipped,
                &lastPrintedPct] {
      processTestEntry(
          entry,
          harness,
          skiplist,
          config,
          results,
          resultsMutex,
          featureSkipped,
          permanentFeatureSkipped);
      reportProgress(completedCount, totalTests, lastPrintedPct);
    });
  }

  queue.finish();
  for (auto &w : workers) {
    w.join();
  }

  llvh::outs() << " \n";
}

} // namespace testrunner
} // namespace hermes
