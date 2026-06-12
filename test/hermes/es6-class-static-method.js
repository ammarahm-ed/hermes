/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xes6-class %s | %FileCheck --match-full-lines %s
// REQUIRES: es6_class

class Parent {
  static nonOverriddenMethod() {
    return "From Parent";
  }

  static overriddenMethod() {
    return "From Parent";
  }
}

class Child extends Parent {
  static overriddenMethod() {
    return "From Child";
  }
}

print(Parent.nonOverriddenMethod());
//CHECK: From Parent

print(Parent.overriddenMethod());
//CHECK: From Parent

print(Child.nonOverriddenMethod());
//CHECK: From Parent

print(Child.overriddenMethod());
//CHECK: From Child
