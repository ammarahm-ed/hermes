/* @nolint */var initPromise = (function () {
  'use strict';
  var useEngineQueue = HermesInternal.useEngineQueue();
  function noop() {}

  // States:
  //
  // 0 - pending
  // 1 - fulfilled with _value
  // 2 - rejected with _value
  // 3 - adopted the state of another promise, _value
  //
  // once the state is no longer pending (0) it is immutable

  // All `_` prefixed properties will be reduced to `_{random number}`
  // at build time to obfuscate them and discourage their use.
  // We don't use symbols or Object.defineProperty to fully hide them
  // because the performance isn't good enough.


  // to avoid using try/catch inside critical functions, we
  // extract them to here.
  var LAST_ERROR = null;
  var IS_ERROR = {};
  function getThen(obj) {
    try {
      return obj.then;
    } catch (ex) {
      LAST_ERROR = ex;
      return IS_ERROR;
    }
  }

  function tryCallOne(fn, a) {
    try {
      return fn(a);
    } catch (ex) {
      LAST_ERROR = ex;
      return IS_ERROR;
    }
  }
  function tryCallTwo(fn, a, b) {
    try {
      fn(a, b);
    } catch (ex) {
      LAST_ERROR = ex;
      return IS_ERROR;
    }
  }

  var core = Promise;

  function Promise(fn) {
    if (typeof this !== 'object') {
      throw new TypeError('Promises must be constructed via new');
    }
    if (typeof fn !== 'function') {
      throw new TypeError('Promise constructor\'s argument is not a function');
    }
    this._x = 0;
    this._y = 0;
    this._z = null;
    this._A = null;
    if (fn === noop) return;
    doResolve(fn, this);
  }
  Promise._B = null;
  Promise._C = null;
  Promise._D = noop;

  Promise.prototype.then = function(onFulfilled, onRejected) {
    // Per spec §27.2.5.4 step 2: If IsPromise(this) is false, throw
    // TypeError. We approximate IsPromise via instanceof Promise (no
    // real [[PromiseState]] internal slot in pure JS). The check must
    // happen BEFORE reading this.constructor — otherwise a hostile
    // `constructor` getter would observe the call before validation.
    if (!(this instanceof Promise)) {
      throw new TypeError('Promise.prototype.then called on non-Promise');
    }
    // Per spec §27.2.5.4 step 3 + §7.3.22 SpeciesConstructor: when
    // `constructor` is undefined, fall back to %Promise% — i.e. take
    // the fast path. Without this, an `undefined` constructor would
    // hit safeThen → newPromiseCapability(undefined) → spurious
    // TypeError where the spec returns a normal Promise.
    var C = this.constructor;
    if (C !== undefined && C !== Promise) {
      return safeThen(this, onFulfilled, onRejected);
    }
    var res = new Promise(noop);
    handle(this, new Handler(onFulfilled, onRejected, res));
    return res;
  };

  function safeThen(self, onFulfilled, onRejected) {
    // Per spec §27.2.5.4: NewPromiseCapability runs BEFORE
    // PerformPromiseThen attaches the reaction.
    var capability = newPromiseCapability(self.constructor);
    // Intermediate core Promise that holds the result of running
    // onFulfilled/onRejected when self settles.
    var res = new Promise(noop);
    handle(self, new Handler(onFulfilled, onRejected, res));
    // Chain res's settlement to the C-typed capability.
    handle(res, new Handler(capability.resolve, capability.reject,
      new Promise(noop)));
    return capability.promise;
  }

  // Per spec §27.2.1.5 NewPromiseCapability(C). Returns a record with
  // `promise`, `resolve`, and `reject`. Throws TypeError if C is not a
  // constructor, if the executor closure is called when [[Resolve]] or
  // [[Reject]] is already non-undefined (per spec's
  // GetCapabilitiesExecutor §27.2.1.5.1 steps 3-4), or if resolve/reject
  // are not callable after C's constructor returns.
  function newPromiseCapability(C) {
    if (C === null || (typeof C !== 'object' && typeof C !== 'function')) {
      throw new TypeError('Promise constructor is not an object');
    }
    var resolve, reject;
    var promise = new C(function (resolveArg, rejectArg) {
      // Per spec §27.2.1.5.1 steps 3-4: only throw if the corresponding
      // slot has already been set to a non-undefined value. Calls with
      // undefined args from a constructor that calls executor multiple
      // times are tolerated as long as no slot was non-undefined yet.
      if (resolve !== undefined) {
        throw new TypeError('Promise capability resolve already captured');
      }
      if (reject !== undefined) {
        throw new TypeError('Promise capability reject already captured');
      }
      resolve = resolveArg;
      reject = rejectArg;
    });
    if (typeof resolve !== 'function' || typeof reject !== 'function') {
      throw new TypeError('Promise capability resolve/reject is not a function');
    }
    return { promise: promise, resolve: resolve, reject: reject };
  }
  function handle(self, deferred) {
    while (self._y === 3) {
      self = self._z;
    }
    if (Promise._B) {
      Promise._B(self);
    }
    if (self._y === 0) {
      if (self._x === 0) {
        self._x = 1;
        self._A = deferred;
        return;
      }
      if (self._x === 1) {
        self._x = 2;
        self._A = [self._A, deferred];
        return;
      }
      self._A.push(deferred);
      return;
    }
    handleResolved(self, deferred);
  }

  function handleResolved(self, deferred) {
    (useEngineQueue ? HermesInternal.enqueueJob : setImmediate)(function() {
      var cb = self._y === 1 ? deferred.onFulfilled : deferred.onRejected;
      if (cb === null) {
        if (self._y === 1) {
          resolve(deferred.promise, self._z);
        } else {
          reject(deferred.promise, self._z);
        }
        return;
      }
      var ret = tryCallOne(cb, self._z);
      if (ret === IS_ERROR) {
        reject(deferred.promise, LAST_ERROR);
      } else {
        resolve(deferred.promise, ret);
      }
    });
  }
  function resolve(self, newValue) {
    // Promise Resolution Procedure: https://github.com/promises-aplus/promises-spec#the-promise-resolution-procedure
    if (newValue === self) {
      return reject(
        self,
        new TypeError('A promise cannot be resolved with itself.')
      );
    }
    if (
      newValue &&
      (typeof newValue === 'object' || typeof newValue === 'function')
    ) {
      var then = getThen(newValue);
      if (then === IS_ERROR) {
        return reject(self, LAST_ERROR);
      }
      if (
        then === self.then &&
        newValue instanceof Promise &&
        newValue.constructor === Promise
      ) {
        self._y = 3;
        self._z = newValue;
        finale(self);
        return;
      } else if (typeof then === 'function') {
        doResolve(then.bind(newValue), self);
        return;
      }
    }
    self._y = 1;
    self._z = newValue;
    finale(self);
  }

  function reject(self, newValue) {
    self._y = 2;
    self._z = newValue;
    if (Promise._C) {
      Promise._C(self, newValue);
    }
    finale(self);
  }
  function finale(self) {
    if (self._x === 1) {
      handle(self, self._A);
      self._A = null;
    }
    if (self._x === 2) {
      for (var i = 0; i < self._A.length; i++) {
        handle(self, self._A[i]);
      }
      self._A = null;
    }
  }

  function Handler(onFulfilled, onRejected, promise){
    this.onFulfilled = typeof onFulfilled === 'function' ? onFulfilled : null;
    this.onRejected = typeof onRejected === 'function' ? onRejected : null;
    this.promise = promise;
  }

  /**
   * Take a potentially misbehaving resolver function and make sure
   * onFulfilled and onRejected are only called once.
   *
   * Makes no guarantees about asynchrony.
   */
  function doResolve(fn, promise) {
    var done = false;
    var res = tryCallTwo(fn, function (value) {
      if (done) return;
      done = true;
      resolve(promise, value);
    }, function (reason) {
      if (done) return;
      done = true;
      reject(promise, reason);
    });
    if (!done && res === IS_ERROR) {
      done = true;
      reject(promise, LAST_ERROR);
    }
  }

  //This file contains the ES6 extensions to the core Promises/A+ API



  var es6Extensions = core;

  // Per spec §27.2.5.5: Promise.prototype[@@toStringTag] is "Promise".
  Object.defineProperty(core.prototype, Symbol.toStringTag, {
    value: 'Promise',
    writable: false,
    enumerable: false,
    configurable: true
  });

  /* Static Functions */

  var TRUE = valuePromise(true);
  var FALSE = valuePromise(false);
  var NULL = valuePromise(null);
  var UNDEFINED = valuePromise(undefined);
  var ZERO = valuePromise(0);
  var EMPTYSTRING = valuePromise('');

  function valuePromise(value) {
    var p = new core(core._D);
    p._y = 1;
    p._z = value;
    return p;
  }
  core.resolve = function (value) {
    // Per spec §27.2.4.5 step 2: If Type(this) is not Object, throw
    // TypeError. Includes null (typeof null === 'object', but null
    // is the Null type, not Object).
    var C = this;
    if (C === null || (typeof C !== 'object' && typeof C !== 'function')) {
      throw new TypeError('Promise.resolve called on non-object');
    }
    // Per spec §27.2.4.5 step 3: if IsPromise(value) and value's
    // constructor is C, return value. Only read .constructor on actual
    // promises — reading it on arbitrary thenables/objects would be
    // user-observable and break the thenable resolution path.
    if (value instanceof core && value.constructor === C) {
      return value;
    }

    if (C === core) {
      // Fast path for core Promise.
      if (value === null) return NULL;
      if (value === undefined) return UNDEFINED;
      if (value === true) return TRUE;
      if (value === false) return FALSE;
      if (value === 0) return ZERO;
      if (value === '') return EMPTYSTRING;

      if (typeof value === 'object' || typeof value === 'function') {
        try {
          var then = value.then;
          if (typeof then === 'function') {
            return new core(then.bind(value));
          }
        } catch (ex) {
          return new core(function (resolve, reject) {
            reject(ex);
          });
        }
      }
      return valuePromise(value);
    }

    // Subclass path: NewPromiseCapability(C) + capability.[[Resolve]](value)
    // per spec §27.2.4.5 steps 4-6. Extract resolve as a local so the
    // call form passes `this = undefined` (matches spec's
    // Call(F, undefined, args)).
    var capability = newPromiseCapability(C);
    var capResolve = capability.resolve;
    capResolve(value);
    return capability.promise;
  };

  core.all = function (arr) {
    var C = this;
    // Step 2: NewPromiseCapability(C). Throws synchronously if C is
    // not a valid constructor or doesn't capture resolve/reject.
    var capability = newPromiseCapability(C);
    var resultPromise = capability.promise;
    var capResolve = capability.resolve;
    var capReject = capability.reject;
    // Step 3-4: GetPromiseResolve(C), IfAbruptRejectPromise.
    var promiseResolve;
    try {
      promiseResolve = C.resolve;
    } catch (e) {
      capReject(e);
      return resultPromise;
    }
    if (typeof promiseResolve !== 'function') {
      capReject(new TypeError('Promise resolve is not a function'));
      return resultPromise;
    }
    var remainingElementsCount = 1;
    var index = 0;
    var values = [];
    // Per spec §27.2.4.1.3: each Promise.all Resolve Element Function is
    // anonymous (name === ""), captures its own [[AlreadyCalled]], and
    // shares the values array, remainingElementsCount, and capability
    // with its siblings. Returning the closure (rather than naming it
    // via assignment) avoids triggering NamedEvaluation per §8.5.5.
    function makeResolveElement(idx) {
      var alreadyCalled = false;
      return function (value) {
        if (alreadyCalled) return;
        alreadyCalled = true;
        Object.defineProperty(values, idx, {
          value: value, writable: true,
          enumerable: true, configurable: true,
        });
        if (--remainingElementsCount === 0) {
          capResolve(values);
        }
      };
    }
    // Step 7-8: PerformPromiseAll, IfAbruptRejectPromise. Catches
    // abrupt completions from the iterator protocol, promiseResolve,
    // nextPromise.then, the fast-path resolveElement call, and the
    // final capResolve (subclass capabilities can throw).
    try {
      for (var item of arr) {
        var nextPromise = promiseResolve.call(C, item);
        var resolveElement = makeResolveElement(index);
        remainingElementsCount++;
        // Non-compliant fast path retained: synchronously invoke
        // resolveElement when the element is an already-fulfilled core
        // Promise, to preserve the 1-microtask-hop timing that recorded
        // synth traces (e.g. twilight_2024-08-29) depend on. This
        // violates PerformPromiseAll step 8.e (spec requires the .then
        // dispatch to enqueue a PromiseReactionJob), and breaks test262
        // S25.4.4.1_A7.2_T1 / A8.1_T1 — both are skiplisted with a
        // pointer back here.
        nextPromise._y === 1 ? resolveElement(nextPromise._z) : nextPromise.then(resolveElement, capReject);
        index++;
      }
      if (--remainingElementsCount === 0) {
        capResolve(values);
      }
    } catch (e) {
      capReject(e);
    }
    return resultPromise;
  };

  core.allSettled = function (iterable) {
    var C = this;
    // Step 2: NewPromiseCapability(C).
    var capability = newPromiseCapability(C);
    var resultPromise = capability.promise;
    var capResolve = capability.resolve;
    var capReject = capability.reject;
    // Step 3-4: GetPromiseResolve(C), IfAbruptRejectPromise.
    var promiseResolve;
    try {
      promiseResolve = C.resolve;
    } catch (e) {
      capReject(e);
      return resultPromise;
    }
    if (typeof promiseResolve !== 'function') {
      capReject(new TypeError('Promise resolve is not a function'));
      return resultPromise;
    }
    var values = [];
    var remainingElementsCount = 1;
    var index = 0;
    // Per spec §27.2.4.2.1 step q: onRejected and onFulfilled share the
    // same [[AlreadyCalled]] record, so once either fires the other is
    // blocked for this index. Use an array literal (not an object
    // literal) so the inner functions are anonymous (name "") per
    // spec — object property assignment would trigger NamedEvaluation
    // and infer the names from the keys.
    function makeSettledPair(idx) {
      var alreadyCalled = false;
      return [
        function(value) {
          if (alreadyCalled) return;
          alreadyCalled = true;
          Object.defineProperty(values, idx, {
            value: { status: 'fulfilled', value: value },
            writable: true, enumerable: true, configurable: true,
          });
          if (--remainingElementsCount === 0) {
            capResolve(values);
          }
        },
        function(reason) {
          if (alreadyCalled) return;
          alreadyCalled = true;
          Object.defineProperty(values, idx, {
            value: { status: 'rejected', reason: reason },
            writable: true, enumerable: true, configurable: true,
          });
          if (--remainingElementsCount === 0) {
            capResolve(values);
          }
        },
      ];
    }
    // Step 7-8: PerformPromiseAllSettled, IfAbruptRejectPromise.
    try {
      for (var item of iterable) {
        var nextPromise = promiseResolve.call(C, item);
        var pair = makeSettledPair(index);
        remainingElementsCount++;
        nextPromise.then(pair[0], pair[1]);
        index++;
      }
      if (--remainingElementsCount === 0) {
        capResolve(values);
      }
    } catch (e) {
      capReject(e);
    }
    return resultPromise;
  };

  core.reject = function (value) {
    // Per spec §27.2.4.4 steps 1-5: NewPromiseCapability(C) +
    // capability.[[Reject]](value).
    var C = this;
    if (C === null || (typeof C !== 'object' && typeof C !== 'function')) {
      throw new TypeError('Promise.reject called on non-object');
    }
    var capability = newPromiseCapability(C);
    var capReject = capability.reject;
    capReject(value);
    return capability.promise;
  };

  core.race = function (values) {
    var C = this;
    // Step 2: NewPromiseCapability(C).
    var capability = newPromiseCapability(C);
    var resultPromise = capability.promise;
    var capResolve = capability.resolve;
    var capReject = capability.reject;
    // Step 3-4: GetPromiseResolve(C), IfAbruptRejectPromise.
    var promiseResolve;
    try {
      promiseResolve = C.resolve;
    } catch (e) {
      capReject(e);
      return resultPromise;
    }
    if (typeof promiseResolve !== 'function') {
      capReject(new TypeError('Promise resolve is not a function'));
      return resultPromise;
    }
    // Step 7-8: PerformPromiseRace, IfAbruptRejectPromise.
    try {
      for (var value of values) {
        promiseResolve.call(C, value).then(capResolve, capReject);
      }
    } catch (e) {
      capReject(e);
    }
    return resultPromise;
  };

  /* Prototype Methods */

  core.prototype['catch'] = function (onRejected) {
    return this.then(null, onRejected);
  };

  function getAggregateError(errors){
    if(typeof AggregateError === 'function'){
      return new AggregateError(errors,'All promises were rejected');
    }

    var error = new Error('All promises were rejected');

    error.name = 'AggregateError';
    error.errors = errors;

    return error;
  }

  core.any = function promiseAny(values) {
    var C = this;
    // Step 2: NewPromiseCapability(C).
    var capability = newPromiseCapability(C);
    var resultPromise = capability.promise;
    var capResolve = capability.resolve;
    var capReject = capability.reject;
    // Step 3-4: GetPromiseResolve(C), IfAbruptRejectPromise.
    var promiseResolve;
    try {
      promiseResolve = C.resolve;
    } catch (e) {
      capReject(e);
      return resultPromise;
    }
    if (typeof promiseResolve !== 'function') {
      capReject(new TypeError('Promise resolve is not a function'));
      return resultPromise;
    }
    var errors = [];
    var remainingElementsCount = 1;
    var index = 0;
    // Per spec §27.2.4.3.1 step r: onFulfilled is capability.[[Resolve]]
    // directly. Only onRejected is wrapped per element with its own
    // [[AlreadyCalled]] flag.
    function makeRejectElement(idx) {
      var alreadyCalled = false;
      return function(reason) {
        if (alreadyCalled) return;
        alreadyCalled = true;
        Object.defineProperty(errors, idx, {
          value: reason, writable: true,
          enumerable: true, configurable: true,
        });
        if (--remainingElementsCount === 0) {
          capReject(getAggregateError(errors));
        }
      };
    }
    // Step 7-8: PerformPromiseAny, IfAbruptRejectPromise.
    try {
      for (var value of values) {
        var rejectElement = makeRejectElement(index);
        remainingElementsCount++;
        promiseResolve.call(C, value).then(capResolve, rejectElement);
        index++;
      }
      if (--remainingElementsCount === 0) {
        capReject(getAggregateError(errors));
      }
    } catch (e) {
      capReject(e);
    }
    return resultPromise;
  };

  core.withResolvers = function () {
    var resolve, reject;
    var promise = new this(function (res, rej) {
      resolve = res;
      reject = rej;
    });
    return { promise: promise, resolve: resolve, reject: reject };
  };

  // Per spec §27.2.4.9 Promise.try ( callback, ...args )
  Object.defineProperty(core, 'try', {
    writable: true,
    enumerable: false,
    configurable: true,
    value: function (callbackfn) {
      var C = this;
      // Step 2-3: NewPromiseCapability(C).
      var capability = newPromiseCapability(C);
      var capResolve = capability.resolve;
      var capReject = capability.reject;
      // Step 4-6: Call the callback. If it throws (abrupt), reject;
      // otherwise resolve. Throws from resolve/reject themselves
      // propagate per spec §27.2.4.9 steps 5.a / 6.a.
      var ret;
      try {
        if (arguments.length <= 1) {
          // Fast path: no extra args. Per spec step 4 args would be
          // an empty List; Call(callbackfn, undefined, «») is
          // equivalent to callbackfn(). Skips the args-array
          // allocation and .apply dispatch.
          ret = callbackfn();
        } else {
          // Collect extra arguments beyond the first.
          var args = [];
          for (var i = 1; i < arguments.length; i++) {
            args.push(arguments[i]);
          }
          ret = callbackfn.apply(undefined, args);
        }
      } catch (e) {
        capReject(e);
        return capability.promise;
      }
      capResolve(ret);
      return capability.promise;
    },
  });
  // Set name to "try" (NamedEvaluation in the object-literal value
  // position above would have inferred "value" instead).
  Object.defineProperty(core['try'], 'name', {
    value: 'try', writable: false, enumerable: false, configurable: true
  });

  // Per spec §27.2.5.3: Promise.prototype.finally ( onFinally )
  Object.defineProperty(core.prototype, 'finally', {
    writable: true,
    enumerable: false,
    configurable: true,
    value: function(onFinally) {
    // Per spec §27.2.5.3 step 2: If Type(this) is not Object, throw
    // TypeError. Includes null (typeof null === 'object', but null
    // is the Null type, not Object).
    if (this === null ||
        (typeof this !== 'object' && typeof this !== 'function')) {
      throw new TypeError('Promise.prototype.finally called on non-object');
    }
    // Spec step 3 calls SpeciesConstructor(this, %Promise%); we don't
    // support @@species (perf), so fall back to this.constructor (gives
    // subclass dispatch for non-species-overriding subclasses).
    var C = this.constructor;
    var thenFinally, catchFinally;
    if (typeof onFinally !== 'function') {
      thenFinally = onFinally;
      catchFinally = onFinally;
    } else {
      // Then Finally Function and Catch Finally Function per spec
      // are anonymous built-in functions (not constructors, length=1).
      // Use arrow functions so they are not constructors. Use helper
      // function to prevent name inference from variable assignment.
      function promiseResolveAndThen(C, result, callback) {
        var promise;
        if (result !== null && result !== undefined &&
            typeof result === 'object' && result.constructor === C) {
          promise = result;
        } else {
          promise = C.resolve(result);
        }
        return promise.then(callback);
      }
      var fns = [(value) => {
        return promiseResolveAndThen(C, onFinally(), () => value);
      }, (reason) => {
        return promiseResolveAndThen(C, onFinally(), () => { throw reason; });
      }];
      thenFinally = fns[0];
      catchFinally = fns[1];
    }
    return this.then(thenFinally, catchFinally);
  }});
  // Set the name property to "finally" (can't use it as identifier).
  Object.defineProperty(core.prototype['finally'], 'name', {
    value: 'finally', writable: false, enumerable: false, configurable: true
  });

  var DEFAULT_WHITELIST = [
    ReferenceError,
    TypeError,
    RangeError
  ];

  var enabled = false;
  var disable_1 = disable;
  function disable() {
    enabled = false;
    core._B = null;
    core._C = null;
  }

  var enable_1 = enable;
  function enable(options) {
    options = options || {};
    if (enabled) disable();
    enabled = true;
    var id = 0;
    var displayId = 0;
    var rejections = {};
    core._B = function (promise) {
      if (
        promise._y === 2 && // IS REJECTED
        rejections[promise._E]
      ) {
        if (rejections[promise._E].logged) {
          onHandled(promise._E);
        } else {
          clearTimeout(rejections[promise._E].timeout);
        }
        delete rejections[promise._E];
      }
    };
    core._C = function (promise, err) {
      if (promise._x === 0) { // not yet handled
        promise._E = id++;
        rejections[promise._E] = {
          displayId: null,
          error: err,
          promise: promise,
          timeout: setTimeout(
            onUnhandled.bind(null, promise._E),
            // For reference errors and type errors, this almost always
            // means the programmer made a mistake, so log them after just
            // 100ms
            // otherwise, wait 2 seconds to see if they get handled
            matchWhitelist(err, DEFAULT_WHITELIST)
              ? 100
              : 2000
          ),
          logged: false
        };
      }
    };
    function onUnhandled(id) {
      if (
        options.allRejections ||
        matchWhitelist(
          rejections[id].error,
          options.whitelist || DEFAULT_WHITELIST
        )
      ) {
        rejections[id].displayId = displayId++;
        if (options.onUnhandled) {
          rejections[id].logged = true;
          options.onUnhandled(
            rejections[id].displayId,
            rejections[id].error,
            rejections[id].promise
          );
        } else {
          rejections[id].logged = true;
          logError(
            rejections[id].displayId,
            rejections[id].error
          );
        }
      }
    }
    function onHandled(id) {
      if (rejections[id].logged) {
        if (options.onHandled) {
          options.onHandled(
            rejections[id].displayId,
            rejections[id].error,
            rejections[id].promise
          );
        } else if (!rejections[id].onUnhandled) {
          console.warn(
            'Promise Rejection Handled (id: ' + rejections[id].displayId + '):'
          );
          console.warn(
            '  This means you can ignore any previous messages of the form "Possible Unhandled Promise Rejection" with id ' +
            rejections[id].displayId + '.'
          );
        }
      }
    }
  }

  function logError(id, error) {
    console.warn('Possible Unhandled Promise Rejection (id: ' + id + '):');
    var errStr = (error && (error.stack || error)) + '';
    errStr.split('\n').forEach(function (line) {
      console.warn('  ' + line);
    });
  }

  function matchWhitelist(error, list) {
    return list.some(function (cls) {
      return error instanceof cls;
    });
  }

  var rejectionTracking = {
  	disable: disable_1,
  	enable: enable_1
  };

  // @nolint
  // This file is used to generate InternalBytecode/Promise.js
  // See InternalBytecode/README.md for more details.





  // expose Promise to global.
  globalThis.Promise = es6Extensions;

  // register the JavaScript implemented `enable` function into
  // the Hermes' internal promise rejection tracker.
  var enableHook = rejectionTracking.enable;
  HermesInternal?.setPromiseRejectionTrackingHook?.(enableHook);

  var promise = {

  };

  return promise;

});
initPromise();
