#!/usr/bin/env tarantool
test_run = require('test_run').new()
ffi = require('ffi')

ffi.cdef('int vy_run_itr_unit_test();')

ffi.C.vy_run_itr_unit_test()
