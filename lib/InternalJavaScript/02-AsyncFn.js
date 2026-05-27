/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

function initAsyncFn() {
  // promiseCapability.[[Promise]]
  var HermesPromise = globalThis.Promise;

  // Capture the polyfill's internal then-equivalent (exposed by
  // 01-Promise.js). Unlike Promise.prototype.then, it skips IsPromise
  // and SpeciesConstructor checks, so the only user-observable
  // `.constructor` read in Await comes from the explicit check below
  // (matching spec §27.2.1.6 PromiseResolve step 1).
  var performInternalThen = internalBytecodeResult.performInternalThen;

  // This spawn function is borrowed from the
  // [original proposal](https://github.com/tc39/proposal-async-await),
  // then it's modified to
  // - use the captured Promise and methods to immune from user-space hijacking.
  // - to take a third argument "args".
  // TODO(the Babel version seem to be a little bit faster.)
  function spawn(genF, self, args) {
    return new HermesPromise(function (resolve, reject) {
      var gen = genF.apply(self, args);
      function step(nextF) {
        var next;
        try {
          next = nextF();
        } catch (e) {
          // finished with failure, reject the promise
          reject(e);
          return;
        }
        if (next.done) {
          // finished with success, resolve the promise
          resolve(next.value);
          return;
        }
        // not finished, chain off the yielded promise and `step` again.
        // Per spec §27.7.5.3 Await + §27.2.1.6 PromiseResolve step 1:
        // return val as-is only when IsPromise(val) AND
        // val.constructor === %Promise%. Subclass instances and
        // thenables fall through to the wrap path so their custom
        // .then / .constructor are observed via the Promise
        // Resolution Procedure.
        var val = next.value;
        var p;
        if (val instanceof HermesPromise && val.constructor === HermesPromise) {
          p = val;
        } else {
          p = new HermesPromise(function (r) { r(val); });
        }
        performInternalThen(p,
          function (v) {
            step(function () {
              return gen.next(v);
            });
          },
          function (e) {
            step(function () {
              return gen.throw(e);
            });
          }
        );
      }
      step(function () {
        return gen.next(undefined);
      });
    });
  }

  // register as "spawnAsync".
  internalBytecodeResult.spawnAsync = spawn;
}

initAsyncFn();
