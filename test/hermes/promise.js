/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s
// RUN: %hermes -Xmicrotask-queue %s | %FileCheck --match-full-lines %s
// RUN: %hermesc -O -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s

print('promise');
// CHECK-LABEL: promise

print(HermesInternal.hasPromise())
// CHECK-NEXT: true

print('all' in Promise);
// CHECK-NEXT: true

print('allSettled' in Promise);
// CHECK-NEXT: true

print('any' in Promise);
// CHECK-NEXT: true

var promise = new Promise(function(res, rej) {
  res('success!');
});

promise.then(function(message) {
  print('Resolved:', message);
});
// CHECK-NEXT: Resolved: success!

// Promise.allSettled per spec §27.2.4.2.
// Mix of fulfilled/rejected; results preserve input order with
// {status, value/reason} shape.
Promise.allSettled([
  Promise.resolve(1),
  Promise.reject('e'),
  Promise.resolve(3),
]).then(function(results) {
  results.forEach(function(r, i) {
    if (r.status === 'fulfilled') {
      print('allSettled[' + i + ']:', r.status, r.value);
    } else {
      print('allSettled[' + i + ']:', r.status, r.reason);
    }
  });
});

// Empty iterable: fulfilled with [].
Promise.allSettled([]).then(function(r) {
  print('allSettled-empty length:', r.length);
});
// CHECK-NEXT: allSettled-empty length: 0
// CHECK-NEXT: allSettled[0]: fulfilled 1
// CHECK-NEXT: allSettled[1]: rejected e
// CHECK-NEXT: allSettled[2]: fulfilled 3

// Promise.race per spec §27.2.4.5.
// First settled wins (fulfillment).
Promise.race([
  Promise.resolve('first'),
  Promise.reject('second'),
]).then(function(v) { print('race-fulfilled:', v); },
        function(e) { print('race-unexpected-reject:', e); });
// CHECK-NEXT: race-fulfilled: first

// First settled wins (rejection).
Promise.race([
  Promise.reject('rej-first'),
  Promise.resolve('res-second'),
]).then(function(v) { print('race-unexpected-fulfill:', v); },
        function(e) { print('race-rejected:', e); });
// CHECK-NEXT: race-rejected: rej-first

var deferred;

HermesInternal.enablePromiseRejectionTracker({
  allRejections: true,
  onUnhandled: function(id, error, p) {
    print("Unhandled:", error, "samePromise:", p === deferred);
  },
});

var rejectedPromise = new Promise(function(res, rej) {
  rej("failure!");
});

// `.then()` attaches a handler to `rejectedPromise`, which fires `_B` and
// clears its rejection entry. The rejection then propagates to the deferred
// promise that `.then()` returns; that's the promise the tracker eventually
// reports as unhandled.
deferred = rejectedPromise.then(function() {
  print('resolved');
});
// CHECK-NEXT: Unhandled: failure! samePromise: true
