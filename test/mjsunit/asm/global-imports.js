// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax --validate-asm

function MODULE_TEMPLATE(stdlib, foreign, buffer) {
  "use asm";
  var fround = stdlib.Math.fround;
  IMPORT;
  function f(int, flt, dbl) {
    int = int | 0;
    flt = fround(flt);
    dbl = +dbl;
    return EXPRESSION;
  }
  return { f:f };
}

var throws = {};
var test_count = 0;
const stdlib = this;
const buffer = new ArrayBuffer(1024);

function assertThrowsOrEquals(result, fun) {
  if (result === throws) {
    assertThrows(fun, TypeError);
  } else {
    assertEquals(result, fun(1, 2.3, 4.2));
  }
}

function RunAsmJsTest(asm_source, imports, result, valid) {
  var nonasm_source = asm_source.replace(new RegExp("use asm"), "");

  var js_module = eval("(" + nonasm_source + ")")
  var js_instance = js_module(stdlib, imports, buffer);
  assertThrowsOrEquals(result, js_instance.f);

  var asm_module = eval("(" + asm_source + ")");
  var asm_instance = asm_module(stdlib, imports, buffer);
  assertEquals(valid, %IsAsmWasmCode(asm_module));
  assertThrowsOrEquals(result, asm_instance.f);
}

function Run(imp, exp, imports, result, valid) {
  var name = "test" + (++test_count);
  var src = MODULE_TEMPLATE.toString();
  src = src.replace("IMPORT", imp);
  src = src.replace("EXPRESSION", exp);
  src = src.replace("MODULE_TEMPLATE", name);
  RunAsmJsTest(src, imports, result, valid);
}

Run("var x = foreign.x | 0",   "(x + int) | 0", {x:12},  13,     true);
Run("var x = foreign.x | 0",   "(x = int) | 0", {x:12},  1,      true);
Run("var x = foreign.x | 0",   "+(x + dbl)",    {x:12},  16.2,   false);
Run("var x = +foreign.x",      "+(x + dbl)",    {x:1.2}, 5.4,    true);
Run("var x = +foreign.x",      "+(x = dbl)",    {x:1.2}, 4.2,    true);
Run("var x = +foreign.x",      "(x + int) | 0", {x:1.2}, 2,      false);
Run("const x = foreign.x | 0", "(x + int) | 0", {x:12},  13,     true);
Run("const x = foreign.x | 0", "(x = int) | 0", {x:12},  throws, false);
Run("const x = foreign.x | 0", "+(x + dbl)",    {x:12},  16.2,   false);
Run("const x = +foreign.x",    "+(x + dbl)",    {x:1.2}, 5.4,    true);
Run("const x = +foreign.x",    "+(x = dbl)",    {x:1.2}, throws, false);
Run("const x = +foreign.x",    "(x + int) | 0", {x:1.2}, 2,      false);
