<?hh
/**
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *
 */

function bar(C $x): void {}

class C {
  public function foo(): void {}
}

function f(?C $x): void {
  bar($x || (new C()));
}
